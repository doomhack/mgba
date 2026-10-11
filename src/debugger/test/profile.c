/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/internal/debugger/profile.h>
#include <mgba/internal/debugger/profiler.h>
#include <mgba-util/vfs.h>

#ifdef M_CORE_GBA
#include "debugger/test/profile-rom/profile-rom.h"
#endif

#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STB_LOCAL 0
#define STB_GLOBAL 1

#define SECTION_TEXT 1
#define SECTION_DATA 2

struct TestSymbol {
	const char* name;
	uint32_t value;
	uint32_t size;
	int type;
	int binding;
	uint16_t section;
};

struct TestELF {
	uint8_t data[4096];
	size_t size;
};

static void _put16(uint8_t* out, uint16_t value) {
	out[0] = value;
	out[1] = value >> 8;
}

static void _put32(uint8_t* out, uint32_t value) {
	out[0] = value;
	out[1] = value >> 8;
	out[2] = value >> 16;
	out[3] = value >> 24;
}

static void _section(uint8_t* out, uint32_t type, uint32_t flags, uint32_t addr, uint32_t offset, uint32_t size,
                     uint32_t link, uint32_t entsize) {
	memset(out, 0, 40);
	_put32(&out[4], type);
	_put32(&out[8], flags);
	_put32(&out[12], addr);
	_put32(&out[16], offset);
	_put32(&out[20], size);
	_put32(&out[24], link);
	_put32(&out[36], entsize);
}

// Builds a minimal ELF32 little-endian image with .text, .data, .symtab and .strtab sections
static void _buildELF(struct TestELF* elf, const struct TestSymbol* symbols, size_t count) {
	memset(elf, 0, sizeof(*elf));
	uint8_t* data = elf->data;
	memcpy(data,
	       "\x7F"
	       "ELF",
	       4);
	data[4] = 1; // ELFCLASS32
	data[5] = 1; // ELFDATA2LSB
	data[6] = 1;
	_put16(&data[0x10], 2); // ET_EXEC
	_put16(&data[0x12], 40); // EM_ARM

	size_t strtabOffset = 0x40;
	size_t strtabSize = 1;
	uint32_t nameOffsets[32];
	size_t i;
	for (i = 0; i < count; ++i) {
		nameOffsets[i] = strtabSize;
		size_t length = strlen(symbols[i].name) + 1;
		memcpy(&data[strtabOffset + strtabSize], symbols[i].name, length);
		strtabSize += length;
	}

	size_t symtabOffset = (strtabOffset + strtabSize + 3) & ~3;
	size_t symtabSize = (count + 1) * 16;
	for (i = 0; i < count; ++i) {
		uint8_t* entry = &data[symtabOffset + (i + 1) * 16];
		_put32(&entry[0], nameOffsets[i]);
		_put32(&entry[4], symbols[i].value);
		_put32(&entry[8], symbols[i].size);
		entry[12] = (symbols[i].binding << 4) | symbols[i].type;
		_put16(&entry[14], symbols[i].section);
	}

	size_t shoff = (symtabOffset + symtabSize + 3) & ~3;
	_section(&data[shoff + 40 * 0], 0, 0, 0, 0, 0, 0, 0);
	_section(&data[shoff + 40 * 1], 1, 0x6, 0x08000000, 0, 0x100, 0, 0);
	_section(&data[shoff + 40 * 2], 1, 0x3, 0x02000000, 0, 0x10, 0, 0);
	_section(&data[shoff + 40 * 3], 2, 0, 0, symtabOffset, symtabSize, 4, 16);
	_section(&data[shoff + 40 * 4], 3, 0, 0, strtabOffset, strtabSize, 0, 0);
	_put32(&data[0x20], shoff);
	_put16(&data[0x2E], 40);
	_put16(&data[0x30], 5);
	elf->size = shoff + 40 * 5;
}

static const struct TestSymbol _testSymbols[] = {
	{ "alias_main", 0x08000001, 0x20, STT_FUNC, STB_GLOBAL, SECTION_TEXT },
	{ "main", 0x08000001, 0x20, STT_FUNC, STB_GLOBAL, SECTION_TEXT },
	{ "inner_label", 0x08000004, 0, STT_NOTYPE, STB_GLOBAL, SECTION_TEXT },
	{ "helper.constprop.0", 0x08000020, 0x10, STT_FUNC, STB_LOCAL, SECTION_TEXT },
	{ "asm_label", 0x08000040, 0, STT_NOTYPE, STB_GLOBAL, SECTION_TEXT },
	{ "__target_veneer", 0x08000060, 0x8, STT_FUNC, STB_LOCAL, SECTION_TEXT },
	{ "target", 0x08000070, 0x10, STT_FUNC, STB_GLOBAL, SECTION_TEXT },
	{ "tail_label", 0x080000F0, 0, STT_NOTYPE, STB_GLOBAL, SECTION_TEXT },
	{ "$t", 0x08000000, 0, STT_NOTYPE, STB_LOCAL, SECTION_TEXT },
	{ "data_object", 0x02000000, 0x10, STT_OBJECT, STB_GLOBAL, SECTION_DATA },
	{ "undefined", 0, 0, STT_FUNC, STB_GLOBAL, 0 },
};

static void _loadTestSymbols(struct mProfile* profile) {
	struct TestELF elf;
	_buildELF(&elf, _testSymbols, sizeof(_testSymbols) / sizeof(*_testSymbols));
	struct VFile* vf = VFileFromConstMemory(elf.data, elf.size);
	assert_true(mProfileLoadSymbols(profile, vf));
	vf->close(vf);
}

static const struct mProfileFunction* _function(const struct mProfile* profile, const char* name) {
	ssize_t index = mProfileFindFunctionByName(profile, name);
	assert_true(index >= 0);
	return mProfileFunctionListGetConstPointer(&profile->functions, index);
}

M_TEST_DEFINE(countersAndRegions) {
	struct mProfile profile;
	mProfileInit(&profile);
	mProfileSetRegionName(&profile, 0, "[BIOS]");
	mProfileSetRegionName(&profile, 8, "[unknown ROM]");

	mProfileCount(&profile, 0x08000000, 3);
	mProfileCount(&profile, 0x08000002, 2);
	mProfileCount(&profile, 0x08000002, 4);
	mProfileCount(&profile, 0x03000000, 5);
	mProfileCount(&profile, 0x00000010, 1);
	mProfileCount(&profile, 0x12345678, 7);

	const struct mProfileCounter* counter = mProfileGetCounter(&profile, 0x08000002);
	assert_non_null(counter);
	assert_int_equal(counter->cycles, 6);
	assert_int_equal(counter->executions, 2);
	assert_null(mProfileGetCounter(&profile, 0x08001000));

	mProfileFinish(&profile);
	assert_true(profile.finished);
	assert_int_equal(profile.totalCycles, 22);
	assert_int_equal(profile.totalExecutions, 6);
	assert_int_equal(profile.symbolCount, 0);
	assert_int_equal(_function(&profile, "[unknown ROM]")->selfCycles, 9);
	assert_int_equal(_function(&profile, "[BIOS]")->selfCycles, 1);
	assert_int_equal(_function(&profile, "[unknown 0x03000000]")->selfCycles, 5);
	assert_int_equal(_function(&profile, "[unknown]")->selfCycles, 7);

	// Finishing again must not duplicate pseudo-functions
	size_t functions = mProfileFunctionListSize(&profile.functions);
	mProfileFinish(&profile);
	assert_int_equal(mProfileFunctionListSize(&profile.functions), functions);
	assert_int_equal(profile.totalCycles, 22);

	mProfileClear(&profile);
	assert_int_equal(profile.totalCycles, 0);
	assert_null(mProfileGetCounter(&profile, 0x08000002));
	mProfileDeinit(&profile);
}

