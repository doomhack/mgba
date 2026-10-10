/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba/internal/debugger/profile.h>
#include <mgba-util/vfs.h>

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

M_TEST_SUITE_DEFINE(Profile, cmocka_unit_test(countersAndRegions), cmocka_unit_test(elfSymbols),
                    cmocka_unit_test(elfRejectsGarbage), cmocka_unit_test(attribution),
                    cmocka_unit_test(saveLoadRoundTrip), cmocka_unit_test(loadValidation),
                    cmocka_unit_test(callgrindExport))
