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
| `80000592`â€“`8000059C` cdvdfsv | Init, S/N commands, sector reads, SearchFile, disc ready |
| `10000000` HIS sound driver | Version `0x10802`. The driver's own disc reader (256 open, 257 read, 258 status = 5 done, 265 size). No audio yet |
| `80000100`/`101` padman | XInput and keyboard, written to the pad DMA area each vblank |
| `80000400` mcserv | Answers init; every card operation reports "no card" for now |

`disc.cpp` reads ISO9660 straight from the user's image (`--iso`).

### Hardware modelled for the boot

- Timers 0â€“2 count in real time and raise INTC 9â€“11 on compare/overflow. The game's clock is timer 0 at bus/16 plus an overflow accumulator.
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

## First level (2026-11-05)

New Game now loads the first level, and its in-engine cutscenes render. Three fixes
got it there:

- **Stable HIS file ids.** The game keeps its own PAK size table at `0x48d4a0`,
  indexed by HIS file id and filled from fno 265. It reopens the same PAKs over and
  over without closing them. When each open handed out a fresh id, the ids ran off
  the end of that table. The game then read a size of 0 and clamped every later
  read to a negative count, so it sat on "now loading" forever. `open` now returns
  the same id for the same file.
- **VU1 branches after flag reads.** A branch normally sees the *old* value of a VI
  register that the previous pair wrote. That rule does not apply to flag reads
  (`FMAND`, `FCAND`, `FSAND`, ...). RenderWare's clipper (`2aa8`) tests a flag and
  branches on it in the very next pair. With the old value, every inside/outside
  decision came one edge late. Clipped polygons then grew until they overflowed the
  output buffer into qword 1023, and the program looped forever.
- **VU1 clip flags.** Only CLIP and FCSET change the clip flag. FCSET also cancels
  any CLIP results still in the 4-cycle pipeline. Before this fix, every queued
  FMAC flag entry carried a copy of the clip flag.

Also new:

- SIF replies reach the EE after a short delay (`--sif-latency <usec>`, default 500).
  Previously they arrived at the next syscall.
- A simple VU1 register stall model.
- A cap on microprogram length. If a microprogram runs too long, the runtime
  reports it and stops it.
- `TWONK_VU_TRACE=<pc>`: when the program starting at `<pc>` runs away, replay it
  from a snapshot taken at its start. The replay writes every pair to
  `vu1_replay.txt` and saves the program and its starting data to `vu1_replay.bin`.
- Scripted input takes a hold length: `--input 12000:up:200` holds up for 200 frames
  (default 15).

## Speed, FPS counter and sound (2026-11-05)

### Threads

| Thread | Work |
|---|---|
| game | Recompiled EE code, kernel, IOP services, and the HIS.IRX interpreter |
| `gfx` (`hw.cpp`) | DMA channels 1 and 2: VIF1, VU1 microprograms, PATH3. D1/D2 registers live in `g_gfx_regs`. EE reads of D_STAT, GIF_STAT or D1/D2 registers sync with this thread |
| `gs` (`gs.cpp`) | GS register writes, image transfers and presents, queued in batches. Rasterisation runs on a pool of `hardware_concurrency - 2` threads |
| `audio` (`spu2.cpp`) | SPU2 mixing and XAudio2 output. The audio clock drives voices and block transfers |

When the EE spins on a register (`poll_point` sees identical GPRs twice), it
waits for the gfx worker to make progress instead of burning the core.

VU1 caches each decoded upper instruction and runs it with SSE. Results are
bit-exact, including the VU float clamp and the MAC/status flags.

### Rasteriser

- Each triangle row is clipped to its covered span before the per-pixel test.
- Sprites precompute u for each column.
- A CLUT load with draws still queued snapshots the palette, so those draws keep it.
- An image transfer flushes queued draws only if they overlap it.

### FPS counter

The window shows game FPS (DISPFB1 flips per second) and speed (real vblanks
as a share of 59.94). Green means 97% or more, yellow 75% or more, red below
that. F1 toggles it. The window title shows the same numbers.

### Sound

HIS.IRX, the game's sound driver, now runs as shipped. `iop_irx.cpp` provides:

- an R3000 interpreter;
- the IRX loader, with relocation and symbols;
- a small IOP kernel: threads, event flags, semaphores, delays, and an
  interrupt-callback queue;
- native imports for SIF, CDVD (reads from the ISO) and libsd.

Each HIS RPC copies the request to `rpcInData` and runs
`ProcessCommand__FUiPvi(fno, data, size)`. The module is loaded on the first
call, once the disc is open. Its threads also run every vblank.

`spu2.cpp` emulates the SPU2:

- 2 cores of 24 voices: ADPCM, loop flags, ENDX, ADSR, pitch with linear
  interpolation, and volumes;
- block-transfer PCM (movie audio), with the transfer handler raised at each
  half buffer.

`sceSdInit` leaves every voice on the dry mix: the game never writes VMIXL or
VMIXR. Not modelled: reverb, noise, pitch modulation, volume sweeps.

Verified from WAV captures: intro movie audio, title music (a streamed ADPCM
voice), demo music and effects, and level music. There is no clipping.

