# ps2runtime: running the recompiled code

`twonk.exe` loads `SLUS_208.87`'s segments into a 32 MB guest RAM buffer and calls
the recompiled entry point `f_00100008`. Everything the game expects from the
console is provided natively.

```sh
twonkrecomp gen extracted/disc/SLUS_208.87 config/regions.txt analysis/functions.tsv build/generated config/hle.txt
cmake -S . -B build/msvc -DTWONK_GENERATED_DIR=<abs path>/build/generated
cmake --build build/msvc --config Release --target twonk --parallel 8
build/msvc/Release/twonk.exe extracted/disc/SLUS_208.87 --log kernel,boot,hle   # also: mem, cpu, all
```

## Components

| File | What it does |
|---|---|
| `runtime/include/ps2/recomp.hpp` | Everything generated code uses: memory map, unaligned access, mult/div, EE FPU clamping, and the hooks. |
| `runtime/include/ps2/exec_impl.hpp` | Instruction semantics for the operations the recompiler does not inline. Covers every MMI and VU0 macro instruction the game uses, plus COP0 and CFC2/CTC2. |
| `runtime/src/kernel.cpp` | EE kernel HLE. Details below. |
| `runtime/src/hw.cpp` | Hardware registers. Details below. |
| `runtime/src/vu.cpp` | VU0 control registers (I, Q, R, VPU-STAT, CMSAR). |
| `runtime/src/cop0.cpp` | COP0 registers, including Count at 294.912 MHz. |
| `runtime/src/hle_*.cpp` + `config/hle.txt` | Native replacements for guest functions. |
| `runtime/src/services.cpp` | Indirect call dispatch, fatal errors, and the trace ring buffer. |

### Kernel (`kernel.cpp`)

- **Syscalls:** the boot calls, semaphores, interrupt handler tables and alarms.
- **Threads:** each guest thread is a Win32 fiber with its own register context.
  Scheduling follows the EE kernel: strict priority, round-robin within a
  priority level, and a switch only when a thread blocks or yields.
- **Interrupts:** VBlank interrupts and alarms are delivered at syscall
  boundaries. Their handlers run on an interrupt stack in kernel RAM.

### Hardware registers (`hw.cpp`)

- Timers count in real time.
- The GS CSR reports vsync.
- INTC, DMAC and SIF registers hold sane values.
- Unknown registers are stored and logged the first time they are read.
- A watchdog stops on a stuck polling loop.

## HLE replacements (`config/hle.txt`)

An address listed there is forced to be a function entry, so it is split out of
any candidate that contains it. The generator then emits that function as a call
to `hle::<name>`.

Current replacements are three libkernl routines that patch the BIOS kernel:

- `0x117948`
- `0x117070`
- `0x118010`

There is no BIOS under HLE, so they are no-ops, except `0x118010`, which is a
plain store.

## Boot progress (2026-11-04)

The run currently gets through these steps:

1. crt0: clears registers and bss, then calls SetupThread and SetupHeap.
2. `_InitSys`:
   - semaphores;
   - the alarm and timer setup on Timer 3, which installs the INTC 11 handler;
   - the top thread (thread 2);
   - OSD config.
3. C++ static constructors: 15 functions in the code island at `0x47b680`.
4. `main` (`0x1480f0`) and its first initialization.
5. **Stop:** `SifSetDChain`, where `sceSifInit` starts talking to the IOP.

### Recompiler fixes found while booting

- **Tail jumps.** A `j` that leaves its function now marks its target as an
  entry, as a `jal` target does.
- **Code inside `.data`.** This covers the static constructors, and adds two
  new region kinds:
  - `ptrs`: a table where every word is a function entry, used for `.ctors`;
  - a prologue check for code pointers stored in data.
- **Stale output.** The generator now clears its old `recomp_*.cpp` files.
- **Result:** 0 external branches and 0 residual cross-branches across 6,926
  functions.

## Next: IOP / SIF

Everything past this point goes through SIF RPC to the IOP: CD/DVD reads, pad,
memory card, and the sound driver. The plan is to replace Sony's EE-side SIF
library at the API level. The functions are near the `SceSifrpc*` strings at
about `0x110000–0x112000`:

- `sceSifInitRpc`
- `sceSifBindRpc`
- `sceSifCallRpc`
- `sceSifLoadModule`
- `sceCdInit`, `sceCdRead`
- `scePadInit`, `scePadRead`
- `sceMcOpen`

Each one is backed by a native service:

- the extracted disc files;
- SDL input;
- save files;
- native audio.
