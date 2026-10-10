/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/debugger/profile.h>

#include <mgba/core/version.h>
#include <mgba-util/vfs.h>

#include <stdarg.h>

// .mprof is line-oriented text. Each line is a record type followed by its fields, and any
// free-text field (names, titles, paths) comes last so it can contain spaces. Readers skip
// record types they don't know, so later versions can add records without breaking them.
#define MPROF_MAGIC "mprof"
#define MPROF_VERSION 1

struct mProfileSavedNode {
	int64_t parent;
	int64_t function;
	uint64_t calls;
	uint64_t recursiveCalls;
	uint64_t selfCycles;
	uint64_t selfExecutions;
};

DECLARE_VECTOR(mProfileSavedNodeList, struct mProfileSavedNode);
DEFINE_VECTOR(mProfileSavedNodeList, struct mProfileSavedNode);

ATTRIBUTE_FORMAT(printf, 2, 3)
static bool _writef(struct VFile* vf, const char* format, ...) {
	char buffer[1024];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (length < 0) {
		return false;
	}
	if ((size_t) length < sizeof(buffer)) {
		return vf->write(vf, buffer, length) == length;
	}
	char* large = malloc(length + 1);
	va_start(args, format);
	vsnprintf(large, length + 1, format, args);
	va_end(args);
	bool success = vf->write(vf, large, length) == length;
	free(large);
	return success;
}

typedef bool (*mProfileCounterVisitor)(uint32_t address, const struct mProfileCounter*, void* user);

static bool _visitCounters(const struct mProfile* profile, mProfileCounterVisitor visitor, void* user) {
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
				if (!counters[slot].executions) {
					continue;
				}
				uint32_t address = (region << 24) | (uint32_t) (page << mPROFILE_PAGE_BITS) | (uint32_t) (slot << 1);
				if (!visitor(address, &counters[slot], user)) {
					return false;
				}
			}
		}
	}
	return true;
}

static bool _saveCounter(uint32_t address, const struct mProfileCounter* counter, void* user) {
	return _writef(user, "count %08X %" PRIu64 " %" PRIu64 "\n", address, counter->cycles, counter->executions);
}

bool mProfileSave(const struct mProfile* profile, struct VFile* vf) {
	if (!_writef(vf, "%s %i\n", MPROF_MAGIC, MPROF_VERSION)) {
		return false;
	}
	_writef(vf, "meta mgba-version %s\n", projectVersion);
	_writef(vf, "meta elapsed-cycles %" PRIu64 "\n", profile->elapsedCycles);
	_writef(vf, "meta frames %u\n", profile->frames);
	_writef(vf, "meta rom-crc32 %08X\n", profile->romCrc32);
	if (profile->romTitle) {
		_writef(vf, "meta rom-title %s\n", profile->romTitle);
	}
	if (profile->symbolSource) {
		_writef(vf, "meta symbols %s\n", profile->symbolSource);
	}
	if (profile->idleOptimization) {
		_writef(vf, "meta idle-optimization %s\n", profile->idleOptimization);
	}

	unsigned region;
	for (region = 0; region < mPROFILE_REGIONS; ++region) {
		if (profile->regionNames[region]) {
			_writef(vf, "region %u %s\n", region, profile->regionNames[region]);
		}
	}

	size_t i;
	for (i = 0; i < profile->symbolCount; ++i) {
		const struct mProfileFunction* function = mProfileFunctionListGetConstPointer(&profile->functions, i);
		_writef(vf, "func %08X %u %i %" PRIi64 " %s\n", function->start, function->size, function->thumb,
		        (int64_t) function->foldInto, function->name);
	}

	if (!_visitCounters(profile, _saveCounter, vf)) {
		return false;
	}
	if (profile->other.executions) {
		_writef(vf, "other %" PRIu64 " %" PRIu64 "\n", profile->other.cycles, profile->other.executions);
	}

	// Nodes in index order; function IDs count symbols first, then pseudo-functions
	for (i = 0; i < mProfileNodeListSize(&profile->nodes); ++i) {
		const struct mProfileNode* node = mProfileNodeListGetConstPointer(&profile->nodes, i);
		int64_t parent = node->parent == mPROFILE_NONE ? -1 : (int64_t) node->parent;
		_writef(vf, "node %" PRIi64 " %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", parent, node->function,
		        node->calls, node->recursiveCalls, node->selfCycles, node->selfExecutions);
	}
	for (i = 0; i < mProfileEdgeListSize(&profile->recursion); ++i) {
		const struct mProfileEdge* edge = mProfileEdgeListGetConstPointer(&profile->recursion, i);
		_writef(vf, "recursion %u %u %" PRIu64 "\n", edge->caller, edge->callee, edge->recursiveCalls);
	}
	return _writef(vf, "end\n");
}

