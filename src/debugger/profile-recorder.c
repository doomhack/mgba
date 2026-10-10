/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/debugger/profile.h>

// Rebuilds the call tree from control-flow events. Calls, returns and interrupts are recognized
// from where each jump actually went and the registers afterwards, not by decoding instructions:
//
// - interrupt entry: a jump to the IRQ vector in IRQ mode starts a new context under [IRQ]
// - return: the target is the return address of a frame on the shadow stack
// - interrupt exit: the target is where the interrupted code resumes
// - call: LR points just past the instruction that jumped
// - anything else is a branch, which may enter a different function (a tail call)
//
// Recursion is folded: entering a function that's already on the current path reuses its node.

DEFINE_VECTOR(mProfileRecorderFrameList, struct mProfileRecorderFrame);

#define VECTOR_IRQ 0x18
#define VECTOR_END 0x20
#define MAX_FRAMES 0x10000
#define MAX_RETURN_SEARCH 64

enum {
	FRAME_ROOT,
	FRAME_CALL,
	FRAME_INTERRUPT,
};

enum {
	MODE_USER = 0x10,
	MODE_FIQ = 0x11,
	MODE_IRQ = 0x12,
	MODE_SUPERVISOR = 0x13,
	MODE_ABORT = 0x17,
	MODE_UNDEFINED = 0x1B,
	MODE_SYSTEM = 0x1F,
};

static uint8_t _bank(uint32_t status) {
	switch (status & 0x1F) {
	case MODE_FIQ:
		return 1;
	case MODE_IRQ:
		return 2;
	case MODE_SUPERVISOR:
		return 3;
	case MODE_ABORT:
		return 4;
	case MODE_UNDEFINED:
		return 5;
	default:
		return 0;
	}
}

static uint32_t _functionAt(struct mProfileRecorder* recorder, uint32_t address) {
	if (address - recorder->cacheStart < recorder->cacheEnd - recorder->cacheStart) {
		return recorder->cacheFunction;
	}
	struct mProfile* profile = recorder->profile;
	uint32_t function = mProfileFunctionAt(profile, address);
	ssize_t symbol = mProfileFindFunction(profile, address);
	if (symbol >= 0) {
		const struct mProfileFunction* entry = mProfileFunctionListGetConstPointer(&profile->functions, symbol);
		recorder->cacheStart = entry->start;
		recorder->cacheEnd = entry->start + entry->size;
		recorder->cacheFunction = function;
	}
	return function;
}

static inline struct mProfileNode* _node(struct mProfileRecorder* recorder, uint32_t node) {
	return mProfileNodeListGetPointer(&recorder->profile->nodes, node);
}

static inline uint32_t _nodeFunction(struct mProfileRecorder* recorder, uint32_t node) {
	return _node(recorder, node)->function;
}

static inline struct mProfileRecorderFrame* _top(struct mProfileRecorder* recorder) {
	return mProfileRecorderFrameListGetPointer(&recorder->frames, mProfileRecorderFrameListSize(&recorder->frames) - 1);
}

static inline uint32_t* _active(struct mProfileRecorder* recorder) {
	unsigned context = recorder->depth < mPROFILE_MAX_CONTEXTS ? recorder->depth : mPROFILE_MAX_CONTEXTS - 1;
	return recorder->active[context];
}

static void _ensureActive(struct mProfileRecorder* recorder) {
	size_t size = mProfileFunctionListSize(&recorder->profile->functions);
	if (size <= recorder->activeSize) {
		return;
	}
	unsigned context;
	for (context = 0; context < mPROFILE_MAX_CONTEXTS; ++context) {
		recorder->active[context] = realloc(recorder->active[context], size * sizeof(uint32_t));
		memset(&recorder->active[context][recorder->activeSize], 0, (size - recorder->activeSize) * sizeof(uint32_t));
	}
	recorder->activeSize = size;
}

static void _release(struct mProfileRecorder* recorder, uint32_t* owned, uint32_t node) {
	if (*owned == mPROFILE_NONE) {
		return;
	}
	uint32_t* active = _active(recorder);
	if (active[*owned] == node + 1) {
		active[*owned] = 0;
	}
	*owned = mPROFILE_NONE;
}