M_TEST_DEFINE(elfSymbols) {
	struct mProfile profile;
	mProfileInit(&profile);
	_loadTestSymbols(&profile);

	assert_int_equal(profile.symbolCount, 6);
	const char* expected[] = { "main", "helper.constprop.0", "asm_label", "__target_veneer", "target", "tail_label" };
	size_t i;
	for (i = 0; i < profile.symbolCount; ++i) {
		assert_string_equal(mProfileFunctionListGetPointer(&profile.functions, i)->name, expected[i]);
	}

	const struct mProfileFunction* function = _function(&profile, "main");
	assert_int_equal(function->start, 0x08000000);
	assert_int_equal(function->size, 0x20);
	assert_true(function->thumb);

	function = _function(&profile, "helper.constprop.0");
	assert_int_equal(function->baseLength, 6);
	assert_false(function->thumb);

	function = _function(&profile, "asm_label");
	assert_int_equal(function->start, 0x08000040);
	assert_int_equal(function->size, 0x20);

	function = _function(&profile, "tail_label");
	assert_int_equal(function->size, 0x10);

	function = _function(&profile, "__target_veneer");
	assert_int_equal(function->foldInto, mProfileFindFunctionByName(&profile, "target"));

	assert_int_equal(mProfileFindFunction(&profile, 0x08000006), mProfileFindFunctionByName(&profile, "main"));
	assert_int_equal(mProfileFindFunction(&profile, 0x0800002E),
	                 mProfileFindFunctionByName(&profile, "helper.constprop.0"));
	assert_int_equal(mProfileFindFunction(&profile, 0x08000030), -1);
	assert_int_equal(mProfileFindFunction(&profile, 0x07FFFFFE), -1);
	assert_int_equal(mProfileFindFunction(&profile, 0x08000100), -1);

	mProfileDeinit(&profile);
}

M_TEST_DEFINE(elfRejectsGarbage) {
	struct mProfile profile;
	mProfileInit(&profile);
	static const uint8_t garbage[64] = { 'N', 'O', 'T', 'E', 'L', 'F' };
	struct VFile* vf = VFileFromConstMemory(garbage, sizeof(garbage));
	assert_false(mProfileLoadSymbols(&profile, vf));
	vf->close(vf);

	struct TestELF elf;
	_buildELF(&elf, _testSymbols, 1);
	elf.data[4] = 2; // ELFCLASS64
	vf = VFileFromConstMemory(elf.data, elf.size);
	assert_false(mProfileLoadSymbols(&profile, vf));
	vf->close(vf);

	// A truncated file must fail cleanly rather than read out of bounds
	_buildELF(&elf, _testSymbols, sizeof(_testSymbols) / sizeof(*_testSymbols));
	vf = VFileFromConstMemory(elf.data, elf.size - 60);
	assert_false(mProfileLoadSymbols(&profile, vf));
	vf->close(vf);
	mProfileDeinit(&profile);
}

static void _countTestProgram(struct mProfile* profile) {
	mProfileCount(profile, 0x08000000, 10); // main
	mProfileCount(profile, 0x08000006, 5); // main
	mProfileCount(profile, 0x08000020, 7); // helper.constprop.0
	mProfileCount(profile, 0x08000062, 4); // __target_veneer, folded into target
	mProfileCount(profile, 0x08000070, 20); // target
	mProfileCount(profile, 0x08000030, 3); // a gap between symbols
	mProfileCount(profile, 0x03000100, 2); // IWRAM, no symbols
	profile->elapsedCycles = 51;
}

M_TEST_DEFINE(attribution) {
	struct mProfile profile;
	mProfileInit(&profile);
	mProfileSetRegionName(&profile, 8, "[unknown ROM]");
	_loadTestSymbols(&profile);
	_countTestProgram(&profile);
	mProfileFinish(&profile);

	assert_int_equal(profile.totalCycles, 51);
	assert_int_equal(profile.totalCycles, profile.elapsedCycles);
	assert_int_equal(_function(&profile, "main")->selfCycles, 15);
	assert_int_equal(_function(&profile, "main")->executions, 2);
	assert_int_equal(_function(&profile, "helper.constprop.0")->selfCycles, 7);
	assert_int_equal(_function(&profile, "__target_veneer")->selfCycles, 0);
	assert_int_equal(_function(&profile, "target")->selfCycles, 24);
	assert_int_equal(_function(&profile, "[unknown ROM]")->selfCycles, 3);
	assert_int_equal(_function(&profile, "[unknown 0x03000000]")->selfCycles, 2);

	size_t top[3];
	assert_int_equal(mProfileTopFunctions(&profile, top, 3), 3);
	assert_string_equal(mProfileFunctionListGetPointer(&profile.functions, top[0])->name, "target");
	assert_string_equal(mProfileFunctionListGetPointer(&profile.functions, top[1])->name, "main");
	assert_string_equal(mProfileFunctionListGetPointer(&profile.functions, top[2])->name, "helper.constprop.0");
	mProfileDeinit(&profile);
}

M_TEST_DEFINE(saveLoadRoundTrip) {
	struct mProfile profile;
	mProfileInit(&profile);
	mProfileSetRegionName(&profile, 8, "[unknown ROM]");
	_loadTestSymbols(&profile);
	mProfileSetSymbolSource(&profile, "C:/projects/my game/game.elf");
	profile.romTitle = strdup("TEST TITLE");
	profile.romCrc32 = 0xDEADBEEF;
	profile.idleOptimization = strdup("ignore");
	profile.frames = 12;
	_countTestProgram(&profile);
	mProfileCount(&profile, 0x20000000, 9);
	profile.elapsedCycles += 9;
	mProfileFinish(&profile);

	struct VFile* vf = VFileMemChunk(NULL, 0);
	assert_true(mProfileSave(&profile, vf));

	struct mProfile loaded;
	mProfileInit(&loaded);
	assert_true(mProfileLoad(&loaded, vf));
	vf->close(vf);

	assert_true(loaded.finished);
	assert_int_equal(loaded.symbolCount, profile.symbolCount);
	size_t i;
	for (i = 0; i < profile.symbolCount; ++i) {
		const struct mProfileFunction* a = mProfileFunctionListGetPointer(&profile.functions, i);
		const struct mProfileFunction* b = mProfileFunctionListGetPointer(&loaded.functions, i);
		assert_string_equal(a->name, b->name);
		assert_int_equal(a->baseLength, b->baseLength);
		assert_int_equal(a->start, b->start);
		assert_int_equal(a->size, b->size);
		assert_int_equal(a->thumb, b->thumb);
		assert_int_equal(a->foldInto, b->foldInto);
		assert_int_equal(a->selfCycles, b->selfCycles);
		assert_int_equal(a->executions, b->executions);
	}
	assert_int_equal(mProfileFunctionListSize(&loaded.functions), mProfileFunctionListSize(&profile.functions));
	assert_int_equal(loaded.totalCycles, profile.totalCycles);
	assert_int_equal(loaded.totalExecutions, profile.totalExecutions);
	assert_int_equal(loaded.elapsedCycles, profile.elapsedCycles);
	assert_int_equal(loaded.frames, 12);
	assert_int_equal(loaded.romCrc32, 0xDEADBEEF);
	assert_string_equal(loaded.romTitle, "TEST TITLE");
	assert_string_equal(loaded.symbolSource, "C:/projects/my game/game.elf");
	assert_string_equal(loaded.idleOptimization, "ignore");
	assert_string_equal(loaded.regionNames[8], "[unknown ROM]");
	assert_null(loaded.regionNames[3]);
	assert_int_equal(loaded.other.cycles, 9);
	const struct mProfileCounter* counter = mProfileGetCounter(&loaded, 0x08000070);
	assert_non_null(counter);
	assert_int_equal(counter->cycles, 20);
	assert_int_equal(counter->executions, 1);

	mProfileDeinit(&loaded);
	mProfileDeinit(&profile);
}

static bool _loadText(struct mProfile* profile, const char* text) {
	struct VFile* vf = VFileFromConstMemory(text, strlen(text));
	bool success = mProfileLoad(profile, vf);
	vf->close(vf);
	return success;
}

