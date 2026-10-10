# GBA Profiler: Design

Status: accepted, implementation not started.
Branch: `profiler`, based on upstream `master` at `143adf5aa`.

This is an independent fork. It is developed without coordination with upstream, but structured so that upstream could merge it unchanged if they ever chose to (see "Keeping the fork adoptable").

## Summary

Add a cycle-accurate function profiler for GBA software (primarily homebrew built with debug symbols) to mGBA. It reports, per function and per instruction, how many emulated cycles were spent, both self and inclusive, along with a call tree. Interrupt handlers get their own tree. DMA and halt time are accounted for explicitly.

The profiler is implemented as an `mDebuggerModule` in `src/debugger/`, written in C, so it is available to every frontend. Results are written in the callgrind format so that existing tools (KCachegrind, QCachegrind) work immediately. On top of that sit a headless command-line mode for scripts and CI, and a Qt view.

## Background: the prototype

A prototype ([doomhack/mgba](https://github.com/doomhack/mgba) `master`, based on mGBA 0.10.0-764) proved the concept and produced usable results on real projects. It also showed what needs to change:

| Prototype | Problem | This design |
|---|---|---|
| New hooks in `ARMStep`/`ThumbStep`, new `CPU_COMPONENT_PROFILER` slot, new `mCore` vtable entries | Touches the hot path, takes a reserved slot, and the generic ARM core casts `cpu->master` to `struct GBA*` | Uses the existing debugger-module callback, so there are no CPU core changes |
| Cycle delta measured with `cpu->cycles` inside one step | `cpu->cycles` is reset by the event loop; DMA stalls and halts happen outside the step and were lost | Deltas between steps from `mTimingGlobalTime` |
| Calls/returns inferred by comparing the current function with a shadow stack | IRQ handlers appeared as calls from whatever was interrupted, scattering them across the tree | Reuse the ARM debugger's stack trace, which decodes branches and marks interrupt frames |
| Symbols from `arm-none-eabi-objdump` at a hard-coded path via `_popen` | Windows-only, needs devkitPro installed | Read the ELF symbol table with mGBA's existing ELF support |
| Vendored libelf and libelfin, CMake edits | Build complexity, licensing review | No new dependencies |
| C++ collector with global state | Not usable from C frontends, one session per process | C, state owned by the module instance |
| Inclusive cycles updated by walking to the root on every instruction | O(depth) per instruction | Self cycles only while recording; inclusive computed at export |

## Goals

- Self and inclusive emulated cycles and call counts per function, and a call tree.
- Cycles and execution counts per instruction, for annotated disassembly.
- Interrupt handlers profiled in a separate tree, not mixed into the interrupted code.
- DMA time charged to the code that started it; DMA triggered by hardware (HBlank, VBlank, sound FIFO) and halt/idle time reported in their own buckets.
- Usable headlessly from the command line, and from Qt with save, load and compare.
- Profiling overhead affects only wall-clock speed. Emulated cycle counts are identical with and without the profiler.

## Non-goals (for now)

- Game Boy (SM83) support. Interfaces should stay platform-neutral where it costs nothing, so it can follow later.
- Wall-clock (host) profiling of mGBA itself.
- DWARF line tables or source-level views. Function symbols and addresses are enough for v1, and the callgrind format leaves room for adding source lines later.

## Design

### 1. A debugger module

When a module calls `mDebuggerModuleSetNeedsCallback`, the debugger enters `DEBUGGER_CALLBACK`. `mDebuggerRunTimeout` then runs, per instruction:

```
core->step(core);                              // events, one instruction, events
debugger->platform->checkBreakpoints(platform); // also updates the stack trace
module->custom(module);                         // profiler sample
```

`mDebuggerAccessLogger` already works this way. The profiler is a new module type:

```c
enum mDebuggerType { ..., DEBUGGER_ACCESS_LOGGER, DEBUGGER_PROFILER, DEBUGGER_MAX };

struct mDebuggerProfiler {
	struct mDebuggerModule d;
	struct mProfile* profile;      // accumulated results, see §6
	uint64_t lastTime;             // mTimingGlobalTime at the previous sample
	uint32_t lastPC;               // instruction executed by the previous step
	...
};

void mDebuggerProfilerInit(struct mDebuggerProfiler*);
void mDebuggerProfilerDeinit(struct mDebuggerProfiler*);
bool mDebuggerProfilerLoadSymbols(struct mDebuggerProfiler*, struct VFile* elf);
void mDebuggerProfilerStart(struct mDebuggerProfiler*);
void mDebuggerProfilerStop(struct mDebuggerProfiler*);
bool mDebuggerProfilerWriteCallgrind(struct mDebuggerProfiler*, struct VFile*);
```

Because it is a module, it coexists with the CLI debugger, the GDB stub and the access logger, and it attaches the same way in every frontend (in Qt, `CoreController::attachDebuggerModule`).

### 2. Clock and cost attribution

Each callback computes `delta = mTimingGlobalTime(timing) - lastTime`. That covers everything the previous `core->step` did: the instruction itself, including wait states and prefetch, plus any events processed around it (DMA blocking, halt, IRQ entry). The delta is charged to the instruction at `lastPC`, after removing DMA and halt time (§5).

Using the global clock rather than `cpu->cycles` means no sample is ever negative or dropped. It also means the sum of all charged cycles equals the elapsed emulated time, which is a useful invariant for tests.

The PC comes from `nextInstructionInfo`, which also decodes the instruction. That's wasted work on every step, but it needs no API change. If milestone 1 shows it matters, a "current PC" accessor on `mDebuggerPlatform` is added as a separate commit (decision D1).

### 3. Symbols

- Source: an ELF file. By default this is the loaded ROM if it is an ELF, otherwise `<rom basename>.elf` next to the ROM if present. Otherwise the user chooses one (Qt), or passes it with a flag (CLI).
- Read `.symtab` with the existing `mgba-util/elf.h` helpers, keeping `STT_FUNC` entries with their `st_size`. Clear bit 0 of `st_value` (it marks Thumb functions).
- Keep the functions in an array sorted by address. Look up with a last-hit cache first, then binary search. Most consecutive samples fall in the same function, so the cache hit rate is high.
- Fallbacks when there is no symbol: `[BIOS]` for 0x00000000–0x00003FFF, otherwise `[unknown <region>]` for each memory region (ROM, IWRAM, EWRAM), so cycles are never dropped.
- `mCoreLoadELFSymbols` discards sizes and types, so the profiler parses the ELF itself (decision D3) and leaves the shared symbol table unchanged.

### 4. Call stacks and interrupt contexts

The ARM debugger platform maintains `mDebugger.stackTrace` when the stack-trace mode is enabled. `ARMDebuggerUpdateStackTraceInternal` decodes each instruction: it pushes a frame on `BL` / `BX` with LR set; it pops on `BX LR`, `POP {…, pc}`, `LDM … pc` and exception returns, and when SP rises above a frame's base. When the PC lands on the exception vectors in an exception mode, it pushes a frame with `interrupt = true`.

The profiler enables `STACK_TRACE_ENABLED` while recording. Then, for each sample:

1. Find the topmost frame with `interrupt` set. If there is one, the sample belongs to the **IRQ context**: its call path is the frames above that frame, under a synthetic `[IRQ]` root. Otherwise it belongs to the **main context**, under the program root.
2. The call path is the function containing each frame's `entryAddress`.
3. The leaf is the function containing the PC. If that differs from the top frame's function (a tail call, `B` into another function, or fall-through), the leaf is added as a child of the top frame's node. That way cycles always go to the code that actually ran.

The tree is stored as nodes keyed by `(parent, function)`. The profiler keeps a pointer to the current node, and when the stack depth and top frame haven't changed (the common case) it reuses that node without re-walking the stack.

Nested interrupts (handlers that re-enable IRQs, such as libtonc's nested IRQ dispatcher, which switches to System mode) are the riskiest part of relying on the upstream stack trace. Milestone 2 starts with a test ROM that covers them. If the stack trace gets them wrong, the fix goes into the stack trace itself, as a separate commit, since it benefits the debugger too (decision D5).

### 5. DMA, halt and BIOS

Within one step's `delta` the profiler needs to know how much was DMA (and of which kind) and how much was halt. Neither is observable from outside the GBA core today, so this needs a small core addition (decision D2):

```c
struct GBAProfileCounters {      // in struct GBA, updated only when enabled
	bool enabled;
	uint64_t dmaCycles[4];       // cycles the CPU was blocked, per channel
	uint64_t haltCycles;         // cycles spent halted
};
```

They are updated in `GBADMAService` / `GBAProcessEvents` behind a single branch. The profiler reads the deltas at each sample and splits the step:

- **Immediate DMA** (`GBA_DMA_TIMING_NOW`): charged to the instruction that started it. This is the instruction at `lastPC`, the store to `DMAxCNT`. It is shown as a child pseudo-function `[DMA n]` under that function, so it counts in the caller's inclusive time but stays visible as a separate item.
- **HBlank/VBlank/sound FIFO/video-capture DMA**: not caused by the interrupted code. Charged to top-level `[DMA HBlank]`, `[DMA VBlank]` or `[DMA FIFO]` roots.
- **Halt** (`Halt`, `IntrWait`, `VBlankIntrWait`, and the idle-loop optimisation): charged to a top-level `[Idle]` root, so "frame budget used" is `total - idle`.

The BIOS and SWIs:

- With the HLE BIOS, an SWI runs as C code inside one step, and its cost lands on the `SWI` instruction. The profiler shows it as a child `[SWI 0x0B CpuSet]` (the name comes from the SWI number) under the caller.
- With a real BIOS, BIOS code executes instruction by instruction and falls into `[BIOS]` unless BIOS symbols are supplied.

Idle-loop optimisation (`idleOptimization`, which `perf-main` defaults to `detect`) skips busy-wait loops. That is correct emulated time, but it converts busy-waiting into halt. The profiler reports the setting in the output header, and the CLI mode defaults it to `ignore` so the busy-wait shows up as cycles in the waiting function.

### 6. Data model

All accumulation goes through a recorder API that knows nothing about the emulator. This keeps it unit-testable with synthetic samples:

```c
struct mProfileSample {
	uint32_t pc;
	uint32_t cycles;
	const struct mProfileFrame* stack;  // resolved call path (function ids)
	size_t depth;
	enum mProfileContext context;       // MAIN, IRQ, DMA_HW, IDLE
};

void mProfileRecord(struct mProfile*, const struct mProfileSample*);
```

- **Functions**: a table of symbols and pseudo-functions (`[IRQ]`, `[Idle]`, `[DMA n]`, `[BIOS]`, `[unknown …]`, `[SWI …]`).
- **Call tree**: nodes `{function, parent, children (Table by function id), calls, selfCycles}`. Inclusive cycles are computed by one post-order pass at export or display time, never while recording.
- **Instructions**: arrays allocated lazily for each function, sized from `st_size` and indexed by `(pc - start) >> 1`, holding `{cycles, executions}`. Unknown regions use a sparse `Table`. No address-wide arrays (ROM alone would need 128 MB).
- **Metadata**: ROM title and CRC, ELF path, frames recorded, total cycles, BIOS type, idle-loop setting, mGBA version.

Record boundaries: start/stop at any time; or "reset and start" from boot; or "start at symbol" (for example `main`, as the prototype did). Frame boundaries (frame counter changes) are counted so per-frame averages can be reported. Per-frame breakdowns can come later.

### 7. Output format

The callgrind format is the on-disk format for both export and import:

- It's text and documented, and it handles costs per instruction (`positions: instr`), self costs per function, call edges with inclusive costs (`calls=` lines), and multiple events.
- KCachegrind and QCachegrind open it directly, with annotated disassembly when given the ELF, call graphs and comparisons, so a useful UI exists from day one.
- Events: `Cycles`, `Executions`. DMA and idle are pseudo-functions, not separate events, so they appear in the tree where they were charged.
- Contexts: the main, IRQ, hardware-DMA and idle trees each become a separate root (`[main]`, `[IRQ]`, `[DMA HBlank]`, `[Idle]` …) in one file.

The Qt view reads the same files back, so saving, reopening and comparing all go through one format with no private binary format to maintain. A speedscope JSON exporter can be added later if flame graphs are wanted.

### 8. Frontends

**Headless / command line** (`src/platform/test/perf-main.c`, binary `mgba-perf`). It already runs a ROM for `-F` frames or `-S` seconds with no video. New options:

```
  --profile FILE        Write a callgrind profile to FILE on exit
  --profile-symbols ELF Symbol file (default: ROM if ELF, else <rom>.elf)
  --profile-start SYM   Begin recording when SYM is first executed
```

Example: `mgba-perf -N -F 600 --profile doom.callgrind GBADoom.elf`. This runs on all desktop platforms, works in CI, and makes before/after comparisons scriptable.

**CLI debugger** (SDL frontend, and Qt's debugger console): `profile start|stop|reset|save FILE|top [N]`, implemented in `cli-debugger.c` on the same module API.

**Qt** (`src/platform/qt/ProfilerController.{h,cpp}`, `ProfilerView.{h,cpp,ui}`, modelled on `MemoryAccessLogController` / `MemoryAccessLogView`), under Tools:

1. Start/stop/reset recording, symbol file selection, and a live top-N function list refreshed on a timer.
2. A call tree view with self/inclusive cycles, percentages and call counts, and a flat function list. Per-instruction costs for the selected function, disassembled with mGBA's own decoder (no objdump).
3. Save and open callgrind files.
4. Compare two profiles: a side-by-side function list with cycle and percentage deltas, matched by function name.

**Scripting** (later): expose start/stop/save to Lua through the existing scripting bridge.

## Changes outside the new files

Kept deliberately small, each in its own commit:

1. `enum mDebuggerType`: add `DEBUGGER_PROFILER`. Update `supportsDebuggerType` for the GBA core.
2. GBA core: `GBAProfileCounters` and the updates in `dma.c` / `gba.c` (§5), compiled under `ENABLE_DEBUGGERS` (D2).
3. Only if measurements show it's needed: a "current PC" accessor on `mDebuggerPlatform` (D1).
4. Only if the nested-IRQ tests find problems: fixes to the ARM stack trace (D5).

## Build changes

None beyond adding files to existing lists:

- `src/debugger/CMakeLists.txt`: add `profiler.c` and `profile.c` to `SOURCE_FILES`, and `test/profile.c` to `TEST_FILES`.
- `src/platform/qt/CMakeLists.txt`: add the new controller and view sources and the `.ui` file to the existing lists.

There is no new option: the profiler is part of `ENABLE_DEBUGGERS` and needs no third-party code. ELF symbol loading uses the existing `USE_ELF` support. Without it, the profiler still works with the region-based fallbacks.

## Performance

Profiling switches emulation from `runLoop` to one `step` per instruction, plus a stack-trace update and a sample. That costs wall-clock speed but has no effect on emulated cycle counts. The measurement is deterministic, unlike a sampling profiler.

Targets: the recording path does no allocation after warm-up, does O(1) work in the common case (same function, same stack depth), and stays at least real-time on a typical desktop for a homebrew game. Milestone 1 measures this with `mgba-perf` against the unprofiled baseline.

## Testing

- **Unit tests** (cmocka, `BUILD_SUITE`): feed synthetic sample sequences into `mProfileRecord`. Check self/inclusive totals, tree shape for calls, tail calls and recursion, context separation, and that the total equals the sum of inputs. Check that the callgrind writer round-trips through the reader.
- **Test ROM**: a small devkitARM program with known cycle costs: fixed loops, nested calls, a timer IRQ (plain and nested), immediate and HBlank DMA, and `VBlankIntrWait`. Assert that the profile matches the expected buckets to within a tolerance. It can be driven by `mgba-perf --profile` in CI.
- **Regression against the prototype**: profile the same build of a real project (for example GBADoom) with both and compare the top functions. Differences should be explainable by the IRQ/DMA fixes.

## Milestones

1. **Flat profile.** Module, symbols, clock, per-function and per-instruction self cycles, callgrind export, `mgba-perf --profile`, unit tests. This already reproduces most of the prototype's value.
2. **Call tree and IRQ context.** Stack-trace integration, the IRQ root, tail-call handling, and the nested-IRQ test ROM.
3. **DMA and idle accounting.** GBA core counters, DMA and idle buckets, SWI pseudo-functions.
4. **Qt UI.** Controller and live view, tree, per-instruction disassembly, save/open, compare.
5. **Extras.** The CLI debugger `profile` command, Lua bindings, per-frame breakdowns, and a speedscope export.

Each milestone is a self-contained step that leaves the branch building and working.

## Decisions

Where there was a choice, the option that touches the least shared code wins, because that is what keeps the fork adoptable.

- **D1. Hook point.** A debugger module using `needsCallback`, not a new per-instruction hook in the CPU. It forces one `step` per instruction while profiling, which costs wall-clock speed only. The PC comes from `nextInstructionInfo`; a cheaper accessor is added only if milestone 1's measurements call for it.
- **D2. DMA and halt counters.** Small counters in `struct GBA`, updated behind one `enabled` check and compiled only with `ENABLE_DEBUGGERS`. There's no way to observe this from outside the core, and this is the smallest change that makes it possible.
- **D3. Symbols.** The profiler reads the ELF symbol table itself through `mgba-util/elf.h`. `mDebuggerSymbols` stays unchanged.
- **D4. Output format.** Callgrind for both export and import.
- **D5. Stack trace.** Use the existing ARM stack trace. If the nested-IRQ test ROM shows problems, fix them in the stack trace itself, in commits separate from the profiler.
- **D6. UI placement.** A Tools → Profiler window, in the same style as the memory access logger.

## Keeping the fork adoptable

The fork is developed without coordination with upstream. These rules keep it possible for upstream to merge it unchanged:

- **Clean history on current upstream.** `profiler` is periodically rebased onto upstream `master` rather than merged, so it stays a clean series of commits on top. It is a personal branch, so force-pushing it is fine.
- **Separable commits.** Each change to shared code (the items in "Changes outside the new files") is its own commit with its own rationale, so it can be taken or dropped independently. Profiler code lives in new files.
- **Upstream conventions.** Coding style and naming from `CONTRIBUTING.md`, `.clang-format`, component-prefixed commit messages (`Debugger:`, `GBA:`, `Qt:`), and MPL 2.0 headers on new files. C in the core, and C++ only in the Qt frontend.
- **Zero cost when unused.** No new build options, no new dependencies, nothing on the hot path when no profiler is attached, and everything under the existing `ENABLE_DEBUGGERS`.
- **Every frontend builds.** Changes must not break the SDL, libretro, headless or console builds even where the profiler isn't exposed. Check at least the Qt, SDL and `mgba-perf` targets, and a build with `ENABLE_DEBUGGERS=OFF`.
- **Fork-only material stays apart.** This document and anything only the fork needs live in separate commits that would be easy to leave out of a merge.
