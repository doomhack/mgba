/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/debugger/profile.h>

#include <mgba-util/table.h>
#include <mgba-util/vfs.h>

// A minimal ELF32 little-endian reader: just enough to find function symbols.
// It doesn't depend on libelf, so profiling works in builds without ELF support.

#define ELF_HEADER_SIZE 0x34
#define ELF_SECTION_HEADER_SIZE 40
#define ELF_SYMBOL_SIZE 16

#define SHT_SYMTAB 2
#define SHF_EXECINSTR 0x4
#define SHN_UNDEF 0
#define SHN_LORESERVE 0xFF00

#define STT_NOTYPE 0
#define STT_FUNC 2
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2

struct mProfileELFSection {
	uint32_t type;
	uint32_t flags;
	uint32_t addr;
	uint32_t offset;
	uint32_t size;
	uint32_t link;
};

struct mProfileELFSymbol {
	const char* name;
	uint32_t start;
	uint32_t size;
	uint32_t sectionEnd;
	bool thumb;
	bool sized;
	int priority;
};

DECLARE_VECTOR(mProfileELFSymbolList, struct mProfileELFSymbol);
DEFINE_VECTOR(mProfileELFSymbolList, struct mProfileELFSymbol);

static uint16_t _read16(const uint8_t* bytes) {
	return bytes[0] | (bytes[1] << 8);
}

