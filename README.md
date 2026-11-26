# Jimmy Neutron: Attack of the Twonkies — PC port (static recompilation)

A native Windows port of *Nickelodeon Jimmy Neutron: Boy Genius — Attack of the
Twonkies* (PS2, USA). The game's Emotion Engine code is statically recompiled to
C++ and linked against a runtime that provides the PS2 hardware natively: kernel,
IOP services, GS (Direct3D 11), VU1, IPU movies, SPU2 audio, pad and memory card.

## No game content

**This repository contains no game data, code or assets, and none may be added.**
You need your own legally obtained copy of the disc. Everything game-specific is
produced on your machine from your disc and stays out of git (see `.gitignore`):

| Made locally from your disc | Where |
|---|---|
| Extracted disc files and the executable | `extracted/` |
| Function list (contains the executable's strings) | `analysis/functions.tsv` |
| Recompiled C++ | `build/generated/` |
| VU1 microcode dumps, snapshots, logs | `analysis/vu_dumps/`, `local/` |
| Texture dumps and upscaled texture packs | `texture_dump/`, `textures/` |
| Saves and settings | `memcard/`, `twonk_settings.ini` |

What *is* here is original work: the recompiler, the runtime, tools, and
documentation. The files in `config/` and the hooks in the runtime refer to the
game only by addresses. No PS2 BIOS is used or needed.

*Jimmy Neutron* and related names are trademarks of their respective owners. This
project is not affiliated with or endorsed by Nickelodeon, Viacom/Paramount, THQ,
or Sony Interactive Entertainment.

## Supported disc

| | |
|---|---|
| Disc | PS2 USA, `SLUS-208.87`, disc version 1.01, NTSC |
| Executable | `SLUS_208.87`, SHA-1 `3dc783935b4a853ad42f97a6c1697aae128ffca3` |

## Easy setup

Double-click **`Setup.cmd`**. The setup window:

1. checks for the build tools (Visual Studio C++ build tools, CMake, Python 3) and
   can install missing ones with `winget`;
2. asks for your disc image and checks that it is the supported version;
3. moves it into `Game iso/` (or copies it, if you choose), then runs every build
   step below and can add a desktop shortcut;
4. finally runs the game for about a minute without a window to take a few
   screenshots, which become the setup window's background. They are made from
   your disc, stay in `local/setup_art/`, and are never committed.

The build takes a while. Progress is shown in the window and saved to
`local/logs/setup.log`. Without the window:
`powershell -ExecutionPolicy Bypass -File tools/setup/setup.ps1 -Headless -Iso <image> [-Copy]`.

## Building

Requirements: Windows 10/11, Visual Studio 2022 (C++ desktop workload), CMake 3.20+,
Python 3 (plus `numpy` and `Pillow` for the texture tools).

```sh
# 1. Extract your disc image
python tools/extract_iso.py "Game iso/<your image>.iso" extracted/disc

# 2. Analyse the executable (writes analysis/functions.tsv)
python -I tools/analysis/elfmap.py extracted/disc/SLUS_208.87 analysis 36ef00

# 3. Build the recompiler and recompile the game code
cmake -S recomp -B recomp/build && cmake --build recomp/build --config Release
recomp/build/Release/twonkrecomp gen extracted/disc/SLUS_208.87 config/regions.txt analysis/functions.tsv build/generated config/hle.txt

# 4. Build the port
cmake -S . -B build/msvc -DTWONK_GENERATED_DIR=<absolute path>/build/generated
cmake --build build/msvc --config Release --target twonk --parallel
```

Optional, for speed: VU1 microcode runs interpreted until it is compiled ahead of
time. Play with `TWONK_VU_DUMP=analysis/vu_dumps` set to collect the programs, then
run `twonk --vu-aot analysis/vu_dumps build/generated/vu1_aot_gen.inc` and rebuild.
See `docs/runtime.md`.

## Running

```sh
build/msvc/Release/twonk.exe extracted/disc/SLUS_208.87 --iso "Game iso/<your image>.iso" --memcard memcard
```

| Key | |
|---|---|
| F4 | Video settings: resolution (auto, 1-8x), supersampling, upscale filter, sharp text, aspect ratio (4:3, 16:9, 16:10, 21:9, stretch), fullscreen |
| Alt+Enter / F11 | Fullscreen |
| F1 | FPS counter |
| START in a cutscene | Fast-forward |

Command-line options: `--scale N`, `--aspect 16:9`, `--fullscreen`, `--renderer sw`,
`--no-window`, `--log <channels>`, `--memcard <dir>`.

### Upscaled textures (optional)

Run once with `TWONK_TEX_DUMP=texture_dump` to dump the textures you see, then
build a pack with `python tools/texpack_upscale.py`. It uses
[Real-ESRGAN ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN) when you have
downloaded it into `tools/realesrgan/`, and Lanczos otherwise. Solid wall and floor
textures keep more detail with `--model realesrgan-x4plus`. The game loads packs
from `textures/`.

## License

The code and documentation here are under the MIT License (see `LICENSE`). It
covers this project only, not the game.

## Documentation

- `docs/recompiler.md`: how the recompiler works.
- `docs/runtime.md`: the runtime and its hardware emulation.
- `docs/M0-binary-map.md`: the layout of the executable.