// Enter a function below a node, folding recursion. Returns the node now executing.
static uint32_t _enter(struct mProfileRecorder* recorder, uint32_t parent, uint32_t function, uint32_t* owned) {
	uint32_t* active = _active(recorder);
	*owned = mPROFILE_NONE;
	if (active[function]) {
		uint32_t node = active[function] - 1;
		++_node(recorder, node)->recursiveCalls;
		mProfileAddRecursion(recorder->profile, _nodeFunction(recorder, parent), function, 1);
		return node;
	}
	uint32_t node = mProfileGetChild(recorder->profile, parent, function);
	++_node(recorder, node)->calls;
	active[function] = node + 1;
	*owned = function;
	return node;
}

static bool _isTrampoline(struct mProfileRecorder* recorder, uint32_t function) {
	return mProfileFunctionListGetConstPointer(&recorder->profile->functions, function)->trampoline;
}

// Control moved to a different function without a call or a return
static void _transition(struct mProfileRecorder* recorder, struct mProfileRecorderFrame* frame, uint32_t function) {
	if (function == _nodeFunction(recorder, frame->node)) {
		return;
	}
	if (frame->stubCaller != mPROFILE_NONE) {
		if (_isTrampoline(recorder, function)) {
			// Still in the stub
			return;
		}
		// The stub jumped to the function it calls
		frame->baseNode = _enter(recorder, frame->stubCaller, function, &frame->baseOwned);
		frame->node = frame->baseNode;
		frame->stubCaller = mPROFILE_NONE;
		return;
	}
	_release(recorder, &frame->leafOwned, frame->node);
	if (function == _nodeFunction(recorder, frame->baseNode)) {
		frame->node = frame->baseNode;
		return;
	}
	frame->node = _enter(recorder, frame->baseNode, function, &frame->leafOwned);
}

static void _pop(struct mProfileRecorder* recorder) {
	struct mProfileRecorderFrame* frame = _top(recorder);
	_release(recorder, &frame->leafOwned, frame->node);
	_release(recorder, &frame->baseOwned, frame->baseNode);
	bool context = frame->kind != FRAME_CALL;
	if (frame->kind == FRAME_INTERRUPT && recorder->depth) {
		--recorder->depth;
	}
	mProfileRecorderFrameListResize(&recorder->frames, -1);
	if (!context) {
		return;
	}
	// Only popping a root or interrupt frame changes which context is current
	recorder->contextBase = 0;
	size_t i = mProfileRecorderFrameListSize(&recorder->frames);
	while (i-- > 0) {
		if (mProfileRecorderFrameListGetPointer(&recorder->frames, i)->kind != FRAME_CALL) {
			recorder->contextBase = i;
			break;
		}
	}
}

static void _popTo(struct mProfileRecorder* recorder, size_t size) {
	while (mProfileRecorderFrameListSize(&recorder->frames) > size) {
		_pop(recorder);
	}
}

static void _pushCall(struct mProfileRecorder* recorder, uint32_t function, const struct mProfileEvent* event) {
	if (mProfileRecorderFrameListSize(&recorder->frames) >= MAX_FRAMES) {
		_transition(recorder, _top(recorder), function);
		return;
	}
	uint32_t parent = _top(recorder)->node;
	// GCC calls through function pointers in Thumb code with a BL to a "bx rN" stub: a named one
	// such as _call_via_r3, or a local one inside or just after the calling function, which may
	// have no symbol. The callee is wherever the stub jumps, not the stub. A BL to the start of
	// the calling function is ordinary recursion.
	bool stub;
	uint32_t fallback = mPROFILE_NONE;
	if (function < recorder->profile->symbolCount) {
		const struct mProfileFunction* target =
		    mProfileFunctionListGetConstPointer(&recorder->profile->functions, function);
		stub = target->trampoline || (function == _nodeFunction(recorder, parent) && event->to != target->start);
	} else {
		// Code with no symbol may be a stub, or a function that just has no symbol
		stub = true;
		fallback = function;
	}
	struct mProfileRecorderFrame* frame = mProfileRecorderFrameListAppend(&recorder->frames);
	memset(frame, 0, sizeof(*frame));
	frame->kind = FRAME_CALL;
	frame->returnAddress = event->lr & ~1U;
	frame->sp = event->sp;
	frame->bank = _bank(event->status);
	frame->leafOwned = mPROFILE_NONE;
	if (stub) {
		frame->baseNode = parent;
		frame->baseOwned = mPROFILE_NONE;
		frame->stubCaller = parent;
		frame->stubAddress = event->to;
		frame->stubFallback = fallback;
	} else {
		frame->baseNode = _enter(recorder, parent, function, &frame->baseOwned);
		frame->stubCaller = mPROFILE_NONE;
	}
	frame->node = frame->baseNode;
}