static const char* _field(const char** cursor) {
	const char* start = *cursor;
	while (*start == ' ') {
		++start;
	}
	const char* end = start;
	while (*end && *end != ' ') {
		++end;
	}
	*cursor = end;
	return start;
}

static const char* _rest(const char* cursor) {
	if (*cursor == ' ') {
		++cursor;
	}
	return cursor;
}

static void _setString(char** destination, const char* value) {
	free(*destination);
	*destination = value ? strdup(value) : NULL;
}

static bool _loadMeta(struct mProfile* profile, const char* cursor) {
	const char* key = _field(&cursor);
	size_t keyLength = cursor - key;
	const char* value = _rest(cursor);
	if (keyLength == 14 && strncmp(key, "elapsed-cycles", keyLength) == 0) {
		profile->elapsedCycles = strtoull(value, NULL, 10);
	} else if (keyLength == 6 && strncmp(key, "frames", keyLength) == 0) {
		profile->frames = strtoul(value, NULL, 10);
	} else if (keyLength == 9 && strncmp(key, "rom-crc32", keyLength) == 0) {
		profile->romCrc32 = strtoul(value, NULL, 16);
	} else if (keyLength == 9 && strncmp(key, "rom-title", keyLength) == 0) {
		_setString(&profile->romTitle, value);
	} else if (keyLength == 7 && strncmp(key, "symbols", keyLength) == 0) {
		_setString(&profile->symbolSource, value);
	} else if (keyLength == 17 && strncmp(key, "idle-optimization", keyLength) == 0) {
		_setString(&profile->idleOptimization, value);
	}
	return true;
}

static bool _loadFunction(struct mProfile* profile, const char* cursor) {
	char* end;
	uint32_t start = strtoul(_field(&cursor), &end, 16);
	uint32_t size = strtoul(_field(&cursor), &end, 10);
	bool thumb = strtol(_field(&cursor), &end, 10) != 0;
	long long foldInto = strtoll(_field(&cursor), &end, 10);
	const char* name = _rest(cursor);
	if (!name[0]) {
		return false;
	}
	if (profile->symbolCount &&
	    start < mProfileFunctionListGetPointer(&profile->functions, profile->symbolCount - 1)->start) {
		// Functions must be sorted for lookups
		return false;
	}
	struct mProfileFunction* function = mProfileAppendSymbol(profile, name);
	function->start = start;
	function->size = size;
	function->thumb = thumb;
	function->foldInto = foldInto;
	return true;
}

static bool _loadCounter(struct mProfile* profile, const char* cursor, bool other) {
	char* end;
	struct mProfileCounter* counter;
	if (other) {
		counter = &profile->other;
	} else {
		uint32_t address = strtoul(_field(&cursor), &end, 16);
		counter = mProfileAllocCounter(profile, address);
	}
	counter->cycles = strtoull(_field(&cursor), &end, 10);
	counter->executions = strtoull(_field(&cursor), &end, 10);
	return true;
}

static bool _loadNode(struct mProfileSavedNodeList* nodes, const char* cursor) {
	struct mProfileSavedNode* node = mProfileSavedNodeListAppend(nodes);
	node->parent = strtoll(_field(&cursor), NULL, 10);
	node->function = strtoll(_field(&cursor), NULL, 10);
	node->calls = strtoull(_field(&cursor), NULL, 10);
	node->recursiveCalls = strtoull(_field(&cursor), NULL, 10);
	node->selfCycles = strtoull(_field(&cursor), NULL, 10);
	node->selfExecutions = strtoull(_field(&cursor), NULL, 10);
	return true;
}

