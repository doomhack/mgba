/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/debugger/profiler.h>

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/timing.h>
#include <mgba-util/vfs.h>

// The emulation thread does as little as possible per instruction: it adds the instruction's
// cycles to its counter, and pushes an event when control flow doesn't continue to the next
// instruction. A consumer thread turns the events into the call tree.

#define EVENT_BUFFER_SIZE 0x400000
#define CONSUMER_BATCH 4096
#define CONSUMER_POLL_MS 5
#define MAX_SEGMENT_CYCLES 0x7FFFFFFFU

static void _drain(struct mDebuggerProfiler* profiler) {
	struct mProfileEvent event;
	while (RingFIFORead(&profiler->events, &event, sizeof(event))) {
		mProfileRecorderConsume(&profiler->recorder, &event);
	}
}

#ifndef DISABLE_THREADING
static THREAD_ENTRY _consumerThread(void* context) {
	struct mDebuggerProfiler* profiler = context;
	ThreadSetName("Profiler");
	while (true) {
		size_t processed = 0;
		struct mProfileEvent event;
		MutexLock(&profiler->dataMutex);
		while (processed < CONSUMER_BATCH && RingFIFORead(&profiler->events, &event, sizeof(event))) {
			mProfileRecorderConsume(&profiler->recorder, &event);
			++processed;
		}
		MutexUnlock(&profiler->dataMutex);

		MutexLock(&profiler->signalMutex);
		if (processed) {
			ConditionWake(&profiler->spaceAvailable);
		}
		if (processed < CONSUMER_BATCH) {
			if (profiler->stopConsumer && !RingFIFOSize(&profiler->events)) {
				MutexUnlock(&profiler->signalMutex);
				break;
			}
			ConditionWaitTimed(&profiler->dataAvailable, &profiler->signalMutex, CONSUMER_POLL_MS);
		}
		MutexUnlock(&profiler->signalMutex);
	}
	THREAD_EXIT(0);
}
#endif

static void _push(struct mDebuggerProfiler* profiler, const struct mProfileEvent* event) {
	while (!RingFIFOWrite(&profiler->events, event, sizeof(*event))) {
		// Never drop events: a missing call or return would corrupt the rest of the call tree
#ifndef DISABLE_THREADING
		if (profiler->consumerRunning) {
			MutexLock(&profiler->signalMutex);
			ConditionWake(&profiler->dataAvailable);
			ConditionWaitTimed(&profiler->spaceAvailable, &profiler->signalMutex, 1);
			MutexUnlock(&profiler->signalMutex);
			continue;
		}
#endif
		_drain(profiler);
	}
#ifdef DISABLE_THREADING
	if (RingFIFOSize(&profiler->events) > EVENT_BUFFER_SIZE / 2) {
		_drain(profiler);
	}
#endif
}

static void _emit(struct mDebuggerProfiler* profiler, uint8_t type, const struct mDebuggerRegisterSnapshot* snapshot) {
	struct mProfileEvent event;
	memset(&event, 0, sizeof(event));
	while (profiler->segmentCycles > MAX_SEGMENT_CYCLES) {
		event.type = mPROFILE_EVENT_CYCLES;
		event.cycles = MAX_SEGMENT_CYCLES;
		_push(profiler, &event);
		profiler->segmentCycles -= MAX_SEGMENT_CYCLES;
	}
	event.type = type;
	event.width = profiler->lastWidth;
	event.cycles = profiler->segmentCycles;
	event.instructions = profiler->segmentInstructions;
	event.from = profiler->lastPC;
	if (snapshot) {
		event.to = snapshot->pc;
		event.sp = snapshot->sp;
		event.lr = snapshot->lr;
		event.status = snapshot->status;
	}
	_push(profiler, &event);
	profiler->segmentCycles = 0;
	profiler->segmentInstructions = 0;
}

static void _flushSegment(struct mDebuggerProfiler* profiler) {
	if (profiler->segmentCycles || profiler->segmentInstructions) {
		_emit(profiler, mPROFILE_EVENT_CYCLES, NULL);
	}
}

