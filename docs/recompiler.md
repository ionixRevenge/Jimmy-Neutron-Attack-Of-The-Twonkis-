# twonkrecomp: static recompiler design

`twonkrecomp` translates the Emotion Engine code in `SLUS_208.87` into C++. The
output links against `ps2runtime`, which provides CPU state, memory and services.
The launcher will ship the same tool and run it on the user's machine.

```sh
cmake -S recomp -B recomp/build && cmake --build recomp/build --config Release
twonkrecomp coverage extracted/disc/SLUS_208.87 config/regions.txt
twonkrecomp fn  extracted/disc/SLUS_208.87 config/regions.txt analysis/functions.tsv 1f3e90
twonkrecomp gen extracted/disc/SLUS_208.87 config/regions.txt analysis/functions.tsv build/generated
cmake -S . -B build/msvc -DTWONK_GENERATED_DIR=<abs path to build/generated>
cmake --build build/msvc --config Release --target twonk_game --parallel
```

## Pipeline

1. **Decode** (`recomp/src/r5900.*`). The decoder is table-driven from
   `runtime/include/ps2/r5900_ops.inc`. The recompiler and the runtime share
   this list, so `ps2::Op` in generated code always matches the decoder.
   - It covers base MIPS III, the R5900 MMI instructions (MMI0–3), COP0,
     COP1 single precision, COP2 transfers and VU0 macro mode (Special1 and
     Special2).
   - Result: all 639,134 words in the EE regions decode.
2. **Function boundaries** (`recomp/src/analysis.*`).
   - Candidate starts come from `tools/analysis/elfmap.py`: `jal` targets,
     prologues that follow a return, and code pointers in data.
   - The entry point and `jal` targets are *strong* entries.
   - Any weak start that an intra-procedural branch crosses is merged into its
     neighbour, repeated until nothing changes.
   - Result: 7,316 candidates become 6,879 functions, and external branches
     drop from 2,680 to 44.
   - Merging, not splitting, is deliberate. A branch into the middle of another
     C++ function can only be a tail call, and C++ does not guarantee tail
     calls, so a loop that crosses a split would recurse without bound.
3. **Code generation** (`recomp/src/codegen.*`). Each guest function becomes
   `void f_XXXXXXXX(ps2::Context*)`.
   - Branches become `goto` to labels.
   - Delay slots are placed after the branch condition is evaluated and
     before the transfer.
   - Branch-likely instructions nullify the delay slot when not taken.
   - `jal` becomes a direct C++ call, and `jr ra` becomes `return`.
   - `jr reg` becomes a `switch` over jump-table targets (code pointers in data
     that land inside the function), falling back to `ps2::jump_indirect`.
   - `jalr` becomes `ps2::call_indirect`, a binary search over the generated
     `g_func_table`.
   - Falling off the end of a function becomes a tail call to the next one.
4. **Instruction semantics.**
   - About 98.8% of instructions are emitted as plain C++. This covers integer
     ops, loads and stores, 128-bit `lq`/`sq`, unaligned pairs, mult/div with
     both pipelines, the FPU and COP2 transfers.
   - Everything else is emitted as `ps2::exec<ps2::Op::X>(ctx, pc, raw)`. This
     covers VU0 macro ops, most MMI and COP0. `pc` and `raw` are constants, so
     inlined specializations constant-fold. The primary template traps, so a
     missing implementation fails loudly at runtime and never silently.

## Build result (2026-11-04)

The whole executable, with all 636k instructions, compiles cleanly with MSVC
2022 at `/O2`.

- **Output:** 31 MB of generated C++ in 21 files, building a 24 MB
  `twonk_game.lib`.
- **Time:** 2 min 11 s using 8 parallel jobs. This is a first estimate for the
  launcher's one-time "build the game" step on a user's PC.
- **Generation:** about 0.5 s.

## Semantics notes

- **GPRs** are 128-bit (`ps2::u128`). 32-bit ops sign-extend into the low
  64 bits and leave the upper 64 untouched. `$zero` is never written.
- **The EE FPU** has no NaN, Inf or denormals. Results clamp to ±FLT_MAX and
  denormals flush to zero (`ps2::fclamp`). Divide-by-zero returns signed max
  and sets the D flag. `cvt.w.s` saturates.
- **DIV and DIVU** follow EE results on divide-by-zero and on
  `INT_MIN / -1`, and never trap.
- **Not modelled:** overflow traps for `add`, `sub`, `addi` and `dadd`.
  Compiled C does not rely on them.
- **Memory:** RAM is 32 MB with mirrors at 0x2/0x3/0x8/0xA. The scratchpad is
  at 0x70000000. Anything else goes to `ps2::mmio_read`/`mmio_write`, the
  future hardware HLE.
- **Regions:** VU1 microcode (`vu`) and kernel stubs in `.data` (`kstub`) are
  never recompiled. See `config/regions.txt`.

## Known gaps (next work)

- Implement `exec<>` specializations: VU0 macro ops (SSE), MMI, COP0, CTC2/CFC2
  control registers (including VCALLMS/CMSAR, which start VU0 microprograms).
- Review the 44 residual cross-branches and the 4 control-flow anomalies.
- Jump tables are found from any code pointer in data. Proper analysis should
  follow the `lui/addiu + sll + addu + lw + jr` pattern instead.
- `jr` through a register other than `ra` that is really a return (setjmp/longjmp
  style) needs runtime support.
- Kernel syscall HLE, IOP/SIF HLE, and the RenderWare bridge.
