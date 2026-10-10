# GBA Profiler: Design

Status: accepted, objectives set 2026-10-10, implementation not started.
Branch: `profiler`, based on upstream `master` at `143adf5aa`.

This is an independent fork. It is developed without coordination with upstream, but structured so that upstream could merge it unchanged if they ever chose to (see "Keeping the fork adoptable").

## Summary

Add a cycle-accurate function profiler to mGBA for GBA homebrew built with symbols. It reports, per function and per instruction, how many emulated cycles were spent, both self and inclusive, along with a call tree and how much of each frame's cycle budget was used. Interrupt handlers get their own tree. DMA and halt time are accounted for explicitly.

The profiler is an `mDebuggerModule` in `src/debugger/`, written in C, so any frontend can use it. The emulation thread does as little as possible: it updates per-instruction counters and, only when control flow changes, pushes a small event onto a queue. A consumer thread turns the events into the call tree. Version 1 exposes the profiler through the Qt frontend and through a `profile` command in the CLI debugger. Each profile holds both a call tree (what was called from where, with call counts and subtree timings) and a flat per-function list. Profiles are saved in a native format that keeps all of this, and can be exported to callgrind for existing tools such as KCachegrind.

## Background: the prototype

A prototype ([doomhack/mgba](https://github.com/doomhack/mgba) branch `profiler-prototype`, based on mGBA 0.10.0-764) proved the concept and produced usable results on real projects. It also showed what needs to change:

| Prototype | Problem | This design |
|---|---|---|
| New hooks in `ARMStep`/`ThumbStep`, new `CPU_COMPONENT_PROFILER` slot, new `mCore` vtable entries | Touches the hot path, takes a reserved slot, and the generic ARM core casts `cpu->master` to `struct GBA*` | Uses the existing debugger-module callback, so there are no CPU core changes |
| Symbol lookup, call detection and tree updates done inline for every instruction | Slowed emulation noticeably | Inline work is counters plus an event on control-flow changes; everything else runs on a consumer thread (§2) |
| Cycle delta measured with `cpu->cycles` inside one step | `cpu->cycles` is reset by the event loop; DMA stalls and halts happen outside the step and were lost | Deltas between steps from `mTimingGlobalTime` |
| Calls/returns inferred by comparing the current function with a shadow stack | IRQ handlers appeared as calls from whatever was interrupted, scattering them across the tree | Calls, returns and interrupts rebuilt from actual branch targets, LR, SP and CPU mode (§5) |
| Symbols from `arm-none-eabi-objdump` at a hard-coded path via `_popen` | Windows-only, needs devkitPro installed | Read the ELF symbol table with mGBA's existing ELF support |
| Vendored libelf and libelfin, CMake edits | Build complexity, licensing review | No new dependencies |
| C++ collector with global state | Not usable from C frontends, one session per process | C, state owned by the module instance |
| Inclusive cycles updated by walking to the root on every instruction | O(depth) per instruction | Self cycles only while recording; inclusive computed afterwards |

## Audience

GBA homebrew developers building with devkitARM or another toolchain that produces an ELF with a symbol table. The author's own projects (for example GBADoom) are the reference cases, but the UI, messages and docs assume other users too. ROMs without symbols still work, at the level of memory regions (§4), but get no further effort in v1.

## Objectives (definition of done for v1)

Version 1 is done when all of these hold:

1. **No external tools.** Any GBA ROM with an ELF can be profiled without devkitPro, objdump or anything else installed at runtime, on every platform the Qt frontend supports.
2. **All time accounted for.** Over a recording, the cycles charged to code, IRQ handlers, DMA and idle add up to exactly the emulated time elapsed. Automated tests check this.
3. **Correct attribution.** IRQ handlers, including nested ones, appear only under the `[IRQ]` root. DMA started by code is charged to that code. Hardware-triggered DMA and idle time appear in their own buckets. A purpose-built test ROM verifies all of this against known costs.
4. **Useful reports.** Every profile has both views of the data (§7):
   - a **call tree**: what was called from where, with call counts and self and subtree (inclusive) cycles at each node. Recursion is folded into the existing node rather than nesting, so deep recursion such as BSP traversal stays one level, with its recursive calls counted separately;
   - a **flat function list**: one row per function with call count, recursive call count, self and inclusive cycles, and its callers and callees.

   Also per-instruction cycles with disassembly from mGBA's own decoder, and frame-budget figures: average and worst frame, as cycles and as a percentage of the 280,896-cycle frame, with idle time excluded. C++ function names are shown demangled in the Qt frontend.
5. **Save, open, compare.** Profiles save to a native file that reopens in mGBA with the full call tree, flat list, per-instruction data and frame figures intact, and two can be compared side by side. They also export as callgrind files for KCachegrind/QCachegrind. Runs are not made reproducible, so a comparison includes run-to-run variation, and the UI says so.
6. **Playable while profiling.** With the profiler recording, GBADoom runs at 50% of full speed or better on the author's machine, so a game can be played into the scene of interest. Cycle counts are exact at any speed.
7. **Two ways in.** Qt Tools → Profiler window, and the `profile` command in the CLI debugger (covering the SDL frontend).
8. **Still adoptable.** Based on current upstream; the Qt, SDL, libretro and `mgba-perf` targets build, as does a build with `ENABLE_DEBUGGERS=OFF`; nothing runs when no profiler is attached.

## Non-goals for v1

- Headless profiling through `mgba-perf`, and Lua scripting bindings. Both are straightforward on the same module API (see "After v1").
- Reproducible runs (input recording and replay).
- Recording raw event traces to disk for later analysis (see "After v1").
- Better support for ROMs without symbols (address-based grouping, user-supplied symbol files other than ELF).
- Game Boy (SM83) support. Interfaces stay platform-neutral where it costs nothing, so it can follow later.
- Source-level views (see "After v1"). Nothing extra needs recording for them later, since they're computed from the per-instruction counters and the ELF.
- Demangled names in the SDL CLI debugger (see §4 and "After v1").
- Wall-clock (host) profiling of mGBA itself.

## Design

### 1. A debugger module

When a module calls `mDebuggerModuleSetNeedsCallback`, the debugger enters `DEBUGGER_CALLBACK`. `mDebuggerRunTimeout` then runs, per instruction:

```
core->step(core);                              // events, one instruction, events
debugger->platform->checkBreakpoints(platform);
module->custom(module);                         // profiler producer (§2)
```

`mDebuggerAccessLogger` already works this way. The profiler is a new module type:

```c
enum mDebuggerType { ..., DEBUGGER_ACCESS_LOGGER, DEBUGGER_PROFILER, DEBUGGER_MAX };

struct mDebuggerProfiler {
	struct mDebuggerModule d;
	struct mProfileProducer producer;  // emulation-thread state, §2
	struct mProfile* profile;          // owned by the consumer while recording, §7
	...
};

void mDebuggerProfilerInit(struct mDebuggerProfiler*);
void mDebuggerProfilerDeinit(struct mDebuggerProfiler*);
bool mDebuggerProfilerLoadSymbols(struct mDebuggerProfiler*, struct VFile* elf);
void mDebuggerProfilerStart(struct mDebuggerProfiler*);
void mDebuggerProfilerStop(struct mDebuggerProfiler*);   // drains the queue, joins the consumer
bool mDebuggerProfilerSave(struct mDebuggerProfiler*, struct VFile*);           // native .mprof
bool mDebuggerProfilerExportCallgrind(struct mDebuggerProfiler*, struct VFile*);
```

Because it is a module, it coexists with the CLI debugger, the GDB stub and the access logger, and it attaches the same way in every frontend (in Qt, `CoreController::attachDebuggerModule`). The profiler doesn't use the debugger's stack trace, so it can't interfere with the CLI debugger's backtraces. While emulation is paused at a breakpoint, emulated time doesn't advance, so nothing is charged.

The module needs the PC, CPSR, SP and LR after each step, cheaply. `nextInstructionInfo` decodes the instruction, which is exactly the inline work to avoid, and `core->readRegister` looks registers up by name. So the debugger platform gains a small accessor that copies the current register values into a struct (decision D1):

```c
struct mDebuggerRegisterSnapshot {
	uint32_t pc;      // address of the next instruction to execute
	uint32_t status;  // CPSR on ARM
	uint32_t sp;
	uint32_t lr;
	uint8_t width;    // 2 for Thumb, 4 for ARM
};

void (*registerSnapshot)(struct mDebuggerPlatform*, struct mDebuggerRegisterSnapshot*);
```

It's implemented for the ARM platform; the fields are generic enough for SM83 to fill later.

### 2. Event pipeline

The work is split between the emulation thread (the **producer**) and a **consumer** thread:

- **Producer, inline, every instruction:** read the clock and registers, charge the instruction's cycles to a per-instruction counter, and push an event when control flow changes. No symbol lookup, no instruction decoding, no tree.
- **Consumer, on its own thread:** read events in batches, look up symbols, rebuild calls, returns and interrupts, update the call tree.
- **Afterwards, when recording stops:** derive inclusive cycles, the flat list and per-function self cycles.

**Producer.** After each step:

```
snap = registerSnapshot()
now = mTimingGlobalTime(timing)
delta = now - lastTime; lastTime = now

if counters.blockedCycles != lastBlocked or counters.hleSwiCount != lastSwi:
	split delta into DMA, halt and SWI parts (§6), push those events, subtract them from delta

instruction counters for lastPC:  cycles += delta; executions += 1   (§4: two loads and two adds)
segmentCycles += delta

if snap.pc != lastPC + lastWidth:          // control flow changed
	push BRANCH { from = lastPC, to = snap.pc, sp, lr, status, cycles = segmentCycles }
	segmentCycles = 0

lastPC = snap.pc; lastWidth = snap.width
```

Most instructions run in sequence, so a BRANCH event goes out for roughly one instruction in six to ten. The producer doesn't classify branches. Calls, returns, interrupt entries and exits are all just "the PC went somewhere unexpected", with the registers needed to tell them apart later.

**Events.** Fixed-size 32-byte records:

| Type | Sent when | Payload |
|---|---|---|
| `BRANCH` | the PC isn't the next sequential instruction | from, to, SP, LR, status |
| `DMA` | DMA ran during the step | channel, trigger timing, cycles |
| `HALT` | the CPU was halted during the step | cycles |
| `SWI` | the HLE BIOS handled an SWI during the step | SWI number, cycles |
| `FRAME` | a frame ended (`videoFrameEnded` core callback) | global time |
| `START` / `STOP` | recording started or stopped | global time, initial registers |

Every event also carries `cycles`: the code cycles since the previous event. The consumer charges them to the current node before applying the event. Together with the DMA, halt and SWI amounts, this accounts for every emulated cycle (objective 2).

**Queue.** A `RingFIFO` (`mgba-util/ring-fifo.h`), the lock-free single-producer/single-consumer ring that the video proxy already uses between threads. The default capacity is 4 MB, about 130,000 events, which absorbs bursts.

- **Backpressure, never dropping.** If the ring is full, the producer waits on a condition variable until the consumer frees space. A lost event would corrupt the call-stack reconstruction for the rest of the session, so exactness wins over speed here.
- **Consumer thread.** Created on start with `mgba-util/threading.h`, and woken when the ring passes a threshold rather than on every event. On stop, the producer pushes `STOP`, the consumer drains the ring and exits, and the module joins it.
- **Builds without threads** (`DISABLE_THREADING`): the producer calls the same consumer function itself whenever the ring is half full, and on stop. That's slower than a thread, but it's still batched and keeps the same results.
- **Live view.** The consumer applies each batch under a mutex. A frontend that wants a live snapshot takes the same mutex briefly and copies what it needs, such as the top functions by self cycles from the tree.

**Determinism.** The consumer's results depend only on the event sequence, never on thread timing. Unit tests check that a threaded run and a single-threaded drain of the same events give identical profiles.

### 3. Clock

Each step's `delta` comes from `mTimingGlobalTime`, not `cpu->cycles`. It covers everything the step did: the instruction itself, including wait states and prefetch, plus any events processed around it (DMA blocking, halt, IRQ entry). The global clock never resets, so no sample is ever negative or dropped, and the sum of all charged cycles equals the elapsed emulated time.

### 4. Symbols and per-instruction counters

**Per-instruction counters (producer).** The producer has to update a counter for every instruction without any lookup. Counters live in a two-level page table per memory region (BIOS, EWRAM, IWRAM, ROM):

- `region = pc >> 24`, then a directory of pages, each covering 4 KB of address space (2,048 halfword slots of `{uint64 cycles, uint32 executions}`, 24 KB).
- A page is allocated the first time code in it runs. That's rare, so it doesn't count against the "no allocation in the hot path" rule in practice.
- This works with or without an ELF, and for code copied into IWRAM/EWRAM at runtime, and only uses memory for code that actually ran.

**Symbols (consumer and afterwards).**

- Source: an ELF file. By default this is the loaded ROM if it is an ELF, otherwise `<rom basename>.elf` next to the ROM if present. Otherwise the user chooses one (Qt) or names it with `profile symbols FILE` (CLI debugger).
- Read `.symtab` with the existing `mgba-util/elf.h` helpers, keeping `STT_FUNC` entries with their `st_size`. Clear bit 0 of `st_value` (it marks Thumb functions).
- Fold linker veneers (`__*_from_thumb`, `__*_from_arm`, `__*_veneer`) into the function they jump to, so they don't appear as extra levels in the call tree.
- Keep the functions in an array sorted by address. The consumer looks up branch targets with a last-hit cache first, then binary search.
- Fallbacks when there is no symbol: `[BIOS]` for 0x00000000–0x00003FFF, otherwise `[unknown <region>]` for each memory region (ROM, IWRAM, EWRAM), so cycles are never dropped.
- `mCoreLoadELFSymbols` discards sizes and types, so the profiler parses the ELF itself (decision D3) and leaves the shared symbol table unchanged.

**C++ names.** The symbol table holds mangled names (`_ZN2bn10sprite_ptr12set_positionERKNS_9fixed_pointE`). These are kept as each function's identity everywhere: in the core, in `.mprof` files, and as the key when comparing profiles. They're unique even for overloads, so nothing is ambiguous. Demangling happens only when a name is displayed (decision D11):

- The profile API has a display hook, `mProfileSetDemangler(struct mProfile*, size_t (*demangle)(const char* mangled, char* out, size_t size))`. Without one, mangled names are shown.
- The Qt frontend vendors LLVM's Itanium demangler (`src/third-party/llvm-demangle`, C++, Apache-2.0 with LLVM exception, written to be copied into other projects) and registers it. Qt's debugger console runs the same CLI debugger code, so it shows demangled names too.
- The core stays C, so the SDL CLI debugger shows mangled names in v1 (see "After v1").
- A name that fails to demangle is shown mangled, never dropped.
- Template-heavy names get very long, so the views offer a short form that drops template arguments and parameter lists (`bn::sprite_ptr::set_position`), with the full name in a tooltip and in exports.
- Callgrind exports contain mangled names. KCachegrind demangles them itself.

**Two independent paths to self cycles.** A function's self cycles can be summed from the per-instruction counters over its address range, or from the call tree's nodes for that function. The two are computed independently, by the producer and the consumer, so they must agree exactly. Tests check this, and a debug build can assert it on stop.

### 5. Rebuilding calls, returns and interrupts (consumer)

The consumer keeps a shadow call stack. Each frame holds its tree node, the return address expected when it returns, and the SP at entry. Each `BRANCH` event is classified using the actual target and the registers after the branch:

- **Interrupt entry:** the target is the IRQ vector (`0x18`) and the status register shows IRQ mode. Push an interrupt marker that records the resume address (LR − 4) and the interrupted mode. The sample context becomes IRQ.
- **Interrupt exit:** the target is the resume address of the topmost interrupt marker, and the mode is back to the interrupted one. Pop everything above the marker, and the marker itself.
- **Return:** the target matches the expected return address of a frame on the shadow stack (usually the top one). Pop down to and including that frame. This covers `BX LR`, `POP {…, pc}`, `LDM … pc` and returns through a register. Frames whose entry SP is below the current SP, in the same register bank, are also popped. That handles exits that skip frames, such as `longjmp`.
- **Call:** after the branch, LR points just past the branch instruction (`from + width`, ignoring the Thumb bit). That's true for `BL` and for the ARMv4 `MOV LR, PC; BX Rn` sequence, and not for plain branches. Push a frame whose expected return address is that LR.
- **Anything else** is a plain branch. Within the current function it changes nothing. Into a different function, it's a tail call or a branch into another symbol: the target becomes a leaf child of the current frame's node and counts a call. Branching back into the frame's own function returns to the frame's node without counting a call.

Because the target is known rather than predicted, this doesn't need the instruction's opcode, and it works for hand-written assembly with unusual return sequences as long as control eventually reaches an expected return address.

**Contexts.** If there's an interrupt marker on the shadow stack, the sample belongs to the IRQ context: its path is the frames above the topmost marker, under a synthetic `[IRQ]` root. Otherwise it belongs to the main context, under the program root. A nested interrupt (handlers that re-enable IRQs, such as libtonc's nested dispatcher, which switches to System mode) pushes a second marker, so its handler appears under `[IRQ]` again rather than inside the first handler. The test ROM covers this case.

**The tree.** Nodes are keyed by `(parent, function)`. The current node only changes on events, so charging cycles is one add on the current node.

**Recursion folding.** Recursion is always folded (decision D9). When a function is entered while it's already on the current path, the consumer doesn't create a child node. It pushes a frame pointing at the nearest ancestor node for that function, so the recursive call's cycles are charged to that existing node. When the recursive call returns, the pop restores the previous node. The same rule applies to tail-call leaves.

Finding the ancestor doesn't search the stack. Each context keeps an array, indexed by function id, of the node where that function is currently active on the path, or none. A non-folded entry sets the slot and its pop clears it. A folded entry reads the slot and leaves it alone. Each frame records which of the two it was, so every call and return stays O(1) however deep the recursion goes.

- This covers direct recursion (`R_RenderBSPNode` calling itself) and mutual recursion (A → B → A → B …). Both would otherwise build chains hundreds of nodes deep.
- Folding A → B → A gives the tree A → B: the inner A's cycles go to the outer A node, and calls the inner A makes land under the outer A node.
- A function appears at most once on any path from a root. The flat list (§7) relies on this.
- Folding never crosses contexts: a function that runs in both main code and an IRQ handler has separate nodes under each root.

**Call counting.** Calls are counted on entry:

- A call pushes a frame and counts one call on the new node. If the entry was folded, it counts one *recursive call* on the reused ancestor node instead, so ordinary call counts aren't inflated by recursion.
- A tail call or branch into a different function counts one call on the leaf node, with the same folding rule.
- Returning to a function after its callee finishes is not a call.
- Each interrupt entry counts one call on the `[IRQ]` root, so its count is the number of interrupts taken.
- `[DMA n]` and `[SWI …]` count one call per DMA transfer or SWI.

### 6. DMA, halt and BIOS

Within one step's `delta` the producer needs to know how much was DMA (and of which kind), how much was halt, and whether the HLE BIOS ran an SWI. None of this is observable from outside the GBA core today, so it needs a small core addition (decision D2):

```c
struct GBAProfileCounters {        // in struct GBA, updated only when enabled
	bool enabled;
	uint64_t blockedCycles;        // DMA + halt total: the only value checked every step
	uint64_t dmaCycles[4][4];      // per channel, per GBADMATiming
	uint64_t haltCycles;
	uint32_t hleSwiCount;          // SWIs handled by the HLE BIOS
	uint8_t lastHleSwi;            // number of the most recent one
};
```

They're updated in `GBADMAService`, `GBAProcessEvents` and the HLE SWI handler, behind a single `enabled` check. The producer compares `blockedCycles` and `hleSwiCount` with their previous values each step, which is two loads. Only when one has changed does it read the details and push `DMA`, `HALT` or `SWI` events. The consumer charges them:

- **Immediate DMA** (`GBA_DMA_TIMING_NOW`): charged to the code that started it, as a child pseudo-function `[DMA n]` of the current node. It counts in the caller's inclusive time but stays visible as a separate item.
- **HBlank/VBlank/sound FIFO/video-capture DMA**: not caused by the interrupted code. Charged to top-level `[DMA HBlank]`, `[DMA VBlank]` or `[DMA FIFO]` roots.
- **Halt** (`Halt`, `IntrWait`, `VBlankIntrWait`, and the idle-loop optimisation): charged to a top-level `[Idle]` root, so "frame budget used" is `total - idle`.
- **HLE BIOS SWI**: the SWI runs as C code inside one step, so it shows as a child `[SWI 0x0B CpuSet]` (the name comes from the SWI number) of the current node.

With a real BIOS, an SWI is an exception into BIOS code at `0x08`. It appears as a branch event in Supervisor mode, and the BIOS code falls into `[BIOS]` unless BIOS symbols are supplied.

Idle-loop optimisation (the `idleOptimization` setting) skips busy-wait loops. That is correct emulated time, but it turns busy-waiting into halt, so a game that waits in a loop shows that time as `[Idle]` rather than as cycles in the waiting function. The profiler leaves the setting alone (decision D8). It records it in the profile's metadata, and the Qt view shows a notice when it isn't `ignore`.

### 7. Data model

The consumer side is an API that knows nothing about the emulator, so it can be unit-tested with synthetic event streams:

```c
struct mProfileEvent {
	uint8_t type;       // BRANCH, DMA, HALT, SWI, FRAME, START, STOP
	uint8_t arg;        // DMA channel and timing, or SWI number
	uint32_t cycles;    // code cycles since the previous event
	uint32_t from, to, sp, lr, status;
	uint32_t amount;    // DMA / halt / SWI cycles
};

void mProfileConsume(struct mProfile*, const struct mProfileEvent*, size_t count);
void mProfileFinish(struct mProfile*);  // derive inclusive cycles and the flat list
```

What a profile holds:

- **Functions**: a table of symbols and pseudo-functions (`[IRQ]`, `[Idle]`, `[DMA n]`, `[BIOS]`, `[unknown …]`, `[SWI …]`).
- **Per-instruction counters** (recorded by the producer): the page tables from §4.
- **Call tree** (recorded by the consumer): nodes `{function, parent, children (Table by function id), calls, recursiveCalls, selfCycles}`. Each distinct call path gets its own node, so the same function called from two places appears twice with separate counts and cycles. Recursion is folded (§5), so a function appears at most once on any path.
- **Recursion edges** (recorded by the consumer): a small table of `(caller function, callee function) → count` for folded entries. A folded entry doesn't create a tree edge, so this table keeps "who recursed into whom" for the flat list. It's only touched on recursive entries.
- **Flat function list** (derived by `mProfileFinish` when a recording stops or a file loads, never while recording):
  - *calls*, *recursive calls* and *self cycles*: the sums over all of the function's nodes;
  - *inclusive cycles*: the sum of the subtree cycles of the function's nodes. Folding guarantees none of those nodes is inside another, so nothing is counted twice;
  - *callers* and *callees*: for each caller→callee function pair, the calls and the callee's inclusive cycles summed over all tree edges between them, plus recursive calls from the recursion-edge table.

  Every figure is also shown as a percentage of total recorded cycles.
- **Frames** (recorded by the consumer from `FRAME` events): a growable array with each frame's busy cycles (total minus idle) and total cycles. That's enough for the average, worst and percentage-of-budget figures (objective 4); at 8 bytes per frame, an hour of recording is under 2 MB. Partial frames at the start and end of a recording are excluded from the figures.
- **Metadata**: ROM title and CRC, ELF path, frames recorded, total cycles, BIOS type, idle-loop setting, mGBA version.

Recording can start and stop at any time, or "reset and start" from boot, or "start at symbol" (for example `main`, as the prototype did). Starting mid-run, the consumer seeds the shadow stack with a single frame for the function at the current PC, since the calls that led there weren't seen.

### 8. File formats

**Native format (`.mprof`): save and open.** Callgrind can't hold the call tree. It stores costs per function and caller→callee edges merged across call sites, not call paths, so subtree timings would be lost on reload. The native format stores exactly what was recorded:

- Text, line-oriented and versioned, so it's easy to write and parse in C, and diffable. Unknown record types are skipped, so later versions can add data without breaking older readers.
- A header with the metadata (§7), then records for the function table, the call tree (one line per node: id, parent, function, calls, recursive calls, self cycles), recursion edges, per-instruction counters (only addresses that ran) and per-frame cycles.
- Only recorded data is stored. Inclusive cycles and the flat list are recomputed on load by `mProfileFinish`, so they can't disagree with a fresh recording.

Writer and reader live in the core library (`profile.c`), so any frontend can save and load.

**Callgrind: export only.** For KCachegrind/QCachegrind, which give call graphs and annotated disassembly when pointed at the ELF:

- Costs per instruction (`positions: instr`), self costs per function, and call edges with inclusive costs (`calls=` lines), built from the flat list's caller/callee data.
- Events: `Cycles`, `Executions`. DMA and idle are pseudo-functions, so they appear where they were charged.
- The main, IRQ, hardware-DMA and idle contexts each become a separate root (`[main]`, `[IRQ]`, `[DMA HBlank]`, `[Idle]` …).
- Frame figures and metadata go in `desc:` header lines.

The export loses the per-path tree, as any callgrind file must; that's what the native format is for.

### 9. Frontends

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

1. Start/stop/reset recording, symbol file selection, a live top-N function list refreshed on a timer from consumer snapshots (§2), and a notice when idle-loop optimisation is on.
2. **Call tree** view: expandable tree with calls, recursive calls, self cycles, subtree cycles and percentages at each node, sorted by subtree cycles.
3. **Flat list** view: sortable, searchable table of functions with calls, recursive calls, self and inclusive cycles and percentages. Names are demangled, with a short/full name toggle (§4). Selecting a function shows its callers and callees (with calls and cycles for each), and its per-instruction costs disassembled with mGBA's own decoder. Selecting a function in either view can jump to its nodes in the other.
4. A frame-budget summary: average and worst frame, as cycles and as a percentage of the budget.
5. Save and open `.mprof` files; export callgrind.
6. Compare two profiles: the flat lists side by side matched by function name, and the trees matched by call path, each with cycle and percentage deltas. Frame-budget figures are compared too, and the view notes that runs aren't reproducible.

The full tree, flat list and per-instruction views show the finished profile: after recording stops, or a file is opened. During recording only the live summary updates.

## Changes outside the new files

Kept deliberately small, each in its own commit:

1. `enum mDebuggerType`: add `DEBUGGER_PROFILER`. Update `supportsDebuggerType` for the GBA core.
2. `mDebuggerPlatform`: the `registerSnapshot` accessor, implemented for ARM (D1).
3. GBA core: `GBAProfileCounters` and the updates in `dma.c`, `gba.c` and the HLE BIOS (§6), compiled under `ENABLE_DEBUGGERS` (D2).
4. CLI debugger: the `profile` command set.

## Build changes

None beyond adding files to existing lists:

- `src/debugger/CMakeLists.txt`: add `profiler.c` (module and producer) and `profile.c` (consumer, data model, file formats) to `SOURCE_FILES`, and the tests to `TEST_FILES`.
- `src/platform/qt/CMakeLists.txt`: add the new controller and view sources and the `.ui` file to the existing lists.

There is no new option: the profiler is part of `ENABLE_DEBUGGERS` and needs no third-party code. ELF symbol loading uses the existing `USE_ELF` support. Without it, the profiler still works with the region-based fallbacks.

## Performance

Profiling switches emulation from `runLoop` to one `step` per instruction. That costs wall-clock speed but has no effect on emulated cycle counts. The measurement is deterministic, unlike a sampling profiler.

The profiler's own inline cost per instruction is a register snapshot (a few loads), one clock read, two counter comparisons, a page lookup with two adds, and one compare. On roughly one instruction in six to ten it also writes a 32-byte event into the ring. So the remaining cost is mostly the debugger's step-per-instruction loop itself.

The consumer needs to keep up with about one event per six to ten emulated instructions. Each event costs a cached symbol lookup and an O(1) tree operation, which is well within one host core. If it falls behind, backpressure slows emulation rather than losing data.

Target (objective 6): 50% of full speed or better for GBADoom on the author's machine. Milestone 1 measures it first: run a fixed number of frames in-process with and without the profiler attached, and time both. If the target is missed, the fixes in order of increasing invasiveness are:

1. Profile the producer and fix whatever is slow in it.
2. Tune the ring size and the consumer's wake-up threshold, if backpressure is what's slowing emulation.
3. As a last resort, a dedicated per-instruction hook in the ARM core that skips the debugger's step loop, revisiting D1.

## Testing

- **Consumer unit tests** (cmocka, `BUILD_SUITE`): feed synthetic event streams into `mProfileConsume`.
  - Calls by the LR rule (`BL`, `MOV LR, PC; BX`), returns by return-address match (`BX LR`, `POP {pc}`, `LDM`), SP-based unwinding, tail calls and branches into other functions.
  - Interrupt entry and exit, nested interrupts, and context separation.
  - Recursion folding: direct recursion several hundred levels deep stays one node; mutual recursion A → B → A folds to A → B; recursive calls are counted separately; returning out of folded calls restores the right node.
  - DMA, halt and SWI charging, frame accounting, and that the total equals the sum of all event cycles.
  - The derived flat list: calls, recursive calls, self and inclusive cycles, and caller/callee figures including recursion edges.
  - `.mprof` files round-trip exactly (save, load, compare equal), and the callgrind export's totals match the profile's.
- **Pipeline tests**: the same event stream through the threaded path with a deliberately tiny ring (forcing backpressure), and through a single-threaded drain, gives identical profiles. Also: per-function self cycles from the instruction counters equal those from the tree.
- **Test ROM**: a small devkitARM program with known cycle costs: fixed loops, nested calls, a timer IRQ (plain and nested), immediate and HBlank DMA, `VBlankIntrWait`, and a long-call veneer. Its source and a prebuilt binary are committed, so running the tests doesn't need devkitARM. A cmocka test loads it into a GBA core in-process, attaches the profiler, runs a fixed number of frames, and checks the profile against the expected buckets to within a tolerance (objectives 2 and 3).
- **Demangling**: a list of mangled names with their expected output, generated with `arm-none-eabi-c++filt` from a real C++ project (for example a Butano build), checked against the Qt demangler hook. Includes names that should fail and fall back to the mangled form, and checks of the short-name form. The test ROM also includes one C++ translation unit, so the end-to-end test sees a mangled symbol.
- **Regression against the prototype**: profile the same build of a real project (for example GBADoom) with both and compare the top functions. Differences should be explainable by the IRQ/DMA fixes.

## Milestones

1. **Flat profile.** Speed measurement first. Then the module, `registerSnapshot`, the producer with per-instruction counters, symbols (with veneer folding), per-function self cycles summed from the counters, `.mprof` save/load and callgrind export of that data, the CLI debugger `profile` command (`start`/`stop`/`reset`/`symbols`/`save`/`export`/`top`), and unit tests. No events or threads yet.
2. **Event pipeline, call tree and IRQ context.** Branch events, the ring and the consumer thread (and the no-threads fallback), call/return/interrupt reconstruction, the call tree with call counting and recursion folding, the derived flat list (inclusive cycles, callers, callees), `profile tree` and `profile func`, the consumer and pipeline tests, the test ROM and its in-process test, and checking that the CLI debugger still works while profiling.
3. **DMA, idle and frames.** GBA core counters, DMA/HALT/SWI/FRAME events, DMA and idle buckets, SWI pseudo-functions, frame-budget figures.
4. **Qt UI.** Controller and live view, call tree and flat list views with cross-navigation, callers/callees, per-instruction disassembly, frame-budget summary, save/open/export, compare, the vendored demangler with short/full names, and the demangling tests.

Each milestone is a self-contained step that leaves the branch building and working. Version 1 is complete when all the objectives above hold.

## After v1

Candidates, in no particular order:

- **Source-level view.** Selecting a function shows its source code with cycles and executions per line, next to the existing per-instruction view, plus a mixed view with each source line followed by its instructions (as `objdump -S` does).
  - No new recording is needed: per-line figures are the sums of the per-instruction counters for every address the line table maps to that line. It works on saved `.mprof` files too, as long as the ELF is available.
  - Needs a small C reader for the DWARF line table (`.debug_line`, plus `.debug_line_str` for DWARF 5) in the core library: only the line-number program, not the rest of DWARF. It must handle DWARF versions 2 to 5. Recent GCC versions, including current devkitARM, emit DWARF 5 by default, which libelfin (used by the prototype) doesn't read; that's a likely reason the prototype couldn't get line information.
  - Instructions from one source line are often scattered, and inlined code maps to the inlined function's file and lines, so a function's source view groups lines by file and includes headers where code was inlined.
  - Source files are read from the paths recorded in DWARF (compilation directory plus file name). A user-set path remapping covers projects built on another machine or in a container. A missing file still shows per-line figures, keyed by file and line number.
- **Demangling in the SDL CLI debugger:** a C demangler covering the common subset of the Itanium scheme, falling back to the mangled name, so C-only frontends can register it through the same hook.
- **Raw trace recording:** write the event stream to a file and analyse it later. It's roughly 20 MB per emulated second, so it's for short captures, but it allows re-analysing one run with different settings, for example with recursion unfolded, without recording again.
- Headless profiling through `mgba-perf` (`--profile FILE`) for scripts and CI; Lua bindings.
- Input recording for reproducible comparisons.
- Better support for ROMs without symbols.
- Per-frame drill-down (which functions made the worst frame expensive).
- An option to record recursion unfolded.
- A speedscope exporter for flame graphs.
- Game Boy support.

## Decisions

Where there was a choice, the option that touches the least shared code wins, because that is what keeps the fork adoptable.

- **D1. Hook point.** A debugger module using `needsCallback`, not a new per-instruction hook in the CPU. It forces one `step` per instruction while profiling, which costs wall-clock speed only. A small `registerSnapshot` accessor on `mDebuggerPlatform` is added, because the existing ways to read registers either decode the instruction or look registers up by name.
- **D2. DMA, halt and SWI counters.** Small counters in `struct GBA`, updated behind one `enabled` check and compiled only with `ENABLE_DEBUGGERS`. There's no way to observe this from outside the core, and this is the smallest change that makes it possible.
- **D3. Symbols.** The profiler reads the ELF symbol table itself through `mgba-util/elf.h`. `mDebuggerSymbols` stays unchanged.
- **D4. File formats.** A native text format (`.mprof`) for save and open, because callgrind can't store the call tree. Callgrind as an export for KCachegrind/QCachegrind.
- **D5. Call reconstruction.** The profiler rebuilds calls, returns and interrupts itself on the consumer, from actual branch targets and registers. It doesn't use the ARM debugger's stack trace, which runs inline, decodes every instruction and predicts targets. The cost is owning this logic; the benefits are less inline work, more reliable results, and no interference with the CLI debugger.
- **D6. UI placement.** A Tools → Profiler window, in the same style as the memory access logger.
- **D7. Frontends for v1.** Qt and the CLI debugger. Headless `mgba-perf` profiling and Lua move to "After v1".
- **D8. Idle-loop optimisation.** Left as the user configured it; recorded in the profile and flagged in the UI, not overridden.
- **D9. Recursion folding.** Always on, applied while recording, for both direct and mutual recursion: a recursive entry reuses the nearest ancestor node for that function. This keeps deep recursion readable, keeps the tree small, and makes inclusive figures simple. The cost is that the unfolded shape (which recursion depth spent what) isn't kept; an option to record it, and raw trace recording that would allow re-analysis, are listed under "After v1".
- **D10. Event pipeline.** Inline work is limited to per-instruction counters and fixed-size events on control-flow changes, DMA, halt, SWIs and frame ends. Events go through a lock-free ring to a consumer thread, with backpressure rather than dropping, and the same consumer code runs on the emulation thread in builds without threads.
- **D11. Demangling.** Mangled names are the identity everywhere; demangling is display-only, through a hook. Qt supplies a vendored copy of LLVM's Itanium demangler, because it's complete and permissively licensed, and keeping it in the Qt frontend keeps C++ out of the core library. GCC's own demangler (libiberty) isn't used because of its GPL licensing, and `__cxa_demangle` isn't available in MSVC builds.

## Keeping the fork adoptable

The fork is developed without coordination with upstream. These rules keep it possible for upstream to merge it unchanged:

- **Clean history on current upstream.** `profiler` is periodically rebased onto upstream `master` rather than merged, so it stays a clean series of commits on top. It is a personal branch, so force-pushing it is fine.
- **Separable commits.** Each change to shared code (the items in "Changes outside the new files") is its own commit with its own rationale, so it can be taken or dropped independently. Profiler code lives in new files.
- **Upstream conventions.** Coding style and naming from `CONTRIBUTING.md`, `.clang-format`, component-prefixed commit messages (`Debugger:`, `GBA:`, `Qt:`), and MPL 2.0 headers on new files. C in the core, and C++ only in the Qt frontend.
- **Zero cost when unused.** No new build options, no new dependencies, nothing on the hot path when no profiler is attached, and everything under the existing `ENABLE_DEBUGGERS`.
- **Every frontend builds.** Changes must not break the SDL, libretro, headless or console builds even where the profiler isn't exposed. Check at least the Qt, SDL, libretro and `mgba-perf` targets, and a build with `ENABLE_DEBUGGERS=OFF`.
- **Fork-only material stays apart.** This document and anything only the fork needs live in separate commits that would be easy to leave out of a merge.
