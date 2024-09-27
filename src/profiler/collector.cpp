#include <mgba/profiler/collector.h>

#include <elf/elf++.hh>


std::map<void*, uint64_t> instructionMap;
std::vector<functionEntry*> functionList;
std::vector<stackFrame*> callStack;
std::map<functionEntry*, functionStats*> functionCounts;

callTreeNode* callTree = nullptr;
callTreeNode* currentNode = nullptr;

uint64_t totalCycles = 0;

const std::map<void*, uint64_t>& GetInstructionMap()
{
	return instructionMap;
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

	stackFrame* frame = callStack.back();

	currentNode->cycleCount += cycles;

	if (frame && frame->function == thisFunction)
		return true;

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

				return true;
			}
		}
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