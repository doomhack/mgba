# GBA Profiler: Design

Status: accepted, objectives set 2026-10-10, implementation not started.
Branch: `profiler`, based on upstream `master` at `143adf5aa`.

This is an independent fork. It is developed without coordination with upstream, but structured so that upstream could merge it unchanged if they ever chose to (see "Keeping the fork adoptable").

## Summary

Add a cycle-accurate function profiler to mGBA for GBA homebrew built with symbols. It reports, per function and per instruction, how many emulated cycles were spent, both self and inclusive, along with a call tree and how much of each frame's cycle budget was used. Interrupt handlers get their own tree. DMA and halt time are accounted for explicitly.

The profiler is implemented as an `mDebuggerModule` in `src/debugger/`, written in C, so any frontend can use it. Version 1 exposes it through the Qt frontend and through a `profile` command in the CLI debugger. Each profile holds both a call tree (what was called from where, with call counts and subtree timings) and a flat per-function list. Profiles are saved in a native format that keeps all of this, and can be exported to callgrind for existing tools such as KCachegrind.

## Background: the prototype

A prototype ([doomhack/mgba](https://github.com/doomhack/mgba) branch `profiler-prototype`, based on mGBA 0.10.0-764) proved the concept and produced usable results on real projects. It also showed what needs to change:

| Prototype | Problem | This design |
|---|---|---|
| New hooks in `ARMStep`/`ThumbStep`, new `CPU_COMPONENT_PROFILER` slot, new `mCore` vtable entries | Touches the hot path, takes a reserved slot, and the generic ARM core casts `cpu->master` to `struct GBA*` | Uses the existing debugger-module callback, so there are no CPU core changes |
| Cycle delta measured with `cpu->cycles` inside one step | `cpu->cycles` is reset by the event loop; DMA stalls and halts happen outside the step and were lost | Deltas between steps from `mTimingGlobalTime` |
| Calls/returns inferred by comparing the current function with a shadow stack | IRQ handlers appeared as calls from whatever was interrupted, scattering them across the tree | Reuse the ARM debugger's stack trace, which decodes branches and marks interrupt frames |
| Symbols from `arm-none-eabi-objdump` at a hard-coded path via `_popen` | Windows-only, needs devkitPro installed | Read the ELF symbol table with mGBA's existing ELF support |
| Vendored libelf and libelfin, CMake edits | Build complexity, licensing review | No new dependencies |
| C++ collector with global state | Not usable from C frontends, one session per process | C, state owned by the module instance |
| Inclusive cycles updated by walking to the root on every instruction | O(depth) per instruction | Self cycles only while recording; inclusive computed at export |

## Audience

GBA homebrew developers building with devkitARM or another toolchain that produces an ELF with a symbol table. The author's own projects (for example GBADoom) are the reference cases, but the UI, messages and docs assume other users too. ROMs without symbols still work, at the level of memory regions (§3), but get no further effort in v1.

## Objectives (definition of done for v1)

Version 1 is done when all of these hold:

1. **No external tools.** Any GBA ROM with an ELF can be profiled without devkitPro, objdump or anything else installed at runtime, on every platform the Qt frontend supports.
2. **All time accounted for.** Over a recording, the cycles charged to code, IRQ handlers, DMA and idle add up to exactly the emulated time elapsed. Automated tests check this.
3. **Correct attribution.** IRQ handlers, including nested ones, appear only under the `[IRQ]` root. DMA started by code is charged to that code. Hardware-triggered DMA and idle time appear in their own buckets. A purpose-built test ROM verifies all of this against known costs.
4. **Useful reports.** Every profile has both views of the data (§6):
   - a **call tree**: what was called from where, with call counts and self and subtree (inclusive) cycles at each node;
   - a **flat function list**: one row per function with call count, self and inclusive cycles, and its callers and callees.

   Also per-instruction cycles with disassembly from mGBA's own decoder, and frame-budget figures: average and worst frame, as cycles and as a percentage of the 280,896-cycle frame, with idle time excluded.
5. **Save, open, compare.** Profiles save to a native file that reopens in mGBA with the full call tree, flat list, per-instruction data and frame figures intact, and two can be compared side by side. They also export as callgrind files for KCachegrind/QCachegrind. Runs are not made reproducible, so a comparison includes run-to-run variation, and the UI says so.
6. **Playable while profiling.** With the profiler recording, GBADoom runs at 50% of full speed or better on the author's machine, so a game can be played into the scene of interest. Cycle counts are exact at any speed.
7. **Two ways in.** Qt Tools → Profiler window, and the `profile` command in the CLI debugger (covering the SDL frontend).
8. **Still adoptable.** Based on current upstream; the Qt, SDL, libretro and `mgba-perf` targets build, as does a build with `ENABLE_DEBUGGERS=OFF`; nothing runs when no profiler is attached.

## Non-goals for v1

- Headless profiling through `mgba-perf`, and Lua scripting bindings. Both are straightforward on the same module API (see "After v1").
- Reproducible runs (input recording and replay).
- Better support for ROMs without symbols (address-based grouping, user-supplied symbol files other than ELF).
- Game Boy (SM83) support. Interfaces stay platform-neutral where it costs nothing, so it can follow later.
- DWARF line tables or source-level views. Both file formats leave room for adding source lines later.
- Wall-clock (host) profiling of mGBA itself.

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
bool mDebuggerProfilerSave(struct mDebuggerProfiler*, struct VFile*);           // native .mprof
bool mDebuggerProfilerExportCallgrind(struct mDebuggerProfiler*, struct VFile*);
```

Because it is a module, it coexists with the CLI debugger, the GDB stub and the access logger, and it attaches the same way in every frontend (in Qt, `CoreController::attachDebuggerModule`). Using the CLI debugger while profiling must keep working; milestone 2 checks this, since both rely on the stack trace.

### 2. Clock and cost attribution

Each callback computes `delta = mTimingGlobalTime(timing) - lastTime`. That covers everything the previous `core->step` did: the instruction itself, including wait states and prefetch, plus any events processed around it (DMA blocking, halt, IRQ entry). The delta is charged to the instruction at `lastPC`, after removing DMA and halt time (§5).

Using the global clock rather than `cpu->cycles` means no sample is ever negative or dropped, and that the sum of all charged cycles equals the elapsed emulated time (objective 2).

The PC comes from `nextInstructionInfo`, which also decodes the instruction. That's wasted work on every step, but it needs no API change. If the speed objective isn't met, this is the first thing to replace (see "Performance").

### 3. Symbols

- Source: an ELF file. By default this is the loaded ROM if it is an ELF, otherwise `<rom basename>.elf` next to the ROM if present. Otherwise the user chooses one (Qt) or names it with `profile symbols FILE` (CLI debugger).
- Read `.symtab` with the existing `mgba-util/elf.h` helpers, keeping `STT_FUNC` entries with their `st_size`. Clear bit 0 of `st_value` (it marks Thumb functions).
- Fold linker veneers (`__*_from_thumb`, `__*_from_arm`, `__*_veneer`) into the function they jump to, so they don't appear as extra levels in the call tree.
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

Calls are counted on entry, not per sample:

- A stack frame being pushed counts one call on the new node.
- Control moving from the top frame's function into a different leaf function (a tail call or plain branch, step 3) counts one call on that leaf node.
- Returning to a function after its callee finishes is not a call.
- Each interrupt entry counts one call on the `[IRQ]` root, so its count is the number of interrupts taken.
- `[DMA n]` and `[SWI …]` count one call per DMA transfer or SWI.

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

Idle-loop optimisation (the `idleOptimization` setting) skips busy-wait loops. That is correct emulated time, but it turns busy-waiting into halt, so a game that waits in a loop shows that time as `[Idle]` rather than as cycles in the waiting function. The profiler leaves the setting alone. It records it in the profile's metadata, and the Qt view shows a notice when it isn't `ignore`.

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
void mProfileEndFrame(struct mProfile*);
```

- **Functions**: a table of symbols and pseudo-functions (`[IRQ]`, `[Idle]`, `[DMA n]`, `[BIOS]`, `[unknown …]`, `[SWI …]`).
- **Call tree** (recorded): nodes `{function, parent, children (Table by function id), calls, selfCycles}`. Each distinct call path gets its own node, so the same function called from two places appears twice with separate counts and cycles. Recursion is kept as it happened: each recursion level is a deeper node.
- **Flat function list** (derived from the tree when a recording stops or a file loads, never while recording):
  - *calls* and *self cycles*: the sums over all of the function's nodes;
  - *inclusive cycles*: the sum of the subtree cycles of the function's nodes that have no ancestor of the same function, so recursion isn't counted twice;
  - *callers* and *callees*: for each caller→callee function pair, the calls and the callee's inclusive cycles summed over all tree edges between them.

  Every figure is also shown as a percentage of total recorded cycles.
- **Instructions**: arrays allocated lazily for each function, sized from `st_size` and indexed by `(pc - start) >> 1`, holding `{cycles, executions}`. Unknown regions use a sparse `Table`. No address-wide arrays (ROM alone would need 128 MB).
- **Frames**: a growable array with each frame's busy cycles (total minus idle) and total cycles, appended at each frame-counter change. That's enough for the average, worst and percentage-of-budget figures (objective 4); at 8 bytes per frame, an hour of recording is under 2 MB. Partial frames at the start and end of a recording are excluded from the figures.
- **Metadata**: ROM title and CRC, ELF path, frames recorded, total cycles, BIOS type, idle-loop setting, mGBA version.

Recording can start and stop at any time, or "reset and start" from boot, or "start at symbol" (for example `main`, as the prototype did).

### 7. File formats

**Native format (`.mprof`): save and open.** Callgrind can't hold the call tree. It stores costs per function and caller→callee edges merged across call sites, not call paths, so subtree timings would be lost on reload. The native format stores exactly what was recorded:

- Text, line-oriented and versioned, so it's easy to write and parse in C, and diffable. Unknown record types are skipped, so later versions can add data without breaking older readers.
- A header with the metadata (§6), then records for the function table, the call tree (one line per node: id, parent, function, calls, self cycles), per-instruction costs and per-frame cycles.
- Only recorded data is stored. Inclusive cycles and the flat list are recomputed on load by the same code that derives them after recording, so the two can't disagree.

Writer and reader live in the core library (`profile.c`), so any frontend can save and load.

**Callgrind: export only.** For KCachegrind/QCachegrind, which give call graphs and annotated disassembly when pointed at the ELF:

- Costs per instruction (`positions: instr`), self costs per function, and call edges with inclusive costs (`calls=` lines), built from the flat list's caller/callee data.
- Events: `Cycles`, `Executions`. DMA and idle are pseudo-functions, so they appear where they were charged.
- The main, IRQ, hardware-DMA and idle contexts each become a separate root (`[main]`, `[IRQ]`, `[DMA HBlank]`, `[Idle]` …).
- Frame figures and metadata go in `desc:` header lines.

The export loses the per-path tree, as any callgrind file must; that's what the native format is for.

### 8. Frontends

**CLI debugger** (the SDL frontend's `-d` debugger, and Qt's debugger console), implemented in `cli-debugger.c` on the module API:

```
profile start            Start recording (attaches the profiler if needed)
profile stop             Stop recording; results are kept
profile reset            Discard results
profile symbols FILE     Load symbols from an ELF
profile save FILE        Save the profile (.mprof)
profile export FILE      Export a callgrind file
profile top [N]          Flat list: the N most expensive functions, self and inclusive
profile tree [DEPTH]     Call tree with calls, self and subtree cycles, to DEPTH levels
profile func NAME        One function: totals, callers and callees
profile frames           Print the frame-budget figures
```

This is also the first frontend built (milestone 1), because it's the quickest way to exercise the module before any UI exists.

**Qt** (`src/platform/qt/ProfilerController.{h,cpp}`, `ProfilerView.{h,cpp,ui}`, modelled on `MemoryAccessLogController` / `MemoryAccessLogView`), under Tools → Profiler:

1. Start/stop/reset recording, symbol file selection, a live top-N function list refreshed on a timer, and a notice when idle-loop optimisation is on.
2. **Call tree** view: expandable tree with calls, self cycles, subtree cycles and percentages at each node, sorted by subtree cycles.
3. **Flat list** view: sortable table of functions with calls, self and inclusive cycles and percentages. Selecting a function shows its callers and callees (with calls and cycles for each), and its per-instruction costs disassembled with mGBA's own decoder. Selecting a function in either view can jump to its nodes in the other.
4. A frame-budget summary: average and worst frame, as cycles and as a percentage of the budget.
5. Save and open `.mprof` files; export callgrind.
6. Compare two profiles: the flat lists side by side matched by function name, and the trees matched by call path, each with cycle and percentage deltas. Frame-budget figures are compared too, and the view notes that runs aren't reproducible.

## Changes outside the new files

Kept deliberately small, each in its own commit:

1. `enum mDebuggerType`: add `DEBUGGER_PROFILER`. Update `supportsDebuggerType` for the GBA core.
2. GBA core: `GBAProfileCounters` and the updates in `dma.c` / `gba.c` (§5), compiled under `ENABLE_DEBUGGERS` (D2).
3. CLI debugger: the `profile` command set.
4. Only if the speed objective needs it: a "current PC" accessor on `mDebuggerPlatform` (D1).
5. Only if the nested-IRQ tests find problems: fixes to the ARM stack trace (D5).

## Build changes

None beyond adding files to existing lists:

- `src/debugger/CMakeLists.txt`: add `profiler.c` and `profile.c` to `SOURCE_FILES`, and the tests to `TEST_FILES`.
- `src/platform/qt/CMakeLists.txt`: add the new controller and view sources and the `.ui` file to the existing lists.

There is no new option: the profiler is part of `ENABLE_DEBUGGERS` and needs no third-party code. ELF symbol loading uses the existing `USE_ELF` support. Without it, the profiler still works with the region-based fallbacks.

## Performance

Profiling switches emulation from `runLoop` to one `step` per instruction, plus a stack-trace update and a sample. That costs wall-clock speed but has no effect on emulated cycle counts. The measurement is deterministic, unlike a sampling profiler.

Target (objective 6): 50% of full speed or better for GBADoom on the author's machine, with the recording path doing no allocation after warm-up and O(1) work in the common case (same function, same stack depth).

This is the biggest unknown in the design, so milestone 1 measures it first: run a fixed number of frames in-process with and without the profiler attached, and time both. If the target is missed, the fixes in order of increasing invasiveness are:

1. Profile the recording path and fix whatever is slow in the profiler itself.
2. Add the "current PC" accessor (D1) so the instruction isn't decoded twice.
3. Only update the stack trace when the instruction can change control flow.
4. As a last resort, a dedicated per-instruction hook in the ARM core, revisiting D1.

## Testing

- **Unit tests** (cmocka, `BUILD_SUITE`): feed synthetic sample sequences into `mProfileRecord`. Check self/subtree totals, tree shape and call counts for calls, tail calls and recursion, context separation, frame accounting, and that the total equals the sum of inputs. Check the derived flat list: calls, self, recursion-safe inclusive cycles, and caller/callee figures. Check that `.mprof` files round-trip exactly (save, load, compare equal), and that the callgrind export's totals match the profile's.
- **Test ROM**: a small devkitARM program with known cycle costs: fixed loops, nested calls, a timer IRQ (plain and nested), immediate and HBlank DMA, `VBlankIntrWait`, and a long-call veneer. Its source and a prebuilt binary are committed, so running the tests doesn't need devkitARM. A cmocka test loads it into a GBA core in-process, attaches the profiler, runs a fixed number of frames, and checks the profile against the expected buckets to within a tolerance (objectives 2 and 3).
- **Regression against the prototype**: profile the same build of a real project (for example GBADoom) with both and compare the top functions. Differences should be explainable by the IRQ/DMA fixes.

## Milestones

1. **Flat profile.** Speed measurement first. Then the module, symbols (with veneer folding), the clock, per-function and per-instruction self cycles, `.mprof` save/load, callgrind export, the CLI debugger `profile` command (`start`/`stop`/`reset`/`symbols`/`save`/`export`/`top`), and unit tests.
2. **Call tree and IRQ context.** Stack-trace integration, the call tree with call counting, the derived flat list (inclusive cycles, callers, callees), the IRQ root, tail-call handling, `profile tree` and `profile func`, the test ROM and its in-process test, and checking that the CLI debugger still works while profiling.
3. **DMA, idle and frames.** GBA core counters, DMA and idle buckets, SWI pseudo-functions, frame-budget figures.
4. **Qt UI.** Controller and live view, call tree and flat list views with cross-navigation, callers/callees, per-instruction disassembly, frame-budget summary, save/open/export, compare.

Each milestone is a self-contained step that leaves the branch building and working. Version 1 is complete when all the objectives above hold.

## After v1

Candidates, in no particular order: headless profiling through `mgba-perf` (`--profile FILE`) for scripts and CI; Lua bindings; input recording for reproducible comparisons; better support for ROMs without symbols; per-frame drill-down (which functions made the worst frame expensive); a speedscope exporter for flame graphs; Game Boy support.

## Decisions

Where there was a choice, the option that touches the least shared code wins, because that is what keeps the fork adoptable.

- **D1. Hook point.** A debugger module using `needsCallback`, not a new per-instruction hook in the CPU. It forces one `step` per instruction while profiling, which costs wall-clock speed only. The PC comes from `nextInstructionInfo`; the escalation path in "Performance" applies if the speed target is missed.
- **D2. DMA and halt counters.** Small counters in `struct GBA`, updated behind one `enabled` check and compiled only with `ENABLE_DEBUGGERS`. There's no way to observe this from outside the core, and this is the smallest change that makes it possible.
- **D3. Symbols.** The profiler reads the ELF symbol table itself through `mgba-util/elf.h`. `mDebuggerSymbols` stays unchanged.
- **D4. File formats.** A native text format (`.mprof`) for save and open, because callgrind can't store the call tree. Callgrind as an export for KCachegrind/QCachegrind.
- **D5. Stack trace.** Use the existing ARM stack trace. If the nested-IRQ test ROM shows problems, fix them in the stack trace itself, in commits separate from the profiler.
- **D6. UI placement.** A Tools → Profiler window, in the same style as the memory access logger.
- **D7. Frontends for v1.** Qt and the CLI debugger. Headless `mgba-perf` profiling and Lua move to "After v1".
- **D8. Idle-loop optimisation.** Left as the user configured it; recorded in the profile and flagged in the UI, not overridden.

## Keeping the fork adoptable

The fork is developed without coordination with upstream. These rules keep it possible for upstream to merge it unchanged:

- **Clean history on current upstream.** `profiler` is periodically rebased onto upstream `master` rather than merged, so it stays a clean series of commits on top. It is a personal branch, so force-pushing it is fine.
- **Separable commits.** Each change to shared code (the items in "Changes outside the new files") is its own commit with its own rationale, so it can be taken or dropped independently. Profiler code lives in new files.
- **Upstream conventions.** Coding style and naming from `CONTRIBUTING.md`, `.clang-format`, component-prefixed commit messages (`Debugger:`, `GBA:`, `Qt:`), and MPL 2.0 headers on new files. C in the core, and C++ only in the Qt frontend.
- **Zero cost when unused.** No new build options, no new dependencies, nothing on the hot path when no profiler is attached, and everything under the existing `ENABLE_DEBUGGERS`.
- **Every frontend builds.** Changes must not break the SDL, libretro, headless or console builds even where the profiler isn't exposed. Check at least the Qt, SDL, libretro and `mgba-perf` targets, and a build with `ENABLE_DEBUGGERS=OFF`.
- **Fork-only material stays apart.** This document and anything only the fork needs live in separate commits that would be easy to leave out of a merge.
