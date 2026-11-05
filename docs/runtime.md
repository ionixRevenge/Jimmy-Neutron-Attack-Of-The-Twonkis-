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

## IOP / SIF (`iop*.cpp`, `disc.cpp`)

The EE side runs Sony's sifcmd/sifrpc libraries unmodified. The IOP is emulated
at the SIF syscall boundary (`SifSetDma`, `SifSetDChain`, `SifSetReg`/`GetReg`):

- packets the EE sends to the IOP command buffer are decoded natively;
- replies are queued and delivered as DMAC channel 5 interrupts.

On top of that sit native RPC servers:

| Server | Service |
|---|---|
| `80000003` / `80000006` | IOP heap; module loading (modules are "loaded" by name, all native) |
| `80000001` fileio 2.2 | Reports version `3000`, which the EE library checks. Files are served from the ISO; writes to fd 1/2 go to `[tty]` |
| `80000592`–`8000059C` cdvdfsv | Init, S/N commands, sector reads, SearchFile, disc ready |
| `10000000` HIS sound driver | Version `0x10802`. The driver's own disc reader (256 open, 257 read, 258 status = 5 done, 265 size). No audio yet |
| `80000100`/`101` padman | XInput and keyboard, written to the pad DMA area each vblank |
| `80000400` mcserv | Answers init; every card operation reports "no card" for now |

`disc.cpp` reads ISO9660 straight from the user's image (`--iso`).

### Hardware modelled for the boot

- Timers 0–2 count in real time and raise INTC 9–11 on compare/overflow. The game's clock is timer 0 at bus/16 plus an overflow accumulator.
- DMA completes instantly. PATH3 masking is modelled: a VIF1 `MSKPATH3` written to the FIFO sets GIF_STAT.M3P, a GIF transfer started while masked waits with FQC != 0, and the next VIF1 DMA releases it.

## Boot progress (2026-11-04, continued)

The game now does all of this:

- resets the IOP and loads its modules;
- initialises RenderWare and its GameMonkey scripts;
- opens and reads CONFIG.TXT, BOOT.PAK, MENU.PAK and FMV.PAK (fileio and the HIS reader);
- opens the pad and probes the memory card;
- runs its frame loop, with one HIS 513 call per frame, without crashing for over 60 s.

Nothing is drawn yet: GIF/VIF packets are consumed but not rendered.

### Recompiler fixes found in this stretch

Register-built pointers:
- `lui`/`addiu` pairs up to 24 instructions apart are now matched;
- the scan follows a branch when the `lui` sits in its delay slot;
- a `jal`'s target bits no longer read as a register write.

These found the hash callback `0x29dd10` and the RenderWare plugin callbacks `0x176270` and `0x247420`.

## Next

- Graphics: a GS/RenderWare bridge to draw the GIF/VIF1 output.
- Sound: reverse the HIS driver protocol (functions 0, 512, 513, 514, ...).
- Memory card: a virtual card in the user's save folder.
