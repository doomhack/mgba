/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_DEBUGGER_PROFILE_H
#define M_DEBUGGER_PROFILE_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba-util/vector.h>

#define mPROFILE_REGIONS 16
#define mPROFILE_PAGE_BITS 12
#define mPROFILE_PAGE_SIZE (1 << mPROFILE_PAGE_BITS)
#define mPROFILE_PAGES (1 << (24 - mPROFILE_PAGE_BITS))
#define mPROFILE_SLOTS (mPROFILE_PAGE_SIZE >> 1)

struct VFile;

struct mProfileCounter {
	uint64_t cycles;
	uint64_t executions;
};

struct mProfileFunction {
	// Name as it appears in the symbol table, including any clone suffix such as ".constprop.0"
	char* name;
	// Length of the name without the clone suffix
	size_t baseLength;
	uint32_t start;
	uint32_t size;
	bool thumb;
	bool pseudo;
	// Index of the function a linker veneer belongs to, or -1
	ssize_t foldInto;

	// Derived by mProfileFinish
	uint64_t selfCycles;
	uint64_t executions;
};

DECLARE_VECTOR(mProfileFunctionList, struct mProfileFunction);

struct mProfile {
	// Per-instruction counters: regions (address >> 24) -> pages -> halfword slots
	struct mProfileCounter** pages[mPROFILE_REGIONS];
	// Instructions at addresses above the last region
	struct mProfileCounter other;

	// Symbols, sorted by address, followed by pseudo-functions added by mProfileFinish
	struct mProfileFunctionList functions;
	size_t symbolCount;
	char* regionNames[mPROFILE_REGIONS];

	char* symbolSource;
	char* romTitle;
	uint32_t romCrc32;
	char* idleOptimization;
	uint64_t elapsedCycles;
	uint32_t frames;

	// Derived by mProfileFinish
	bool finished;
	uint64_t totalCycles;
	uint64_t totalExecutions;
};

void mProfileInit(struct mProfile*);
void mProfileDeinit(struct mProfile*);
void mProfileClear(struct mProfile*);

bool mProfileLoadSymbols(struct mProfile*, struct VFile* elf);
void mProfileClearSymbols(struct mProfile*);
void mProfileSetSymbolSource(struct mProfile*, const char* source);
void mProfileSetRegionName(struct mProfile*, unsigned region, const char* name);

struct mProfileCounter* mProfileAllocCounter(struct mProfile*, uint32_t address);
const struct mProfileCounter* mProfileGetCounter(const struct mProfile*, uint32_t address);

static inline void mProfileCount(struct mProfile* profile, uint32_t address, uint64_t cycles) {
	struct mProfileCounter* counter;
	uint32_t region = address >> 24;
	if (region >= mPROFILE_REGIONS) {
		counter = &profile->other;
	} else {
		struct mProfileCounter** directory = profile->pages[region];
		struct mProfileCounter* page =
		    directory ? directory[(address >> mPROFILE_PAGE_BITS) & (mPROFILE_PAGES - 1)] : NULL;
		if (page) {
			counter = &page[(address & (mPROFILE_PAGE_SIZE - 1)) >> 1];
		} else {
			counter = mProfileAllocCounter(profile, address);
		}
	}
	counter->cycles += cycles;
	++counter->executions;
}

void mProfileFinish(struct mProfile*);
ssize_t mProfileFindFunction(const struct mProfile*, uint32_t address);
ssize_t mProfileFindFunctionByName(const struct mProfile*, const char* name);
size_t mProfileTopFunctions(const struct mProfile*, size_t* indices, size_t max);

bool mProfileSave(const struct mProfile*, struct VFile*);
bool mProfileLoad(struct mProfile*, struct VFile*);
bool mProfileExportCallgrind(const struct mProfile*, struct VFile*);

CXX_GUARD_END

#endif
