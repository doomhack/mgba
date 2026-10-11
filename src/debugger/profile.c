/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/debugger/profile.h>

DEFINE_VECTOR(mProfileFunctionList, struct mProfileFunction);
DEFINE_VECTOR(mProfileNodeList, struct mProfileNode);
DEFINE_VECTOR(mProfileEdgeList, struct mProfileEdge);
DEFINE_VECTOR(mProfileFrameList, struct mProfileFrame);

static const char* const _swiNames[mPROFILE_SWIS] = {
	"SoftReset",
	"RegisterRamReset",
	"Halt",
	"Stop",
	"IntrWait",
	"VBlankIntrWait",
	"Div",
	"DivArm",
	"Sqrt",
	"ArcTan",
	"ArcTan2",
	"CpuSet",
	"CpuFastSet",
	"GetBiosChecksum",
	"BgAffineSet",
	"ObjAffineSet",
	"BitUnPack",
	"LZ77UnCompWram",
	"LZ77UnCompVram",
	"HuffUnComp",
	"RLUnCompWram",
	"RLUnCompVram",
	"Diff8bitUnFilterWram",
	"Diff8bitUnFilterVram",
	"Diff16bitUnFilter",
	"SoundBias",
	"SoundDriverInit",
	"SoundDriverMode",
	"SoundDriverMain",
	"SoundDriverVSync",
	"SoundChannelClear",
	"MidiKey2Freq",
	"MusicPlayerOpen",
	"MusicPlayerStart",
	"MusicPlayerStop",
	"MusicPlayerContinue",
	"MusicPlayerFadeOut",
	"MultiBoot",
	"HardReset",
	"CustomHalt",
	"SoundDriverVSyncOff",
	"SoundDriverVSyncOn",
	"SoundGetJumpList",
};

struct mProfileRanking {
	uint64_t cycles;
	size_t index;
};

struct mProfileEdgeKey {
	uint32_t from;
	uint32_t to;
};

static void _freeFunctions(struct mProfile* profile, size_t from) {
	size_t i;
	for (i = from; i < mProfileFunctionListSize(&profile->functions); ++i) {
		free(mProfileFunctionListGetPointer(&profile->functions, i)->name);
	}
	mProfileFunctionListResize(&profile->functions,
	                           (ssize_t) from - (ssize_t) mProfileFunctionListSize(&profile->functions));
}

static void _freeCounters(struct mProfile* profile) {
	unsigned region;
	for (region = 0; region < mPROFILE_REGIONS; ++region) {
		struct mProfileCounter** directory = profile->pages[region];
		if (!directory) {
			continue;
		}
		size_t page;
		for (page = 0; page < mPROFILE_PAGES; ++page) {
			free(directory[page]);
		}
		free(directory);
		profile->pages[region] = NULL;
	}
	memset(&profile->other, 0, sizeof(profile->other));
}

static void _pseudoName(const struct mProfile* profile, unsigned pseudo, char* name, size_t size) {
	if (pseudo < mPROFILE_PSEUDO_OTHER) {
		unsigned region = pseudo - mPROFILE_PSEUDO_REGION;
		if (profile->regionNames[region]) {
			snprintf(name, size, "%s", profile->regionNames[region]);
		} else {
			snprintf(name, size, "[unknown 0x%02X000000]", region);
		}
	} else if (pseudo == mPROFILE_PSEUDO_OTHER) {
		snprintf(name, size, "[unknown]");
	} else if (pseudo == mPROFILE_PSEUDO_MAIN) {
		snprintf(name, size, "[main]");
	} else if (pseudo == mPROFILE_PSEUDO_IRQ) {
		snprintf(name, size, "[IRQ]");
	} else if (pseudo < mPROFILE_PSEUDO_DMA_HBLANK) {
		snprintf(name, size, "[DMA %u]", pseudo - mPROFILE_PSEUDO_DMA);
	} else if (pseudo == mPROFILE_PSEUDO_DMA_HBLANK) {
		snprintf(name, size, "[DMA HBlank]");
	} else if (pseudo == mPROFILE_PSEUDO_DMA_VBLANK) {
		snprintf(name, size, "[DMA VBlank]");
	} else if (pseudo == mPROFILE_PSEUDO_DMA_FIFO) {
		snprintf(name, size, "[DMA sound FIFO]");
	} else if (pseudo == mPROFILE_PSEUDO_DMA_VIDEO) {
		snprintf(name, size, "[DMA video capture]");
	} else if (pseudo == mPROFILE_PSEUDO_IDLE) {
		snprintf(name, size, "[Idle]");
	} else if (pseudo < mPROFILE_PSEUDO_SWI_OTHER) {
		unsigned number = pseudo - mPROFILE_PSEUDO_SWI;
		snprintf(name, size, "[SWI 0x%02X %s]", number, _swiNames[number]);
	} else {
		snprintf(name, size, "[SWI]");
	}
}

