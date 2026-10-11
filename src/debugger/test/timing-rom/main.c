/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// A ROM for checking the profiler by hand: functions with known costs, run once every frame, so a
// recording of any length gives exact cycles per call, and calls in proportion to the frames.
//
// The timed functions are written in assembly so the compiler can't change them, and run with
// interrupts off during VBlank, when nothing else can take bus time from them. ROM wait states
// are set to 3 for the first access and 1 after, with the prefetch buffer off, so the costs
// follow the ARM7TDMI's cycle counts with these access times (in cycles):
//
//                 Thumb fetch    ARM fetch     Data
//                 N     S        N     S
//   ROM           4     2        6     4       (16-bit bus)
//   IWRAM         1     1        1     1
//   EWRAM         3     3        6     6       (16-bit bus)
//
// Each instruction costs its code fetches (S for the next instruction, or N after a jump or a data
// access) plus any data accesses (N) and internal cycles (I). A jump costs S + N + S: the
// instruction, then the first two fetches at the target, in the target's region. So a countdown
// loop of n iterations ("subs; bne; bx lr") costs n * (3S + N) - N, plus the return, where the
// "bx lr" fetches the caller's code: N + S of the caller's region. main is Thumb code in ROM, so a
// return there costs 4 + 2 = 6.
//
// Expected costs, per call, once per frame:
//
//   Function           Code          Called from     Self cycles
//   thumbRomLoop       Thumb, ROM    main            10n + 2  = 5002   (n = 500)
//   armRomLoop         ARM, ROM      main            18n      = 9000
//   thumbIwramLoop     Thumb, IWRAM  main            4n + 5   = 2005
//   armIwramLoop       ARM, IWRAM    main            4n + 5   = 2005
//   thumbEwramLoop     Thumb, EWRAM  main            12n + 3  = 6003
//
// Calls and jumps between modes and memory regions: chainStart (Thumb, ROM) calls chainMiddle
// (ARM, IWRAM) through a "bx" stub, as GCC does. chainMiddle calls thumbIwramLoop, then jumps to
// armRomLoop, which returns straight to chainStart. In the call tree:
//
//   chainStart                     Thumb, ROM                    44
//     chainMiddle                  ARM, IWRAM                    28
//       thumbIwramLoop  (n = 100)  returns to ARM, IWRAM         4n + 1   = 401
//       armRomLoop      (n = 100)  tail call; returns to Thumb   18n      = 1800
//
// chainStart: push 5 (N + 1 to write the stack), ldr from ROM 11 (N + 6 + I), bl 2 + 8, pop 6
// (N + 1 + I), bx back to main 8, and the stub's bx into IWRAM 4 (2 + 1 + 1). chainMiddle: push 2,
// mov 1, ldr 3, mov 1, bx 3, pop 3, mov 1, ldr 3, bx into ROM 11 (1 + 6 + 4).
//
// DMA started by code, as [DMA 3] under dmaCopies, twice per frame. A transfer of n units costs
// 2 per unit plus the wait states of each read and write, plus 2 (2N + 2(n-1)S + 2I):
//
//   256 words, IWRAM to IWRAM      2n + 2          = 514
//   256 words, EWRAM to IWRAM      7n + 2          = 1794   (each read 6, each write 1)
//
// In the rest of the frame, interrupts are on:
//
// - [DMA HBlank]: 4 halfwords from IWRAM to the background scroll registers every HBlank, 160
//   transfers per frame, 2n + 2 = 10 cycles each.
// - [IRQ]: timer 0 overflows every 46,816 cycles, exactly 6 times per frame. Its handler,
//   timerIrq (ARM, IWRAM, called by libgba's dispatcher in IWRAM), costs 4n + 2 = 402 (n = 100,
//   plus the mov that sets it). There's also a VBlank and a VCOUNT interrupt each frame.
// - busyWait (Thumb, ROM) spins on a flag in IWRAM until the VCOUNT interrupt at scanline 80 sets
//   it: a busy loop, around 150,000 cycles. With mGBA's idle-loop optimization set to "detect",
//   mGBA recognizes it and halts the CPU instead, so most of that time moves to [Idle]. (mGBA
//   doesn't recognize loops that poll I/O registers, such as VCOUNT, so those stay busy.)
// - VBlankIntrWait halts the CPU from scanline 80 until VBlank: [Idle].