static void _emitStall(struct mDebuggerProfiler* profiler, uint8_t type, uint16_t arg, uint64_t cycles) {
	struct mProfileEvent event;
	memset(&event, 0, sizeof(event));
	event.type = type;
	event.arg = arg;
	event.from = profiler->lastPC;
	// Always at least one event, so even an SWI that took no time counts as a call
	do {
		event.cycles = cycles > MAX_SEGMENT_CYCLES ? MAX_SEGMENT_CYCLES : (uint32_t) cycles;
		_push(profiler, &event);
		cycles -= event.cycles;
	} while (cycles);
}

// Take up to *budget cycles from a counter that went up from last to now
static uint64_t _take(uint64_t now, uint64_t last, uint64_t* budget) {
	uint64_t cycles = now - last;
	if (cycles > *budget) {
		// The core only counts time that passed during the step, so this shouldn't happen
		cycles = *budget;
	}
	*budget -= cycles;
	return cycles;
}

// The core counted time the CPU didn't run code during this step: split it out of the step's
// cycles into events of its own, and charge the rest to the instruction. Returns whether a frame
// ended.
static bool _chargeStalls(struct mDebuggerProfiler* profiler, uint64_t delta) {
	const struct mDebuggerCycleCounters* counters = &profiler->counters;
	struct mDebuggerCycleCounters* last = &profiler->lastCounters;
	uint64_t budget = delta;
	uint64_t dma[mDEBUGGER_DMA_CHANNELS][mDEBUGGER_DMA_TRIGGERS];
	unsigned channel;
	unsigned trigger;
	for (channel = 0; channel < mDEBUGGER_DMA_CHANNELS; ++channel) {
		for (trigger = 0; trigger < mDEBUGGER_DMA_TRIGGERS; ++trigger) {
			dma[channel][trigger] =
			    _take(counters->dmaCycles[channel][trigger], last->dmaCycles[channel][trigger], &budget);
		}
	}
	uint64_t halt = _take(counters->haltCycles, last->haltCycles, &budget);
	uint64_t swi = _take(counters->swiCycles, last->swiCycles, &budget);
	bool swiCalled = counters->swiCount != last->swiCount;
	bool frameEnded = counters->frames != last->frames;

	// What's left is the instruction's own time. Charge it to the current node first, since the
	// events below are charged to nodes of their own.
	mProfileCount(&profiler->profile, profiler->lastPC, budget);
	profiler->segmentCycles += budget;
	++profiler->segmentInstructions;
	_flushSegment(profiler);

	for (channel = 0; channel < mDEBUGGER_DMA_CHANNELS; ++channel) {
		for (trigger = 0; trigger < mDEBUGGER_DMA_TRIGGERS; ++trigger) {
			if (dma[channel][trigger]) {
				_emitStall(profiler, mPROFILE_EVENT_DMA, channel | (trigger << 2), dma[channel][trigger]);
			}
		}
	}
	if (halt) {
		_emitStall(profiler, mPROFILE_EVENT_HALT, 0, halt);
		profiler->frameIdle += halt;
	}
	if (swiCalled) {
		_emitStall(profiler, mPROFILE_EVENT_SWI, counters->lastSwi, swi);
	}
	*last = *counters;
	return frameEnded;
}

static void _charge(struct mDebuggerProfiler* profiler, uint64_t delta, uint64_t now) {
	if (LIKELY(profiler->counters.updates == profiler->lastCounters.updates)) {
		mProfileCount(&profiler->profile, profiler->lastPC, delta);
		profiler->segmentCycles += delta;
		++profiler->segmentInstructions;
		return;
	}
	if (!_chargeStalls(profiler, delta)) {
		return;
	}
	// A frame ended during this step. The step counts towards the frame that ended, which is
	// right for the usual case of a game halted waiting for VBlank.
	if (profiler->frameStarted) {
		uint64_t cycles = now - profiler->frameStart;
		struct mProfileFrame* frame = mProfileFrameListAppend(&profiler->profile.frameCosts);
		frame->cycles = cycles > UINT32_MAX ? UINT32_MAX : (uint32_t) cycles;
		frame->idleCycles = profiler->frameIdle > frame->cycles ? frame->cycles : (uint32_t) profiler->frameIdle;
	}
	profiler->frameStarted = true;
	profiler->frameStart = now;
	profiler->frameIdle = 0;
}