static bool _isBucket(const struct mProfile* profile, uint32_t function) {
	return function >= mProfilePseudoFunction(profile, mPROFILE_PSEUDO_DMA) &&
	    function < mProfilePseudoFunction(profile, mPROFILE_PSEUDO_MAX);
}

static void _resetTree(struct mProfile* profile) {
	mProfileNodeListClear(&profile->nodes);
	HashTableClear(&profile->children);
	mProfileEdgeListClear(&profile->recursion);
	HashTableClear(&profile->recursionIndex);
	mProfileEdgeListClear(&profile->edges);

	// The fixed roots are nodes 0 and 1
	mProfileGetChild(profile, mPROFILE_NONE, mProfilePseudoFunction(profile, mPROFILE_PSEUDO_MAIN));
	mProfileGetChild(profile, mPROFILE_NONE, mProfilePseudoFunction(profile, mPROFILE_PSEUDO_IRQ));
}

static void _rebuildPseudoFunctions(struct mProfile* profile) {
	_freeFunctions(profile, profile->symbolCount);
	unsigned pseudo;
	for (pseudo = 0; pseudo < mPROFILE_PSEUDO_MAX; ++pseudo) {
		char name[64];
		_pseudoName(profile, pseudo, name, sizeof(name));
		struct mProfileFunction* function = mProfileFunctionListAppend(&profile->functions);
		memset(function, 0, sizeof(*function));
		function->name = strdup(name);
		function->baseLength = strlen(name);
		function->pseudo = true;
		function->foldInto = -1;
		if (pseudo < mPROFILE_PSEUDO_OTHER) {
			function->start = (pseudo - mPROFILE_PSEUDO_REGION) << 24;
			function->size = 1 << 24;
		} else if (pseudo == mPROFILE_PSEUDO_OTHER) {
			function->start = (uint32_t) mPROFILE_REGIONS << 24;
			function->size = 0 - function->start;
		}
	}
	// Node functions refer to positions in the function table, so the tree starts over
	_resetTree(profile);
	profile->finished = false;
}

void mProfileInit(struct mProfile* profile) {
	memset(profile, 0, sizeof(*profile));
	mProfileFunctionListInit(&profile->functions, 0);
	mProfileNodeListInit(&profile->nodes, 0);
	mProfileEdgeListInit(&profile->recursion, 0);
	mProfileEdgeListInit(&profile->edges, 0);
	mProfileFrameListInit(&profile->frameCosts, 0);
	HashTableInit(&profile->children, 0, NULL);
	HashTableInit(&profile->recursionIndex, 0, NULL);
	_rebuildPseudoFunctions(profile);
}

void mProfileDeinit(struct mProfile* profile) {
	_freeCounters(profile);
	_freeFunctions(profile, 0);
	mProfileFunctionListDeinit(&profile->functions);
	mProfileNodeListDeinit(&profile->nodes);
	mProfileEdgeListDeinit(&profile->recursion);
	mProfileEdgeListDeinit(&profile->edges);
	mProfileFrameListDeinit(&profile->frameCosts);
	HashTableDeinit(&profile->children);
	HashTableDeinit(&profile->recursionIndex);
	unsigned region;
	for (region = 0; region < mPROFILE_REGIONS; ++region) {
		free(profile->regionNames[region]);
	}
	free(profile->symbolSource);
	free(profile->romTitle);
	free(profile->idleOptimization);
	memset(profile, 0, sizeof(*profile));
}

