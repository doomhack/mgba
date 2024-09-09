#include <mgba/profiler/profiler.h>

#include <mgba/core/core.h>

mLOG_DEFINE_CATEGORY(PROFILER, "Profiler", "core.profiler");

const uint32_t PROFILER_ID = 0xC0DEBA5E;


void mProfilerInit(struct mProfiler* profiler) {
	memset(profiler, 0, sizeof(*profiler));
}

void mProfilerDeinit(struct mProfiler* profiler) {
	free(profiler->module);
	memset(profiler, 0, sizeof(*profiler));
}

void mProfilerAttach(struct mProfiler* profiler, struct mCore* core) {
	profiler->module = malloc(sizeof(struct mProfilerModule));
	memset(profiler->module, 0, sizeof(struct mProfilerModule));

	profiler->core = core;

	profiler->d.id = PROFILER_ID;
	profiler->d.init = _ProfilerInit;
	profiler->d.deinit = _ProfilerDeInit;

	profiler->module->enterInstruction = _ProfilerEnterInstruction;
	profiler->module->exitInstruction = _ProfilerExitInstruction;
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

void _ProfilerEnterInstruction(struct mProfilerModule* module, void* instr, uint32_t cycles) {

}

void _ProfilerExitInstruction(struct mProfilerModule* module, uint32_t cycles) {

}

void _ProfilerEnterInterrupt(struct mProfilerModule* module, uint32_t interrupt, uint32_t cycles) {

}

void _ProfilerExitInterrupt(struct mProfilerModule* module, uint32_t cycles) {

}

void _ProfilerEnterDMA(struct mProfilerModule* module, uint32_t channel, uint32_t cycles) {

}

void _ProfilerExitDMA(struct mProfilerModule* module, uint32_t cycles) {

}