#include <gba.h>

#define REG_WAITCNT (*(vu16*) 0x04000204)
// WS0: 3 wait states for the first access, 1 after; prefetch buffer off
#define WAITCNT_TIMING 0x0014

// Timer 0 overflows 6 times per 280,896-cycle frame
#define TIMER_RELOAD (0x10000 - 280896 / 6)

void thumbRomLoop(int n);
void armRomLoop(int n);
void thumbIwramLoop(int n);
void armIwramLoop(int n);
void thumbEwramLoop(int n);
void chainStart(void);
void timerIrq(void);
void busyWait(volatile u8* flag);

// The timed functions. Each loop is "subs; bne; bx lr"; only the code's mode and where it lives
// differ. (In Thumb code, in GCC's default divided syntax, "sub" is the flag-setting "subs".)
__asm__(
	// Thumb, ROM
	"	.text\n"
	"	.thumb\n"
	"	.align 2\n"
	"	.global thumbRomLoop\n"
	"	.thumb_func\n"
	"	.type thumbRomLoop, %function\n"
	"thumbRomLoop:\n"
	"1:	sub r0, #1\n"
	"	bne 1b\n"
	"	bx lr\n"
	"	.size thumbRomLoop, .-thumbRomLoop\n"

	// ARM, ROM
	"	.arm\n"
	"	.align 2\n"
	"	.global armRomLoop\n"
	"	.type armRomLoop, %function\n"
	"armRomLoop:\n"
	"1:	subs r0, r0, #1\n"
	"	bne 1b\n"
	"	bx lr\n"
	"	.size armRomLoop, .-armRomLoop\n"

	// Thumb, ROM: spins until the byte at r0 isn't 0
	"	.thumb\n"
	"	.align 2\n"
	"	.global busyWait\n"
	"	.thumb_func\n"
	"	.type busyWait, %function\n"
	"busyWait:\n"
	"1:	ldrb r1, [r0]\n"
	"	cmp r1, #0\n"
	"	beq 1b\n"
	"	bx lr\n"
	"	.size busyWait, .-busyWait\n"

	// Thumb, ROM: calls chainMiddle in ARM code through a "bx" stub, as GCC does
	"	.thumb\n"
	"	.align 2\n"
	"	.global chainStart\n"
	"	.thumb_func\n"
	"	.type chainStart, %function\n"
	"chainStart:\n"
	"	push {lr}\n"
	"	ldr r1, =chainMiddle\n"
	"	bl 2f\n"
	"	pop {r0}\n"
	"	bx r0\n"
	"2:	bx r1\n"
	"	.align 2\n"
	"	.ltorg\n"
	"	.size chainStart, .-chainStart\n"

	// ARM, IWRAM: calls Thumb code, then jumps to ARM code in ROM, which returns to chainStart
	"	.section .iwram,\"ax\",%progbits\n"
	"	.arm\n"
	"	.align 2\n"
	"	.global chainMiddle\n"
	"	.type chainMiddle, %function\n"
	"chainMiddle:\n"
	"	push {lr}\n"
	"	mov r0, #100\n"
	"	ldr r1, =thumbIwramLoop\n"
	"	mov lr, pc\n"
	"	bx r1\n"
	"	pop {lr}\n"
	"	mov r0, #100\n"
	"	ldr r1, =armRomLoop\n"
	"	bx r1\n"
	"	.ltorg\n"
	"	.size chainMiddle, .-chainMiddle\n"

	// ARM, IWRAM
	"	.global armIwramLoop\n"
	"	.type armIwramLoop, %function\n"
	"armIwramLoop:\n"
	"1:	subs r0, r0, #1\n"
	"	bne 1b\n"
	"	bx lr\n"
	"	.size armIwramLoop, .-armIwramLoop\n"

	// ARM, IWRAM: the timer interrupt's handler
	"	.global timerIrq\n"
	"	.type timerIrq, %function\n"
	"timerIrq:\n"
	"	mov r0, #100\n"
	"1:	subs r0, r0, #1\n"
	"	bne 1b\n"
	"	bx lr\n"
	"	.size timerIrq, .-timerIrq\n"

	// Thumb, IWRAM
	"	.thumb\n"
	"	.align 2\n"
	"	.global thumbIwramLoop\n"
	"	.thumb_func\n"
	"	.type thumbIwramLoop, %function\n"
	"thumbIwramLoop:\n"
	"1:	sub r0, #1\n"
	"	bne 1b\n"
	"	bx lr\n"
	"	.size thumbIwramLoop, .-thumbIwramLoop\n"

	// Thumb, EWRAM
	"	.section .ewram.text,\"ax\",%progbits\n"
	"	.thumb\n"
	"	.align 2\n"
	"	.global thumbEwramLoop\n"
	"	.thumb_func\n"
	"	.type thumbEwramLoop, %function\n"
	"thumbEwramLoop:\n"
	"1:	sub r0, #1\n"
	"	bne 1b\n"
	"	bx lr\n"
	"	.size thumbEwramLoop, .-thumbEwramLoop\n"

	"	.text\n");