M_TEST_DEFINE(loadValidation) {
	struct mProfile profile;
	mProfileInit(&profile);
	assert_false(_loadText(&profile, "not a profile\nend\n"));
	assert_false(_loadText(&profile, "mprof 1\ncount 08000000 4 1\n"));
	assert_false(_loadText(&profile, "mprof 1\nfunc 08000010 4 0 -1 b\nfunc 08000000 4 0 -1 a\nend\n"));
	assert_int_equal(profile.symbolCount, 0);
	assert_null(mProfileGetCounter(&profile, 0x08000000));

	// Unknown records are skipped, CRLF line endings are accepted, and bad fold indices are dropped
	assert_true(_loadText(&profile,
	                      "mprof 2\r\nfuture-record 1 2 3\r\nfunc 08000000 4 1 7 a\r\ncount 08000000 4 1\r\nend\r\n"));
	assert_int_equal(profile.symbolCount, 1);
	assert_int_equal(mProfileFunctionListGetPointer(&profile.functions, 0)->foldInto, -1);
	assert_int_equal(_function(&profile, "a")->selfCycles, 4);
	mProfileDeinit(&profile);
}

M_TEST_DEFINE(callgrindExport) {
	struct mProfile profile;
	mProfileInit(&profile);
	_loadTestSymbols(&profile);
	_countTestProgram(&profile);

	struct VFile* vf = VFileMemChunk(NULL, 0);
	assert_false(mProfileExportCallgrind(&profile, vf));
	mProfileFinish(&profile);
	assert_true(mProfileExportCallgrind(&profile, vf));

	size_t size = vf->size(vf);
	char* text = calloc(1, size + 1);
	vf->seek(vf, 0, SEEK_SET);
	assert_int_equal(vf->read(vf, text, size), size);
	vf->close(vf);

	assert_non_null(strstr(text, "events: Cycles Executions\n"));
	assert_non_null(strstr(text, "summary: 51 7\n"));
	assert_non_null(strstr(text, "fn=main\n0x08000000 10 1\n0x08000006 5 1\n"));
	assert_non_null(strstr(text, "fn=target\n0x08000062 4 1\n0x08000070 20 1\n"));
	assert_null(strstr(text, "fn=__target_veneer"));
	free(text);
	mProfileDeinit(&profile);
}

// Call tree reconstruction, driven by synthetic event streams like the ones the profiler emits

#define STATUS_THUMB_SYSTEM 0x3F
#define STATUS_ARM_SYSTEM 0x1F
#define STATUS_ARM_IRQ 0x92
#define STATUS_ARM_SUPERVISOR 0x93

#define SP_MAIN 0x03007F00
#define SP_IRQ 0x03007FA0
#define SP_SUPERVISOR 0x03007FE0

struct RecorderTest {
	struct mProfile profile;
	struct mProfileRecorder recorder;
	uint64_t charged;
};

static void _setupProgram(struct RecorderTest* test) {
	static const struct {
		const char* name;
		uint32_t start;
		uint32_t size;
	} symbols[] = {
		{ "handler", 0x03000000, 0x80 },     { "main", 0x08000100, 0x100 },   { "a", 0x08000200, 0x40 },
		{ "b", 0x08000240, 0x40 },           { "rec", 0x08000280, 0x40 },     { "m1", 0x080002C0, 0x40 },
		{ "m2", 0x08000300, 0x40 },          { "tailsrc", 0x08000340, 0x40 }, { "taildst", 0x08000380, 0x40 },
		{ "_call_via_r3", 0x08000400, 0x8 },
	};
	memset(test, 0, sizeof(*test));
	mProfileInit(&test->profile);
	mProfileBeginSymbols(&test->profile);
	size_t i;
	for (i = 0; i < sizeof(symbols) / sizeof(*symbols); ++i) {
		struct mProfileFunction* function = mProfileAppendSymbol(&test->profile, symbols[i].name);
		function->start = symbols[i].start;
		function->size = symbols[i].size;
	}
	mProfileEndSymbols(&test->profile);
	mProfileSetRegionName(&test->profile, 0, "[BIOS]");
	mProfileRecorderInit(&test->recorder, &test->profile);
}

static void _teardownProgram(struct RecorderTest* test) {
	mProfileRecorderDeinit(&test->recorder);
	mProfileDeinit(&test->profile);
}

static void _event(struct RecorderTest* test, uint8_t type, uint8_t width, uint32_t cycles, uint32_t from, uint32_t to,
                   uint32_t sp, uint32_t lr, uint32_t status) {
	struct mProfileEvent event;
	memset(&event, 0, sizeof(event));
	event.type = type;
	event.width = width;
	event.cycles = cycles;
	event.instructions = cycles ? 1 : 0;
	event.from = from;
	event.to = to;
	event.sp = sp;
	event.lr = lr;
	event.status = status;
	mProfileRecorderConsume(&test->recorder, &event);
	test->charged += cycles;
}

static void _start(struct RecorderTest* test, uint32_t pc) {
	_event(test, mPROFILE_EVENT_START, 0, 0, 0, pc, SP_MAIN, 0, STATUS_THUMB_SYSTEM);
}

// A Thumb BL: LR points just past the instruction that jumped
static void _call(struct RecorderTest* test, uint32_t cycles, uint32_t from, uint32_t to, uint32_t sp) {
	_event(test, mPROFILE_EVENT_BRANCH, 2, cycles, from, to, sp, (from + 2) | 1, STATUS_THUMB_SYSTEM);
}

// Any other jump: a return, a tail call, or a branch
static void _jump(struct RecorderTest* test, uint32_t cycles, uint32_t from, uint32_t to, uint32_t sp) {
	_event(test, mPROFILE_EVENT_BRANCH, 2, cycles, from, to, sp, 0, STATUS_THUMB_SYSTEM);
}

static void _charge(struct RecorderTest* test, uint32_t cycles) {
	_event(test, mPROFILE_EVENT_CYCLES, 0, cycles, 0, 0, 0, 0, 0);
}

static uint32_t _functionId(const struct mProfile* profile, const char* name) {
	ssize_t index = mProfileFindFunctionByName(profile, name);
	assert_true(index >= 0);
	return index;
}

static const struct mProfileNode* _child(const struct mProfile* profile, uint32_t parent, const char* name) {
	uint32_t function = _functionId(profile, name);
	size_t i;
	for (i = 0; i < mProfileNodeListSize(&profile->nodes); ++i) {
		const struct mProfileNode* node = mProfileNodeListGetConstPointer(&profile->nodes, i);
		if (node->parent == parent && node->function == function) {
			return node;
		}
	}
	fail_msg("No node for %s under node %u", name, parent);
	return NULL;
}

static uint32_t _index(const struct mProfile* profile, const struct mProfileNode* node) {
	return mProfileNodeListIndex(&profile->nodes, node);
}

static const struct mProfileEdge* _edgeBetween(const struct mProfile* profile, const char* caller, const char* callee) {
	uint32_t from = _functionId(profile, caller);
	uint32_t to = _functionId(profile, callee);
	size_t i;
	for (i = 0; i < mProfileEdgeListSize(&profile->edges); ++i) {
		const struct mProfileEdge* edge = mProfileEdgeListGetConstPointer(&profile->edges, i);
		if (edge->caller == from && edge->callee == to) {
			return edge;
		}
	}
	fail_msg("No edge from %s to %s", caller, callee);
	return NULL;
}

static void _assertAllCharged(struct RecorderTest* test) {
	uint64_t total = 0;
	size_t i;
	for (i = 0; i < mProfileNodeListSize(&test->profile.nodes); ++i) {
		total += mProfileNodeListGetConstPointer(&test->profile.nodes, i)->selfCycles;
	}
	assert_int_equal(total, test->charged);
}

// main calls a twice; the first time, a calls b
static void _runCalls(struct RecorderTest* test) {
	_start(test, 0x08000100);
	_call(test, 10, 0x08000110, 0x08000200, SP_MAIN);
	_call(test, 5, 0x08000210, 0x08000240, SP_MAIN - 0x18);
	_jump(test, 7, 0x08000250, 0x08000212, SP_MAIN - 0x18);
	_jump(test, 3, 0x08000220, 0x08000112, SP_MAIN);
	_call(test, 4, 0x08000120, 0x08000200, SP_MAIN);
	_jump(test, 6, 0x08000222, 0x08000122, SP_MAIN);
	_charge(test, 2);
}

