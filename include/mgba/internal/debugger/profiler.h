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

struct mDebuggerProfiler {
	struct mDebuggerModule d;
	struct mProfile profile;

	bool recording;
	uint32_t lastPC;
	uint64_t lastTime;
	uint64_t segmentStart;
	uint32_t startFrame;
};

void mDebuggerProfilerInit(struct mDebuggerProfiler*);
void mDebuggerProfilerDeinit(struct mDebuggerProfiler*);

bool mDebuggerProfilerLoadSymbols(struct mDebuggerProfiler*, struct VFile* elf, const char* source);
bool mDebuggerProfilerAutoloadSymbols(struct mDebuggerProfiler*);

bool mDebuggerProfilerStart(struct mDebuggerProfiler*);
void mDebuggerProfilerStop(struct mDebuggerProfiler*);
void mDebuggerProfilerReset(struct mDebuggerProfiler*);

CXX_GUARD_END

#endif
