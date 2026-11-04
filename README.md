# Jimmy Neutron: Attack of the Twonkies — Decomp / PC Port

Decompilation of *Nickelodeon Jimmy Neutron: Boy Genius — Attack of the Twonkies*
(PS2, USA) aimed at a native Windows port.

**No game data is included or ever committed.** You need your own copy of the disc.

## Target

| | |
|---|---|
| Disc | PS2 USA, `SLUS-208.87`, disc version 1.01, NTSC |
| Executable | `SLUS_208.87`, SHA-1 `3dc783935b4a853ad42f97a6c1697aae128ffca3` |
| Code | One `main` segment at `0x00100000` (3.6 MB), entry point `0x00100008`, **no symbols (stripped)** |
| Compiler | Metrowerks CodeWarrior, `MW MIPS C Compiler (2.4.1.01)` |
| Engine | RenderWare 3.6 (Sky2/PS2 backend). Plugins: MatFX, PTank, HAnim, UVAnim, Rt2d, PITexDict |
| Game code | C++ (`PS2All.cpp`, `slXml.cpp`, `THQFont.cpp`, ...) |
| IOP modules | `LIBSD`, `PADMAN`, `MCMAN`/`MCSERV`, `SIO2MAN`, `HIS`, `IOPRP300.IMG` |

## Data formats

- `DATA/*.PAK`: archives with the `kcap` magic; the format is documented in `tools/extract_pak.py`.
  There are 23 archives, about 11k files.
- Inside the archives:
  - `.rws`: RenderWare streams (models, textures, worlds).
  - `.anm`: animations (RW HAnim).
  - `.gm`, `.fx`, `.csr`, `.str`, `.xml`, `.rad`, `.fon`, `.sounds`, `.fmv`: game-specific formats, not yet decoded.

## Setup

```sh
python tools/extract_iso.py "Game iso/<game>.iso" extracted/disc
python tools/extract_pak.py extracted/disc/DATA extracted/data
```

## Roadmap

1. **Analysis.** Load the ELF into Ghidra with a MIPS R5900 (Emotion Engine) loader.
   Identify RenderWare 3.6 functions using the RW assert/ID strings and signatures.
   This isolates the engine code from the game code.
2. **Disassembly split.** Use `splat` to split the ELF into per-function asm and data,
   and maintain a symbol map.
3. **Decompile game code to C++.** Work subsystem by subsystem: boot/main loop,
   file system/PAK, XML/scripting, entities, player, camera, UI, audio.
4. **Platform layer.** Replace PS2 hardware and SDK use (GS/VU1 DMA, SPU2, pad,
   memory card, CD) with SDL2 for windowing, input and audio. Saves go to files.
5. **Renderer.** Replace RenderWare Sky2 with [librw](https://github.com/aap/librw),
   an open-source RW reimplementation. It can read PS2-native RW geometry and
   texture dictionaries, and renders through D3D9 or OpenGL.
6. **Video and audio.** Decode the FMV and stream formats.
