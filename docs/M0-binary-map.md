# M0: Binary map of SLUS_208.87

Regenerate with:

```sh
python -I tools/analysis/elfmap.py extracted/disc/SLUS_208.87 analysis 36ef00
python -I tools/analysis/regions.py config/regions.txt analysis/functions.tsv
```

The authoritative region table is `config/regions.txt`.

## Summary

| Part | Size | Functions | What happens to it in the port |
|---|---:|---:|---|
| Game code (A + B + C) | ~1,707 KB | ~4,980 | **Recompile**, then replace with hand-written C++ over time |
| RenderWare 3.6 (core + plugins + toolkits) | ~555 KB | ~1,510 | **HLE**: native reimplementation on guest structs, drawing via librw |
| Sony EE libraries + crt0 | ~176 KB | ~666 | **HLE** at the API level (sceCd*, scePad*, sceMc*, sif, kernel threads) |
| MSL C runtime | ~28 KB | ~105 | **HLE** with host libc (printf, string, math) |
| Unclassified | ~25 KB | 53 | To be determined |
| VU1 microcode | ~175 KB | n/a | **Dropped**: RenderWare HLE replaces it |
| Data | ~2.1 MB | n/a | Loaded as-is into the 32 MB guest RAM image |

Function discovery uses three sources: `jal` targets, prologues that follow a
return, and code pointers found in data. It finds 7,316 EE functions. Expect a
few percent error until switch tables and tail calls are handled properly.

## Findings that shape the design

1. **Game logic is largely GameMonkey Script.** There are 592 `.gm` files of
   compiled GM bytecode (`gml` library header, `func` blocks). The executable
   registers GM's built-ins: `sysCollectGarbage`, `sysTime`, `yield`, `signal`,
   `tableCount`, and others.
   - GameMonkey is open source (MIT). If we pin the exact GM version, we can run
     the original scripts on the native GM source instead of the recompiled VM.
   - The VM is in game code A, at roughly `0x17b000–0x190000`.
2. **RenderWare is contiguous and identifiable.** Plugin registration immediates
   anchor it: HAnim `0x11E` at `0x1ffa60`, MatFX `0x120` at `0x2010b0`, PTank
   `0x130` at `0x2115a8`, UVAnim `0x135` at `0x23bbe0`. The PS2All pipeline
   strings are at `0x24f140`.
3. **VU0 macro mode is used heavily.** The block `0x224000–0x234000` is almost
   entirely COP2 vector instructions. The recompiler must translate these to
   SSE, and it should do so early, because math is everywhere.
4. **VU1 microcode is embedded in .text** at `0x36ef00–0x39acc0`. It must be
   excluded from recompilation. Under HLE it becomes unnecessary.
5. **FMV uses Sony libmpeg/libipu.** The `.fmv` files are PS2 MPEG-2 streams,
   so they can be decoded natively with FFmpeg.
6. **Code inside .data** at `0x39b270–0x39c5e8` is probably libkernl stubs that
   get copied into the kernel. HLE the syscalls that use them instead of
   running this code.
7. **Saves** use the memory card ID `BASLUS-20887` with `%sSave` directories.
   Map these to files under the user's profile.

## Open questions and next steps

- **Exact RenderWare and Sony function identification.** Region-level
  classification is done. Function-level names need signatures. Options:
  - match against other RenderWare 3.6 PS2 titles that shipped with symbols;
  - build our own fingerprints from RenderWare's documented behaviour (librw
    and re3 are references for structure layouts).
- **Analyse the IOP side** (`HIS.IRX`, the sound driver) to define the audio
  HLE interface.
- **Classify the unclassified block** at `0x2648f0–0x26ad80`.

## GameMonkey Script: verified

`tools/analysis/gmcheck.py extracted/data` validates every script with no errors:

- 592 files, 2,980 functions, 181,214 instructions.
- No debug info is included, so there are no source or line tables.

Verification method: each script was checked against the **GameMonkey 1.2x
bytecode set**, using the 56 opcodes from `BC_GETDOT = 0` to `BC_FORK = 55`.
The check uses the library layout from upstream `gmLibHooks.cpp`: the `gml0`
header, the string table, and `func` records. For every instruction it confirms:

- the opcode is known;
- branch targets land on instruction boundaries;
- string and symbol operands point to the start of a string-table entry;
- `PUSHFN` operands name an existing function.

Reference used: the 1.28 tree at github.com/mrozbarry/gamemonkey-unofficial.

To see a script's bytecode: `python -I tools/analysis/gmcheck.py x -v <file.gm>`.

Consequences for the port:

- **The original scripts can run on native, open-source GameMonkey.** The
  script VM does not need recompiling.
- **Operands are 4 bytes in the files.** The upstream loader assumes operands
  are pointer-sized (`gmptr`). A 64-bit build therefore needs a small loader
  patch that widens operands to 8 bytes and fixes up branch offsets.
  Alternatively, build GM with 32-bit operand storage.
- **Native API.** Scripts read 95 globals they never assign. Removing GM's own
  standard library (`print`, `sleep`, `yield`, `thread`, `sin`, ...) leaves
  about 70 engine bindings:
  - camera: `CameraFadeIn`, `CameraFadeOut`, `CameraPush`, ...
  - objects: `CreateObject`, `GetObject`, `SetState`, ...
  - sound: `PlaySound`, `MusicManager`, ...
  - zones and loading: `LoadZoneGroup`, `LoadLockBegin`, ...
  - HUD and report card: `HudCommand`, `ReportCardAddPoints`, ...
  - input: `JNGetPadAxis`, `JNSetPadRumble`, ...

  Methods called on objects through `GETDOT` are additional surface that has
  not been catalogued yet.
- **Opcodes the scripts never use:** BOR, BXOR, SHL, SHR, BINV, POS, NOP, LINE,
  DUP2, SWAP, FORK.
