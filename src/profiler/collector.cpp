#include <map>
#include <mgba/profiler/collector.h>


std::map<void*, uint64_t> instructionMap;

extern "C" void CollectorArmInstruction(uint32_t* instr, uint32_t cycles) {
	instructionMap[instr] += cycles;
}

extern "C" void CollectorThumbInstruction(uint16_t* instr, uint32_t cycles) {
	instructionMap[instr] += cycles;
}