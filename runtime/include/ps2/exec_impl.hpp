// exec<> specialisations: semantics for instructions the recompiler does not inline.
// Included at the end of ps2/recomp.hpp so every generated TU sees them.
//
// Not modelled yet: VU MAC/status flag registers (vi17/vi16) for arithmetic ops,
// VU0 data memory (VLQI/VSQI/VILWR/VISWR), VCALLMS microprograms. Those trap.
#pragma once
#include <algorithm>
#include <limits>

namespace ps2 {

// Out-of-line implementations (runtime/src/cop0.cpp, runtime/src/vu.cpp)
uint32_t cop0_read(Context* ctx, unsigned reg);
void cop0_write(Context* ctx, unsigned reg, uint32_t value);
uint32_t vu0_ctrl_read(Context* ctx, unsigned reg);
void vu0_ctrl_write(Context* ctx, unsigned reg, uint32_t value);

namespace mmi {
inline unsigned RS(uint32_t r) { return (r >> 21) & 31; }
inline unsigned RT(uint32_t r) { return (r >> 16) & 31; }
inline unsigned RD(uint32_t r) { return (r >> 11) & 31; }
inline unsigned SA(uint32_t r) { return (r >> 6) & 31; }
inline void put(Context* c, unsigned rd, const u128& v) { if (rd) c->r[rd] = v; }
template <typename T> inline T sat(int64_t v) {
    const int64_t lo = (int64_t)std::numeric_limits<T>::min(), hi = (int64_t)std::numeric_limits<T>::max();
    return (T)(v < lo ? lo : v > hi ? hi : v);
}
} // namespace mmi

#define PS2_EXEC(OPNAME) template <> inline void exec<Op::OPNAME>(Context* ctx, uint32_t pc, uint32_t raw)
#define PS2_UNUSED_PC (void)pc

// ---- COP0 -------------------------------------------------------------------------
PS2_EXEC(MFC0) { PS2_UNUSED_PC; const uint32_t v = cop0_read(ctx, mmi::RD(raw)); if (mmi::RT(raw)) ctx->r[mmi::RT(raw)].s64[0] = (int32_t)v; }
PS2_EXEC(MTC0) { PS2_UNUSED_PC; cop0_write(ctx, mmi::RD(raw), ctx->r[mmi::RT(raw)].u32[0]); }
PS2_EXEC(EI) { PS2_UNUSED_PC; (void)raw; ctx->cop0[12] |= 0x10000u; }
PS2_EXEC(DI) { PS2_UNUSED_PC; (void)raw; ctx->cop0[12] &= ~0x10000u; }

// ---- MMI: element-wise helpers --------------------------------------------------------
#define PS2_LANES(OPNAME, FIELD, N, EXPR)                                        \
    PS2_EXEC(OPNAME) {                                                           \
        PS2_UNUSED_PC;                                                           \
        const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];           \
        u128 d;                                                                  \
        for (int i = 0; i < N; ++i) { auto a = s.FIELD[i]; auto b = t.FIELD[i]; (void)a; (void)b; d.FIELD[i] = (EXPR); } \
        mmi::put(ctx, mmi::RD(raw), d);                                          \
    }

