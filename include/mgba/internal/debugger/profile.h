/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_DEBUGGER_PROFILE_H
#define M_DEBUGGER_PROFILE_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba-util/table.h>
#include <mgba-util/vector.h>

#define mPROFILE_REGIONS 16
#define mPROFILE_PAGE_BITS 12
#define mPROFILE_PAGE_SIZE (1 << mPROFILE_PAGE_BITS)
#define mPROFILE_PAGES (1 << (24 - mPROFILE_PAGE_BITS))
#define mPROFILE_SLOTS (mPROFILE_PAGE_SIZE >> 1)

#define mPROFILE_NONE 0xFFFFFFFFU
#define mPROFILE_ROOT_MAIN 0
#define mPROFILE_ROOT_IRQ 1

struct VFile;

// Pseudo-functions follow the symbols in the function table, in this order. New kinds go at the
// end, so the IDs of existing ones stay the same in saved profiles.
enum mProfilePseudoFunction {
	mPROFILE_PSEUDO_REGION = 0,
	mPROFILE_PSEUDO_OTHER = mPROFILE_PSEUDO_REGION + mPROFILE_REGIONS,
	mPROFILE_PSEUDO_MAIN,
	mPROFILE_PSEUDO_IRQ,
	mPROFILE_PSEUDO_MAX
};

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
	// Interworking helpers such as _call_via_r3, which are skipped over in the call tree
	bool trampoline;
	// Index of the function a linker veneer belongs to, or -1
	ssize_t foldInto;

	// Derived by mProfileFinish
	uint64_t selfCycles;
	uint64_t executions;
	uint64_t calls;
	uint64_t recursiveCalls;
	uint64_t inclusiveCycles;
	uint64_t inclusiveExecutions;
};

struct mProfileNode {
	uint32_t function;
	uint32_t parent;
	uint64_t calls;
	uint64_t recursiveCalls;
	uint64_t selfCycles;
	uint64_t selfExecutions;

	// Derived by mProfileFinish
	uint64_t subtreeCycles;
	uint64_t subtreeExecutions;
};

struct mProfileEdge {
	uint32_t caller;
	uint32_t callee;
	uint64_t calls;
	uint64_t recursiveCalls;
	uint64_t inclusiveCycles;
	uint64_t inclusiveExecutions;
};

DECLARE_VECTOR(mProfileFunctionList, struct mProfileFunction);
DECLARE_VECTOR(mProfileNodeList, struct mProfileNode);
DECLARE_VECTOR(mProfileEdgeList, struct mProfileEdge);

struct mProfile {
	// Per-instruction counters: regions (address >> 24) -> pages -> halfword slots
	struct mProfileCounter** pages[mPROFILE_REGIONS];
	// Instructions at addresses above the last region
	struct mProfileCounter other;

	// Symbols sorted by address, followed by mPROFILE_PSEUDO_MAX pseudo-functions
	struct mProfileFunctionList functions;
	size_t symbolCount;
	char* regionNames[mPROFILE_REGIONS];

	// Call tree: node 0 is the main root and node 1 the interrupt root. A child always has a
	// higher index than its parent.
	struct mProfileNodeList nodes;
	struct Table children;
	// Recursive entries that were folded into an existing node, by caller and callee function
	struct mProfileEdgeList recursion;
	struct Table recursionIndex;

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
	// Calls between functions, merged over all call sites, including recursive ones
	struct mProfileEdgeList edges;
};

void mProfileInit(struct mProfile*);
void mProfileDeinit(struct mProfile*);
void mProfileClear(struct mProfile*);

bool mProfileLoadSymbols(struct mProfile*, struct VFile* elf);
void mProfileClearSymbols(struct mProfile*);
void mProfileSetSymbolSource(struct mProfile*, const char* source);
void mProfileSetRegionName(struct mProfile*, unsigned region, const char* name);

// Used by symbol loaders: replace the symbols with the ones appended between these calls
void mProfileBeginSymbols(struct mProfile*);
struct mProfileFunction* mProfileAppendSymbol(struct mProfile*, const char* name);
void mProfileEndSymbols(struct mProfile*);

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

static inline uint32_t mProfilePseudoFunction(const struct mProfile* profile, enum mProfilePseudoFunction pseudo) {
	return profile->symbolCount + pseudo;
}

ssize_t mProfileFindFunction(const struct mProfile*, uint32_t address);
uint32_t mProfileFunctionAt(const struct mProfile*, uint32_t address);
ssize_t mProfileFindFunctionByName(const struct mProfile*, const char* name);

uint32_t mProfileGetChild(struct mProfile*, uint32_t parent, uint32_t function);
void mProfileAddRecursion(struct mProfile*, uint32_t caller, uint32_t callee, uint64_t count);

void mProfileFinish(struct mProfile*);
size_t mProfileTopFunctions(const struct mProfile*, size_t* indices, size_t max);

bool mProfileSave(const struct mProfile*, struct VFile*);
bool mProfileLoad(struct mProfile*, struct VFile*);
bool mProfileExportCallgrind(const struct mProfile*, struct VFile*);

// Rebuilding calls, returns and interrupts from control-flow events. This knows nothing about
// the emulator, so it can be tested with synthetic event streams.
enum mProfileEventType {
	mPROFILE_EVENT_BRANCH = 1,
	// Only charges cycles, for very long stretches without a branch
	mPROFILE_EVENT_CYCLES,
	// Recording started or the emulator was reset: rebuild the call stack from scratch
	mPROFILE_EVENT_START,
};

struct mProfileEvent {
	uint8_t type;
	// BRANCH: width of the instruction at from
	uint8_t width;
	uint16_t reserved;
	// Charged to the current call tree node before the event is applied
	uint32_t cycles;
	uint32_t instructions;
	// BRANCH: the last instruction before the jump and its target; START: target is the PC
	uint32_t from;
	uint32_t to;
	// Registers after the jump
	uint32_t sp;
	uint32_t lr;
	uint32_t status;
};

#define mPROFILE_MAX_CONTEXTS 16

struct mProfileRecorderFrame {
	uint32_t baseNode;
	uint32_t node;
	uint32_t returnAddress;
	uint32_t sp;
	uint32_t resumeAddress;
	uint32_t baseOwned;
	uint32_t leafOwned;
	// A call into an interworking stub: the callee is the function the stub jumps to, entered
	// below this node. mPROFILE_NONE once known.
	uint32_t stubCaller;
	uint32_t stubAddress;
	// If it turns out not to be a stub: the function called, or mPROFILE_NONE for a long jump
	uint32_t stubFallback;
	// Interrupt frames: the last instruction before the interrupt, which may itself have jumped
	uint32_t interruptedFrom;
	uint8_t interruptedWidth;
	uint8_t bank;
	uint8_t kind;
};

DECLARE_VECTOR(mProfileRecorderFrameList, struct mProfileRecorderFrame);

struct mProfileRecorder {
	struct mProfile* profile;
	struct mProfileRecorderFrameList frames;
	// For each context (main, then each nested interrupt): the node where each function is
	// currently active on the call path, plus one, or zero
	uint32_t* active[mPROFILE_MAX_CONTEXTS];
	size_t activeSize;
	size_t contextBase;
	unsigned depth;
	uint32_t cacheStart;
	uint32_t cacheEnd;
	uint32_t cacheFunction;
};

void mProfileRecorderInit(struct mProfileRecorder*, struct mProfile*);
void mProfileRecorderDeinit(struct mProfileRecorder*);
void mProfileRecorderConsume(struct mProfileRecorder*, const struct mProfileEvent*);

CXX_GUARD_END

#endif
