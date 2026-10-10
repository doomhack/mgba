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

static void _profilerEntered(struct mDebuggerModule* module, enum mDebuggerEntryReason reason,
                             struct mDebuggerEntryInfo* info) {
	UNUSED(reason);
	UNUSED(info);
	// The profiler never holds emulation paused itself
	module->isPaused = false;
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
			// Time went backwards, from a reset or a state load: close the segment and resynchronize
			profiler->profile.elapsedCycles += profiler->lastTime - profiler->segmentStart;
			profiler->segmentStart = now;
			profiler->lastTime = now;
		}
		// Otherwise nothing ran since the last call, e.g. while another module holds emulation paused
		profiler->lastPC = snapshot.pc;
		return;
	}
	mProfileCount(&profiler->profile, profiler->lastPC, now - profiler->lastTime);
	profiler->lastTime = now;
	profiler->lastPC = snapshot.pc;
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
	profile->idleOptimization = idleOptimization ? strdup(idleOptimization) : NULL;
}

void mDebuggerProfilerInit(struct mDebuggerProfiler* profiler) {
	memset(profiler, 0, sizeof(*profiler));
	mProfileInit(&profiler->profile);
	profiler->d.type = DEBUGGER_PROFILER;
	profiler->d.entered = _profilerEntered;
	profiler->d.custom = _profilerCallback;
}

void mDebuggerProfilerDeinit(struct mDebuggerProfiler* profiler) {
	mProfileDeinit(&profiler->profile);
	profiler->recording = false;
}

bool mDebuggerProfilerLoadSymbols(struct mDebuggerProfiler* profiler, struct VFile* elf, const char* source) {
	if (!mProfileLoadSymbols(&profiler->profile, elf)) {
		return false;
	}
	mProfileSetSymbolSource(&profiler->profile, source);
	if (!profiler->recording) {
		mProfileFinish(&profiler->profile);
	}
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
	_describeCore(profiler, core);

	struct mDebuggerRegisterSnapshot snapshot;
	debugger->platform->registerSnapshot(debugger->platform, &snapshot);
	profiler->lastPC = snapshot.pc;
	profiler->lastTime = mTimingGlobalTime(core->timing);
	profiler->segmentStart = profiler->lastTime;
	profiler->startFrame = core->frameCounter(core);
	profiler->profile.finished = false;
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
		mProfileCount(&profiler->profile, profiler->lastPC, now - profiler->lastTime);
		profiler->lastTime = now;
	}
	profiler->profile.elapsedCycles += profiler->lastTime - profiler->segmentStart;
	uint32_t frame = core->frameCounter(core);
	if (frame >= profiler->startFrame) {
		profiler->profile.frames += frame - profiler->startFrame;
	}
	profiler->recording = false;
	mDebuggerModuleClearNeedsCallback(&profiler->d);
	mProfileFinish(&profiler->profile);
}

void mDebuggerProfilerReset(struct mDebuggerProfiler* profiler) {
	mProfileClear(&profiler->profile);
	if (profiler->recording) {
		profiler->segmentStart = profiler->lastTime;
		profiler->startFrame = profiler->d.p->core->frameCounter(profiler->d.p->core);
	} else {
		mProfileFinish(&profiler->profile);
	}
}