PS2_LANES(PADDW,  u32, 4, a + b)
PS2_LANES(PSUBW,  u32, 4, a - b)
PS2_LANES(PADDH,  u16, 8, (uint16_t)(a + b))
PS2_LANES(PSUBH,  u16, 8, (uint16_t)(a - b))
PS2_LANES(PADDB,  u8, 16, (uint8_t)(a + b))
PS2_LANES(PSUBB,  u8, 16, (uint8_t)(a - b))
PS2_LANES(PADDSW, s32, 4, mmi::sat<int32_t>((int64_t)a + b))
PS2_LANES(PSUBSW, s32, 4, mmi::sat<int32_t>((int64_t)a - b))
PS2_LANES(PADDSH, s16, 8, mmi::sat<int16_t>((int64_t)a + b))
PS2_LANES(PSUBSH, s16, 8, mmi::sat<int16_t>((int64_t)a - b))
PS2_LANES(PADDSB, s8, 16, mmi::sat<int8_t>((int64_t)a + b))
PS2_LANES(PSUBSB, s8, 16, mmi::sat<int8_t>((int64_t)a - b))
PS2_LANES(PADDUW, u32, 4, (uint32_t)std::min<uint64_t>((uint64_t)a + b, 0xFFFFFFFFull))
PS2_LANES(PSUBUW, u32, 4, a > b ? a - b : 0u)
PS2_LANES(PADDUH, u16, 8, (uint16_t)std::min<uint32_t>((uint32_t)a + b, 0xFFFFu))
PS2_LANES(PSUBUH, u16, 8, (uint16_t)(a > b ? a - b : 0))
PS2_LANES(PADDUB, u8, 16, (uint8_t)std::min<uint32_t>((uint32_t)a + b, 0xFFu))
PS2_LANES(PSUBUB, u8, 16, (uint8_t)(a > b ? a - b : 0))
PS2_LANES(PCGTW,  s32, 4, a > b ? -1 : 0)
PS2_LANES(PCGTH,  s16, 8, (int16_t)(a > b ? -1 : 0))
PS2_LANES(PCGTB,  s8, 16, (int8_t)(a > b ? -1 : 0))
PS2_LANES(PCEQW,  u32, 4, a == b ? 0xFFFFFFFFu : 0u)
PS2_LANES(PCEQH,  u16, 8, (uint16_t)(a == b ? 0xFFFF : 0))
PS2_LANES(PCEQB,  u8, 16, (uint8_t)(a == b ? 0xFF : 0))
PS2_LANES(PMAXW,  s32, 4, std::max(a, b))
PS2_LANES(PMINW,  s32, 4, std::min(a, b))
PS2_LANES(PMAXH,  s16, 8, std::max(a, b))
PS2_LANES(PMINH,  s16, 8, std::min(a, b))
PS2_LANES(PAND,   u64, 2, a & b)
PS2_LANES(POR,    u64, 2, a | b)
PS2_LANES(PXOR,   u64, 2, a ^ b)
PS2_LANES(PNOR,   u64, 2, ~(a | b))
#undef PS2_LANES

// unary on rt
#define PS2_UNARY(OPNAME, FIELD, N, EXPR)                                        \
    PS2_EXEC(OPNAME) {                                                           \
        PS2_UNUSED_PC;                                                           \
        const u128 t = ctx->r[mmi::RT(raw)];                                     \
        u128 d;                                                                  \
        for (int i = 0; i < N; ++i) { auto a = t.FIELD[i]; d.FIELD[i] = (EXPR); } \
        mmi::put(ctx, mmi::RD(raw), d);                                          \
    }
PS2_UNARY(PABSW, s32, 4, a == INT32_MIN ? INT32_MAX : (a < 0 ? -a : a))
PS2_UNARY(PABSH, s16, 8, (int16_t)(a == INT16_MIN ? INT16_MAX : (a < 0 ? -a : a)))
#undef PS2_UNARY

// shifts by immediate
#define PS2_SHIFT(OPNAME, FIELD, N, MASK, EXPR)                                  \
    PS2_EXEC(OPNAME) {                                                           \
        PS2_UNUSED_PC;                                                           \
        const u128 t = ctx->r[mmi::RT(raw)];                                     \
        const unsigned sh = mmi::SA(raw) & MASK;                                 \
        u128 d;                                                                  \
        for (int i = 0; i < N; ++i) { auto a = t.FIELD[i]; d.FIELD[i] = (EXPR); } \
        mmi::put(ctx, mmi::RD(raw), d);                                          \
    }
PS2_SHIFT(PSLLH, u16, 8, 15, (uint16_t)(a << sh))
PS2_SHIFT(PSRLH, u16, 8, 15, (uint16_t)(a >> sh))
PS2_SHIFT(PSRAH, s16, 8, 15, (int16_t)(a >> sh))
PS2_SHIFT(PSLLW, u32, 4, 31, a << sh)
PS2_SHIFT(PSRLW, u32, 4, 31, a >> sh)
PS2_SHIFT(PSRAW, s32, 4, 31, a >> sh)
#undef PS2_SHIFT

