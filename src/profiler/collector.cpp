#include <map>
#include <stack>

#include <mgba/profiler/collector.h>

#include <elf/elf++.hh>


typedef struct functionEntry
{
	uint32_t base_addr = 0;
	uint32_t length = 0;
	std::string name;
};

typedef struct stackFrame
{
	functionEntry* function = nullptr;
	stackFrame* prevFrame = nullptr;
	void* linkReg = nullptr;
};

typedef struct callTreeNode
{
	std::map<functionEntry*, callTreeNode*> childNodes;
	callTreeNode* parentNode = nullptr;
	unsigned int callCount = 0;
	uint64_t cycleCount = 0;
};

std::map<void*, uint64_t> instructionMap;
std::vector<functionEntry*> functionList;
std::vector<stackFrame*> callStack;

callTreeNode* callTree = nullptr;
callTreeNode* currentNode = nullptr;


uint64_t totalCycles = 0;

static functionEntry* findFunction(void* address)
{
	for (int i = 0; i < functionList.size(); i++)
	{
		uint32_t start = functionList[i]->base_addr;
		uint32_t end = functionList[i]->base_addr + functionList[i]->length;

		uint32_t addr = (uint32_t) address;

		if (addr >= start && addr <= end)
			return functionList[i];
	}

	return nullptr;
}

static void checkCall(void* instr, void* linkReg, uint32_t cycles)
{
	if (!currentNode) 
	{
		callTree = new callTreeNode();
		currentNode = callTree;
		currentNode->callCount++;

		stackFrame* frame = new stackFrame();
		frame->function = findFunction(instr);
		frame->linkReg = linkReg;

		callStack.push_back(frame);
	}

	functionEntry* thisFunction = findFunction(instr);

	stackFrame* frame = callStack.back();

	currentNode->cycleCount += cycles;

	if (frame->function != thisFunction)
	{
		stackFrame* p = frame;

		while (p = p->prevFrame)
		{
			if (p->function == thisFunction)
			{
				while (p != callStack.back())
				{
					callStack.pop_back();
					currentNode = currentNode->parentNode;
				}

				return;
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
			currentNode->childNodes[thisFunction] = newNode;

		}
		currentNode = currentNode->childNodes[thisFunction];
		currentNode->callCount++;

	}
}

extern "C" void CollectorArmInstruction(uint32_t* instr, uint32_t* linkReg, uint32_t cycles) {
	totalCycles += cycles;
	instructionMap[instr] += cycles;

	checkCall(instr, linkReg, cycles);
}

extern "C" void CollectorThumbInstruction(uint16_t* instr, uint32_t* linkReg, uint32_t cycles) {
	totalCycles += cycles;
	instructionMap[instr] += cycles;
	
	checkCall(instr, linkReg, cycles);
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
				functionEntry* fe = new functionEntry;
				fe->base_addr = d.value;
				fe->length = d.size;
				fe->name = sym.get_name();

				functionList.push_back(fe);
			}
		}
	}
}