#ifndef PROFILER_H
#define PROFILER_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/cpu.h>

struct mProfilerModule;

struct mProfiler {
	struct mCPUComponent d;
	struct mCore* core;

	struct mProfilerModule* module;
};

struct mProfilerModule {
	struct mProfiler* p;

	void (*init)(struct mProfilerModule*);
	void (*deinit)(struct mProfilerModule*);

	void (*enterInstruction)(struct mProfilerModule*, void* instr, uint32_t cycles);
	void (*exitInstruction)(struct mProfilerModule*, uint32_t cycles);

	void (*enterInterrupt)(struct mProfilerModule*, uint32_t interrupt, uint32_t cycles);
	void (*exitInterrupt)(struct mProfilerModule*, uint32_t cycles);

	void (*enterDMA)(struct mProfilerModule*, uint32_t channel, uint32_t cycles);
	void (*exitDMA)(struct mProfilerModule*, uint32_t cycles);
};

void mProfilerInit(struct mProfiler*);
void mProfilerDeinit(struct mProfiler*);

void mProfilerAttach(struct mProfiler*, struct mCore*);
void mProfilerAttachModule(struct mProfiler*, struct mProfilerModule*);
void mProfilerDetachModule(struct mProfiler*, struct mProfilerModule*);



void _ProfilerInit(struct mProfilerModule*);
void _ProfilerDeInit(struct mProfilerModule*);

void _ProfilerEnterInstruction(struct mProfilerModule*, void* instr, uint32_t cycles);
void _ProfilerExitInstruction(struct mProfilerModule*, uint32_t cycles);

void _ProfilerEnterInterrupt(struct mProfilerModule*, uint32_t interrupt, uint32_t cycles);
void _ProfilerExitInterrupt(struct mProfilerModule*, uint32_t cycles);

void _ProfilerEnterDMA(struct mProfilerModule*, uint32_t channel, uint32_t cycles);
void _ProfilerExitDMA(struct mProfilerModule*, uint32_t cycles);


inline struct mProfilerModule* mProfilerModule(struct mCPUComponent* component) {
	return component ? ((struct mProfiler*) component)->module : NULL;
}

CXX_GUARD_END

#endif