// variable word shifts on words 0 and 2, sign-extended into 64-bit lanes
#define PS2_VSHIFT(OPNAME, EXPR)                                                 \
    PS2_EXEC(OPNAME) {                                                           \
        PS2_UNUSED_PC;                                                           \
        const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];           \
        u128 d;                                                                  \
        for (int i = 0; i < 2; ++i) { const uint32_t a = t.u32[i * 2]; const unsigned sh = s.u32[i * 2] & 31; d.s64[i] = (int32_t)(EXPR); } \
        mmi::put(ctx, mmi::RD(raw), d);                                          \
    }
PS2_VSHIFT(PSLLVW, a << sh)
PS2_VSHIFT(PSRLVW, a >> sh)
PS2_VSHIFT(PSRAVW, (uint32_t)((int32_t)a >> sh))
#undef PS2_VSHIFT

// ---- MMI: permutes ---------------------------------------------------------------------
PS2_EXEC(PCPYH) {
    PS2_UNUSED_PC;
    const u128 t = ctx->r[mmi::RT(raw)];
    u128 d;
    for (int i = 0; i < 4; ++i) { d.u16[i] = t.u16[0]; d.u16[4 + i] = t.u16[4]; }
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PCPYLD) { PS2_UNUSED_PC; u128 d; d.u64[0] = ctx->r[mmi::RT(raw)].u64[0]; d.u64[1] = ctx->r[mmi::RS(raw)].u64[0]; mmi::put(ctx, mmi::RD(raw), d); }
PS2_EXEC(PCPYUD) { PS2_UNUSED_PC; u128 d; d.u64[0] = ctx->r[mmi::RS(raw)].u64[1]; d.u64[1] = ctx->r[mmi::RT(raw)].u64[1]; mmi::put(ctx, mmi::RD(raw), d); }
PS2_EXEC(PPACW) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    u128 d; d.u32[0] = t.u32[0]; d.u32[1] = t.u32[2]; d.u32[2] = s.u32[0]; d.u32[3] = s.u32[2];
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PPACH) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    u128 d;
    for (int i = 0; i < 4; ++i) { d.u16[i] = t.u16[i * 2]; d.u16[4 + i] = s.u16[i * 2]; }
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PPACB) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    u128 d;
    for (int i = 0; i < 8; ++i) { d.u8[i] = t.u8[i * 2]; d.u8[8 + i] = s.u8[i * 2]; }
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PEXTLW) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    u128 d; d.u32[0] = t.u32[0]; d.u32[1] = s.u32[0]; d.u32[2] = t.u32[1]; d.u32[3] = s.u32[1];
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PEXTUW) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    u128 d; d.u32[0] = t.u32[2]; d.u32[1] = s.u32[2]; d.u32[2] = t.u32[3]; d.u32[3] = s.u32[3];
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PEXTLH) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    u128 d;
    for (int i = 0; i < 4; ++i) { d.u16[2 * i] = t.u16[i]; d.u16[2 * i + 1] = s.u16[i]; }
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PEXTUH) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    u128 d;
    for (int i = 0; i < 4; ++i) { d.u16[2 * i] = t.u16[4 + i]; d.u16[2 * i + 1] = s.u16[4 + i]; }
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PEXTLB) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    u128 d;
    for (int i = 0; i < 8; ++i) { d.u8[2 * i] = t.u8[i]; d.u8[2 * i + 1] = s.u8[i]; }
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PEXTUB) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    u128 d;
    for (int i = 0; i < 8; ++i) { d.u8[2 * i] = t.u8[8 + i]; d.u8[2 * i + 1] = s.u8[8 + i]; }
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PINTH) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    u128 d;
    for (int i = 0; i < 4; ++i) { d.u16[2 * i] = t.u16[i]; d.u16[2 * i + 1] = s.u16[4 + i]; }
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PINTEH) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    u128 d;
    for (int i = 0; i < 4; ++i) { d.u16[2 * i] = t.u16[2 * i]; d.u16[2 * i + 1] = s.u16[2 * i]; }
    mmi::put(ctx, mmi::RD(raw), d);
}
#define PS2_PERM_W(OPNAME, A, B, C, D)                                           \
    PS2_EXEC(OPNAME) {                                                           \
        PS2_UNUSED_PC;                                                           \
        const u128 t = ctx->r[mmi::RT(raw)];                                     \
        u128 d; d.u32[0] = t.u32[A]; d.u32[1] = t.u32[B]; d.u32[2] = t.u32[C]; d.u32[3] = t.u32[D]; \
        mmi::put(ctx, mmi::RD(raw), d);                                          \
    }