static bool _applyNodes(struct mProfile* profile, const struct mProfileSavedNodeList* saved) {
	int64_t functionCount = mProfileFunctionListSize(&profile->functions);
	size_t i;
	for (i = 0; i < mProfileSavedNodeListSize(saved); ++i) {
		const struct mProfileSavedNode* source = mProfileSavedNodeListGetConstPointer(saved, i);
		struct mProfileNode* node;
		if (i <= mPROFILE_ROOT_IRQ) {
			// The roots already exist
			if (source->parent != -1) {
				return false;
			}
			node = mProfileNodeListGetPointer(&profile->nodes, i);
		} else {
			if (source->parent < 0 || (size_t) source->parent >= i || source->function < 0 ||
			    source->function >= functionCount) {
				return false;
			}
			uint32_t index = mProfileGetChild(profile, source->parent, source->function);
			if (index != i) {
				// Two saved nodes with the same parent and function
				return false;
			}
			node = mProfileNodeListGetPointer(&profile->nodes, index);
		}
		node->calls = source->calls;
		node->recursiveCalls = source->recursiveCalls;
		node->selfCycles = source->selfCycles;
		node->selfExecutions = source->selfExecutions;
	}
	return true;
}

static bool _loadRecursion(struct mProfile* profile, const char* cursor) {
	int64_t functionCount = mProfileFunctionListSize(&profile->functions);
	int64_t caller = strtoll(_field(&cursor), NULL, 10);
	int64_t callee = strtoll(_field(&cursor), NULL, 10);
	uint64_t count = strtoull(_field(&cursor), NULL, 10);
	if (caller < 0 || caller >= functionCount || callee < 0 || callee >= functionCount) {
		return false;
	}
	mProfileAddRecursion(profile, caller, callee, count);
	return true;
}

static bool _isRecord(const char* type, size_t length, const char* name) {
	return length == strlen(name) && strncmp(type, name, length) == 0;
}

bool mProfileLoad(struct mProfile* profile, struct VFile* vf) {
	ssize_t size = vf->size(vf);
	if (size <= 0) {
		return false;
	}
	char* text = malloc(size + 1);
	vf->seek(vf, 0, SEEK_SET);
	if (vf->read(vf, text, size) != size) {
		free(text);
		return false;
	}
	text[size] = '\0';

	mProfileClear(profile);
	mProfileBeginSymbols(profile);
	unsigned region;
	for (region = 0; region < mPROFILE_REGIONS; ++region) {
		_setString(&profile->regionNames[region], NULL);
	}
	_setString(&profile->symbolSource, NULL);
	_setString(&profile->romTitle, NULL);
	_setString(&profile->idleOptimization, NULL);
	profile->romCrc32 = 0;

	struct mProfileSavedNodeList nodes;
	mProfileSavedNodeListInit(&nodes, 0);
	// Recursion records refer to function IDs, which only exist once all functions are loaded
	char** recursion = NULL;
	size_t recursionCount = 0;

	bool success = false;
	bool first = true;
	char* line = text;
	while (line && *line) {
		char* next = strchr(line, '\n');
		if (next) {
			*next = '\0';
			++next;
		}
		size_t length = strlen(line);
		if (length && line[length - 1] == '\r') {
			line[length - 1] = '\0';
		}
		const char* cursor = line;
		const char* type = _field(&cursor);
		size_t typeLength = cursor - type;
		bool ok = true;
		if (first) {
			ok = _isRecord(type, typeLength, MPROF_MAGIC) && strtol(cursor, NULL, 10) >= 1;
			first = false;
		} else if (_isRecord(type, typeLength, "meta")) {
			ok = _loadMeta(profile, cursor);
		} else if (_isRecord(type, typeLength, "region")) {
			unsigned index = strtoul(_field(&cursor), NULL, 10);
			if (index < mPROFILE_REGIONS) {
				_setString(&profile->regionNames[index], _rest(cursor));
			}
		} else if (_isRecord(type, typeLength, "func")) {
			ok = _loadFunction(profile, cursor);
		} else if (_isRecord(type, typeLength, "count")) {
			ok = _loadCounter(profile, cursor, false);
		} else if (_isRecord(type, typeLength, "other")) {
			ok = _loadCounter(profile, cursor, true);
		} else if (_isRecord(type, typeLength, "node")) {
			ok = _loadNode(&nodes, cursor);
		} else if (_isRecord(type, typeLength, "recursion")) {
			recursion = realloc(recursion, sizeof(*recursion) * (recursionCount + 1));
			recursion[recursionCount] = (char*) cursor;
			++recursionCount;
		} else if (_isRecord(type, typeLength, "end")) {
			success = true;
			break;
		}
		if (!ok) {
			break;
		}
		line = next;
	}

	mProfileEndSymbols(profile);
	size_t i;
	for (i = 0; i < profile->symbolCount; ++i) {
		struct mProfileFunction* function = mProfileFunctionListGetPointer(&profile->functions, i);
		if (function->foldInto >= (ssize_t) profile->symbolCount || function->foldInto < -1) {
			function->foldInto = -1;
		}
	}
	if (success) {
		success = _applyNodes(profile, &nodes);
	}
	for (i = 0; success && i < recursionCount; ++i) {
		success = _loadRecursion(profile, recursion[i]);
	}
	free(recursion);
	mProfileSavedNodeListDeinit(&nodes);
	free(text);

	if (!success) {
		mProfileClear(profile);
		mProfileClearSymbols(profile);
		return false;
	}
	mProfileFinish(profile);
	return true;
}