static void _pushInterrupt(struct mProfileRecorder* recorder, const struct mProfileEvent* event) {
	if (recorder->depth + 1 >= mPROFILE_MAX_CONTEXTS ||
	    mProfileRecorderFrameListSize(&recorder->frames) >= MAX_FRAMES) {
		// Too deep to track separately: treat the handler as called from the interrupted code
		_pushCall(recorder, _functionAt(recorder, event->to), event);
		return;
	}
	++recorder->depth;
	memset(_active(recorder), 0, recorder->activeSize * sizeof(uint32_t));
	struct mProfileRecorderFrame* frame = mProfileRecorderFrameListAppend(&recorder->frames);
	memset(frame, 0, sizeof(*frame));
	frame->kind = FRAME_INTERRUPT;
	frame->baseNode = mPROFILE_ROOT_IRQ;
	frame->node = mPROFILE_ROOT_IRQ;
	frame->resumeAddress = event->lr - 4;
	frame->interruptedFrom = event->from;
	frame->interruptedWidth = event->width;
	frame->sp = event->sp;
	frame->bank = _bank(event->status);
	frame->baseOwned = mPROFILE_NONE;
	frame->leafOwned = mPROFILE_NONE;
	frame->stubCaller = mPROFILE_NONE;
	recorder->contextBase = mProfileRecorderFrameListSize(&recorder->frames) - 1;
	++_node(recorder, mPROFILE_ROOT_IRQ)->calls;
	_transition(recorder, frame, _functionAt(recorder, event->to));
}

static void _start(struct mProfileRecorder* recorder, const struct mProfileEvent* event) {
	_ensureActive(recorder);
	while (mProfileRecorderFrameListSize(&recorder->frames)) {
		_pop(recorder);
	}
	recorder->depth = 0;
	unsigned context;
	for (context = 0; context < mPROFILE_MAX_CONTEXTS; ++context) {
		memset(recorder->active[context], 0, recorder->activeSize * sizeof(uint32_t));
	}
	struct mProfileRecorderFrame* frame = mProfileRecorderFrameListAppend(&recorder->frames);
	memset(frame, 0, sizeof(*frame));
	frame->kind = FRAME_ROOT;
	frame->baseNode = mPROFILE_ROOT_MAIN;
	frame->node = mPROFILE_ROOT_MAIN;
	frame->baseOwned = mPROFILE_NONE;
	frame->leafOwned = mPROFILE_NONE;
	frame->stubCaller = mPROFILE_NONE;
	recorder->contextBase = 0;
	recorder->cacheStart = 0;
	recorder->cacheEnd = 0;
	_transition(recorder, frame, _functionAt(recorder, event->to));
}