// Start over from the core's counters as they are now, discarding any partial frame
static void _resyncCounters(struct mDebuggerProfiler* profiler) {
	profiler->lastCounters = profiler->counters;
	profiler->frameStarted = false;
	profiler->frameIdle = 0;
}

static void _stopConsumer(struct mDebuggerProfiler* profiler) {
#ifndef DISABLE_THREADING
	if (profiler->consumerRunning) {
		MutexLock(&profiler->signalMutex);
		profiler->stopConsumer = true;
		ConditionWake(&profiler->dataAvailable);
		MutexUnlock(&profiler->signalMutex);
		ThreadJoin(&profiler->consumer);
		profiler->consumerRunning = false;
	}
#endif
	if (profiler->events.data) {
		_drain(profiler);
	}
}

static void _profilerEntered(struct mDebuggerModule* module, enum mDebuggerEntryReason reason,
                             struct mDebuggerEntryInfo* info) {
	UNUSED(reason);
	UNUSED(info);
	// The profiler never holds emulation paused itself
	module->isPaused = false;
}

static void _releaseCounters(struct mDebuggerProfiler* profiler) {
	if (profiler->d.p && profiler->d.p->cycleCounters == &profiler->counters) {
		profiler->d.p->cycleCounters = NULL;
	}
}

static void _profilerDetached(struct mDebuggerModule* module) {
	// The core mustn't keep counting into a profiler that's going away
	_releaseCounters((struct mDebuggerProfiler*) module);
}

static void _profilerCallback(struct mDebuggerModule* module) {
	struct mDebuggerProfiler* profiler = (struct mDebuggerProfiler*) module;
	if (!profiler->recording) {
		return;
	}
	struct mDebugger* debugger = module->p;
	struct mDebuggerRegisterSnapshot snapshot;
	debugger->platform->registerSnapshot(debugger->platform, &snapshot);
	uint64_t now = mTimingGlobalTime(debugger->core->timing);
	if (now <= profiler->lastTime) {
		if (now < profiler->lastTime) {
			// Time went backwards, from a reset or a state load: close the segment and start the
			// call stack over from where execution is now
			_flushSegment(profiler);
			profiler->profile.elapsedCycles += profiler->lastTime - profiler->segmentStart;
			profiler->segmentStart = now;
			profiler->lastTime = now;
			profiler->lastPC = snapshot.pc;
			profiler->lastWidth = snapshot.width;
			_resyncCounters(profiler);
			_emit(profiler, mPROFILE_EVENT_START, &snapshot);
		}
		// Otherwise nothing ran since the last call, e.g. while another module holds emulation paused
		profiler->lastPC = snapshot.pc;
		profiler->lastWidth = snapshot.width;
		return;
	}
	_charge(profiler, now - profiler->lastTime, now);
	if (snapshot.pc != profiler->lastPC + profiler->lastWidth) {
		_emit(profiler, mPROFILE_EVENT_BRANCH, &snapshot);
	} else if (profiler->segmentCycles > MAX_SEGMENT_CYCLES) {
		_flushSegment(profiler);
	}
	profiler->lastTime = now;
	profiler->lastPC = snapshot.pc;
	profiler->lastWidth = snapshot.width;
}

static void _describeCore(struct mDebuggerProfiler* profiler, struct mCore* core) {
	struct mProfile* profile = &profiler->profile;
	unsigned region;
	for (region = 0; region < mPROFILE_REGIONS; ++region) {
		const struct mCoreMemoryBlock* block = mCoreGetMemoryBlockInfo(core, region << 24);
		if (!block || (block->flags & mCORE_MEMORY_VIRTUAL)) {
			mProfileSetRegionName(profile, region, NULL);
			continue;
		}
		char name[64];
		if (strcmp(block->internalName, "bios") == 0) {
			snprintf(name, sizeof(name), "[BIOS]");
		} else {
			snprintf(name, sizeof(name), "[unknown %s]", block->shortName);
		}
		mProfileSetRegionName(profile, region, name);
	}

	struct mGameInfo info;
	core->getGameInfo(core, &info);
	free(profile->romTitle);
	profile->romTitle = strdup(info.title);
	core->checksum(core, &profile->romCrc32, mCHECKSUM_CRC32);
	free(profile->idleOptimization);
	const char* idleOptimization = mCoreConfigGetValue(&core->config, "idleOptimization");
	// Unset means the core's default, which removes known idle loops
	profile->idleOptimization = strdup(idleOptimization ? idleOptimization : "remove");
}