void mProfileClear(struct mProfile* profile) {
	_freeCounters(profile);
	_resetTree(profile);
	size_t i;
	for (i = 0; i < mProfileFunctionListSize(&profile->functions); ++i) {
		struct mProfileFunction* function = mProfileFunctionListGetPointer(&profile->functions, i);
		function->selfCycles = 0;
		function->executions = 0;
		function->calls = 0;
		function->recursiveCalls = 0;
		function->inclusiveCycles = 0;
		function->inclusiveExecutions = 0;
	}
	profile->elapsedCycles = 0;
	profile->frames = 0;
	mProfileFrameListClear(&profile->frameCosts);
	profile->finished = false;
	profile->totalCycles = 0;
	profile->totalExecutions = 0;
}

void mProfileBeginSymbols(struct mProfile* profile) {
	_freeFunctions(profile, 0);
	profile->symbolCount = 0;
}

struct mProfileFunction* mProfileAppendSymbol(struct mProfile* profile, const char* name) {
	struct mProfileFunction* function = mProfileFunctionListAppend(&profile->functions);
	memset(function, 0, sizeof(*function));
	function->name = strdup(name);
	const char* dot = strchr(name, '.');
	function->baseLength = dot && dot != name ? (size_t) (dot - name) : strlen(name);
	function->trampoline = strncmp(name, "_call_via_", 10) == 0 || strncmp(name, "__call_via_", 11) == 0;
	function->foldInto = -1;
	++profile->symbolCount;
	return function;
}

void mProfileEndSymbols(struct mProfile* profile) {
	_rebuildPseudoFunctions(profile);
}

void mProfileClearSymbols(struct mProfile* profile) {
	mProfileBeginSymbols(profile);
	mProfileEndSymbols(profile);
	free(profile->symbolSource);
	profile->symbolSource = NULL;
}

void mProfileSetSymbolSource(struct mProfile* profile, const char* source) {
	free(profile->symbolSource);
	profile->symbolSource = source ? strdup(source) : NULL;
}

void mProfileSetRegionName(struct mProfile* profile, unsigned region, const char* name) {
	if (region >= mPROFILE_REGIONS) {
		return;
	}
	free(profile->regionNames[region]);
	profile->regionNames[region] = name ? strdup(name) : NULL;

	unsigned pseudo = mPROFILE_PSEUDO_REGION + region;
	size_t index = mProfilePseudoFunction(profile, pseudo);
	if (index >= mProfileFunctionListSize(&profile->functions)) {
		// Between mProfileBeginSymbols and mProfileEndSymbols, which names the pseudo-functions
		return;
	}
	struct mProfileFunction* function = mProfileFunctionListGetPointer(&profile->functions, index);
	char buffer[64];
	_pseudoName(profile, pseudo, buffer, sizeof(buffer));
	free(function->name);
	function->name = strdup(buffer);
	function->baseLength = strlen(buffer);
}

struct mProfileCounter* mProfileAllocCounter(struct mProfile* profile, uint32_t address) {
	uint32_t region = address >> 24;
	if (region >= mPROFILE_REGIONS) {
		return &profile->other;
	}
	struct mProfileCounter** directory = profile->pages[region];
	if (!directory) {
		directory = calloc(mPROFILE_PAGES, sizeof(*directory));
		profile->pages[region] = directory;
	}
	struct mProfileCounter** page = &directory[(address >> mPROFILE_PAGE_BITS) & (mPROFILE_PAGES - 1)];
	if (!*page) {
		*page = calloc(mPROFILE_SLOTS, sizeof(**page));
	}
	return &(*page)[(address & (mPROFILE_PAGE_SIZE - 1)) >> 1];
}