### Environment variables

| Variable | Effect |
|---|---|
| `TWONK_GS_SYNC=1` | Run the GS on the game thread |
| `TWONK_GFX_SYNC=1` | Run VIF1/VU1/PATH3 inline (no gfx worker) |
| `TWONK_PROFILE_THREAD=gfx\|gs` | Make `--profile` sample that thread |
| `TWONK_VU_CHECK=1` | Check the VU1 fast path against the reference interpreter |
| `TWONK_HIS_NATIVE=1` | Use the old silent HIS placeholder instead of HIS.IRX |
| `TWONK_WAV=<file.wav>` | Record the audio output |

With `--log iop`, the log also shows:

- SPU2 state every 5 s;
- changes in HIS voice status;
- the IRX's CD reads;
- the driver's own printf output.

### Speed so far

| Scene | Before | Now |
|---|---|---|
| Real-time vblanks | 22/s | 60/s |
| Title screen | 16 fps | 24–25 fps |
| Attract demo | | 60 fps |

In-level cutscenes run at 10–20 fps against a target of 30. Since sound runs
in real time, a cutscene's music can end before its picture does.

## GPU renderer (`gs_hw.cpp`, 2026-11-05)

The GS is drawn on Direct3D 11 at N times the PS2 resolution. This is the
default renderer; `--renderer sw` (or `TWONK_RENDERER=sw`) selects the old
software rasteriser. `--scale N` (or `TWONK_SCALE=N`, where N is 1 to 8) sets
the starting scale, which defaults to 2.

**F4** opens the resolution menu, which offers 1x, 2x and 3x:

- Up/Down selects, Enter applies, and 1/2/3 apply directly.
- Esc or F4 closes the menu.
- While the menu is open, the keyboard does not reach the game's pad.

A change is applied at the next vblank: every GPU-drawn surface is written back
to GS memory and dropped, and surfaces are then recreated at the new scale as
they are used.

- gs.cpp still decodes registers and builds primitives, then hands them to
  `hw::draw`. GS memory (`g_gsmem`) remains authoritative for CPU writes.
- Frame and Z buffers are GPU surfaces at scale N, keyed by base pointer.
  Colour is RGBA8, with alpha stored as a/128. Depth is D32 and holds z/zmax.
- Coherence between GS memory and the GPU surfaces:
  - `hw::sync(lo, hi)` downloads GPU-written surfaces before the CPU reads or
    partly overwrites them (transfers, CLUT loads);
  - `hw::written(lo, hi)` marks surfaces for refresh and bumps 8 KB page
    generations, which invalidate cached textures.

  Pending refreshes are applied only when needed, with row intervals merged
  per surface.
- Textures are decoded on the CPU (CLUT and TEXA applied) and cached. A
  texture that is exactly a surface is sampled straight from the GPU at full
  resolution. If it is the current target, a feedback copy is sampled instead.
- One uber pixel shader covers:
  - TFX, TCC and fog;
  - alpha test, with the fail modes drawn as a second, inverted pass;
  - wrap modes, including region repeat;
  - bilinear filtering.

  Blending uses D3D blend states with dual-source alpha. The Cs*(1+C) and
  dest-colour cases are handled in the shader.
- Presentation goes through a DXGI swap chain on the game window, with the FPS
  overlay drawn through GDI. Screenshots are taken at full resolution.
- `TWONK_HW_TRACE=<present#>` traces the draws of two frames. `--log gpu`
  prints renderer statistics every 600 presents and the first download reasons.
- An unhandled exception now prints a symbolised stack trace (host_main.cpp).

Verified at 1x and 3x against the software renderer on the title, movies, the
attract demo and the first level. The title runs at 30 fps (24 with sw), and
gameplay causes no downloads.

### Where the level time goes

Level cutscenes run at 10-12 fps under both renderers, so the GS is not the
limit there.

- The gfx thread is about 84% busy in the VU1 interpreter (`run` 33%,
  `upper_fast` 26%, `lower` 5%).
- The game thread's hot function `f_00267d10` is the frame-end wait: it
  spins until the previous frame's DMA/VU work drains, then flips buffers.

VU1 speed (recompiling the microprograms) is therefore the next big speed-up.

## Next

- Movies drop to about 19 pictures/s in clips with audio. Profile this.

- Graphics: a GS/RenderWare bridge to draw the GIF/VIF1 output.
- Speed in levels (10-20 fps): profile the GS and VU1 load there.
- Settings menu with upscaling: needs a hardware (GPU) GS renderer; the software
  rasteriser cannot upscale at a usable speed.
- Sound: reverb (ESA/EEA, EVOL), noise, pitch modulation.

## VU1 fast path (2026-11-06)

`vu1.cpp` now decodes each microcode pair once and caches it by address. MPG
uploads that change micro memory invalidate the changed pairs and the 4 before them.
The cycle, stall, Q and flag-latency model is unchanged. Three things make it faster:

- Flag results and Q land lazily: `sync_flags()`/`qv()` fold them in when something reads them.
- A MAC/status result is not queued when nothing can observe it: the next pair writes
  flags, and the 4 pairs after it run in straight line with no FM*/FS* reads.