PS2_PERM_W(PEXEW, 2, 1, 0, 3)
PS2_PERM_W(PEXCW, 0, 2, 1, 3)
PS2_PERM_W(PROT3W, 1, 2, 0, 3)
#undef PS2_PERM_W
#define PS2_PERM_H(OPNAME, A, B, C, D)                                           \
    PS2_EXEC(OPNAME) {                                                           \
        PS2_UNUSED_PC;                                                           \
        const u128 t = ctx->r[mmi::RT(raw)];                                     \
        u128 d;                                                                  \
        for (int k = 0; k < 8; k += 4) { d.u16[k] = t.u16[k + A]; d.u16[k + 1] = t.u16[k + B]; d.u16[k + 2] = t.u16[k + C]; d.u16[k + 3] = t.u16[k + D]; } \
        mmi::put(ctx, mmi::RD(raw), d);                                          \
    }
PS2_PERM_H(PEXEH, 2, 1, 0, 3)
PS2_PERM_H(PEXCH, 0, 2, 1, 3)
PS2_PERM_H(PREVH, 3, 2, 1, 0)
#undef PS2_PERM_H

PS2_EXEC(QFSRV) {   // funnel shift (rs:rt) right by SA bits, keep the low 128
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)], t = ctx->r[mmi::RT(raw)];
    uint8_t buf[32];
    std::memcpy(buf, &t, 16);
    std::memcpy(buf + 16, &s, 16);
    u128 d;
    std::memcpy(&d, buf + ((ctx->sa >> 3) & 15), 16);
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(MTSAB) { PS2_UNUSED_PC; ctx->sa = ((ctx->r[mmi::RS(raw)].u32[0] & 15) ^ (raw & 15)) * 8; }
PS2_EXEC(MTSAH) { PS2_UNUSED_PC; ctx->sa = ((ctx->r[mmi::RS(raw)].u32[0] & 7) ^ (raw & 7)) * 16; }
PS2_EXEC(PLZCW) {
    PS2_UNUSED_PC;
    const u128 s = ctx->r[mmi::RS(raw)];
    if (!mmi::RD(raw)) return;
    for (int i = 0; i < 2; ++i) {
        uint32_t v = s.u32[i];
        if (v & 0x80000000u) v = ~v;
        unsigned n = 0;
        while (n < 32 && !(v & (0x80000000u >> n))) ++n;
        ctx->r[mmi::RD(raw)].u32[i] = n - 1;
    }
}
PS2_EXEC(PMFHI) { PS2_UNUSED_PC; mmi::put(ctx, mmi::RD(raw), ctx->hi); }
PS2_EXEC(PMFLO) { PS2_UNUSED_PC; mmi::put(ctx, mmi::RD(raw), ctx->lo); }
PS2_EXEC(PMTHI) { PS2_UNUSED_PC; ctx->hi = ctx->r[mmi::RS(raw)]; }
PS2_EXEC(PMTLO) { PS2_UNUSED_PC; ctx->lo = ctx->r[mmi::RS(raw)]; }
PS2_EXEC(PMFHL) {
    PS2_UNUSED_PC;
    u128 d;
    const u128 &hi = ctx->hi, &lo = ctx->lo;
    switch (mmi::SA(raw)) {
    case 0: d.u32[0] = lo.u32[0]; d.u32[1] = hi.u32[0]; d.u32[2] = lo.u32[2]; d.u32[3] = hi.u32[2]; break;   // .LW
    case 1: d.u32[0] = lo.u32[1]; d.u32[1] = hi.u32[1]; d.u32[2] = lo.u32[3]; d.u32[3] = hi.u32[3]; break;   // .UW
    default: unimplemented(ctx, pc, raw, "pmfhl variant");
    }
    mmi::put(ctx, mmi::RD(raw), d);
}
PS2_EXEC(PMTHL) {
    PS2_UNUSED_PC;
    if (mmi::SA(raw) != 0) unimplemented(ctx, pc, raw, "pmthl variant");
    const u128 s = ctx->r[mmi::RS(raw)];
    ctx->lo.u32[0] = s.u32[0]; ctx->hi.u32[0] = s.u32[1]; ctx->lo.u32[2] = s.u32[2]; ctx->hi.u32[2] = s.u32[3];
}

