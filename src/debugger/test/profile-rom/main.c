/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Test program for the profiler. Each function exercises one thing the call tree has to get
// right, a known number of times, and the interrupt phase nests HBlank interrupts inside a slow
// timer interrupt. The test in ../profile.c runs this in mGBA and checks the profile.
//
// Results the test reads back, at the end of EWRAM
#define RESULT_DONE (*(volatile unsigned*) 0x0203FFF0)
#define RESULT_TIMER_TICKS (*(volatile unsigned*) 0x0203FFF4)
#define RESULT_HBLANK_TICKS (*(volatile unsigned*) 0x0203FFF8)
#define RESULT_NESTED_HBLANKS (*(volatile unsigned*) 0x0203FFFC)

#include <gba.h>

#define NOINLINE __attribute__((noinline))
#define IWRAM_ARM __attribute__((section(".iwram"), long_call, noinline, target("arm")))

volatile int sink;
static volatile int inTimer;

// main -> callsLeaf (1 call) -> leaf (10 calls)
NOINLINE int leaf(int n) {
	int sum = 0;
	int i;
	for (i = 0; i < n; ++i) {
		sum += i;
		sink = sum;
	}
	return sum;
}

NOINLINE int callsLeaf(void) {
	int sum = 0;
	int i;
	for (i = 0; i < 10; ++i) {
		sum += leaf(20);
	}
	return sum;
}

// Direct recursion: 3 calls from main, each 50 levels deep
NOINLINE int recurse(int depth) {
	if (depth <= 0) {
		return 0;
	}
	int result = recurse(depth - 1);
	sink = result;
	return result + depth;
}

// Mutual recursion: 2 calls from main, each alternating 20 levels deep
NOINLINE int mutualB(int depth);

NOINLINE int mutualA(int depth) {
	if (depth <= 0) {
		return 0;
	}
	int result = mutualB(depth - 1);
	sink = result;
	return result + 1;
}

NOINLINE int mutualB(int depth) {
	if (depth <= 0) {
		return 0;
	}
	int result = mutualA(depth - 1);
	sink = result;
	return result + 2;
}

// A function-pointer call, which Thumb code makes through a "bx rN" stub: 5 calls
NOINLINE int pointerTarget(int x) {
	sink = x;
	return x * 3;
}

int (*volatile pointer)(int) = pointerTarget;

NOINLINE int callsPointer(void) {
	int sum = 0;
	int i;
	for (i = 0; i < 5; ++i) {
		sum += pointer(i);
	}
	return sum;
}

// A tail call: tailSource jumps to tailTarget instead of calling it, 4 times
NOINLINE int tailTarget(int x) {
	int sum = 0;
	int i;
	for (i = 0; i < x; ++i) {
		sum += i;
	}
	sink = sum;
	return sum;
}

// Thumb GCC rarely emits real tail calls, so this jumps to tailTarget by hand, leaving LR
// pointing back into callsTail
__attribute__((naked, noinline, target("arm"))) int tailSource(int x) {
	__asm__("add r0, r0, #1\n"
	        "ldr r3, =tailTarget\n"
	        "bx r3\n"
	        ".ltorg\n");
}

NOINLINE int callsTail(void) {
	int sum = 0;
	int i;
	for (i = 0; i < 4; ++i) {
		sum += tailSource(i);
	}
	return sum;
}

// ARM code in IWRAM called from Thumb code in ROM, through a linker veneer: 6 calls
IWRAM_ARM int iwramWork(int x) {
	int sum = 0;
	int i;
	for (i = 0; i < x; ++i) {
		sum += i * x;
	}
	// A side effect, so the compiler can't call it once and reuse the result
	sink = sum;
	return sum;
}

NOINLINE int callsIwram(void) {
	int sum = 0;
	int i;
	for (i = 0; i < 6; ++i) {
		sum += iwramWork(10);
	}
	return sum;
}

// Interrupts. libgba's dispatcher runs handlers in System mode with IME cleared; the timer
// handler sets IME again, so HBlank interrupts (every scanline) nest inside it.
NOINLINE void hblankWork(void) {
	++RESULT_HBLANK_TICKS;
	if (inTimer) {
		++RESULT_NESTED_HBLANKS;
	}
}

NOINLINE void timerWork(void) {
	inTimer = 1;
	int i;
	for (i = 0; i < 400; ++i) {
		sink = i;
	}
	inTimer = 0;
	++RESULT_TIMER_TICKS;
}

static void onHBlank(void) {
	hblankWork();
}

static void onTimer(void) {
	REG_IME = 1;
	timerWork();
}

int main(void) {
	RESULT_DONE = 0;
	RESULT_TIMER_TICKS = 0;
	RESULT_HBLANK_TICKS = 0;
	RESULT_NESTED_HBLANKS = 0;
	irqInit();
	irqEnable(IRQ_VBLANK);

	sink = callsLeaf();
	sink = recurse(50);
	sink = recurse(50);
	sink = recurse(50);
	sink = mutualA(20);
	sink = mutualA(20);
	sink = callsPointer();
	sink = callsTail();
	sink = callsIwram();

	irqSet(IRQ_HBLANK, onHBlank);
	irqSet(IRQ_TIMER0, onTimer);
	REG_TM0CNT_L = 0xC000;
	REG_TM0CNT_H = TIMER_START | TIMER_IRQ | 2;
	irqEnable(IRQ_HBLANK | IRQ_TIMER0);
	while (RESULT_TIMER_TICKS < 8) {
		VBlankIntrWait();
	}
	irqDisable(IRQ_HBLANK | IRQ_TIMER0);
	REG_TM0CNT_H = 0;

	RESULT_DONE = 1;
	while (1) {
		VBlankIntrWait();
	}
}