// Called through pointers, as GCC calls through _call_via_rN. A direct call to ARM code would go
// through a linker veneer, which the profiler counts as part of the function.
static void (*volatile const loops[])(int) = {
	thumbRomLoop, armRomLoop, thumbIwramLoop, armIwramLoop, thumbEwramLoop,
};
static void (*volatile const chain)(void) = chainStart;
static void (*volatile const spin)(volatile u8*) = busyWait;

static u32 iwramBuffer[256];
static u32 iwramSource[256];
static u32 ewramSource[256] __attribute__((section(".sbss")));
static u16 scroll;

// Set when scanline 80 starts
static volatile u8 lineReached;

static void onLine(void) {
	lineReached = 1;
}

static void dmaCopy32(const void* source, void* dest, int words) {
	REG_DMA3SAD = (u32) source;
	REG_DMA3DAD = (u32) dest;
	REG_DMA3CNT = DMA_ENABLE | DMA_IMMEDIATE | DMA32 | words;
}

__attribute__((noinline)) void dmaCopies(void) {
	dmaCopy32(iwramSource, iwramBuffer, 256);
	dmaCopy32(ewramSource, iwramBuffer, 256);
}

int main(void) {
	REG_WAITCNT = WAITCNT_TIMING;

	irqInit();
	irqSet(IRQ_TIMER0, timerIrq);
	irqSet(IRQ_VCOUNT, onLine);
	REG_DISPSTAT = (REG_DISPSTAT & 0xFF) | (80 << 8);
	irqEnable(IRQ_VBLANK | IRQ_TIMER0 | IRQ_VCOUNT);
	REG_TM0CNT_L = TIMER_RELOAD;
	REG_TM0CNT_H = TIMER_START | TIMER_IRQ;

	// 4 scroll registers every HBlank, all from the same halfword in IWRAM
	REG_DMA0SAD = (u32) &scroll;
	REG_DMA0DAD = (u32) &REG_BG0HOFS;
	REG_DMA0CNT = DMA_ENABLE | DMA_HBLANK | DMA_REPEAT | DMA16 | DMA_SRC_FIXED | DMA_DST_RELOAD | 4;

	while (1) {
		VBlankIntrWait();
		lineReached = 0;

		// In VBlank, so no HBlank DMA, and with interrupts off: nothing but the code itself
		REG_IME = 0;
		int i;
		for (i = 0; i < 5; ++i) {
			loops[i](500);
		}
		chain();
		dmaCopies();
		REG_IME = 1;

		// Until scanline 80 of the next frame
		spin(&lineReached);
	}
}