M_TEST_DEFINE(recorderCalls) {
	struct RecorderTest test;
	_setupProgram(&test);
	_runCalls(&test);
	mProfileFinish(&test.profile);
	_assertAllCharged(&test);

	const struct mProfileNode* main = _child(&test.profile, mPROFILE_ROOT_MAIN, "main");
	const struct mProfileNode* a = _child(&test.profile, _index(&test.profile, main), "a");
	const struct mProfileNode* b = _child(&test.profile, _index(&test.profile, a), "b");
	assert_int_equal(main->calls, 1);
	assert_int_equal(main->selfCycles, 16);
	assert_int_equal(main->subtreeCycles, 37);
	assert_int_equal(a->calls, 2);
	assert_int_equal(a->selfCycles, 14);
	assert_int_equal(a->subtreeCycles, 21);
	assert_int_equal(b->calls, 1);
	assert_int_equal(b->selfCycles, 7);
	assert_int_equal(mProfileNodeListSize(&test.profile.nodes), 5);

	const struct mProfileFunction* function = _function(&test.profile, "a");
	assert_int_equal(function->calls, 2);
	assert_int_equal(function->inclusiveCycles, 21);
	assert_int_equal(_edgeBetween(&test.profile, "main", "a")->calls, 2);
	assert_int_equal(_edgeBetween(&test.profile, "main", "a")->inclusiveCycles, 21);
	assert_int_equal(_edgeBetween(&test.profile, "a", "b")->calls, 1);
	assert_int_equal(_edgeBetween(&test.profile, "[main]", "main")->calls, 1);
	_teardownProgram(&test);
}

M_TEST_DEFINE(recorderRecursion) {
	struct RecorderTest test;
	_setupProgram(&test);
	_start(&test, 0x08000100);

	// rec calls itself 300 levels deep, then returns all the way out
	_call(&test, 1, 0x08000110, 0x08000280, SP_MAIN);
	unsigned level;
	for (level = 1; level <= 300; ++level) {
		_call(&test, 2, 0x08000290, 0x08000280, SP_MAIN - level * 8);
	}
	for (level = 300; level >= 1; --level) {
		_jump(&test, 3, 0x080002A0, 0x08000292, SP_MAIN - level * 8);
	}
	_jump(&test, 3, 0x080002A0, 0x08000112, SP_MAIN);

	// m1 -> m2 -> m1 -> m2 -> m1, then back out
	_call(&test, 1, 0x08000120, 0x080002C0, SP_MAIN);
	_call(&test, 1, 0x080002D0, 0x08000300, SP_MAIN - 8);
	_call(&test, 1, 0x08000310, 0x080002C0, SP_MAIN - 16);
	_call(&test, 1, 0x080002D0, 0x08000300, SP_MAIN - 24);
	_call(&test, 1, 0x08000310, 0x080002C0, SP_MAIN - 32);
	_jump(&test, 1, 0x080002E0, 0x08000312, SP_MAIN - 32);
	_jump(&test, 1, 0x08000320, 0x080002D2, SP_MAIN - 24);
	_jump(&test, 1, 0x080002E0, 0x08000312, SP_MAIN - 16);
	_jump(&test, 1, 0x08000320, 0x080002D2, SP_MAIN - 8);
	_jump(&test, 1, 0x080002E0, 0x08000122, SP_MAIN);

	// With everything returned, m2 called directly from main gets a node of its own
	_call(&test, 4, 0x08000130, 0x08000300, SP_MAIN);
	_jump(&test, 2, 0x08000320, 0x08000132, SP_MAIN);
	_charge(&test, 1);
	mProfileFinish(&test.profile);
	_assertAllCharged(&test);

	uint32_t main = _index(&test.profile, _child(&test.profile, mPROFILE_ROOT_MAIN, "main"));
	const struct mProfileNode* rec = _child(&test.profile, main, "rec");
	assert_int_equal(rec->calls, 1);
	assert_int_equal(rec->recursiveCalls, 300);
	assert_int_equal(rec->selfCycles, 300 * 2 + 300 * 3 + 3);
	assert_int_equal(_function(&test.profile, "rec")->inclusiveCycles, rec->subtreeCycles);
	assert_int_equal(_edgeBetween(&test.profile, "rec", "rec")->recursiveCalls, 300);

	const struct mProfileNode* m1 = _child(&test.profile, main, "m1");
	const struct mProfileNode* m2 = _child(&test.profile, _index(&test.profile, m1), "m2");
	assert_int_equal(m1->calls, 1);
	assert_int_equal(m1->recursiveCalls, 2);
	assert_int_equal(m2->calls, 1);
	assert_int_equal(m2->recursiveCalls, 1);
	assert_int_equal(_edgeBetween(&test.profile, "m2", "m1")->recursiveCalls, 2);
	assert_int_equal(_edgeBetween(&test.profile, "m1", "m2")->recursiveCalls, 1);

	const struct mProfileNode* direct = _child(&test.profile, main, "m2");
	assert_int_equal(direct->calls, 1);
	assert_int_equal(direct->selfCycles, 2);
	// Roots, main, rec, m1, m1/m2 and main/m2
	assert_int_equal(mProfileNodeListSize(&test.profile.nodes), 7);
	_teardownProgram(&test);
}

M_TEST_DEFINE(recorderTailCalls) {
	struct RecorderTest test;
	_setupProgram(&test);
	_start(&test, 0x08000100);
	_call(&test, 1, 0x08000110, 0x08000340, SP_MAIN);
	// tailsrc tail-calls taildst, which jumps back into tailsrc, which tail-calls taildst again
	_jump(&test, 4, 0x08000350, 0x08000380, SP_MAIN);
	_jump(&test, 5, 0x08000390, 0x08000358, SP_MAIN);
	_jump(&test, 2, 0x0800035A, 0x08000380, SP_MAIN);
	// taildst returns straight to main
	_jump(&test, 6, 0x08000398, 0x08000112, SP_MAIN);
	_charge(&test, 1);
	mProfileFinish(&test.profile);
	_assertAllCharged(&test);

	uint32_t main = _index(&test.profile, _child(&test.profile, mPROFILE_ROOT_MAIN, "main"));
	const struct mProfileNode* source = _child(&test.profile, main, "tailsrc");
	const struct mProfileNode* target = _child(&test.profile, _index(&test.profile, source), "taildst");
	assert_int_equal(source->calls, 1);
	assert_int_equal(source->selfCycles, 6);
	assert_int_equal(target->calls, 2);
	assert_int_equal(target->selfCycles, 11);
	assert_int_equal(_child(&test.profile, mPROFILE_ROOT_MAIN, "main")->selfCycles, 2);
	_teardownProgram(&test);
}

