#include <mgba/profiler/collector.h>

#include <elf/elf++.hh>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>


std::map<void*, uint64_t> instructionMap;
std::vector<functionEntry*> functionList;
std::vector<stackFrame*> callStack;
std::map<functionEntry*, functionStats*> functionCounts;
std::map<void*, std::pair<uint64_t, std::string>> functionInstructions;
std::map<void*, std::string> instructionDisassembly;

callTreeNode* callTree = nullptr;
callTreeNode* currentNode = nullptr;

uint64_t totalCycles = 0;

bool hitMain = false;

const std::map<void*, uint64_t>& GetInstructionMap()
{
	return instructionMap;
}

const std::map<void*, std::pair<uint64_t, std::string>>& GetInstructionsForFunction(const char* functionName)
{
	functionInstructions.clear();

	//Find function
	for (int i = 0; i < functionList.size(); i++)
	{
		functionEntry* function = functionList[i];

		if (function->name == functionName)
		{
			// Find instructions for function
			uint32_t start = function->base_addr;

			for (int i = 0; i < function->length; i++)
			{
				if (instructionMap.count((void*)(i + start)) > 0)
				{
					functionInstructions[(void*) (i + start)] = std::pair<uint64_t, std::string>(instructionMap[(void*) (i + start)], instructionDisassembly[(void*) (i + start)]);
				}
			}
		}
	}

	return functionInstructions;
}


const std::map<functionEntry*, functionStats*>& GetFunctionCounts()
{
	return functionCounts;
}

const callTreeNode* GetCallTree()
{
	return callTree;
}

static bool addrInFunction(void* address, functionEntry* func)
{
	uint32_t start = func->base_addr;
	uint32_t end = func->base_addr + func->length;

	uint32_t addr = (uint32_t) address;

	if (addr >= start && addr < end)
		return true;

	return false;
}

static functionEntry* findFunction(void* address)
{
	static functionEntry* lastFunction = nullptr;

	if (lastFunction && addrInFunction(address, lastFunction))
		return lastFunction;

	for (int i = 0; i < functionList.size(); i++)
	{
		if (addrInFunction(address, functionList[i]))
		{
			lastFunction = functionList[i];
			return functionList[i];
		}
			
	}

	return nullptr;
}

static bool checkCall2(void* instr, void* linkReg, uint32_t cycles)
{
	functionEntry* thisFunction = findFunction(instr);

	if (!functionCounts.count(thisFunction))
	{
		functionCounts[thisFunction] = new functionStats();
	}

	functionCounts[thisFunction]->callCount++;
	functionCounts[thisFunction]->cycles += cycles;

	return true;
}

static bool checkCall(void* instr, void* linkReg, uint32_t cycles)
{	
	functionEntry* thisFunction = findFunction(instr);

	if (!hitMain)
	{
		if (thisFunction && thisFunction->name == "main")
			hitMain = true;
		else
			return false;
	}

		if (!functionCounts.count(thisFunction))
		functionCounts[thisFunction] = new functionStats();

	functionCounts[thisFunction]->cycles += cycles;

	if (thisFunction->base_addr == 0x4000)
		return true;
		

	stackFrame* frame = callStack.back();

	if (frame && frame->function == thisFunction)
	{
		callTreeNode* n = currentNode;

		while (n)
		{
			n->inclusiveCycleCount += cycles;
			n = n->parentNode;
		}

		currentNode->cycleCount += cycles;
		return true;
	}
		

	if (frame) {
		stackFrame* p = frame;

		while (p = p->prevFrame)
		{
			if (p->function == thisFunction)
			{
				while (p != callStack.back())
				{
					delete callStack.back();
					callStack.pop_back();
					currentNode = currentNode->parentNode;
				}

				callTreeNode* n = currentNode;

				while (n)
				{
					n->inclusiveCycleCount += cycles;
					n = n->parentNode;
				}
				currentNode->cycleCount += cycles;


				return true;
			}
		}
	}

	callTreeNode* n = currentNode;

	while (n) {
		n->inclusiveCycleCount += cycles;
		n = n->parentNode;
	}

	//Call
	stackFrame* newFrame = new stackFrame();
	newFrame->function = thisFunction;
	newFrame->linkReg = linkReg;
	newFrame->prevFrame = frame;

	callStack.push_back(newFrame);

	if (!currentNode->childNodes.count(thisFunction))
	{
		callTreeNode* newNode = new callTreeNode();
		newNode->parentNode = currentNode;
		newNode->function = thisFunction;
		currentNode->childNodes[thisFunction] = newNode;

	}
	currentNode = currentNode->childNodes[thisFunction];
	currentNode->callCount++;
	currentNode->cycleCount += cycles;
	currentNode->inclusiveCycleCount += cycles;

	functionCounts[thisFunction]->callCount++;

	return true;
}

