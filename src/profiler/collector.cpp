#include <map>
#include <mgba/profiler/collector.h>

#include <elf/elf++.hh>


typedef struct functionEntry
{
	uint32_t base_addr = 0;
	uint32_t length = 0;
	std::string name;
};

std::map<void*, uint64_t> instructionMap;
std::vector<functionEntry> functionList;

uint64_t totalCycles = 0;

extern "C" void CollectorArmInstruction(uint32_t* instr, uint32_t cycles, bool executed) {
	totalCycles += cycles;
	instructionMap[instr] += cycles;
}

extern "C" void CollectorThumbInstruction(uint16_t* instr, uint32_t cycles, bool executed) {
	totalCycles += cycles;
	instructionMap[instr] += cycles;
}

extern "C" void CollectorInitSymbols(const char* filePath)
{
	FILE* fd = fopen(filePath, "rb");

	if (!fd)
		return;

		elf::elf f(elf::create_mmap_loader(fd));

	for (auto& sec : f.sections())
	{
		if (sec.get_hdr().type != elf::sht::symtab && sec.get_hdr().type != elf::sht::dynsym)
			continue;

		for (auto sym : sec.as_symtab())
		{
			auto& d = sym.get_data();

			if (d.type() == elf::stt::func)
			{
				functionEntry fe;
				fe.base_addr = d.value;
				fe.length = d.size;
				fe.name = sym.get_name();

				functionList.push_back(fe);
			}
		}
	}
}