// ---- VU0 macro mode ----------------------------------------------------------------------
namespace vu {
inline unsigned FT(uint32_t r) { return (r >> 16) & 31; }
inline unsigned FS(uint32_t r) { return (r >> 11) & 31; }
inline unsigned FD(uint32_t r) { return (r >> 6) & 31; }
inline unsigned DEST(uint32_t r) { return (r >> 21) & 15; }
inline unsigned BC(uint32_t r) { return r & 3; }
inline bool on(unsigned dest, int i) { return (dest >> (3 - i)) & 1; }
// write masked lanes; vf0 is read-only
inline void put(Context* c, unsigned reg, unsigned dest, const float v[4]) {
    if (!reg) return;
    for (int i = 0; i < 4; ++i) if (on(dest, i)) c->vf[reg].f[i] = fclamp(v[i]);
}
inline void put_acc(Context* c, unsigned dest, const float v[4]) {
    for (int i = 0; i < 4; ++i) if (on(dest, i)) c->vacc.f[i] = fclamp(v[i]);
}
enum class Src { Vec, Bc, Q, I };
inline float src(Context* c, uint32_t raw, Src k, int i) {
    switch (k) {
    case Src::Vec: return c->vf[FT(raw)].f[i];
    case Src::Bc: return c->vf[FT(raw)].f[BC(raw)];
    case Src::Q: return c->vq;
    default: return c->vI;
    }
}
} // namespace vu

// fd = f(fs, src, acc)
#define PS2_VU(OPNAME, KIND, TOACC, EXPR)                                         \
    PS2_EXEC(OPNAME) {                                                            \
        PS2_UNUSED_PC;                                                            \
        float r[4];                                                               \
        const u128 fs = ctx->vf[vu::FS(raw)], acc = ctx->vacc;                    \
        for (int i = 0; i < 4; ++i) {                                             \
            const float a = fs.f[i], b = vu::src(ctx, raw, vu::Src::KIND, i), c = acc.f[i]; (void)c; \
            r[i] = (EXPR);                                                        \
        }                                                                         \
        if (TOACC) vu::put_acc(ctx, vu::DEST(raw), r); else vu::put(ctx, vu::FD(raw), vu::DEST(raw), r); \
    }
#define PS2_VU_SET(BASE, KIND, TOACC, EXPR) PS2_VU(BASE, KIND, TOACC, EXPR)