static void _branch(struct mProfileRecorder* recorder, const struct mProfileEvent* event) {
	uint32_t mode = event->status & 0x1F;
	uint32_t target = _functionAt(recorder, event->to);

	// A stub's "bx" is the first thing it runs. If something else jumped first, the BL was a long
	// jump within the calling function (Thumb code uses BL for jumps beyond the range of B), not a
	// call, so nothing will return to it.
	while (mProfileRecorderFrameListSize(&recorder->frames) - 1 > recorder->contextBase) {
		struct mProfileRecorderFrame* frame = _top(recorder);
		if (frame->stubCaller == mPROFILE_NONE || event->from == frame->stubAddress) {
			break;
		}
		if (frame->stubFallback != mPROFILE_NONE) {
			// A call into code with no symbol that wasn't a stub after all
			frame->baseNode = _enter(recorder, frame->stubCaller, frame->stubFallback, &frame->baseOwned);
			frame->node = frame->baseNode;
			frame->stubCaller = mPROFILE_NONE;
			break;
		}
		_pop(recorder);
	}

	// Frames whose stack has been popped have finished, e.g. after a longjmp
	uint8_t bank = _bank(event->status);
	while (mProfileRecorderFrameListSize(&recorder->frames) - 1 > recorder->contextBase) {
		struct mProfileRecorderFrame* frame = _top(recorder);
		if (frame->kind != FRAME_CALL || frame->bank != bank || event->sp <= frame->sp) {
			break;
		}
		_pop(recorder);
	}

	if (event->to < VECTOR_END && mode != MODE_USER && mode != MODE_SYSTEM) {
		if (event->to == VECTOR_IRQ && mode == MODE_IRQ) {
			_pushInterrupt(recorder, event);
		} else {
			// Other exceptions, such as SWIs into a real BIOS, return like calls
			_pushCall(recorder, target, event);
		}
		return;
	}

	size_t size = mProfileRecorderFrameListSize(&recorder->frames);
	size_t i;
	for (i = size - 1; i > recorder->contextBase && size - i <= MAX_RETURN_SEARCH; --i) {
		if (mProfileRecorderFrameListGetPointer(&recorder->frames, i)->returnAddress == event->to) {
			_popTo(recorder, i);
			_transition(recorder, _top(recorder), target);
			return;
		}
	}

	struct mProfileRecorderFrame* base = mProfileRecorderFrameListGetPointer(&recorder->frames, recorder->contextBase);
	if (base->kind == FRAME_INTERRUPT && event->to == base->resumeAddress) {
		uint32_t interruptedFrom = base->interruptedFrom;
		uint8_t interruptedWidth = base->interruptedWidth;
		_popTo(recorder, recorder->contextBase);
		if (event->to != interruptedFrom + interruptedWidth) {
			// The interrupt was taken straight after an instruction that jumped, so that jump
			// was never seen. Handle it now, with the interrupted code's registers restored.
			struct mProfileEvent hidden = *event;
			hidden.from = interruptedFrom;
			hidden.width = interruptedWidth;
			_branch(recorder, &hidden);
			return;
		}
		_transition(recorder, _top(recorder), target);
		return;
	}

	if ((event->lr & ~1U) == event->from + event->width) {
		_pushCall(recorder, target, event);
		return;
	}

	_transition(recorder, _top(recorder), target);
}

void mProfileRecorderInit(struct mProfileRecorder* recorder, struct mProfile* profile) {
	memset(recorder, 0, sizeof(*recorder));
	recorder->profile = profile;
	mProfileRecorderFrameListInit(&recorder->frames, 64);
}

void mProfileRecorderDeinit(struct mProfileRecorder* recorder) {
	mProfileRecorderFrameListDeinit(&recorder->frames);
	unsigned context;
	for (context = 0; context < mPROFILE_MAX_CONTEXTS; ++context) {
		free(recorder->active[context]);
	}
	memset(recorder, 0, sizeof(*recorder));
}

void mProfileRecorderConsume(struct mProfileRecorder* recorder, const struct mProfileEvent* event) {
	if (event->type == mPROFILE_EVENT_START) {
		_start(recorder, event);
		return;
	}
	if (!mProfileRecorderFrameListSize(&recorder->frames)) {
		// Nothing to attribute to before the first START
		return;
	}

	struct mProfileRecorderFrame* frame = _top(recorder);
	if (event->type == mPROFILE_EVENT_BRANCH &&
	    (frame->stubCaller == mPROFILE_NONE || event->from != frame->stubAddress)) {
		// Code that runs from one symbol straight into the next has no branch between them. (A
		// pending stub's own jump is handled as a call to wherever it goes.)
		_transition(recorder, frame, _functionAt(recorder, event->from));
	}
	struct mProfileNode* node = _node(recorder, frame->node);
	node->selfCycles += event->cycles;
	node->selfExecutions += event->instructions;

	if (event->type == mPROFILE_EVENT_BRANCH) {
		_branch(recorder, event);
	}
}