const struct mProfileCounter* mProfileGetCounter(const struct mProfile* profile, uint32_t address) {
	uint32_t region = address >> 24;
	if (region >= mPROFILE_REGIONS) {
		return &profile->other;
	}
	struct mProfileCounter** directory = profile->pages[region];
	if (!directory) {
		return NULL;
	}
	struct mProfileCounter* page = directory[(address >> mPROFILE_PAGE_BITS) & (mPROFILE_PAGES - 1)];
	if (!page) {
		return NULL;
	}
	return &page[(address & (mPROFILE_PAGE_SIZE - 1)) >> 1];
}

ssize_t mProfileFindFunction(const struct mProfile* profile, uint32_t address) {
	size_t low = 0;
	size_t high = profile->symbolCount;
	while (low < high) {
		size_t mid = low + (high - low) / 2;
		const struct mProfileFunction* function = mProfileFunctionListGetConstPointer(&profile->functions, mid);
		if (function->start <= address) {
			low = mid + 1;
		} else {
			high = mid;
		}
	}
	if (!low) {
		return -1;
	}
	const struct mProfileFunction* function = mProfileFunctionListGetConstPointer(&profile->functions, low - 1);
	if (address - function->start >= function->size) {
		return -1;
	}
	return (ssize_t) low - 1;
}

uint32_t mProfileFunctionAt(const struct mProfile* profile, uint32_t address) {
	ssize_t index = mProfileFindFunction(profile, address);
	if (index >= 0) {
		const struct mProfileFunction* function = mProfileFunctionListGetConstPointer(&profile->functions, index);
		return function->foldInto >= 0 ? (uint32_t) function->foldInto : (uint32_t) index;
	}
	uint32_t region = address >> 24;
	if (region >= mPROFILE_REGIONS) {
		return mProfilePseudoFunction(profile, mPROFILE_PSEUDO_OTHER);
	}
	return mProfilePseudoFunction(profile, mPROFILE_PSEUDO_REGION + region);
}

uint32_t mProfileSwiFunction(const struct mProfile* profile, unsigned number) {
	if (number >= mPROFILE_SWIS) {
		return mProfilePseudoFunction(profile, mPROFILE_PSEUDO_SWI_OTHER);
	}
	return mProfilePseudoFunction(profile, mPROFILE_PSEUDO_SWI + number);
}

void mProfileSummarizeFrames(const struct mProfile* profile, struct mProfileFrameSummary* summary) {
	memset(summary, 0, sizeof(*summary));
	size_t i;
	for (i = 0; i < mProfileFrameListSize(&profile->frameCosts); ++i) {
		const struct mProfileFrame* frame = mProfileFrameListGetConstPointer(&profile->frameCosts, i);
		uint32_t busy = frame->cycles - frame->idleCycles;
		summary->cycles += frame->cycles;
		summary->busyCycles += busy;
		if (!summary->count || busy > summary->worstBusyCycles) {
			summary->worst = i;
			summary->worstCycles = frame->cycles;
			summary->worstBusyCycles = busy;
		}
		++summary->count;
	}
}

ssize_t mProfileFindFunctionByName(const struct mProfile* profile, const char* name) {
	size_t i;
	for (i = 0; i < mProfileFunctionListSize(&profile->functions); ++i) {
		if (strcmp(mProfileFunctionListGetConstPointer(&profile->functions, i)->name, name) == 0) {
			return i;
		}
	}
	return -1;
}

uint32_t mProfileGetChild(struct mProfile* profile, uint32_t parent, uint32_t function) {
	struct mProfileEdgeKey key = { parent, function };
	uintptr_t index = (uintptr_t) HashTableLookupBinary(&profile->children, &key, sizeof(key));
	if (index) {
		return index - 1;
	}
	struct mProfileNode* node = mProfileNodeListAppend(&profile->nodes);
	memset(node, 0, sizeof(*node));
	node->function = function;
	node->parent = parent;
	index = mProfileNodeListSize(&profile->nodes);
	HashTableInsertBinary(&profile->children, &key, sizeof(key), (void*) index);
	return index - 1;
}

