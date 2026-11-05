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

## Graphics, movies and intro (2026-11-04)

| File | What it does |
|---|---|
| `vif.cpp`, `vu1.cpp` | VIF1 unpack/MPG/MSCAL and a VU1 interpreter for RenderWare's microcode |
| `gif.cpp` | GIF PATH1/2/3 tag parsing into GS register writes |
| `gs.cpp`, `gs_mem.hpp` | A software GS: swizzled local memory (table-driven address swizzle), CLUTs, sprites, triangles and lines. Primitives are queued and rasterised on a thread pool, where each thread owns interleaved 8-row bands |
| `display.cpp` | Presents the GS display circuit in a window; `--shot path#.png:f1,f2` saves frames |
| `ipu.cpp` | The IPU: MPEG-2 VLC/IDCT/dequantisation, BDEC/IDEC/VDEC/FDEC/CSC, with DMA channels 3/4 pulled lazily. Commands run on a fiber that yields when the input FIFO starves |
| `profiler.cpp` | `--profile [delay:]seconds`: samples the game thread and prints the hottest functions from the PDB |

The intro now plays to the title screen, and attract mode then starts gameplay:
licence screen, Nick splash, THQ, THQ Australia and RenderWare, then the title.

Two problems surfaced in the movie player, and both were timing or end-of-stream issues rather than decoder bugs:

- **Audio drain rate.** The demuxer stops filling the video ring when the
  audio ring is full. It then hands the IPU a padded partial quadword, and the
  decoder hits garbage. HIS voices set up with fno 770 carry 16-bit stereo PCM
  (the movies' `SShd` audio), so they drain at 4 bytes per sample, not at the
  ADPCM rate.
- **Exact-quadword streams.** After `sequence_end_code`, libmpeg issues an FDEC
  that needs 32 more bits. Its wait loop then kicks empty toIPU transfers until
  the command finishes. A starved IPU given an empty transfer reads zeros.

- **PATH3 pacing.** RenderWare uploads textures through PATH3. Before each
  texture's draws, it opens a brief MSKPATH3 off/on window in the VIF1 stream,
  and the hardware lets one GIF packet (up to EOP) through per window. VIF1 runs
  synchronously here, so while it is busy PATH3 pauses at every EOP. Without this
  pause, all of a frame's uploads land before its draws, and textures that share
  VRAM overwrite each other.

The title screen and the attract-mode gameplay demo now render correctly.

## Memory card (`iop_mc.cpp`)

The game uses the old libmc (the v1 RPC numbering) on mcserv `0x80000400`.
Slot 1 is a host directory, `--memcard <dir>`, defaulting to `memcard/`; it
holds `slot1/`. Port 1 reports no card. Functions supported:

| fno | Function | fno | Function |
|---|---|---|---|
| 01 | GetInfo | 0C | ChDir |
| 02 | Open | 0D | GetDir |
| 03 | Close | 0E | SetInfo |
| 04 | Seek | 0F | Delete |
| 05 | Read | 10/11 | Format/Unformat |
| 06 | Write | 12 | GetEntSpace |
| 0A | Flush | FE/70 | Init |

GetInfo writes a 0xC0-byte parameter block: type at 0, free clusters at 4,
format at 0x90. The first GetInfo for a port returns -1 (a new card), as real
hardware does.

The card's free space is computed from the files in the folder: 1-KB clusters per
file, plus one cluster per two directory entries. The game sizes its `pad.dat`
reservation file from the change in free space across a save, so a constant value
makes the save fail. GetInfo carries the port in word 1; word 0 holds stale data.

New Game now saves (`BASLUS-20887Save/` holds `icon.sys`, `static.ico`,
`JN04-0.dat` and `pad.dat`) and goes on to load the first level.

## Sound status (HIS fno 513)

Every frame the game polls fno 513 for three 48-voice masks: playing, keyed on and
ended (the SPU2's ENDX). A sound is removed from its list only after its voice
reports ENDX. Leaving the save screen stops all sounds and waits for that list to
empty. Until the SPU2 is emulated, one-shot voices are reported as started and
already finished, and virtual stream voices as playing.

## Next

- Movies drop to about 19 pictures/s in clips with audio. Profile this.

- Graphics: a GS/RenderWare bridge to draw the GIF/VIF1 output.
- Sound: reverse the HIS driver protocol (functions 0, 512, 513, 514, ...).
