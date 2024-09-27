#ifndef COLLECTOR_H
#define COLLECTOR_H

#include <map>
#include <stack>
#include <string>

#include <mgba-util/common.h>


typedef struct functionEntry {
	uint32_t base_addr = 0;
	uint32_t length = 0;
	std::string name;
};

typedef struct functionStats {
	uint64_t cycles = 0;
	uint32_t callCount = 0;
};

typedef struct stackFrame {
	functionEntry* function = nullptr;
	stackFrame* prevFrame = nullptr;
	void* linkReg = nullptr;
};

typedef struct callTreeNode {
	std::map<functionEntry*, callTreeNode*> childNodes;
	callTreeNode* parentNode = nullptr;
	unsigned int callCount = 0;
	uint64_t cycleCount = 0;
	functionEntry* function = nullptr;
};

const std::map<void*, uint64_t>& GetInstructionMap();
const std::map<functionEntry*, functionStats*>& GetFunctionCounts();
const callTreeNode* GetCallTree();

#endif