- FMAC ops are template-specialised, and the stall check uses a precomputed read list.
  The flag queue avoids store-forwarding stalls.

Speed in isolation is about 13 ns per pair, from about 37. Level cutscenes went from
10-12 fps to 20-30 fps, and the menu flyover from 22 to 59 fps.

| Tool | Use |
|---|---|
| `TWONK_VU_SLOW=1` | Run the original reference interpreter |
| `TWONK_VU_VERIFY=1` | Run every program on both paths and compare the full end state |
| `TWONK_VU_SNAP=<pc>:<n>[,...]` | Save `vu1_snap_<pc>_<n>.bin` at the n-th start of a program |
| `twonk --vu-bench <snap> [runs [profile-s]]` | Replay a snapshot: ns/pair and an end-state checksum |
| `TWONK_VU_HIST=1` | Write per-pc pair counts to `vu1_hist.txt` (with `--log gpu`) |
| `TWONK_PROFILE_LINES=1`, `TWONK_PROFILE_ADDRS=1` | `--profile` by source line or by instruction address |

## Game clock and cutscene skip (2026-11-06)

- The game's clock is timer 0 in compare mode (COMP 0x8000, ZRET) plus a count
  its INTC 9 handler keeps; the handler adds 0x8000 and writes COUNT = 0.
  Interrupts are delivered at syscall/poll points, often milliseconds late, so a
  COUNT write from inside the timer's own handler restarts the count from the
  event time, not from "now" (`g_timer_in_handler`, hw.cpp). Before this, game
  time ran at 35-75% of real time. `TWONK_TIME_DEBUG=1` prints game vs real time.
- START during a cutscene fast-forwards it (services.cpp, `cutscene_update`):
  the cutscene player's update(dt), called from f_00142870 with return address
  0x001429e4, runs 40 extra 50 ms steps per frame until its state (+84) leaves 5.
- `TWONK_LIVE=<dir>`: live control for testing. Write `input <frame>:<button>[:<hold>],...`
  (frames relative to now; buttons include lup/ldown/lleft/lright for the left
  stick) or `shot <name>` to <dir>/cmd.txt; <dir>/frame.txt has the current frame.

## GS read-backs in levels (2026-11-10)

Levels dropped to 10-25 fps with the GS thread at 100%, almost all of it writing
GPU surfaces back to GS memory pixel by pixel (`write_pixel` in `download`).
Two causes, both in `gs_hw.cpp`:

- **A 1x1 texture at the frame buffer's base** (`TEX0` bp 0, bw 0) on many draws:
  the width did not match the frame buffer's (bw 10), so every such batch
  downloaded the whole 640x448 frame buffer, 400-550 times a second. Within one
  row of pages the layout does not depend on the buffer width, so a texture that
  fits there is now sampled from the GPU surface directly. When the draw also
  renders into that surface, only the texels it can reach are copied for feedback.
- **A Z base shared by two widths**: a 64x32 render target (bw 1) and the scene
  (bw 10) both use Z at 0x2300. Each switch wrote back and dropped the whole
  640x448 depth buffer. Now `evict` hands over only the page rows that overlap.
  The other surface keeps the rest and marks those rows `stale`, and `validate`
  reloads them from GS memory before the surface is next used.

Read-backs per second in the first level went from about 500 full-screen ones to
about 30 of 64x32 or 640x32 rows. GS thread load fell from ~100% to 20-45%. The
remaining slow spots (20-35 fps in the space-station area) are VU1-bound: the gfx
thread is at 100% running about 50 M pairs/s.

## Texture packs and 4x (2026-11-10)

`texpack.cpp` dumps and replaces textures for the GPU renderer.

- `TWONK_TEX_DUMP=<dir>` writes each texture decoded from GS memory to
  `<dir>/<hash>.png`, once. The hash covers the decoded RGBA, after the CLUT.
  `<dir>/index.txt` records size, PSM, address and the frame it was first seen.
- Replacements come from `textures/` (or `TWONK_TEX_DIR`), searched recursively.
  A file whose name starts with the 16-digit hash, at a whole multiple of the
  original size, replaces that texture. PNGs load on a worker thread: the original
  shows until the replacement is ready. At most 1 GB of replacements stays on the GPU.
- PNG alpha is GS alpha doubled (0x80 = 255).
- Textures in memory a render target holds are neither dumped nor replaced.
- `tools/texpack_upscale.py` turns a dump into a pack: wrap-padded, upscaled
  (Real-ESRGAN from `tools/realesrgan`, or Lanczos), and cropped. Alpha is upscaled
  separately. It skips textures already in the pack.
- F4 now offers 4x (2560x1792).
- Not covered: movies, including the title's town backdrop, which goes straight to
  the frame buffer. Font pages that the game builds from the glyphs of the current
  text match only text seen during a dump.
- Surfaces unused for 60 presents are written back and dropped. The boot-time clear
  of Z memory (0x2000-0x4000) used to linger: every upload refreshed it, and every
  texture there looked like a render target.