static struct mProfileEdge* _edge(struct mProfileEdgeList* list, struct Table* index, uint32_t caller,
                                  uint32_t callee) {
	struct mProfileEdgeKey key = { caller, callee };
	uintptr_t position = (uintptr_t) HashTableLookupBinary(index, &key, sizeof(key));
	if (position) {
		return mProfileEdgeListGetPointer(list, position - 1);
	}
	struct mProfileEdge* edge = mProfileEdgeListAppend(list);
	memset(edge, 0, sizeof(*edge));
	edge->caller = caller;
	edge->callee = callee;
	HashTableInsertBinary(index, &key, sizeof(key), (void*) (uintptr_t) mProfileEdgeListSize(list));
	return edge;
}

void mProfileAddRecursion(struct mProfile* profile, uint32_t caller, uint32_t callee, uint64_t count) {
	_edge(&profile->recursion, &profile->recursionIndex, caller, callee)->recursiveCalls += count;
}

static bool _hasAncestorFunction(const struct mProfile* profile, const struct mProfileNode* node) {
	uint32_t parent = node->parent;
	while (parent != mPROFILE_NONE) {
		const struct mProfileNode* ancestor = mProfileNodeListGetConstPointer(&profile->nodes, parent);
		if (ancestor->function == node->function) {
			return true;
		}
		parent = ancestor->parent;
	}
	return false;
}