void mDebuggerProfilerInit(struct mDebuggerProfiler* profiler) {
	memset(profiler, 0, sizeof(*profiler));
	mProfileInit(&profiler->profile);
	mProfileRecorderInit(&profiler->recorder, &profiler->profile);
	profiler->d.type = DEBUGGER_PROFILER;
	profiler->d.entered = _profilerEntered;
	profiler->d.custom = _profilerCallback;
	profiler->d.deinit = _profilerDetached;
#ifndef DISABLE_THREADING
	MutexInit(&profiler->dataMutex);
	MutexInit(&profiler->signalMutex);
	ConditionInit(&profiler->dataAvailable);
	ConditionInit(&profiler->spaceAvailable);
#endif
}

void mDebuggerProfilerDeinit(struct mDebuggerProfiler* profiler) {
	_releaseCounters(profiler);
	profiler->recording = false;
	_stopConsumer(profiler);
	if (profiler->events.data) {
		RingFIFODeinit(&profiler->events);
	}
	mProfileRecorderDeinit(&profiler->recorder);
	mProfileDeinit(&profiler->profile);
#ifndef DISABLE_THREADING
	MutexDeinit(&profiler->dataMutex);
	MutexDeinit(&profiler->signalMutex);
	ConditionDeinit(&profiler->dataAvailable);
	ConditionDeinit(&profiler->spaceAvailable);
#endif
}

bool mDebuggerProfilerLoadSymbols(struct mDebuggerProfiler* profiler, struct VFile* elf, const char* source) {
	if (profiler->recording) {
		// Changing symbols changes the function IDs the call tree refers to
		return false;
	}
	if (!mProfileLoadSymbols(&profiler->profile, elf)) {
		return false;
	}
	mProfileSetSymbolSource(&profiler->profile, source);
	mProfileFinish(&profiler->profile);
	return true;
}

bool mDebuggerProfilerAutoloadSymbols(struct mDebuggerProfiler* profiler) {
#if defined(ENABLE_VFS) && defined(ENABLE_DIRECTORIES)
	if (!profiler->d.p) {
		return false;
	}
	struct mCore* core = profiler->d.p->core;
	if (!core->dirs.base || !core->dirs.baseName[0]) {
		return false;
	}
	char name[PATH_MAX + 4];
	snprintf(name, sizeof(name), "%s.elf", core->dirs.baseName);
	struct VFile* vf = core->dirs.base->openFile(core->dirs.base, name, O_RDONLY);
	if (!vf) {
		return false;
	}
	bool success = mDebuggerProfilerLoadSymbols(profiler, vf, name);
	vf->close(vf);
	return success;
#else
	UNUSED(profiler);
	return false;
#endif
}

