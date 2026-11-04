// Everything generated code needs: register accessors, memory access,
// FPU helpers, control-flow hooks and the per-instruction exec<> hook.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "ps2/context.hpp"

namespace ps2 {

// ---- Opcode identities (same list the recompiler decodes) -------------------
enum class Op : uint16_t {
    Invalid = 0,
#define OP(name, mnem, fmt) name,
#include "ps2/r5900_ops.inc"
#undef OP
    Count
};

using RecompFn = void (*)(Context*);
struct FuncEntry { uint32_t addr; RecompFn fn; };

// ---- Runtime services implemented in the runtime library --------------------
[[noreturn]] void fatal(Context* ctx, uint32_t pc, const char* what);
void call_indirect(Context* ctx, uint32_t target);       // jalr / unresolved jal
void jump_indirect(Context* ctx, uint32_t target);       // jr / out-of-function branch (tail jump)
void syscall(Context* ctx, uint32_t pc);
void breakpoint(Context* ctx, uint32_t pc);
bool cop0_condition(Context* ctx);                       // BC0F/BC0T (CPCOND0: DMA idle)
bool vu0_busy(Context* ctx);                             // BC2F/BC2T
uint32_t mmio_read(Context* ctx, uint32_t addr, int size);
void mmio_write(Context* ctx, uint32_t addr, uint64_t value, int size);
[[noreturn]] void unimplemented(Context* ctx, uint32_t pc, uint32_t raw, const char* name);

// Instruction hook: generated code calls exec<Op::X>(ctx, pc, raw) for every
// instruction it does not inline. Implementations live in runtime headers as
// specialisations; the primary template traps so missing ones are found.
template <Op O>
inline void exec(Context* ctx, uint32_t pc, uint32_t raw) {
    unimplemented(ctx, pc, raw, "exec<>");
}

// ---- Memory ------------------------------------------------------------------
// EE virtual map (kernel mode, no TLB use by games):
//   0x00000000-0x01FFFFFF RAM, mirrored at 0x20000000 (uncached) and 0x30000000 (UCAB)
//   0x70000000-0x70003FFF scratchpad
//   0x10000000-0x1FFFFFFF hardware registers / BIOS -> mmio
inline uint8_t* host_ptr(Context* ctx, uint32_t a) {
    const uint32_t seg = a >> 28;
    if ((seg == 0x0 || seg == 0x2 || seg == 0x3) && (a & 0x0FFFFFFF) < kRamSize) return ctx->ram + (a & (kRamSize - 1));
    if (seg == 0x7 && (a & 0x0FFFFFFF) < kSprSize) return ctx->spr + (a & (kSprSize - 1));
    if (seg == 0x8 || seg == 0xA) {  // kseg0/kseg1 mirrors of RAM
        if ((a & 0x1FFFFFFF) < kRamSize) return ctx->ram + (a & (kRamSize - 1));
    }
    return nullptr;
}

template <typename T>
inline T mem_read(Context* ctx, uint32_t a) {
    if (uint8_t* p = host_ptr(ctx, a)) { T v; std::memcpy(&v, p, sizeof v); return v; }
    return (T)mmio_read(ctx, a, (int)sizeof(T));
}
template <typename T>
inline void mem_write(Context* ctx, uint32_t a, T v) {
    if (uint8_t* p = host_ptr(ctx, a)) { std::memcpy(p, &v, sizeof v); return; }
    mmio_write(ctx, a, (uint64_t)v, (int)sizeof(T));
}
inline void mem_read128(Context* ctx, uint32_t a, u128* out) {
    a &= ~15u;
    if (uint8_t* p = host_ptr(ctx, a)) { std::memcpy(out, p, 16); return; }
    out->u64[0] = mem_read<uint64_t>(ctx, a);
    out->u64[1] = mem_read<uint64_t>(ctx, a + 8);
}
inline void mem_write128(Context* ctx, uint32_t a, const u128& v) {
    a &= ~15u;
    if (uint8_t* p = host_ptr(ctx, a)) { std::memcpy(p, &v, 16); return; }
    mem_write<uint64_t>(ctx, a, v.u64[0]);
    mem_write<uint64_t>(ctx, a + 8, v.u64[1]);
}

// Unaligned load/store pairs (little-endian MIPS semantics)
inline void lwl(Context* c, uint32_t a, u128& rt) {
    const uint32_t sh = (a & 3) * 8, w = mem_read<uint32_t>(c, a & ~3u);
    const uint32_t mask = 0x00FFFFFFu >> sh;
    rt.s64[0] = (int32_t)((rt.u32[0] & mask) | (w << (24 - sh)));
}
inline void lwr(Context* c, uint32_t a, u128& rt) {
    const uint32_t sh = (a & 3) * 8, w = mem_read<uint32_t>(c, a & ~3u);
    const uint32_t mask = sh ? (0xFFFFFF00u << (24 - sh)) : 0u;
    const uint32_t v = (rt.u32[0] & mask) | (w >> sh);
    if (sh == 0) rt.s64[0] = (int32_t)v; else rt.u32[0] = v;
}
inline void swl(Context* c, uint32_t a, const u128& rt) {
    const uint32_t sh = (a & 3) * 8, w = mem_read<uint32_t>(c, a & ~3u);
    const uint32_t mask = 0xFFFFFF00u << sh;
    mem_write<uint32_t>(c, a & ~3u, (w & mask) | (rt.u32[0] >> (24 - sh)));
}
inline void swr(Context* c, uint32_t a, const u128& rt) {
    const uint32_t sh = (a & 3) * 8, w = mem_read<uint32_t>(c, a & ~3u);
    const uint32_t mask = sh ? (0x00FFFFFFu >> (24 - sh)) : 0u;
    mem_write<uint32_t>(c, a & ~3u, (w & mask) | (rt.u32[0] << sh));
}
inline void ldl(Context* c, uint32_t a, u128& rt) {
    const uint32_t sh = (a & 7) * 8;
    const uint64_t d = mem_read<uint64_t>(c, a & ~7u);
    const uint64_t mask = 0x00FFFFFFFFFFFFFFull >> sh;
    rt.u64[0] = (rt.u64[0] & mask) | (d << (56 - sh));
}
inline void ldr(Context* c, uint32_t a, u128& rt) {
    const uint32_t sh = (a & 7) * 8;
    const uint64_t d = mem_read<uint64_t>(c, a & ~7u);
    const uint64_t mask = sh ? (0xFFFFFFFFFFFFFF00ull << (56 - sh)) : 0ull;
    rt.u64[0] = (rt.u64[0] & mask) | (d >> sh);
}
inline void sdl(Context* c, uint32_t a, const u128& rt) {
    const uint32_t sh = (a & 7) * 8;
    const uint64_t d = mem_read<uint64_t>(c, a & ~7u);
    const uint64_t mask = 0xFFFFFFFFFFFFFF00ull << sh;
    mem_write<uint64_t>(c, a & ~7u, (d & mask) | (rt.u64[0] >> (56 - sh)));
}
inline void sdr(Context* c, uint32_t a, const u128& rt) {
    const uint32_t sh = (a & 7) * 8;
    const uint64_t d = mem_read<uint64_t>(c, a & ~7u);
    const uint64_t mask = sh ? (0x00FFFFFFFFFFFFFFull >> (56 - sh)) : 0ull;
    mem_write<uint64_t>(c, a & ~7u, (d & mask) | (rt.u64[0] << sh));
}

// ---- Integer helpers -----------------------------------------------------------
inline void div32(Context* c, int pipe, int32_t n, int32_t d) {
    int32_t q, r;
    if (d == 0) { q = n < 0 ? 1 : -1; r = n; }
    else if (n == INT32_MIN && d == -1) { q = INT32_MIN; r = 0; }
    else { q = n / d; r = n % d; }
    c->lo.s64[pipe] = q;
    c->hi.s64[pipe] = r;
}
inline void divu32(Context* c, int pipe, uint32_t n, uint32_t d) {
    uint32_t q, r;
    if (d == 0) { q = 0xFFFFFFFFu; r = n; } else { q = n / d; r = n % d; }
    c->lo.s64[pipe] = (int32_t)q;
    c->hi.s64[pipe] = (int32_t)r;
}
inline void set_hilo64(Context* c, int pipe, uint64_t p) {
    c->lo.s64[pipe] = (int32_t)(uint32_t)p;
    c->hi.s64[pipe] = (int32_t)(uint32_t)(p >> 32);
}
inline uint64_t get_hilo64(Context* c, int pipe) {
    return ((uint64_t)c->hi.u32[pipe * 2] << 32) | c->lo.u32[pipe * 2];
}

// ---- FPU helpers (EE FPU: no NaN/Inf/denormals; results clamp to +-FLT_MAX) ---
inline float fclamp(float v) {
    uint32_t b; std::memcpy(&b, &v, 4);
    if ((b & 0x7F800000u) == 0x7F800000u) b = (b & 0x80000000u) | 0x7F7FFFFFu;  // Inf/NaN -> +-max
    else if ((b & 0x7F800000u) == 0) b &= 0x80000000u;                            // denormal -> 0
    std::memcpy(&v, &b, 4);
    return v;
}
inline float fadd(float a, float b) { return fclamp(a + b); }
inline float fsub(float a, float b) { return fclamp(a - b); }
inline float fmul(float a, float b) { return fclamp(a * b); }
inline float as_float(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
inline uint32_t as_u32(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
inline float fdiv(Context* c, float a, float b) {
    if (b == 0.0f) {  // divide-by-zero: signed max, set D flag
        c->fcr31 |= 0x00010000u;
        return as_float(((as_u32(a) ^ as_u32(b)) & 0x80000000u) | 0x7F7FFFFFu);
    }
    return fclamp(a / b);
}
inline float fsqrt(float a) { return std::sqrt(std::fabs(a)); }
inline float frsqrt(Context* c, float a, float b) { return fdiv(c, a, std::sqrt(std::fabs(b))); }
inline int32_t cvt_w_s(float a) {
    if (a >= 2147483648.0f) return INT32_MAX;
    if (a <= -2147483648.0f) return INT32_MIN;
    return (int32_t)a;   // truncate (EE ignores rounding mode for CVT.W)
}
constexpr uint32_t kFcrC = 1u << 23;   // FPU condition bit in FCR31

} // namespace ps2

// ---- Short names used by generated code ------------------------------------------
#define GPR(n) ctx->r[n]
#define U64(n) ctx->r[n].u64[0]
#define S64(n) ctx->r[n].s64[0]
#define U32(n) ctx->r[n].u32[0]
#define S32(n) ctx->r[n].s32[0]
#define FPR(n) ctx->f[n]
#define ADDR(base, off) ((uint32_t)(U32(base) + (int32_t)(off)))

// Optional function-entry tracing (build the game library with PS2_TRACE=1).
namespace ps2 { void trace_enter(Context* ctx, uint32_t addr); }
#if defined(PS2_TRACE) && PS2_TRACE
#define PS2_FN_ENTER(addr) ps2::trace_enter(ctx, addr)
#else
#define PS2_FN_ENTER(addr) ((void)0)
#endif

#include "ps2/exec_impl.hpp"