void mProfileFinish(struct mProfile* profile) {
	size_t functionCount = mProfileFunctionListSize(&profile->functions);
	size_t i;
	for (i = 0; i < functionCount; ++i) {
		struct mProfileFunction* function = mProfileFunctionListGetPointer(&profile->functions, i);
		function->selfCycles = 0;
		function->executions = 0;
		function->calls = 0;
		function->recursiveCalls = 0;
		function->inclusiveCycles = 0;
		function->inclusiveExecutions = 0;
	}
	profile->totalCycles = 0;
	profile->totalExecutions = 0;

	// Self costs come from the per-instruction counters
	unsigned region;
	for (region = 0; region < mPROFILE_REGIONS; ++region) {
		struct mProfileCounter** directory = profile->pages[region];
		if (!directory) {
			continue;
		}
		size_t page;
		for (page = 0; page < mPROFILE_PAGES; ++page) {
			const struct mProfileCounter* counters = directory[page];
			if (!counters) {
				continue;
			}
			size_t slot;
			for (slot = 0; slot < mPROFILE_SLOTS; ++slot) {
				const struct mProfileCounter* counter = &counters[slot];
				if (!counter->executions) {
					continue;
				}
				uint32_t address = (region << 24) | (uint32_t) (page << mPROFILE_PAGE_BITS) | (uint32_t) (slot << 1);
				struct mProfileFunction* function =
				    mProfileFunctionListGetPointer(&profile->functions, mProfileFunctionAt(profile, address));
				function->selfCycles += counter->cycles;
				function->executions += counter->executions;
				profile->totalCycles += counter->cycles;
				profile->totalExecutions += counter->executions;
			}
		}
	}
	if (profile->other.executions) {
		struct mProfileFunction* function =
		    mProfileFunctionListGetPointer(&profile->functions, mProfilePseudoFunction(profile, mPROFILE_PSEUDO_OTHER));
		function->selfCycles += profile->other.cycles;
		function->executions += profile->other.executions;
		profile->totalCycles += profile->other.cycles;
		profile->totalExecutions += profile->other.executions;
	}

	// Time not spent running code has no per-instruction counters, only call tree nodes
	size_t nodeCount = mProfileNodeListSize(&profile->nodes);
	for (i = 0; i < nodeCount; ++i) {
		const struct mProfileNode* node = mProfileNodeListGetConstPointer(&profile->nodes, i);
		if (_isBucket(profile, node->function)) {
			mProfileFunctionListGetPointer(&profile->functions, node->function)->selfCycles += node->selfCycles;
			profile->totalCycles += node->selfCycles;
		}
	}

	// Subtree costs: children always come after their parents
	for (i = 0; i < nodeCount; ++i) {
		struct mProfileNode* node = mProfileNodeListGetPointer(&profile->nodes, i);
		node->subtreeCycles = node->selfCycles;
		node->subtreeExecutions = node->selfExecutions;
	}
	for (i = nodeCount; i-- > 0;) {
		struct mProfileNode* node = mProfileNodeListGetPointer(&profile->nodes, i);
		if (node->parent != mPROFILE_NONE) {
			struct mProfileNode* parent = mProfileNodeListGetPointer(&profile->nodes, node->parent);
			parent->subtreeCycles += node->subtreeCycles;
			parent->subtreeExecutions += node->subtreeExecutions;
		}
	}

	// Per-function call counts and inclusive costs, and calls between functions
	struct Table edgeIndex;
	HashTableInit(&edgeIndex, 0, NULL);
	mProfileEdgeListClear(&profile->edges);
	for (i = 0; i < nodeCount; ++i) {
		const struct mProfileNode* node = mProfileNodeListGetConstPointer(&profile->nodes, i);
		if (node->function >= functionCount) {
			continue;
		}
		struct mProfileFunction* function = mProfileFunctionListGetPointer(&profile->functions, node->function);
		function->calls += node->calls;
		function->recursiveCalls += node->recursiveCalls;
		bool nested = _hasAncestorFunction(profile, node);
		if (!nested) {
			function->inclusiveCycles += node->subtreeCycles;
			function->inclusiveExecutions += node->subtreeExecutions;
		}
		if (node->parent != mPROFILE_NONE) {
			const struct mProfileNode* parent = mProfileNodeListGetConstPointer(&profile->nodes, node->parent);
			struct mProfileEdge* edge = _edge(&profile->edges, &edgeIndex, parent->function, node->function);
			edge->calls += node->calls;
			if (!nested) {
				edge->inclusiveCycles += node->subtreeCycles;
				edge->inclusiveExecutions += node->subtreeExecutions;
			}
		}
	}
	for (i = 0; i < mProfileEdgeListSize(&profile->recursion); ++i) {
		const struct mProfileEdge* recursion = mProfileEdgeListGetConstPointer(&profile->recursion, i);
		_edge(&profile->edges, &edgeIndex, recursion->caller, recursion->callee)->recursiveCalls +=
		    recursion->recursiveCalls;
	}
	HashTableDeinit(&edgeIndex);
	profile->finished = true;
}

static int _compareRanking(const void* a, const void* b) {
	const struct mProfileRanking* ra = a;
	const struct mProfileRanking* rb = b;
	if (ra->cycles != rb->cycles) {
		return ra->cycles < rb->cycles ? 1 : -1;
	}
	if (ra->index != rb->index) {
		return ra->index < rb->index ? -1 : 1;
	}
	return 0;
}

size_t mProfileTopFunctions(const struct mProfile* profile, size_t* indices, size_t max) {
	size_t count = mProfileFunctionListSize(&profile->functions);
	struct mProfileRanking* rankings = malloc(sizeof(*rankings) * (count ? count : 1));
	size_t ranked = 0;
	size_t i;
	for (i = 0; i < count; ++i) {
		const struct mProfileFunction* function = mProfileFunctionListGetConstPointer(&profile->functions, i);
		if (!function->executions && !function->selfCycles) {
			continue;
		}
		rankings[ranked].cycles = function->selfCycles;
		rankings[ranked].index = i;
		++ranked;
	}
	qsort(rankings, ranked, sizeof(*rankings), _compareRanking);
	if (ranked > max) {
		ranked = max;
	}
	for (i = 0; i < ranked; ++i) {
		indices[i] = rankings[i].index;
	}
	free(rankings);
	return ranked;
}