M_TEST_DEFINE(recorderInterrupts) {
	struct RecorderTest test;
	_setupProgram(&test);
	_start(&test, 0x08000100);
	_call(&test, 1, 0x08000110, 0x08000200, SP_MAIN);

	// An interrupt while a is running: the BIOS calls the handler, which returns, then the
	// BIOS returns to the interrupted code
	_event(&test, mPROFILE_EVENT_BRANCH, 2, 3, 0x08000210, 0x18, SP_IRQ, 0x08000212 + 4, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 4, 0x00000140, 0x03000000, SP_IRQ, 0x00000144, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 5, 0x03000010, 0x03000020, SP_MAIN - 0x40, 0, STATUS_ARM_SYSTEM);

	// A nested interrupt while the handler runs in System mode with interrupts enabled
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 2, 0x03000020, 0x18, SP_IRQ - 0x20, 0x03000024 + 4, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 4, 0x00000140, 0x03000000, SP_IRQ - 0x20, 0x00000144, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 6, 0x03000040, 0x00000144, SP_IRQ - 0x20, 0, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 3, 0x00000150, 0x03000024, SP_MAIN - 0x40, 0, STATUS_ARM_SYSTEM);

	// The outer handler finishes, then the BIOS returns to a
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 7, 0x03000040, 0x00000144, SP_IRQ, 0, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 3, 0x00000150, 0x08000212, SP_MAIN - 0x18, 0, STATUS_THUMB_SYSTEM);
	_jump(&test, 2, 0x08000220, 0x08000112, SP_MAIN);
	_charge(&test, 1);
	mProfileFinish(&test.profile);
	_assertAllCharged(&test);

	uint32_t main = _index(&test.profile, _child(&test.profile, mPROFILE_ROOT_MAIN, "main"));
	const struct mProfileNode* a = _child(&test.profile, main, "a");
	assert_int_equal(a->selfCycles, 3 + 2);
	assert_int_equal(a->subtreeCycles, a->selfCycles);

	const struct mProfileNode* irq = mProfileNodeListGetConstPointer(&test.profile.nodes, mPROFILE_ROOT_IRQ);
	assert_int_equal(irq->calls, 2);
	const struct mProfileNode* bios = _child(&test.profile, mPROFILE_ROOT_IRQ, "[BIOS]");
	const struct mProfileNode* handler = _child(&test.profile, _index(&test.profile, bios), "handler");
	assert_int_equal(bios->calls, 2);
	assert_int_equal(bios->selfCycles, 4 + 3 + 4 + 3);
	assert_int_equal(handler->calls, 2);
	assert_int_equal(handler->selfCycles, 5 + 2 + 6 + 7);
	assert_int_equal(irq->subtreeCycles, 4 + 3 + 4 + 3 + 5 + 2 + 6 + 7);
	_teardownProgram(&test);
}

M_TEST_DEFINE(recorderUnwinding) {
	struct RecorderTest test;
	_setupProgram(&test);
	_start(&test, 0x08000100);
	_call(&test, 1, 0x08000110, 0x08000200, SP_MAIN);
	_call(&test, 1, 0x08000210, 0x08000240, SP_MAIN - 0x18);
	// b longjmps back into main: the stack pointer goes back above both frames
	_jump(&test, 2, 0x08000250, 0x08000150, SP_MAIN + 8);
	// A new call from main must be a child of main, not of b
	_call(&test, 3, 0x08000160, 0x08000200, SP_MAIN + 8);
	_jump(&test, 4, 0x08000220, 0x08000162, SP_MAIN + 8);
	_charge(&test, 1);
	mProfileFinish(&test.profile);
	_assertAllCharged(&test);

	uint32_t main = _index(&test.profile, _child(&test.profile, mPROFILE_ROOT_MAIN, "main"));
	const struct mProfileNode* a = _child(&test.profile, main, "a");
	assert_int_equal(a->calls, 2);
	assert_int_equal(_child(&test.profile, mPROFILE_ROOT_MAIN, "main")->selfCycles, 1 + 3 + 1);
	_teardownProgram(&test);
}

M_TEST_DEFINE(recorderTrampolineAndFallThrough) {
	struct RecorderTest test;
	_setupProgram(&test);
	_start(&test, 0x08000100);
	// main calls b through _call_via_r3, whose bx r3 leaves LR pointing back into main
	_call(&test, 2, 0x08000140, 0x08000400, SP_MAIN);
	_jump(&test, 1, 0x08000400, 0x08000240, SP_MAIN);
	_jump(&test, 5, 0x08000250, 0x08000142, SP_MAIN);

	// Code in a that runs on into b without a branch: b's cycles must be b's
	_jump(&test, 1, 0x08000150, 0x0800023C, SP_MAIN);
	_jump(&test, 4, 0x08000244, 0x08000154, SP_MAIN);

	// A function-pointer call through GCC's local "bx r3" stub near the end of main: a BL into
	// the middle of main, then the stub jumps to a, which returns straight to main
	_call(&test, 1, 0x08000160, 0x080001F8, SP_MAIN);
	_jump(&test, 1, 0x080001F8, 0x08000200, SP_MAIN);
	_jump(&test, 6, 0x08000220, 0x08000162, SP_MAIN);
	_charge(&test, 1);
	mProfileFinish(&test.profile);
	_assertAllCharged(&test);

	const struct mProfileNode* mainNode = _child(&test.profile, mPROFILE_ROOT_MAIN, "main");
	uint32_t main = _index(&test.profile, mainNode);
	const struct mProfileNode* b = _child(&test.profile, main, "b");
	assert_int_equal(b->calls, 1);
	assert_int_equal(b->selfCycles, 5);
	assert_int_equal(_edgeBetween(&test.profile, "main", "b")->calls, 1);
	const struct mProfileNode* a = _child(&test.profile, main, "a");
	assert_int_equal(a->calls, 1);
	assert_int_equal(a->selfCycles, 6);
	assert_int_equal(mainNode->recursiveCalls, 0);
	// Stubs don't appear in the tree
	ssize_t trampoline = mProfileFindFunctionByName(&test.profile, "_call_via_r3");
	size_t i;
	for (i = 0; i < mProfileNodeListSize(&test.profile.nodes); ++i) {
		assert_int_not_equal(mProfileNodeListGetConstPointer(&test.profile.nodes, i)->function, trampoline);
	}
	const struct mProfileNode* fallThrough = _child(&test.profile, mPROFILE_ROOT_MAIN, "b");
	assert_int_equal(fallThrough->selfCycles, 4);
	_teardownProgram(&test);
}

M_TEST_DEFINE(recorderHiddenJumps) {
	struct RecorderTest test;
	_setupProgram(&test);
	_start(&test, 0x08000100);

	// A BL used as a long jump within main: main carries on running, and nothing returns to it
	_call(&test, 1, 0x08000110, 0x08000180, SP_MAIN);
	_jump(&test, 2, 0x08000184, 0x08000150, SP_MAIN);
	_call(&test, 1, 0x08000154, 0x08000240, SP_MAIN);
	_jump(&test, 3, 0x08000250, 0x08000156, SP_MAIN);

	// An interrupt taken straight after main's BL to a, so the BL itself is never seen as a jump.
	// The BIOS returns to a, with main's LR restored.
	_event(&test, mPROFILE_EVENT_BRANCH, 2, 2, 0x08000160, 0x18, SP_IRQ, 0x08000200 + 4, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 4, 0x00000140, 0x03000000, SP_IRQ, 0x00000144, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 5, 0x03000040, 0x00000144, SP_IRQ, 0, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 3, 0x00000150, 0x08000200, SP_MAIN, 0x08000163, STATUS_THUMB_SYSTEM);
	// a returns to main
	_jump(&test, 6, 0x08000220, 0x08000162, SP_MAIN);

	// A stub with no symbol, just after the end of a function: the callee is where it jumps
	_call(&test, 1, 0x08000170, 0x08000420, SP_MAIN);
	_jump(&test, 1, 0x08000420, 0x08000240, SP_MAIN);
	_jump(&test, 2, 0x08000250, 0x08000172, SP_MAIN);
	// A real function with no symbol is an ordinary call to its region
	_call(&test, 1, 0x08000174, 0x08000500, SP_MAIN);
	_jump(&test, 3, 0x08000510, 0x08000504, SP_MAIN);
	_jump(&test, 4, 0x08000520, 0x08000176, SP_MAIN);
	_charge(&test, 1);
	mProfileFinish(&test.profile);
	_assertAllCharged(&test);

	const struct mProfileNode* mainNode = _child(&test.profile, mPROFILE_ROOT_MAIN, "main");
	uint32_t main = _index(&test.profile, mainNode);
	assert_int_equal(mainNode->recursiveCalls, 0);
	assert_int_equal(mainNode->selfCycles, 1 + 2 + 1 + 2 + 1 + 1 + 1 + 1);
	const struct mProfileNode* b = _child(&test.profile, main, "b");
	assert_int_equal(b->calls, 2);
	assert_int_equal(b->selfCycles, 3 + 2);
	const struct mProfileNode* unknown = _child(&test.profile, main, "[unknown 0x08000000]");
	assert_int_equal(unknown->calls, 1);
	assert_int_equal(unknown->selfCycles, 3 + 4);
	const struct mProfileNode* a = _child(&test.profile, main, "a");
	assert_int_equal(a->calls, 1);
	assert_int_equal(a->selfCycles, 6);
	assert_int_equal(
	    _child(&test.profile, _index(&test.profile, _child(&test.profile, mPROFILE_ROOT_IRQ, "[BIOS]")), "handler")
	        ->selfCycles,
	    5);
	// Everything has returned: only the root frame is left
	assert_int_equal(mProfileRecorderFrameListSize(&test.recorder.frames), 1);
	_teardownProgram(&test);
}

