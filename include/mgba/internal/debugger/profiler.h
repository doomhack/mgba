/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_DEBUGGER_PROFILER_H
#define M_DEBUGGER_PROFILER_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/debugger/debugger.h>
#include <mgba/internal/debugger/profile.h>
#include <mgba-util/ring-fifo.h>
#include <mgba-util/threading.h>

struct mDebuggerProfiler {
	struct mDebuggerModule d;
	struct mProfile profile;
	struct mProfileRecorder recorder;

	// Emulation thread (producer) state
	bool recording;
	uint32_t lastPC;
	unsigned lastWidth;
	uint64_t lastTime;
	uint64_t segmentStart;
	uint64_t segmentCycles;
	uint32_t segmentInstructions;
	uint32_t startFrame;
	// Filled in by the core while recording, and as of the last step
	struct mDebuggerCycleCounters counters;
	struct mDebuggerCycleCounters lastCounters;
	// The frame in progress, once a whole one has started
	bool frameStarted;
	uint64_t frameStart;
	uint64_t frameIdle;

	// Events go from the emulation thread to a consumer thread that builds the call tree
	struct RingFIFO events;
#ifndef DISABLE_THREADING
	Thread consumer;
	bool consumerRunning;
	bool stopConsumer;
	// Held while the consumer applies events, and by anyone reading the call tree
	Mutex dataMutex;
	Mutex signalMutex;
	Condition dataAvailable;
	Condition spaceAvailable;
#endif
};

void mDebuggerProfilerInit(struct mDebuggerProfiler*);
void mDebuggerProfilerDeinit(struct mDebuggerProfiler*);

bool mDebuggerProfilerLoadSymbols(struct mDebuggerProfiler*, struct VFile* elf, const char* source);
bool mDebuggerProfilerAutoloadSymbols(struct mDebuggerProfiler*);

bool mDebuggerProfilerStart(struct mDebuggerProfiler*);
void mDebuggerProfilerStop(struct mDebuggerProfiler*);
void mDebuggerProfilerReset(struct mDebuggerProfiler*);

// Bring the profile up to date while recording, e.g. to show it. Call from the emulation thread
// while emulation is paused, and call mDebuggerProfilerUnlock when done reading.
void mDebuggerProfilerLock(struct mDebuggerProfiler*);
void mDebuggerProfilerUnlock(struct mDebuggerProfiler*);

CXX_GUARD_END

#endif