static uint32_t _read32(const uint8_t* bytes) {
	return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

static bool _readSection(const uint8_t* data, size_t size, uint32_t shoff, uint16_t shentsize, uint16_t index,
                         struct mProfileELFSection* section) {
	uint64_t offset = (uint64_t) shoff + (uint64_t) index * shentsize;
	if (offset + ELF_SECTION_HEADER_SIZE > size) {
		return false;
	}
	const uint8_t* header = &data[offset];
	section->type = _read32(&header[4]);
	section->flags = _read32(&header[8]);
	section->addr = _read32(&header[12]);
	section->offset = _read32(&header[16]);
	section->size = _read32(&header[20]);
	section->link = _read32(&header[24]);
	return (uint64_t) section->offset + section->size <= size || section->type == 8; // SHT_NOBITS has no file data
}

// Higher is preferred when several symbols share an address
static int _priority(int type, int binding, const char* name) {
	int priority = 0;
	if (type == STT_FUNC) {
		priority += 8;
	}
	if (binding == STB_GLOBAL) {
		priority += 4;
	} else if (binding == STB_WEAK) {
		priority += 2;
	}
	if (name[0] != '_') {
		priority += 1;
	}
	return priority;
}

static int _compareSymbols(const void* a, const void* b) {
	const struct mProfileELFSymbol* sa = a;
	const struct mProfileELFSymbol* sb = b;
	if (sa->start != sb->start) {
		return sa->start < sb->start ? -1 : 1;
	}
	if (sa->priority != sb->priority) {
		return sa->priority > sb->priority ? -1 : 1;
	}
	size_t la = strlen(sa->name);
	size_t lb = strlen(sb->name);
	if (la != lb) {
		return la < lb ? -1 : 1;
	}
	return strcmp(sa->name, sb->name);
}

static bool _collectSymbols(const uint8_t* data, size_t size, struct mProfileELFSymbolList* symbols) {
	if (size < ELF_HEADER_SIZE ||
	    memcmp(data,
	           "\x7F"
	           "ELF",
	           4) != 0) {
		return false;
	}
	if (data[4] != 1 || data[5] != 1) {
		// Only 32-bit little-endian files
		return false;
	}
	uint32_t shoff = _read32(&data[0x20]);
	uint16_t shentsize = _read16(&data[0x2E]);
	uint16_t shnum = _read16(&data[0x30]);
	if (shentsize < ELF_SECTION_HEADER_SIZE) {
		return false;
	}

	bool found = false;
	uint16_t i;
	for (i = 0; i < shnum; ++i) {
		struct mProfileELFSection symtab;
		if (!_readSection(data, size, shoff, shentsize, i, &symtab) || symtab.type != SHT_SYMTAB) {
			continue;
		}
		struct mProfileELFSection strtab;
		if (symtab.link >= shnum || !_readSection(data, size, shoff, shentsize, symtab.link, &strtab)) {
			continue;
		}
		found = true;
		uint32_t s;
		for (s = 0; s + ELF_SYMBOL_SIZE <= symtab.size; s += ELF_SYMBOL_SIZE) {
			const uint8_t* entry = &data[symtab.offset + s];
			uint32_t nameOffset = _read32(&entry[0]);
			uint32_t value = _read32(&entry[4]);
			uint32_t symSize = _read32(&entry[8]);
			int type = entry[12] & 0xF;
			int binding = entry[12] >> 4;
			uint16_t shndx = _read16(&entry[14]);
			if (shndx == SHN_UNDEF || shndx >= SHN_LORESERVE || shndx >= shnum || nameOffset >= strtab.size) {
				continue;
			}
			const char* name = (const char*) &data[strtab.offset + nameOffset];
			if (!memchr(name, '\0', strtab.size - nameOffset) || !name[0] || name[0] == '$' || name[0] == '.') {
				continue;
			}
			struct mProfileELFSection section;
			if (!_readSection(data, size, shoff, shentsize, shndx, &section) || !(section.flags & SHF_EXECINSTR)) {
				continue;
			}
			if (type == STT_NOTYPE) {
				// Global labels in code are usually hand-written assembly without a .type directive
				if (binding == STB_LOCAL) {
					continue;
				}
			} else if (type != STT_FUNC) {
				continue;
			}
			struct mProfileELFSymbol* symbol = mProfileELFSymbolListAppend(symbols);
			symbol->name = name;
			symbol->thumb = value & 1;
			symbol->start = value & ~1U;
			symbol->size = symSize;
			symbol->sized = type == STT_FUNC && symSize;
			symbol->sectionEnd = section.addr + section.size;
			symbol->priority = _priority(type, binding, name);
		}
	}
	return found;
}

static bool _veneerTarget(const char* name, char* target, size_t targetSize) {
	static const char* const suffixes[] = { "_veneer", "_from_thumb", "_from_arm" };
	if (strncmp(name, "__", 2) != 0) {
		return false;
	}
	size_t length = strlen(name);
	size_t i;
	for (i = 0; i < sizeof(suffixes) / sizeof(*suffixes); ++i) {
		size_t suffixLength = strlen(suffixes[i]);
		if (length <= suffixLength + 2 || strcmp(&name[length - suffixLength], suffixes[i]) != 0) {
			continue;
		}
		size_t targetLength = length - suffixLength - 2;
		if (targetLength + 1 > targetSize) {
			return false;
		}
		memcpy(target, &name[2], targetLength);
		target[targetLength] = '\0';
		return true;
	}
	return false;
}

bool mProfileLoadSymbols(struct mProfile* profile, struct VFile* vf) {
	ssize_t fileSize = vf->size(vf);
	if (fileSize < ELF_HEADER_SIZE) {
		return false;
	}
	uint8_t* data = malloc(fileSize);
	vf->seek(vf, 0, SEEK_SET);
	if (vf->read(vf, data, fileSize) != fileSize) {
		free(data);
		return false;
	}

	struct mProfileELFSymbolList symbols;
	mProfileELFSymbolListInit(&symbols, 0);
	if (!_collectSymbols(data, fileSize, &symbols)) {
		mProfileELFSymbolListDeinit(&symbols);
		free(data);
		return false;
	}
	qsort(mProfileELFSymbolListGetPointer(&symbols, 0), mProfileELFSymbolListSize(&symbols),
	      sizeof(struct mProfileELFSymbol), _compareSymbols);

	mProfileClearSymbols(profile);
	size_t count = mProfileELFSymbolListSize(&symbols);
	size_t i = 0;
	while (i < count) {
		// The first symbol at each address is the preferred one; the rest are aliases
		struct mProfileELFSymbol* symbol = mProfileELFSymbolListGetPointer(&symbols, i);
		size_t next = i + 1;
		while (next < count && mProfileELFSymbolListGetPointer(&symbols, next)->start == symbol->start) {
			++next;
		}
		uint32_t size = symbol->size;
		if (!symbol->sized) {
			// Unsized labels run until the next symbol or the end of their section
			uint32_t end = symbol->sectionEnd;
			if (next < count && mProfileELFSymbolListGetPointer(&symbols, next)->start < end) {
				end = mProfileELFSymbolListGetPointer(&symbols, next)->start;
			}
			size = end > symbol->start ? end - symbol->start : 0;
		}
		if (size) {
			size_t last = profile->symbolCount;
			struct mProfileFunction* previous =
			    last ? mProfileFunctionListGetPointer(&profile->functions, last - 1) : NULL;
			if (previous && previous->start + previous->size > symbol->start) {
				// Overlapping: a label inside a real function belongs to that function
				if (!symbol->sized) {
					i = next;
					continue;
				}
				previous->size = symbol->start - previous->start;
			}
			struct mProfileFunction* function = mProfileFunctionListAppend(&profile->functions);
			memset(function, 0, sizeof(*function));
			function->name = strdup(symbol->name);
			const char* dot = strchr(symbol->name, '.');
			function->baseLength = dot ? (size_t) (dot - symbol->name) : strlen(symbol->name);
			function->start = symbol->start;
			function->size = size;
			function->thumb = symbol->thumb;
			function->foldInto = -1;
			++profile->symbolCount;
		}
		i = next;
	}

	struct Table names;
	HashTableInit(&names, profile->symbolCount, NULL);
	for (i = 0; i < profile->symbolCount; ++i) {
		struct mProfileFunction* function = mProfileFunctionListGetPointer(&profile->functions, i);
		HashTableInsert(&names, function->name, (void*) (uintptr_t) (i + 1));
	}
	for (i = 0; i < profile->symbolCount; ++i) {
		struct mProfileFunction* function = mProfileFunctionListGetPointer(&profile->functions, i);
		char target[512];
		if (!_veneerTarget(function->name, target, sizeof(target))) {
			continue;
		}
		uintptr_t index = (uintptr_t) HashTableLookup(&names, target);
		if (index && index - 1 != i) {
			function->foldInto = index - 1;
		}
	}
	HashTableDeinit(&names);

	mProfileELFSymbolListDeinit(&symbols);
	free(data);
	profile->finished = false;
	return true;
}