PS2_VU(VADDbc, Bc, false, a + b)   PS2_VU(VSUBbc, Bc, false, a - b)   PS2_VU(VMULbc, Bc, false, a * b)
PS2_VU(VMADDbc, Bc, false, c + a * b) PS2_VU(VMSUBbc, Bc, false, c - a * b)
PS2_VU(VMAXbc, Bc, false, std::max(a, b)) PS2_VU(VMINIbc, Bc, false, std::min(a, b))
PS2_VU(VADDq, Q, false, a + b)     PS2_VU(VSUBq, Q, false, a - b)     PS2_VU(VMULq, Q, false, a * b)
PS2_VU(VMADDq, Q, false, c + a * b) PS2_VU(VMSUBq, Q, false, c - a * b)
PS2_VU(VADDi, I, false, a + b)     PS2_VU(VSUBi, I, false, a - b)     PS2_VU(VMULi, I, false, a * b)
PS2_VU(VMADDi, I, false, c + a * b) PS2_VU(VMSUBi, I, false, c - a * b)
PS2_VU(VMAXi, I, false, std::max(a, b)) PS2_VU(VMINIi, I, false, std::min(a, b))
PS2_VU(VADD, Vec, false, a + b)    PS2_VU(VSUB, Vec, false, a - b)    PS2_VU(VMUL, Vec, false, a * b)
PS2_VU(VMADD, Vec, false, c + a * b) PS2_VU(VMSUB, Vec, false, c - a * b)
PS2_VU(VMAX, Vec, false, std::max(a, b)) PS2_VU(VMINI, Vec, false, std::min(a, b))
// accumulator forms
PS2_VU(VADDAbc, Bc, true, a + b)   PS2_VU(VSUBAbc, Bc, true, a - b)   PS2_VU(VMULAbc, Bc, true, a * b)
PS2_VU(VMADDAbc, Bc, true, c + a * b) PS2_VU(VMSUBAbc, Bc, true, c - a * b)
PS2_VU(VADDAq, Q, true, a + b)     PS2_VU(VSUBAq, Q, true, a - b)     PS2_VU(VMULAq, Q, true, a * b)
PS2_VU(VMADDAq, Q, true, c + a * b) PS2_VU(VMSUBAq, Q, true, c - a * b)
PS2_VU(VADDAi, I, true, a + b)     PS2_VU(VSUBAi, I, true, a - b)     PS2_VU(VMULAi, I, true, a * b)
PS2_VU(VMADDAi, I, true, c + a * b) PS2_VU(VMSUBAi, I, true, c - a * b)
PS2_VU(VADDA, Vec, true, a + b)    PS2_VU(VSUBA, Vec, true, a - b)    PS2_VU(VMULA, Vec, true, a * b)
PS2_VU(VMADDA, Vec, true, c + a * b) PS2_VU(VMSUBA, Vec, true, c - a * b)
#undef PS2_VU
#undef PS2_VU_SET

PS2_EXEC(VOPMULA) {   // ACC.xyz = fs.yzx * ft.zxy
    PS2_UNUSED_PC;
    const u128 s = ctx->vf[vu::FS(raw)], t = ctx->vf[vu::FT(raw)];
    const float r[4] = {s.f[1] * t.f[2], s.f[2] * t.f[0], s.f[0] * t.f[1], 0.0f};
    vu::put_acc(ctx, vu::DEST(raw) & 0xE, r);
}
PS2_EXEC(VOPMSUB) {   // fd.xyz = ACC.xyz - fs.yzx * ft.zxy
    PS2_UNUSED_PC;
    const u128 s = ctx->vf[vu::FS(raw)], t = ctx->vf[vu::FT(raw)], a = ctx->vacc;
    const float r[4] = {a.f[0] - s.f[1] * t.f[2], a.f[1] - s.f[2] * t.f[0], a.f[2] - s.f[0] * t.f[1], 0.0f};
    vu::put(ctx, vu::FD(raw), vu::DEST(raw) & 0xE, r);
}
PS2_EXEC(VABS)  { PS2_UNUSED_PC; const u128 s = ctx->vf[vu::FS(raw)]; float r[4]; for (int i = 0; i < 4; ++i) r[i] = std::fabs(s.f[i]); vu::put(ctx, vu::FT(raw), vu::DEST(raw), r); }
PS2_EXEC(VMOVE) { PS2_UNUSED_PC; const u128 s = ctx->vf[vu::FS(raw)]; vu::put(ctx, vu::FT(raw), vu::DEST(raw), s.f); }
PS2_EXEC(VMR32) {
    PS2_UNUSED_PC;
    const u128 s = ctx->vf[vu::FS(raw)];
    const float r[4] = {s.f[1], s.f[2], s.f[3], s.f[0]};
    vu::put(ctx, vu::FT(raw), vu::DEST(raw), r);
}
#define PS2_ITOF(OPNAME, SHIFT)                                                   \
    PS2_EXEC(OPNAME) {                                                            \
        PS2_UNUSED_PC;                                                            \
        const u128 s = ctx->vf[vu::FS(raw)];                                      \
        float r[4]; for (int i = 0; i < 4; ++i) r[i] = (float)s.s32[i] / (float)(1 << SHIFT); \
        vu::put(ctx, vu::FT(raw), vu::DEST(raw), r);                              \
    }
