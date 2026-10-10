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
	*destination = strdup(value);
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
	struct mProfileFunction* function = mProfileFunctionListAppend(&profile->functions);
	memset(function, 0, sizeof(*function));
	function->name = strdup(name);
	const char* dot = strchr(name, '.');
	function->baseLength = dot ? (size_t) (dot - name) : strlen(name);
	function->start = start;
	function->size = size;
	function->thumb = thumb;
	function->foldInto = foldInto;
	++profile->symbolCount;
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
	mProfileClearSymbols(profile);
	unsigned region;
	for (region = 0; region < mPROFILE_REGIONS; ++region) {
		mProfileSetRegionName(profile, region, NULL);
	}

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
			ok = typeLength == strlen(MPROF_MAGIC) && strncmp(type, MPROF_MAGIC, typeLength) == 0 &&
			    strtol(cursor, NULL, 10) >= 1;
			first = false;
		} else if (typeLength == 4 && strncmp(type, "meta", typeLength) == 0) {
			ok = _loadMeta(profile, cursor);
		} else if (typeLength == 6 && strncmp(type, "region", typeLength) == 0) {
			unsigned index = strtoul(_field(&cursor), NULL, 10);
			mProfileSetRegionName(profile, index, _rest(cursor));
		} else if (typeLength == 4 && strncmp(type, "func", typeLength) == 0) {
			ok = _loadFunction(profile, cursor);
		} else if (typeLength == 5 && strncmp(type, "count", typeLength) == 0) {
			ok = _loadCounter(profile, cursor, false);
		} else if (typeLength == 5 && strncmp(type, "other", typeLength) == 0) {
			ok = _loadCounter(profile, cursor, true);
		} else if (typeLength == 3 && strncmp(type, "end", typeLength) == 0) {
			success = true;
			break;
		}
		if (!ok) {
			break;
		}
		line = next;
	}
	free(text);

	size_t i;
	for (i = 0; i < profile->symbolCount; ++i) {
		struct mProfileFunction* function = mProfileFunctionListGetPointer(&profile->functions, i);
		if (function->foldInto >= (ssize_t) profile->symbolCount || function->foldInto < -1) {
			function->foldInto = -1;
		}
	}
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
	ssize_t lastFunction;
};

static ssize_t _pseudoFunction(const struct mProfile* profile, uint32_t address) {
	// Pseudo-functions cover whole regions, so the most specific (latest added) match wins
	size_t i;
	for (i = mProfileFunctionListSize(&profile->functions); i > profile->symbolCount; --i) {
		const struct mProfileFunction* function = mProfileFunctionListGetConstPointer(&profile->functions, i - 1);
		if (address - function->start < function->size) {
			return i - 1;
		}
	}
	return -1;
}

static bool _exportCounter(uint32_t address, const struct mProfileCounter* counter, void* user) {
	struct mProfileCallgrindState* state = user;
	ssize_t index = mProfileFindFunction(state->profile, address);
	if (index >= 0) {
		const struct mProfileFunction* function =
		    mProfileFunctionListGetConstPointer(&state->profile->functions, index);
		if (function->foldInto >= 0) {
			index = function->foldInto;
		}
	} else {
		index = _pseudoFunction(state->profile, address);
	}
	if (index != state->lastFunction) {
		const char* name =
		    index >= 0 ? mProfileFunctionListGetConstPointer(&state->profile->functions, index)->name : "[unknown]";
		if (!_writef(state->vf, "fn=%s\n", name)) {
			return false;
		}
		state->lastFunction = index;
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
		.lastFunction = -2,
	};
	if (!_visitCounters(profile, _exportCounter, &state)) {
		return false;
	}
	if (profile->other.executions) {
		ssize_t index = _pseudoFunction(profile, (uint32_t) mPROFILE_REGIONS << 24);
		const char* name =
		    index >= 0 ? mProfileFunctionListGetConstPointer(&profile->functions, index)->name : "[unknown]";
		_writef(vf, "fn=%s\n", name);
		_writef(vf, "0x%08X %" PRIu64 " %" PRIu64 "\n", (uint32_t) mPROFILE_REGIONS << 24, profile->other.cycles,
		        profile->other.executions);
	}
	return true;
}
