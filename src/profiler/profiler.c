#include <mgba/profiler/profiler.h>

#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/vfs.h>



mLOG_DEFINE_CATEGORY(PROFILER, "Profiler", "core.profiler");

const uint32_t PROFILER_ID = 0xC0DEBA5E;

uint32_t prevCycles = 0;
void* instruction = NULL;
bool armMode = false;

uint8_t* elfData = NULL;


void mProfilerInit(struct mProfiler* profiler) {
	memset(profiler, 0, sizeof(*profiler));
}

void mProfilerDeinit(struct mProfiler* profiler) {
	free(profiler->module);
	memset(profiler, 0, sizeof(*profiler));
}

void mProfilerAttach(struct mProfiler* profiler, struct mCore* core) {
	profiler->module = malloc(sizeof(struct mProfilerModule));

	if (profiler->module == NULL)
		return;

	memset(profiler->module, 0, sizeof(struct mProfilerModule));

	profiler->core = core;

	profiler->d.id = PROFILER_ID;
	profiler->d.init = _ProfilerInit;
	profiler->d.deinit = _ProfilerDeInit;

	profiler->module->enterInstruction = _ProfilerEnterInstruction;
	profiler->module->exitInstruction = _ProfilerExitInstruction;

	CollectorInitSymbols(core->romPath);
}

void mProfilerAttachModule(struct mProfiler* profiler, struct mProfilerModule* module) {
	profiler->core->attachProfiler(profiler->core, profiler);
}

void mProfilerDetachModule(struct mProfiler* profiler, struct mProfilerModule* module) {
	profiler->core->detachProfiler(profiler->core);
}


void _ProfilerInit(struct mProfilerModule* module)
{

}

void _ProfilerDeInit(struct mProfilerModule* module) {

}

void _ProfilerEnterInstruction(struct mProfilerModule* module, void* instr, uint32_t cycles, bool arm) {
	prevCycles = cycles;
	instruction = instr;
	armMode = arm;
}

void _ProfilerExitInstruction(struct mProfilerModule* module, void* linkReg, uint32_t cycles) {
	if (armMode)
		CollectorArmInstruction(instruction, linkReg, cycles - prevCycles);
	else
		CollectorThumbInstruction(instruction, linkReg, cycles - prevCycles);
}

void _ProfilerEnterInterrupt(struct mProfilerModule* module, uint32_t interrupt, uint32_t cycles) {

}

void _ProfilerExitInterrupt(struct mProfilerModule* module, uint32_t cycles) {

}

void _ProfilerEnterDMA(struct mProfilerModule* module, uint32_t channel, uint32_t cycles) {

}

void _ProfilerExitDMA(struct mProfilerModule* module, uint32_t cycles) {

}