#define PS2_FTOI(OPNAME, SHIFT)                                                   \
    PS2_EXEC(OPNAME) {                                                            \
        PS2_UNUSED_PC;                                                            \
        const u128 s = ctx->vf[vu::FS(raw)];                                      \
        const unsigned ft = vu::FT(raw), d = vu::DEST(raw);                       \
        if (!ft) return;                                                          \
        for (int i = 0; i < 4; ++i)                                               \
            if (vu::on(d, i)) ctx->vf[ft].s32[i] = cvt_w_s(s.f[i] * (float)(1 << SHIFT)); \
    }
PS2_ITOF(VITOF0, 0) PS2_ITOF(VITOF4, 4) PS2_ITOF(VITOF12, 12) PS2_ITOF(VITOF15, 15)
PS2_FTOI(VFTOI0, 0) PS2_FTOI(VFTOI4, 4) PS2_FTOI(VFTOI12, 12) PS2_FTOI(VFTOI15, 15)
#undef PS2_ITOF
#undef PS2_FTOI

PS2_EXEC(VDIV) {
    PS2_UNUSED_PC;
    const float a = ctx->vf[vu::FS(raw)].f[(raw >> 21) & 3], b = ctx->vf[vu::FT(raw)].f[(raw >> 23) & 3];
    ctx->vq = (b == 0.0f) ? as_float(((as_u32(a) ^ as_u32(b)) & 0x80000000u) | 0x7F7FFFFFu) : fclamp(a / b);
}
PS2_EXEC(VSQRT) { PS2_UNUSED_PC; ctx->vq = std::sqrt(std::fabs(ctx->vf[vu::FT(raw)].f[(raw >> 23) & 3])); }
PS2_EXEC(VRSQRT) {
    PS2_UNUSED_PC;
    const float a = ctx->vf[vu::FS(raw)].f[(raw >> 21) & 3];
    const float b = std::sqrt(std::fabs(ctx->vf[vu::FT(raw)].f[(raw >> 23) & 3]));
    ctx->vq = (b == 0.0f) ? as_float((as_u32(a) & 0x80000000u) | 0x7F7FFFFFu) : fclamp(a / b);
}
PS2_EXEC(VWAITQ) { PS2_UNUSED_PC; (void)ctx; (void)raw; }
PS2_EXEC(VNOP)   { PS2_UNUSED_PC; (void)ctx; (void)raw; }
PS2_EXEC(VCLIPw) {   // shift clip flags by 6, set |x|,|y|,|z| vs |w| judgement bits
    PS2_UNUSED_PC;
    const u128 s = ctx->vf[vu::FS(raw)];
    const float w = std::fabs(ctx->vf[vu::FT(raw)].f[3]);
    uint32_t f = 0;
    if (s.f[0] > w) f |= 0x01; if (s.f[0] < -w) f |= 0x02;
    if (s.f[1] > w) f |= 0x04; if (s.f[1] < -w) f |= 0x08;
    if (s.f[2] > w) f |= 0x10; if (s.f[2] < -w) f |= 0x20;
    ctx->vi[18] = ((ctx->vi[18] << 6) | f) & 0xFFFFFF;
}
// integer unit
PS2_EXEC(VIADD)  { PS2_UNUSED_PC; const unsigned d = vu::FD(raw) & 15; if (d) ctx->vi[d] = (ctx->vi[vu::FS(raw) & 15] + ctx->vi[vu::FT(raw) & 15]) & 0xFFFF; }
PS2_EXEC(VISUB)  { PS2_UNUSED_PC; const unsigned d = vu::FD(raw) & 15; if (d) ctx->vi[d] = (ctx->vi[vu::FS(raw) & 15] - ctx->vi[vu::FT(raw) & 15]) & 0xFFFF; }
PS2_EXEC(VIAND)  { PS2_UNUSED_PC; const unsigned d = vu::FD(raw) & 15; if (d) ctx->vi[d] = ctx->vi[vu::FS(raw) & 15] & ctx->vi[vu::FT(raw) & 15]; }
PS2_EXEC(VIOR)   { PS2_UNUSED_PC; const unsigned d = vu::FD(raw) & 15; if (d) ctx->vi[d] = ctx->vi[vu::FS(raw) & 15] | ctx->vi[vu::FT(raw) & 15]; }
PS2_EXEC(VIADDI) {
    PS2_UNUSED_PC;
    const unsigned t = vu::FT(raw) & 15;
    const int32_t imm = (int32_t)(((raw >> 6) & 31) << 27) >> 27;
    if (t) ctx->vi[t] = (uint32_t)(ctx->vi[vu::FS(raw) & 15] + imm) & 0xFFFF;
}
PS2_EXEC(VMTIR) { PS2_UNUSED_PC; const unsigned t = vu::FT(raw) & 15; if (t) ctx->vi[t] = ctx->vf[vu::FS(raw)].u32[(raw >> 21) & 3] & 0xFFFF; }
PS2_EXEC(VMFIR) {
    PS2_UNUSED_PC;
    const unsigned ft = vu::FT(raw), d = vu::DEST(raw);
    if (!ft) return;
    const int32_t v = (int16_t)ctx->vi[vu::FS(raw) & 15];
    for (int i = 0; i < 4; ++i) if (vu::on(d, i)) ctx->vf[ft].s32[i] = v;
}
// random unit (23-bit LFSR, as on hardware)
PS2_EXEC(VRINIT) { PS2_UNUSED_PC; ctx->vR = 0x3F800000u | (ctx->vf[vu::FS(raw)].u32[(raw >> 21) & 3] & 0x007FFFFFu); }
PS2_EXEC(VRXOR)  { PS2_UNUSED_PC; ctx->vR = 0x3F800000u | ((ctx->vR ^ ctx->vf[vu::FS(raw)].u32[(raw >> 21) & 3]) & 0x007FFFFFu); }
PS2_EXEC(VRNEXT) {
    PS2_UNUSED_PC;
    uint32_t x = ctx->vR & 0x007FFFFFu;
    const uint32_t bit = ((x >> 4) ^ (x >> 22)) & 1;
    x = ((x << 1) | bit) & 0x007FFFFFu;
    ctx->vR = 0x3F800000u | x;
    const float v = as_float(ctx->vR);
    const float r[4] = {v, v, v, v};
    vu::put(ctx, vu::FT(raw), vu::DEST(raw), r);
}
PS2_EXEC(VRGET) {
    PS2_UNUSED_PC;
    const float v = as_float(ctx->vR);
    const float r[4] = {v, v, v, v};
    vu::put(ctx, vu::FT(raw), vu::DEST(raw), r);
}
// control registers (vi16+): status, MAC, clip, R, I, Q, CMSAR, FBRST, VPU-STAT ...
PS2_EXEC(CFC2) { PS2_UNUSED_PC; const unsigned rt = mmi::RT(raw); const uint32_t v = vu0_ctrl_read(ctx, mmi::RD(raw)); if (rt) ctx->r[rt].s64[0] = (int32_t)v; }
PS2_EXEC(CTC2) { PS2_UNUSED_PC; vu0_ctrl_write(ctx, mmi::RD(raw), ctx->r[mmi::RT(raw)].u32[0]); }

#undef PS2_EXEC
#undef PS2_UNUSED_PC

} // namespace ps2