bool mDebuggerProfilerStart(struct mDebuggerProfiler* profiler) {
	if (profiler->recording) {
		return true;
	}
	struct mDebugger* debugger = profiler->d.p;
	if (!debugger || debugger->state == DEBUGGER_CREATED || debugger->state == DEBUGGER_SHUTDOWN) {
		return false;
	}
	struct mCore* core = debugger->core;
	if (!core->supportsDebuggerType(core, DEBUGGER_PROFILER) || !debugger->platform->registerSnapshot ||
	    !core->timing) {
		return false;
	}
	if (debugger->cycleCounters && debugger->cycleCounters != &profiler->counters) {
		// Another profiler is recording
		return false;
	}
	_describeCore(profiler, core);
	if (profiler->events.data) {
		RingFIFOClear(&profiler->events);
	} else {
		RingFIFOInit(&profiler->events, EVENT_BUFFER_SIZE);
	}

	struct mDebuggerRegisterSnapshot snapshot;
	debugger->platform->registerSnapshot(debugger->platform, &snapshot);
	profiler->lastPC = snapshot.pc;
	profiler->lastWidth = snapshot.width;
	profiler->lastTime = mTimingGlobalTime(core->timing);
	profiler->segmentStart = profiler->lastTime;
	profiler->segmentCycles = 0;
	profiler->segmentInstructions = 0;
	profiler->startFrame = core->frameCounter(core);
	profiler->profile.finished = false;
	debugger->cycleCounters = &profiler->counters;
	_resyncCounters(profiler);
	_emit(profiler, mPROFILE_EVENT_START, &snapshot);

#ifndef DISABLE_THREADING
	profiler->stopConsumer = false;
	if (ThreadCreate(&profiler->consumer, _consumerThread, profiler) == 0) {
		profiler->consumerRunning = true;
	}
#endif
	profiler->recording = true;
	mDebuggerModuleSetNeedsCallback(&profiler->d);
	return true;
}

void mDebuggerProfilerStop(struct mDebuggerProfiler* profiler) {
	if (!profiler->recording) {
		return;
	}
	struct mCore* core = profiler->d.p->core;
	uint64_t now = mTimingGlobalTime(core->timing);
	if (now > profiler->lastTime) {
		// Time that passed without a callback, such as a single step taken by another module
		_charge(profiler, now - profiler->lastTime, now);
		profiler->lastTime = now;
	}
	_flushSegment(profiler);
	_releaseCounters(profiler);
	profiler->profile.elapsedCycles += profiler->lastTime - profiler->segmentStart;
	uint32_t frame = core->frameCounter(core);
	if (frame >= profiler->startFrame) {
		profiler->profile.frames += frame - profiler->startFrame;
	}
	profiler->recording = false;
	mDebuggerModuleClearNeedsCallback(&profiler->d);
	_stopConsumer(profiler);
	mProfileFinish(&profiler->profile);
}

void mDebuggerProfilerLock(struct mDebuggerProfiler* profiler) {
#ifndef DISABLE_THREADING
	if (profiler->consumerRunning) {
		while (RingFIFOSize(&profiler->events)) {
			MutexLock(&profiler->signalMutex);
			ConditionWake(&profiler->dataAvailable);
			ConditionWaitTimed(&profiler->spaceAvailable, &profiler->signalMutex, 1);
			MutexUnlock(&profiler->signalMutex);
		}
		// Waits for the consumer to finish the batch it's applying
		MutexLock(&profiler->dataMutex);
		return;
	}
#endif
	if (profiler->events.data) {
		_drain(profiler);
	}
}

void mDebuggerProfilerUnlock(struct mDebuggerProfiler* profiler) {
#ifndef DISABLE_THREADING
	if (profiler->consumerRunning) {
		MutexUnlock(&profiler->dataMutex);
	}
#else
	UNUSED(profiler);
#endif
}

void mDebuggerProfilerReset(struct mDebuggerProfiler* profiler) {
	if (!profiler->recording) {
		mProfileClear(&profiler->profile);
		mProfileFinish(&profiler->profile);
		return;
	}
	mDebuggerProfilerLock(profiler);
	mProfileClear(&profiler->profile);
	// The call stack referred to the old tree, so rebuild it from where execution is now
	struct mDebugger* debugger = profiler->d.p;
	struct mDebuggerRegisterSnapshot snapshot;
	debugger->platform->registerSnapshot(debugger->platform, &snapshot);
	struct mProfileEvent start;
	memset(&start, 0, sizeof(start));
	start.type = mPROFILE_EVENT_START;
	start.to = snapshot.pc;
	start.sp = snapshot.sp;
	start.lr = snapshot.lr;
	start.status = snapshot.status;
	mProfileRecorderConsume(&profiler->recorder, &start);
	profiler->segmentStart = profiler->lastTime;
	profiler->segmentCycles = 0;
	profiler->segmentInstructions = 0;
	profiler->startFrame = debugger->core->frameCounter(debugger->core);
	_resyncCounters(profiler);
	mDebuggerProfilerUnlock(profiler);
}
