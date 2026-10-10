/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/debugger/profile.h>

DEFINE_VECTOR(mProfileFunctionList, struct mProfileFunction);

struct mProfileRanking {
	uint64_t cycles;
	size_t index;
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

void mProfileInit(struct mProfile* profile) {
	memset(profile, 0, sizeof(*profile));
	mProfileFunctionListInit(&profile->functions, 0);
}

void mProfileDeinit(struct mProfile* profile) {
	_freeCounters(profile);
	_freeFunctions(profile, 0);
	mProfileFunctionListDeinit(&profile->functions);
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
	_freeFunctions(profile, profile->symbolCount);
	size_t i;
	for (i = 0; i < profile->symbolCount; ++i) {
		struct mProfileFunction* function = mProfileFunctionListGetPointer(&profile->functions, i);
		function->selfCycles = 0;
		function->executions = 0;
	}
	profile->elapsedCycles = 0;
	profile->frames = 0;
	profile->finished = false;
	profile->totalCycles = 0;
	profile->totalExecutions = 0;
}

void mProfileClearSymbols(struct mProfile* profile) {
	_freeFunctions(profile, 0);
	profile->symbolCount = 0;
	profile->finished = false;
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

ssize_t mProfileFindFunctionByName(const struct mProfile* profile, const char* name) {
	size_t i;
	for (i = 0; i < mProfileFunctionListSize(&profile->functions); ++i) {
		if (strcmp(mProfileFunctionListGetConstPointer(&profile->functions, i)->name, name) == 0) {
			return i;
		}
	}
	return -1;
}

static size_t _addPseudoFunction(struct mProfile* profile, const char* name, uint32_t start, uint32_t size) {
	struct mProfileFunction* function = mProfileFunctionListAppend(&profile->functions);
	memset(function, 0, sizeof(*function));
	function->name = strdup(name);
	function->baseLength = strlen(name);
	function->start = start;
	function->size = size;
	function->pseudo = true;
	function->foldInto = -1;
	return mProfileFunctionListSize(&profile->functions) - 1;
}

static size_t _regionFunction(struct mProfile* profile, ssize_t* regionFunctions, unsigned region) {
	if (regionFunctions[region] >= 0) {
		return regionFunctions[region];
	}
	char name[64];
	if (region == mPROFILE_REGIONS) {
		snprintf(name, sizeof(name), "[unknown]");
	} else if (profile->regionNames[region]) {
		snprintf(name, sizeof(name), "%s", profile->regionNames[region]);
	} else {
		snprintf(name, sizeof(name), "[unknown 0x%02X000000]", region);
	}
	uint32_t start = region == mPROFILE_REGIONS ? (uint32_t) mPROFILE_REGIONS << 24 : region << 24;
	uint32_t size = region == mPROFILE_REGIONS ? 0 - start : 1 << 24;
	regionFunctions[region] = _addPseudoFunction(profile, name, start, size);
	return regionFunctions[region];
}

static void _attribute(struct mProfile* profile, size_t index, const struct mProfileCounter* counter) {
	struct mProfileFunction* function = mProfileFunctionListGetPointer(&profile->functions, index);
	if (function->foldInto >= 0) {
		function = mProfileFunctionListGetPointer(&profile->functions, function->foldInto);
	}
	function->selfCycles += counter->cycles;
	function->executions += counter->executions;
}

void mProfileFinish(struct mProfile* profile) {
	_freeFunctions(profile, profile->symbolCount);
	size_t i;
	for (i = 0; i < profile->symbolCount; ++i) {
		struct mProfileFunction* function = mProfileFunctionListGetPointer(&profile->functions, i);
		function->selfCycles = 0;
		function->executions = 0;
	}
	profile->totalCycles = 0;
	profile->totalExecutions = 0;

	ssize_t regionFunctions[mPROFILE_REGIONS + 1];
	for (i = 0; i <= mPROFILE_REGIONS; ++i) {
		regionFunctions[i] = -1;
	}

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
				ssize_t index = mProfileFindFunction(profile, address);
				if (index < 0) {
					index = _regionFunction(profile, regionFunctions, region);
				}
				_attribute(profile, index, counter);
				profile->totalCycles += counter->cycles;
				profile->totalExecutions += counter->executions;
			}
		}
	}
	if (profile->other.executions) {
		_attribute(profile, _regionFunction(profile, regionFunctions, mPROFILE_REGIONS), &profile->other);
		profile->totalCycles += profile->other.cycles;
		profile->totalExecutions += profile->other.executions;
	}
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
		if (!function->executions) {
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