extern "C" void CollectorArmInstruction(uint32_t* instr, uint32_t* linkReg, uint32_t cycles) {

	instr--;

	if (checkCall(instr, linkReg, cycles))
	{
		totalCycles += cycles;
		instructionMap[instr] += cycles;
	}
}

extern "C" void CollectorThumbInstruction(uint16_t* instr, uint32_t* linkReg, uint32_t cycles) {

	instr--;

	if (checkCall(instr, linkReg, cycles))
	{
		totalCycles += cycles;
		instructionMap[instr] += cycles;
	}
}

extern "C" void CollectorInitSymbols(const char* filePath)
{
	std::array<char, 4096> buffer {};
	std::string result;

	std::string command = std::string("C:\\devkitPro\\devkitARM\\bin\\arm-none-eabi-objdump.exe -D -C ") + filePath;
	FILE* pipe = _popen(command.c_str(), "r");
	if (!pipe)
		throw std::runtime_error("popen() failed!");

	functionEntry* currentFunction = nullptr;

	while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
	{
		std::string bs = std::string(buffer.data());
		if (buffer[0] == '0')
		{
			char* endptr = &buffer.data()[8];

			unsigned int addr = strtoul(buffer.data(), &endptr, 16);

			if (currentFunction)
			{
				currentFunction->length = (addr - currentFunction->base_addr);
			}

			functionEntry* fe = new functionEntry;
			fe->base_addr = addr;

			std::string name = std::string(&buffer.data()[10]);
			name = name.substr(0, name.find_last_of('>'));

			fe->name = name;

			functionList.push_back(fe);

			currentFunction = fe;
		}
		else if (buffer[0] == ' ' && buffer[8] == ':')
		{
			char* endptr = &buffer.data()[8];

			unsigned int addr = strtoul(&buffer.data()[1], &endptr, 16);

			instructionDisassembly[(void*) addr] = std::string(&buffer.data()[0]);
			instructionDisassembly[(void*) addr].pop_back();
		}
	}

	_pclose(pipe);

	functionEntry* fe = new functionEntry;
	fe->base_addr = 0x4000;
	fe->length = 32;
	fe->name = "IRQ";
	functionList.push_back(fe);

	currentNode = callTree = new callTreeNode();
	callStack.push_back(nullptr);
}

extern "C" void CollectorInitSymbols2(const char* filePath)
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

			if (d.type() == elf::stt::func || d.type() == elf::stt::notype)
			{
				std::string name = sym.get_name();

				if (name.rfind(".", 0) == 0)
					continue; //Skip labels.

				if (name.rfind("$", 0) == 0)
					continue; // Skip $a, $d, $t etc.

				functionEntry* fe = new functionEntry;
				fe->base_addr = d.value & 0xfffffffe;
				fe->length = d.size + (d.size & 1);
				fe->name = sym.get_name();

				functionList.push_back(fe);
			}
		}
	}

	for (int i = 0; i < functionList.size(); i++)
	{
		functionEntry* fe = functionList[i];

		if (fe->length == 0)
		{
			uint32_t nextSym = 0xffffffff;

			for (int j = 0; j < functionList.size(); j++)
			{
				functionEntry* t = functionList[j];

				if (t->base_addr > fe->base_addr)
				{
					if (t->base_addr < nextSym)
						nextSym = t->base_addr;
				}
			}

			fe->length = (nextSym - (fe->base_addr - 2));
		}
	}


	currentNode = callTree = new callTreeNode();
	callStack.push_back(nullptr);
}