struct mProfileCallgrindState {
	const struct mProfile* profile;
	struct VFile* vf;
	uint32_t lastFunction;
};

static const char* _name(const struct mProfile* profile, uint32_t function) {
	return mProfileFunctionListGetConstPointer(&profile->functions, function)->name;
}

static bool _exportCounter(uint32_t address, const struct mProfileCounter* counter, void* user) {
	struct mProfileCallgrindState* state = user;
	uint32_t function = mProfileFunctionAt(state->profile, address);
	if (function != state->lastFunction) {
		if (!_writef(state->vf, "fn=%s\n", _name(state->profile, function))) {
			return false;
		}
		state->lastFunction = function;
	}
	return _writef(state->vf, "0x%08X %" PRIu64 " %" PRIu64 "\n", address, counter->cycles, counter->executions);
}

bool mProfileExportCallgrind(const struct mProfile* profile, struct VFile* vf) {
	if (!profile->finished) {
		return false;
	}
	const char* object =
	    profile->symbolSource ? profile->symbolSource : (profile->romTitle ? profile->romTitle : "rom");
	_writef(vf, "# callgrind format\n");
	_writef(vf, "version: 1\n");
	_writef(vf, "creator: %s %s profiler\n", projectName, projectVersion);
	if (profile->romTitle) {
		_writef(vf, "cmd: %s\n", profile->romTitle);
	}
	_writef(vf, "desc: Elapsed cycles: %" PRIu64 "\n", profile->elapsedCycles);
	_writef(vf, "desc: Frames: %u\n", profile->frames);
	if (profile->idleOptimization) {
		_writef(vf, "desc: Idle-loop optimization: %s\n", profile->idleOptimization);
	}
	_writef(vf, "positions: instr\n");
	_writef(vf, "events: Cycles Executions\n");
	_writef(vf, "summary: %" PRIu64 " %" PRIu64 "\n\n", profile->totalCycles, profile->totalExecutions);
	_writef(vf, "ob=%s\n", object);
	_writef(vf, "fl=%s\n", object);

	struct mProfileCallgrindState state = {
		.profile = profile,
		.vf = vf,
		.lastFunction = mPROFILE_NONE,
	};
	if (!_visitCounters(profile, _exportCounter, &state)) {
		return false;
	}
	if (profile->other.executions) {
		_writef(vf, "fn=%s\n", _name(profile, mProfilePseudoFunction(profile, mPROFILE_PSEUDO_OTHER)));
		_writef(vf, "0x%08X %" PRIu64 " %" PRIu64 "\n", (uint32_t) mPROFILE_REGIONS << 24, profile->other.cycles,
		        profile->other.executions);
	}

	// Calls, with the inclusive cost of each callee, merged over all call sites
	uint32_t caller = mPROFILE_NONE;
	size_t i;
	for (i = 0; i < mProfileEdgeListSize(&profile->edges); ++i) {
		const struct mProfileEdge* edge = mProfileEdgeListGetConstPointer(&profile->edges, i);
		uint64_t calls = edge->calls + edge->recursiveCalls;
		if (!calls) {
			continue;
		}
		const struct mProfileFunction* from = mProfileFunctionListGetConstPointer(&profile->functions, edge->caller);
		const struct mProfileFunction* to = mProfileFunctionListGetConstPointer(&profile->functions, edge->callee);
		if (edge->caller != caller) {
			_writef(vf, "fn=%s\n", from->name);
			caller = edge->caller;
		}
		_writef(vf, "cfn=%s\n", to->name);
		_writef(vf, "calls=%" PRIu64 " 0x%08X\n", calls, to->start);
		if (!_writef(vf, "0x%08X %" PRIu64 " %" PRIu64 "\n", from->start, edge->inclusiveCycles,
		             edge->inclusiveExecutions)) {
			return false;
		}
	}
	return true;
}
