// Guest CPU state shared by generated code and the runtime.
#pragma once
#include <cstdint>

namespace ps2 {

union alignas(16) u128 {
    uint64_t u64[2];
    int64_t s64[2];
    uint32_t u32[4];
    int32_t s32[4];
    uint16_t u16[8];
    int16_t s16[8];
    uint8_t u8[16];
    int8_t s8[16];
    float f[4];
};
static_assert(sizeof(u128) == 16, "u128 must be 16 bytes");

constexpr uint32_t kRamSize = 32u * 1024 * 1024;   // EE main RAM
constexpr uint32_t kSprSize = 16u * 1024;          // scratchpad

struct Context {
    // --- EE core -------------------------------------------------------------
    u128 r[32];          // GPRs, 128-bit. r[0] is never written by generated code.
    u128 hi, lo;         // u64[0] = HI/LO (pipeline 0), u64[1] = HI1/LO1 (pipeline 1)
    uint32_t sa;         // shift amount register (QFSRV)
    uint32_t pc;         // only meaningful at runtime boundaries (syscalls, faults)

    // --- COP1 (FPU) ------------------------------------------------------------
    float f[32];
    float facc;          // FPU accumulator (ADDA.S / MADD.S ...)
    uint32_t fcr0, fcr31;

    // --- COP2 (VU0 in macro mode) ----------------------------------------------
    u128 vf[32];         // vf0 is constant (0,0,0,1)
    uint32_t vi[32];     // vi0-15 integer regs; vi16+ control regs (status, MAC, clip, R, I, Q, ...)
    u128 vacc;           // VU accumulator
    float vq, vp;        // Q / P registers
    float vI;            // I register
    uint32_t vR;         // R (random) register

    // --- COP0 --------------------------------------------------------------------
    uint32_t cop0[32];

    // --- memory (owned by the runtime) --------------------------------------------
    uint8_t* ram;        // kRamSize bytes
    uint8_t* spr;        // kSprSize bytes
};

} // namespace ps2