M_TEST_DEFINE(recorderBiosInternalCalls) {
	struct RecorderTest test;
	_setupProgram(&test);
	_start(&test, 0x08000100);

	// main makes an SWI. The BIOS calls one of its own routines in System mode, an interrupt is
	// taken inside that routine, then both return.
	_event(&test, mPROFILE_EVENT_BRANCH, 2, 1, 0x08000110, 0x08, SP_SUPERVISOR, 0x08000112, STATUS_ARM_SUPERVISOR);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 4, 0x00000008, 0x00000140, SP_SUPERVISOR, 0x08000112,
	       STATUS_ARM_SUPERVISOR);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 2, 0x000000A0, 0x000001C4, SP_MAIN - 0x10, 0x000000A4, STATUS_ARM_SYSTEM);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 3, 0x000001F0, 0x18, SP_IRQ, 0x000001F4 + 4, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 4, 0x00000140, 0x03000000, SP_IRQ, 0x00000144, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 5, 0x03000040, 0x00000144, SP_IRQ, 0, STATUS_ARM_IRQ);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 3, 0x00000150, 0x000001F4, SP_MAIN - 0x10, 0, STATUS_ARM_SYSTEM);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 2, 0x000001D0, 0x000000A4, SP_MAIN - 0x10, 0, STATUS_ARM_SYSTEM);
	_event(&test, mPROFILE_EVENT_BRANCH, 4, 3, 0x00000150, 0x08000112, SP_MAIN, 0, STATUS_THUMB_SYSTEM);
	_charge(&test, 1);
	mProfileFinish(&test.profile);
	_assertAllCharged(&test);

	// The BIOS calling itself is neither a call nor recursion
	uint32_t main = _index(&test.profile, _child(&test.profile, mPROFILE_ROOT_MAIN, "main"));
	const struct mProfileNode* bios = _child(&test.profile, main, "[BIOS]");
	assert_int_equal(bios->calls, 1);
	assert_int_equal(bios->recursiveCalls, 0);
	assert_int_equal(bios->selfCycles, 4 + 2 + 3 + 2 + 3);
	assert_int_equal(mProfileEdgeListSize(&test.profile.recursion), 0);
	const struct mProfileNode* irqBios = _child(&test.profile, mPROFILE_ROOT_IRQ, "[BIOS]");
	assert_int_equal(_child(&test.profile, _index(&test.profile, irqBios), "handler")->selfCycles, 5);
	assert_int_equal(mProfileRecorderFrameListSize(&test.recorder.frames), 1);
	_teardownProgram(&test);
}

#ifdef M_CORE_GBA
// The test ROM (profile-rom/main.c) run in a GBA core with the profiler attached

#define ROM_DONE 0x0203FFF0
#define ROM_TIMER_TICKS 0x0203FFF4
#define ROM_HBLANK_TICKS 0x0203FFF8
#define ROM_NESTED_HBLANKS 0x0203FFFC

static void _silentLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	UNUSED(category);
	UNUSED(level);
	UNUSED(format);
	UNUSED(args);
}

static struct mLogger _silentLogger = { .log = _silentLog };

// The node for a function under a root, wherever it is in that tree
static const struct mProfileNode* _findInTree(const struct mProfile* profile, uint32_t root, const char* name) {
	uint32_t function = _functionId(profile, name);
	size_t i;
	for (i = 0; i < mProfileNodeListSize(&profile->nodes); ++i) {
		const struct mProfileNode* node = mProfileNodeListGetConstPointer(&profile->nodes, i);
		if (node->function != function) {
			continue;
		}
		uint32_t ancestor = i;
		while (mProfileNodeListGetConstPointer(&profile->nodes, ancestor)->parent != mPROFILE_NONE) {
			ancestor = mProfileNodeListGetConstPointer(&profile->nodes, ancestor)->parent;
		}
		if (ancestor == root) {
			return node;
		}
	}
	return NULL;
}

M_TEST_DEFINE(profileTestROM) {
	mLogSetDefaultLogger(&_silentLogger);
	struct VFile* rom = VFileFromConstMemory(profile_rom_gba, profile_rom_gba_len);
	struct mCore* core = mCoreFindVF(rom);
	assert_non_null(core);
	assert_true(core->init(core));
	static mColor videoBuffer[256 * 256];
	core->setVideoBuffer(core, videoBuffer, 256);
	mCoreConfigInit(&core->config, NULL);
	mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "ignore");
	mCoreLoadConfig(core);
	assert_true(core->loadROM(core, rom));
	core->reset(core);

	struct mDebugger debugger;
	mDebuggerInit(&debugger);
	mDebuggerAttach(&debugger, core);
	struct mDebuggerProfiler* profiler = calloc(1, sizeof(*profiler));
	mDebuggerProfilerInit(profiler);
	mDebuggerAttachModule(&debugger, &profiler->d);
	struct VFile* elf = VFileFromConstMemory(profile_rom_elf, profile_rom_elf_len);
	assert_true(mDebuggerProfilerLoadSymbols(profiler, elf, "profile-rom.elf"));
	elf->close(elf);
	assert_true(mDebuggerProfilerStart(profiler));

	int frames;
	for (frames = 0; frames < 600 && !core->busRead32(core, ROM_DONE); ++frames) {
		mDebuggerRunFrame(&debugger);
	}
	mDebuggerRunFrame(&debugger);
	mDebuggerProfilerStop(profiler);
	uint32_t timerTicks = core->busRead32(core, ROM_TIMER_TICKS);
	uint32_t hblankTicks = core->busRead32(core, ROM_HBLANK_TICKS);
	uint32_t nestedHBlanks = core->busRead32(core, ROM_NESTED_HBLANKS);
	assert_true(core->busRead32(core, ROM_DONE));
	assert_int_equal(timerTicks, 8);
	assert_true(hblankTicks > 0);
	// HBlank interrupts must have nested inside the timer handler, or that path isn't tested
	assert_true(nestedHBlanks > 0);

	struct mProfile* profile = &profiler->profile;
	assert_int_equal(profile->totalCycles, profile->elapsedCycles);
	uint64_t treeCycles = 0;
	size_t i;
	for (i = 0; i < mProfileNodeListSize(&profile->nodes); ++i) {
		treeCycles += mProfileNodeListGetConstPointer(&profile->nodes, i)->selfCycles;
	}
	assert_int_equal(treeCycles, profile->totalCycles);

	const struct mProfileNode* mainNode = _findInTree(profile, mPROFILE_ROOT_MAIN, "main");
	assert_non_null(mainNode);
	uint32_t main = _index(profile, mainNode);
	assert_int_equal(mainNode->calls, 1);

	const struct mProfileNode* callsLeaf = _child(profile, main, "callsLeaf");
	assert_int_equal(callsLeaf->calls, 1);
	assert_int_equal(_child(profile, _index(profile, callsLeaf), "leaf")->calls, 10);

	const struct mProfileNode* recurse = _child(profile, main, "recurse");
	assert_int_equal(recurse->calls, 3);
	assert_int_equal(recurse->recursiveCalls, 150);

	const struct mProfileNode* mutualA = _child(profile, main, "mutualA");
	const struct mProfileNode* mutualB = _child(profile, _index(profile, mutualA), "mutualB");
	assert_int_equal(mutualA->calls, 2);
	assert_int_equal(mutualA->recursiveCalls, 20);
	assert_int_equal(mutualB->calls, 2);
	assert_int_equal(mutualB->recursiveCalls, 18);

	// Function-pointer and IWRAM calls go through "bx" stubs with no symbol
	const struct mProfileNode* callsPointer = _child(profile, main, "callsPointer");
	assert_int_equal(_child(profile, _index(profile, callsPointer), "pointerTarget")->calls, 5);
	const struct mProfileNode* callsIwram = _child(profile, main, "callsIwram");
	assert_int_equal(_child(profile, _index(profile, callsIwram), "iwramWork")->calls, 6);

	const struct mProfileNode* callsTail = _child(profile, main, "callsTail");
	const struct mProfileNode* tailSource = _child(profile, _index(profile, callsTail), "tailSource");
	assert_int_equal(tailSource->calls, 4);
	assert_int_equal(_child(profile, _index(profile, tailSource), "tailTarget")->calls, 4);

	// DMA started by code is charged to that code, and an SWI mGBA handles itself to its caller
	const struct mProfileNode* callsDma = _child(profile, main, "callsDma");
	const struct mProfileNode* dma = _child(profile, _index(profile, callsDma), "[DMA 3]");
	assert_int_equal(dma->calls, 3);
	// At least 2 cycles per word: a read and a write
	assert_true(dma->selfCycles >= 3 * 64 * 2);
	assert_int_equal(dma->selfExecutions, 0);
	const struct mProfileNode* callsSwi = _child(profile, main, "callsSwi");
	const struct mProfileNode* swiStub = _child(profile, _index(profile, callsSwi), "ObjAffineSet");
	const struct mProfileNode* swi = _child(profile, _index(profile, swiStub), "[SWI 0x0F ObjAffineSet]");
	assert_int_equal(swiStub->calls, 3);
	assert_int_equal(swi->calls, 3);
	assert_true(swi->selfCycles > 3 * 45);

	// Interrupt handlers only appear under [IRQ], once per interrupt they handled
	mProfileFinish(profile);
	assert_int_equal(_function(profile, "timerWork")->calls, timerTicks);
	assert_int_equal(_function(profile, "hblankWork")->calls, hblankTicks);
	assert_null(_findInTree(profile, mPROFILE_ROOT_MAIN, "timerWork"));
	assert_null(_findInTree(profile, mPROFILE_ROOT_MAIN, "hblankWork"));
	assert_non_null(_findInTree(profile, mPROFILE_ROOT_IRQ, "timerWork"));
	assert_true(mProfileNodeListGetConstPointer(&profile->nodes, mPROFILE_ROOT_IRQ)->calls >= timerTicks + hblankTicks);

	// HBlank DMA and halts have roots of their own, and appear nowhere else
	const struct mProfileNode* hblankDma = _child(profile, mPROFILE_NONE, "[DMA HBlank]");
	assert_true(hblankDma->calls > 0);
	assert_true(hblankDma->selfCycles > 0);
	assert_int_equal(hblankDma->subtreeCycles, hblankDma->selfCycles);
	assert_int_equal(_function(profile, "[DMA HBlank]")->calls, hblankDma->calls);
	const struct mProfileNode* idle = _child(profile, mPROFILE_NONE, "[Idle]");
	assert_true(idle->selfCycles > 0);
	assert_int_equal(_function(profile, "[Idle]")->selfCycles, idle->selfCycles);

	// Each complete frame takes the GBA's 280,896 cycles, give or take where the step that ends it
	// finishes. The startup code clears memory for the first few frames; after that, the CPU is
	// halted for part of every frame.
	assert_true(mProfileFrameListSize(&profile->frameCosts) >= 100);
	assert_true(mProfileFrameListSize(&profile->frameCosts) < profile->frames);
	uint64_t frameIdle = 0;
	for (i = 0; i < mProfileFrameListSize(&profile->frameCosts); ++i) {
		const struct mProfileFrame* frame = mProfileFrameListGetConstPointer(&profile->frameCosts, i);
		if (frame->cycles < 280896 - 64 || frame->cycles > 280896 + 64) {
			fail_msg("Frame %u took %u cycles", (unsigned) i, frame->cycles);
		}
		if (frameIdle && !frame->idleCycles) {
			fail_msg("Frame %u has no idle time", (unsigned) i);
		}
		assert_true(frame->idleCycles < frame->cycles);
		frameIdle += frame->idleCycles;
	}
	assert_true(frameIdle > 0);
	assert_true(frameIdle <= idle->selfCycles);
	struct mProfileFrameSummary summary;
	mProfileSummarizeFrames(profile, &summary);
	assert_int_equal(summary.count, mProfileFrameListSize(&profile->frameCosts));
	assert_true(summary.busyCycles < summary.cycles);
	assert_true(summary.worstBusyCycles >= summary.busyCycles / summary.count);

	// Only the recursive functions have recursive calls
	for (i = 0; i < mProfileNodeListSize(&profile->nodes); ++i) {
		const struct mProfileNode* node = mProfileNodeListGetConstPointer(&profile->nodes, i);
		if (!node->recursiveCalls) {
			continue;
		}
		const char* name = mProfileFunctionListGetConstPointer(&profile->functions, node->function)->name;
		if (strcmp(name, "recurse") != 0 && strcmp(name, "mutualA") != 0 && strcmp(name, "mutualB") != 0) {
			fail_msg("Unexpected recursion in %s: %" PRIu64 " recursive calls", name, node->recursiveCalls);
		}
	}

	core->detachDebugger(core);
	mDebuggerDeinit(&debugger);
	mDebuggerProfilerDeinit(profiler);
	free(profiler);
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
}

// Not #ifdef inside the suite macro's arguments, which MSVC doesn't accept
#define PROFILE_ROM_TESTS cmocka_unit_test(profileTestROM),
#else
#define PROFILE_ROM_TESTS
#endif

M_TEST_DEFINE(treeSaveLoadAndExport) {
	struct RecorderTest test;
	_setupProgram(&test);
	_runCalls(&test);
	mProfileFinish(&test.profile);

	struct VFile* vf = VFileMemChunk(NULL, 0);
	assert_true(mProfileSave(&test.profile, vf));
	struct mProfile loaded;
	mProfileInit(&loaded);
	assert_true(mProfileLoad(&loaded, vf));
	vf->close(vf);

	assert_int_equal(mProfileNodeListSize(&loaded.nodes), mProfileNodeListSize(&test.profile.nodes));
	size_t i;
	for (i = 0; i < mProfileNodeListSize(&loaded.nodes); ++i) {
		const struct mProfileNode* a = mProfileNodeListGetConstPointer(&test.profile.nodes, i);
		const struct mProfileNode* b = mProfileNodeListGetConstPointer(&loaded.nodes, i);
		assert_int_equal(a->function, b->function);
		assert_int_equal(a->parent, b->parent);
		assert_int_equal(a->calls, b->calls);
		assert_int_equal(a->selfCycles, b->selfCycles);
		assert_int_equal(a->subtreeCycles, b->subtreeCycles);
	}
	assert_int_equal(_edgeBetween(&loaded, "main", "a")->calls, 2);

	// A node that names a function that doesn't exist must be rejected
	assert_false(_loadText(&loaded, "mprof 1\nnode -1 0 0 0 0 0\nnode -1 1 0 0 0 0\nnode 0 99 1 0 0 0\nend\n"));
	mProfileDeinit(&loaded);

	vf = VFileMemChunk(NULL, 0);
	assert_true(mProfileExportCallgrind(&test.profile, vf));
	size_t size = vf->size(vf);
	char* text = calloc(1, size + 1);
	vf->seek(vf, 0, SEEK_SET);
	assert_int_equal(vf->read(vf, text, size), size);
	vf->close(vf);
	assert_non_null(strstr(text, "cfn=a\ncalls=2 0x08000200\n0x08000100 21 4\n"));
	assert_non_null(strstr(text, "cfn=b\ncalls=1 0x08000240\n0x08000200 7 1\n"));
	free(text);
	_teardownProgram(&test);
}

static void _stall(struct RecorderTest* test, uint8_t type, uint16_t arg, uint32_t cycles) {
	struct mProfileEvent event;
	memset(&event, 0, sizeof(event));
	event.type = type;
	event.arg = arg;
	event.cycles = cycles;
	mProfileRecorderConsume(&test->recorder, &event);
	test->charged += cycles;
}

M_TEST_DEFINE(recorderStalls) {
	struct RecorderTest test;
	_setupProgram(&test);
	_start(&test, 0x08000100);
	_call(&test, 1, 0x08000110, 0x08000200, SP_MAIN);
	// a starts DMA 3 twice and makes an SWI; meanwhile HBlank DMA runs and the CPU halts
	_stall(&test, mPROFILE_EVENT_DMA, 3 | (mDEBUGGER_DMA_IMMEDIATE << 2), 50);
	_jump(&test, 2, 0x08000210, 0x08000204, SP_MAIN - 0x18);
	_stall(&test, mPROFILE_EVENT_DMA, 3 | (mDEBUGGER_DMA_IMMEDIATE << 2), 40);
	_stall(&test, mPROFILE_EVENT_SWI, 0x0F, 60);
	_stall(&test, mPROFILE_EVENT_DMA, 0 | (mDEBUGGER_DMA_HBLANK << 2), 20);
	_stall(&test, mPROFILE_EVENT_DMA, 1 | (mDEBUGGER_DMA_SPECIAL << 2), 8);
	_stall(&test, mPROFILE_EVENT_DMA, 3 | (mDEBUGGER_DMA_SPECIAL << 2), 9);
	_stall(&test, mPROFILE_EVENT_HALT, 0, 1000);
	_stall(&test, mPROFILE_EVENT_SWI, 0x80, 70);
	_jump(&test, 3, 0x08000220, 0x08000112, SP_MAIN);
	_charge(&test, 1);
	mProfileFinish(&test.profile);
	_assertAllCharged(&test);

	uint32_t main = _index(&test.profile, _child(&test.profile, mPROFILE_ROOT_MAIN, "main"));
	const struct mProfileNode* a = _child(&test.profile, main, "a");
	const struct mProfileNode* dma = _child(&test.profile, _index(&test.profile, a), "[DMA 3]");
	assert_int_equal(dma->calls, 2);
	assert_int_equal(dma->selfCycles, 90);
	assert_int_equal(a->selfCycles, 2 + 3);
	assert_int_equal(a->subtreeCycles, 2 + 3 + 90 + 60 + 70);
	assert_int_equal(_child(&test.profile, _index(&test.profile, a), "[SWI 0x0F ObjAffineSet]")->selfCycles, 60);
	assert_int_equal(_child(&test.profile, _index(&test.profile, a), "[SWI]")->selfCycles, 70);
	assert_int_equal(_child(&test.profile, mPROFILE_NONE, "[DMA HBlank]")->selfCycles, 20);
	assert_int_equal(_child(&test.profile, mPROFILE_NONE, "[DMA sound FIFO]")->selfCycles, 8);
	assert_int_equal(_child(&test.profile, mPROFILE_NONE, "[DMA video capture]")->selfCycles, 9);
	const struct mProfileNode* idle = _child(&test.profile, mPROFILE_NONE, "[Idle]");
	assert_int_equal(idle->calls, 1);
	assert_int_equal(idle->selfCycles, 1000);

	// The flat list: costs come from the tree, and the caller is the code that started the DMA
	const struct mProfileFunction* function = _function(&test.profile, "[DMA 3]");
	assert_int_equal(function->selfCycles, 90);
	assert_int_equal(function->calls, 2);
	assert_int_equal(function->executions, 0);
	assert_int_equal(_edgeBetween(&test.profile, "a", "[DMA 3]")->inclusiveCycles, 90);
	assert_int_equal(_function(&test.profile, "a")->inclusiveCycles, 2 + 3 + 90 + 60 + 70);
	size_t top;
	assert_int_equal(mProfileTopFunctions(&test.profile, &top, 1), 1);
	assert_string_equal(mProfileFunctionListGetConstPointer(&test.profile.functions, top)->name, "[Idle]");

	// Roots and frames survive saving and loading
	struct mProfileFrame* frame = mProfileFrameListAppend(&test.profile.frameCosts);
	frame->cycles = 280896;
	frame->idleCycles = 80896;
	frame = mProfileFrameListAppend(&test.profile.frameCosts);
	frame->cycles = 280900;
	frame->idleCycles = 900;
	struct mProfileFrameSummary summary;
	mProfileSummarizeFrames(&test.profile, &summary);
	assert_int_equal(summary.count, 2);
	assert_int_equal(summary.cycles, 280896 + 280900);
	assert_int_equal(summary.busyCycles, 200000 + 280000);
	assert_int_equal(summary.worst, 1);
	assert_int_equal(summary.worstBusyCycles, 280000);

	struct VFile* vf = VFileMemChunk(NULL, 0);
	assert_true(mProfileSave(&test.profile, vf));
	struct mProfile loaded;
	mProfileInit(&loaded);
	assert_true(mProfileLoad(&loaded, vf));
	vf->close(vf);
	assert_int_equal(mProfileNodeListSize(&loaded.nodes), mProfileNodeListSize(&test.profile.nodes));
	size_t i;
	for (i = 0; i < mProfileNodeListSize(&loaded.nodes); ++i) {
		const struct mProfileNode* x = mProfileNodeListGetConstPointer(&test.profile.nodes, i);
		const struct mProfileNode* y = mProfileNodeListGetConstPointer(&loaded.nodes, i);
		assert_int_equal(x->function, y->function);
		assert_int_equal(x->parent, y->parent);
		assert_int_equal(x->calls, y->calls);
		assert_int_equal(x->selfCycles, y->selfCycles);
	}
	assert_int_equal(loaded.totalCycles, test.profile.totalCycles);
	assert_int_equal(mProfileFrameListSize(&loaded.frameCosts), 2);
	assert_int_equal(mProfileFrameListGetConstPointer(&loaded.frameCosts, 1)->idleCycles, 900);
	// Without symbols, [main] is function 17, [IRQ] 18 and [Idle] 27. Extra roots load, but a
	// second [main] root doesn't, and nor does a frame with more idle time than time.
	assert_true(_loadText(&loaded, "mprof 1\nnode -1 17 0 0 0 0\nnode -1 18 0 0 0 0\nnode -1 27 1 0 5 0\nend\n"));
	assert_int_equal(_child(&loaded, mPROFILE_NONE, "[Idle]")->selfCycles, 5);
	assert_false(_loadText(&loaded, "mprof 1\nnode -1 17 0 0 0 0\nnode -1 18 0 0 0 0\nnode -1 17 0 0 0 0\nend\n"));
	assert_false(_loadText(&loaded, "mprof 1\nframe 100 200\nend\n"));
	mProfileDeinit(&loaded);

	vf = VFileMemChunk(NULL, 0);
	assert_true(mProfileExportCallgrind(&test.profile, vf));
	size_t size = vf->size(vf);
	char* text = calloc(1, size + 1);
	vf->seek(vf, 0, SEEK_SET);
	assert_int_equal(vf->read(vf, text, size), size);
	vf->close(vf);
	assert_non_null(strstr(text, "fn=[Idle]\n0x00000000 1000 0\n"));
	assert_non_null(strstr(text, "cfn=[DMA 3]\ncalls=2 0x00000000\n0x08000200 90 0\n"));
	assert_non_null(strstr(text, "desc: Average frame: 240000 of 280898 cycles busy"));
	free(text);
	_teardownProgram(&test);
}

M_TEST_SUITE_DEFINE(Profile, cmocka_unit_test(countersAndRegions), cmocka_unit_test(elfSymbols),
                    cmocka_unit_test(elfRejectsGarbage), cmocka_unit_test(attribution),
                    cmocka_unit_test(saveLoadRoundTrip), cmocka_unit_test(loadValidation),
                    cmocka_unit_test(callgrindExport), cmocka_unit_test(recorderCalls),
                    cmocka_unit_test(recorderRecursion), cmocka_unit_test(recorderTailCalls),
                    cmocka_unit_test(recorderInterrupts), cmocka_unit_test(recorderUnwinding),
                    cmocka_unit_test(recorderTrampolineAndFallThrough), cmocka_unit_test(recorderHiddenJumps),
                    cmocka_unit_test(recorderBiosInternalCalls), cmocka_unit_test(recorderStalls),
                    cmocka_unit_test(treeSaveLoadAndExport), PROFILE_ROM_TESTS)
