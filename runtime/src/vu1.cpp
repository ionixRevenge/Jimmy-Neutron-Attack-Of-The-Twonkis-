// VU1 micro mode interpreter.
//
// Executes the microprograms RenderWare uploads with MPG (vertex transform,
// lighting, clipping) and sends their XGKICK output to the GIF as PATH1.
// Each 64-bit pair is an upper (FMAC) and a lower instruction. Modelled timing
// that hand-scheduled microcode depends on:
//   - branch delay slots, and the pair after an E-bit;
//   - upper and lower of one pair both read the registers as they were before it;
//   - Q is written 7 cycles after DIV/SQRT (13 after RSQRT); WAITQ waits;
//   - MAC/status/clip flags become visible to FM*/FS*/FC* 4 cycles after the FMAC op;
//   - a conditional branch sees the old value of an integer register written by
//     the immediately preceding pair.
// One pair = one cycle (stalls are not modelled). Encodings: VU User's Manual.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <cstdarg>
#include <filesystem>
#include <string>
#include <emmintrin.h>
#include <intrin.h>

#include "gpu.hpp"
#include "perf.hpp"
#include "ps2/runtime.hpp"

namespace ps2::gpu {

uint32_t vif1_top();

namespace {
inline int ctz32(uint32_t v) {
    unsigned long n;
    _BitScanForward(&n, v);
    return (int)n;
}
} // namespace
uint32_t vif1_itop();

namespace {

std::map<uint32_t, uint64_t> g_starts;
uint32_t g_last_start;   // where the running microprogram was started
uint64_t g_longest;      // most pairs a program has taken to end
uint64_t g_runaways;     // programs stopped at the pair limit
uint64_t g_pairs;        // pairs executed (statistics)
std::map<uint32_t, uint64_t> g_prog_pairs;   // pairs per start address (statistics)
uint64_t g_pc_hist[16 * 1024 / 8];          // TWONK_VU_HIST: pairs executed per pc
const bool g_hist_on = std::getenv("TWONK_VU_HIST") != nullptr;
bool g_aborted;          // the last program was stopped: MSCNT must not resume it

struct Vf { float f[4]; };

struct FlagSnap { uint64_t at; uint32_t mac, status, clip; bool has_clip; };   // has_clip: pushed by CLIP

// Flag results in flight. They are pushed in issue order with a fixed latency, so
// they land in order; at most one per cycle for 4 cycles is pending at a time.
struct FlagQueue {
    FlagSnap e[16];
    uint32_t head = 0, tail = 0;
    bool empty() const { return head == tail; }
    FlagSnap& front() { return e[head & 15]; }
    void pop_front() { ++head; }
    void push_back(const FlagSnap& f) { e[tail++ & 15] = f; }
    template <class F> void each(F fn) { for (uint32_t i = head; i != tail; ++i) fn(e[i & 15]); }
};

struct Vu {
    Vf vf[32];
    uint16_t vi[16];
    Vf acc;
    float q = 1, p = 0, i = 0;
    uint32_t r = 0x3F800000;
    uint32_t mac = 0, status = 0, clip = 0;           // values as written by the FMAC
    uint32_t vmac = 0, vstatus = 0, vclip = 0;        // values the lower unit can see
    FlagQueue flag_q;
    float q_pending = 0; uint64_t q_ready = 0; bool q_busy = false;
    uint64_t cycle = 0;
    uint64_t vf_ready[32] = {};   // cycle at which each VF register's pending write lands
    uint32_t pc = 0;
    // previous pair's integer write (for the branch hazard)
    int last_vi = -1; uint16_t last_vi_old = 0;
} U;

uint8_t* g_vumem;   // vu1().mem, cached by run(): vu1() lives in another translation unit
inline uint8_t* mem() { return g_vumem; }
inline float* memq(uint32_t qaddr) { return reinterpret_cast<float*>(mem() + (qaddr & 0x3FF) * 16); }
inline uint32_t* memw(uint32_t qaddr) { return reinterpret_cast<uint32_t*>(mem() + (qaddr & 0x3FF) * 16); }

inline uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float ffrom(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

// VU floats have no infinities/NaNs and flush denormals.
inline float vu_float(float f) {
    uint32_t u = fbits(f);
    const uint32_t e = (u >> 23) & 0xFF;
    if (e == 0) return ffrom(u & 0x80000000u);
    if (e == 0xFF) return ffrom((u & 0x80000000u) | 0x7F7FFFFFu);
    return f;
}
inline float rd(float f) { return vu_float(f); }

int comp_bit(int c) { return 3 - c; }   // dest bit for component c (x = bit 3)

// ---- flags -------------------------------------------------------------------------
// Flag results and Q land lazily: vmac/vstatus/vclip are brought up to date by
// sync_flags() just before something reads them, and Q by qv(). A value that has
// landed by the current cycle is folded in then, in issue order, which gives exactly
// what landing them every cycle would.
// Fields are written and read one at a time, and head/tail kept in locals: building
// an entry on the stack and copying it, or reading head and tail as one 8-byte word
// right after a 4-byte store of head, defeats store forwarding and stalls every op.
// `cyc`: the current cycle (compiled microprograms keep it in a local, see vu1_aot).
__forceinline void sync_flags_at(uint64_t cyc) {
    FlagQueue& q = U.flag_q;
    uint32_t h = q.head;
    const uint32_t t = q.tail;
    if (h == t) return;
    uint32_t vmac = U.vmac, vstatus = U.vstatus, vclip = U.vclip;
    for (; h != t; ++h) {
        const FlagSnap& f = q.e[h & 15];
        if (f.at > cyc) break;
        vmac = f.mac;
        vstatus = f.status;
        if (f.has_clip) vclip = f.clip;
    }
    q.head = h;
    U.vmac = vmac; U.vstatus = vstatus; U.vclip = vclip;
}

inline void sync_flags() { sync_flags_at(U.cycle); }

__forceinline void push_flags_at(uint64_t cyc, uint64_t at, uint32_t mac, uint32_t status, uint32_t clip, bool has_clip) {
    FlagQueue& q = U.flag_q;
    if (q.tail - q.head >= 12) sync_flags_at(cyc);   // at most 4 are still in flight
    FlagSnap& e = q.e[q.tail & 15];
    e.at = at; e.mac = mac; e.status = status; e.clip = clip; e.has_clip = has_clip;
    ++q.tail;
}

inline void push_flags(uint64_t at, uint32_t mac, uint32_t status, uint32_t clip, bool has_clip) {
    push_flags_at(U.cycle, at, mac, status, clip, has_clip);
}

__forceinline float qv_at(uint64_t cyc) {
    if (U.q_busy && cyc >= U.q_ready) { U.q = U.q_pending; U.q_busy = false; }
    return U.q;
}
inline float qv() { return qv_at(U.cycle); }

void update_flags(uint32_t dest, const float* res, bool write_flags) {
    if (!write_flags) return;
    uint32_t mac = 0;
    for (int c = 0; c < 4; ++c) {
        if (!(dest & (1u << comp_bit(c)))) continue;
        const uint32_t b = 3 - c;   // MAC: bit 3 = x ... bit 0 = w
        if (res[c] == 0.0f) mac |= 1u << b;
        if (std::signbit(res[c])) mac |= 1u << (b + 4);
    }
    U.mac = mac;
    uint32_t st = U.status & 0xFC0;   // sticky bits
    if (mac & 0x000F) st |= 0x001 | 0x040;
    if (mac & 0x00F0) st |= 0x002 | 0x080;
    U.status = (U.status & 0x030) | st;   // keep I/D (set by DIV)
    push_flags(U.cycle + 4, U.mac, U.status, 0, false);
}

inline void advance_cycle() { ++U.cycle; }

inline void finish_q() {
    if (U.q_busy) { U.q = U.q_pending; U.q_busy = false; }
}

__forceinline void start_q_at(uint64_t cyc, float v, int latency) {
    finish_q();   // a second divide waits for the first
    U.q_pending = vu_float(v);
    U.q_ready = cyc + latency;
    U.q_busy = true;
}
void start_q(float v, int latency) { start_q_at(U.cycle, v, latency); }

// ---- upper (FMAC) ------------------------------------------------------------------------
struct Upper {
    bool write = false;      // has a register result to commit
    int fd = 0;              // 0..31, or 32 = ACC
    uint32_t dest = 0;
    alignas(16) float res[4];
};

void upper(uint32_t w, Upper* out) {
    const uint32_t dest = (w >> 21) & 0xF, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31;
    const uint32_t funct = w & 0x3F;
    const Vf& S = U.vf[fs];
    const Vf& T = U.vf[ft];
    float res[4] = {};
    int target = (int)fd;
    bool flags = true;
    enum { Add, Sub, Mul, Madd, Msub, Max, Min } kind = Add;
    float b[4] = {T.f[0], T.f[1], T.f[2], T.f[3]};
    auto bc = [&](int c) { for (int k = 0; k < 4; ++k) b[k] = T.f[c]; };
    auto scalar = [&](float v) { for (int k = 0; k < 4; ++k) b[k] = v; };
    bool to_acc = false;
    uint32_t op = funct;
    if (funct >= 0x3C) {   // special2
        op = 0x100 | (fd << 2) | (funct & 3);
        to_acc = true;
        target = 32;
    }
    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03: bc(funct & 3); kind = Add; break;
    case 0x04: case 0x05: case 0x06: case 0x07: bc(funct & 3); kind = Sub; break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: bc(funct & 3); kind = Madd; break;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: bc(funct & 3); kind = Msub; break;
    case 0x10: case 0x11: case 0x12: case 0x13: bc(funct & 3); kind = Max; break;
    case 0x14: case 0x15: case 0x16: case 0x17: bc(funct & 3); kind = Min; break;
    case 0x18: case 0x19: case 0x1A: case 0x1B: bc(funct & 3); kind = Mul; break;
    case 0x1C: scalar(qv()); kind = Mul; break;
    case 0x1D: scalar(U.i); kind = Max; break;
    case 0x1E: scalar(U.i); kind = Mul; break;
    case 0x1F: scalar(U.i); kind = Min; break;
    case 0x20: scalar(qv()); kind = Add; break;
    case 0x21: scalar(qv()); kind = Madd; break;
    case 0x22: scalar(U.i); kind = Add; break;
    case 0x23: scalar(U.i); kind = Madd; break;
    case 0x24: scalar(qv()); kind = Sub; break;
    case 0x25: scalar(qv()); kind = Msub; break;
    case 0x26: scalar(U.i); kind = Sub; break;
    case 0x27: scalar(U.i); kind = Msub; break;
    case 0x28: kind = Add; break;
    case 0x29: kind = Madd; break;
    case 0x2A: kind = Mul; break;
    case 0x2B: kind = Max; break;
    case 0x2C: kind = Sub; break;
    case 0x2D: kind = Msub; break;
    case 0x2E: {   // OPMSUB: fd.xyz = acc.xyz - fs.yzx * ft.zxy
        const float m[3] = {S.f[1] * T.f[2], S.f[2] * T.f[0], S.f[0] * T.f[1]};
        for (int c = 0; c < 3; ++c) res[c] = vu_float(U.acc.f[c] - m[c]);
        res[3] = U.vf[fd].f[3];
        out->write = true; out->fd = (int)fd; out->dest = dest & 0xE; std::memcpy(out->res, res, 16);
        update_flags(dest & 0xE, res, true);
        return;
    }
    case 0x2F: kind = Min; break;
    // ---- special2 (accumulator / misc) ----
    case 0x100: case 0x101: case 0x102: case 0x103: bc(funct & 3); kind = Add; break;      // ADDAbc
    case 0x104: case 0x105: case 0x106: case 0x107: bc(funct & 3); kind = Sub; break;      // SUBAbc
    case 0x108: case 0x109: case 0x10A: case 0x10B: bc(funct & 3); kind = Madd; break;     // MADDAbc
    case 0x10C: case 0x10D: case 0x10E: case 0x10F: bc(funct & 3); kind = Msub; break;     // MSUBAbc
    case 0x110: case 0x111: case 0x112: case 0x113: {                                       // ITOF0/4/12/15
        static const float div[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
        for (int c = 0; c < 4; ++c) res[c] = (float)(int32_t)fbits(S.f[c]) / div[op & 3];
        out->write = true; out->fd = (int)ft; out->dest = dest; std::memcpy(out->res, res, 16);
        return;
    }
    case 0x114: case 0x115: case 0x116: case 0x117: {                                       // FTOI0/4/12/15
        static const double mul[4] = {1.0, 16.0, 4096.0, 32768.0};
        for (int c = 0; c < 4; ++c) {
            const double v = (double)S.f[c] * mul[op & 3];
            const int32_t iv = v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : (int32_t)v;
            res[c] = ffrom((uint32_t)iv);
        }
        out->write = true; out->fd = (int)ft; out->dest = dest; std::memcpy(out->res, res, 16);
        return;
    }
    case 0x118: case 0x119: case 0x11A: case 0x11B: bc(funct & 3); kind = Mul; break;      // MULAbc
    case 0x11C: scalar(qv()); kind = Mul; break;                                            // MULAq
    case 0x11D:                                                                              // ABS
        for (int c = 0; c < 4; ++c) res[c] = std::fabs(S.f[c]);
        out->write = true; out->fd = (int)ft; out->dest = dest; std::memcpy(out->res, res, 16);
        return;
    case 0x11E: scalar(U.i); kind = Mul; break;                                            // MULAi
    case 0x11F: {                                                                            // CLIP
        const float wv = std::fabs(T.f[3]);
        uint32_t f = 0;
        if (S.f[0] > wv) f |= 1; if (S.f[0] < -wv) f |= 2;
        if (S.f[1] > wv) f |= 4; if (S.f[1] < -wv) f |= 8;
        if (S.f[2] > wv) f |= 16; if (S.f[2] < -wv) f |= 32;
        U.clip = ((U.clip << 6) | f) & 0xFFFFFF;
        push_flags(U.cycle + 4, U.mac, U.status, U.clip, true);
        return;
    }
    case 0x120: scalar(qv()); kind = Add; break;   // ADDAq
    case 0x121: scalar(qv()); kind = Madd; break;  // MADDAq
    case 0x122: scalar(U.i); kind = Add; break;   // ADDAi
    case 0x123: scalar(U.i); kind = Madd; break;  // MADDAi
    case 0x124: scalar(qv()); kind = Sub; break;   // SUBAq
    case 0x125: scalar(qv()); kind = Msub; break;  // MSUBAq
    case 0x126: scalar(U.i); kind = Sub; break;   // SUBAi
    case 0x127: scalar(U.i); kind = Msub; break;  // MSUBAi
    case 0x128: kind = Add; break;                // ADDA
    case 0x129: kind = Madd; break;               // MADDA
    case 0x12A: kind = Mul; break;                // MULA
    case 0x12C: kind = Sub; break;                // SUBA
    case 0x12D: kind = Msub; break;               // MSUBA
    case 0x12E: {                                 // OPMULA: acc.xyz = fs.yzx * ft.zxy
        res[0] = vu_float(S.f[1] * T.f[2]); res[1] = vu_float(S.f[2] * T.f[0]); res[2] = vu_float(S.f[0] * T.f[1]);
        res[3] = U.acc.f[3];
        out->write = true; out->fd = 32; out->dest = dest & 0xE; std::memcpy(out->res, res, 16);
        update_flags(dest & 0xE, res, true);
        return;
    }
    case 0x12F: return;                           // NOP
    default: {
        static unsigned warned;
        if (warned++ < 8) log(Log::Gpu, "VU1: unknown upper op %08x at %04x", w, U.pc);
        return;
    }
    }
    (void)to_acc;
    for (int c = 0; c < 4; ++c) {
        const float a = rd(S.f[c]), bv = rd(b[c]);
        switch (kind) {
        case Add: res[c] = a + bv; break;
        case Sub: res[c] = a - bv; break;
        case Mul: res[c] = a * bv; break;
        case Madd: res[c] = U.acc.f[c] + a * bv; break;
        case Msub: res[c] = U.acc.f[c] - a * bv; break;
        case Max: res[c] = std::max(a, bv); flags = false; break;
        case Min: res[c] = std::min(a, bv); flags = false; break;
        }
        res[c] = vu_float(res[c]);
    }
    out->write = true; out->fd = target; out->dest = dest; std::memcpy(out->res, res, 16);
    update_flags(dest, res, flags);
}

// Lane masks for a dest field (bit 3 = x): all-ones in each written lane.
struct DestMasks {
    alignas(16) uint32_t m[16][4];
    DestMasks() { for (int d = 0; d < 16; ++d) for (int c = 0; c < 4; ++c) m[d][c] = (d & (8 >> c)) ? ~0u : 0u; }
};
const DestMasks g_dest_masks;
inline __m128 dest_mask(uint32_t dest) { return _mm_load_ps(reinterpret_cast<const float*>(g_dest_masks.m[dest & 15])); }

void commit(const Upper& u) {
    if (!u.write) return;
    if (u.fd == 0) return;   // vf0 is constant
    float* d = (u.fd == 32 ? U.acc : U.vf[u.fd]).f;
    const __m128 m = dest_mask(u.dest);
    _mm_storeu_ps(d, _mm_or_ps(_mm_and_ps(m, _mm_load_ps(u.res)), _mm_andnot_ps(m, _mm_loadu_ps(d))));
}

// ---- decoded FMAC fast path ---------------------------------------------------------------
// The arithmetic upper ops (ADD/SUB/MUL/MADD/MSUB/MAX/MIN in all their bc/q/i/acc
// forms) are decoded once per pair and run four lanes at a time. Results and flags
// are bit-identical to upper(): same clamps, same operation order, no contraction.
enum class UKind : uint8_t { Slow, Nop, Add, Sub, Mul, Madd, Msub, Max, Min };
enum : uint8_t { kSrcVec = 4, kSrcQ = 5, kSrcI = 6 };

struct UDec { UKind kind; uint8_t fs, ft, target, dest, src; };

UDec decode_upper(uint32_t w) {
    const uint32_t dest = (w >> 21) & 0xF, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31;
    const uint32_t funct = w & 0x3F;
    UDec d{UKind::Slow, (uint8_t)fs, (uint8_t)ft, (uint8_t)fd, (uint8_t)dest, kSrcVec};
    uint32_t op = funct;
    if (funct >= 0x3C) { op = 0x100 | (fd << 2) | (funct & 3); d.target = 32; }
    const uint8_t bc = (uint8_t)(funct & 3);
    auto set = [&](UKind k, uint8_t src) { d.kind = k; d.src = src; };
    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03: set(UKind::Add, bc); break;
    case 0x04: case 0x05: case 0x06: case 0x07: set(UKind::Sub, bc); break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: set(UKind::Madd, bc); break;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: set(UKind::Msub, bc); break;
    case 0x10: case 0x11: case 0x12: case 0x13: set(UKind::Max, bc); break;
    case 0x14: case 0x15: case 0x16: case 0x17: set(UKind::Min, bc); break;
    case 0x18: case 0x19: case 0x1A: case 0x1B: set(UKind::Mul, bc); break;
    case 0x1C: set(UKind::Mul, kSrcQ); break;
    case 0x1D: set(UKind::Max, kSrcI); break;
    case 0x1E: set(UKind::Mul, kSrcI); break;
    case 0x1F: set(UKind::Min, kSrcI); break;
    case 0x20: set(UKind::Add, kSrcQ); break;
    case 0x21: set(UKind::Madd, kSrcQ); break;
    case 0x22: set(UKind::Add, kSrcI); break;
    case 0x23: set(UKind::Madd, kSrcI); break;
    case 0x24: set(UKind::Sub, kSrcQ); break;
    case 0x25: set(UKind::Msub, kSrcQ); break;
    case 0x26: set(UKind::Sub, kSrcI); break;
    case 0x27: set(UKind::Msub, kSrcI); break;
    case 0x28: set(UKind::Add, kSrcVec); break;
    case 0x29: set(UKind::Madd, kSrcVec); break;
    case 0x2A: set(UKind::Mul, kSrcVec); break;
    case 0x2B: set(UKind::Max, kSrcVec); break;
    case 0x2C: set(UKind::Sub, kSrcVec); break;
    case 0x2D: set(UKind::Msub, kSrcVec); break;
    case 0x2F: set(UKind::Min, kSrcVec); break;
    case 0x100: case 0x101: case 0x102: case 0x103: set(UKind::Add, bc); break;
    case 0x104: case 0x105: case 0x106: case 0x107: set(UKind::Sub, bc); break;
    case 0x108: case 0x109: case 0x10A: case 0x10B: set(UKind::Madd, bc); break;
    case 0x10C: case 0x10D: case 0x10E: case 0x10F: set(UKind::Msub, bc); break;
    case 0x118: case 0x119: case 0x11A: case 0x11B: set(UKind::Mul, bc); break;
    case 0x11C: set(UKind::Mul, kSrcQ); break;
    case 0x11E: set(UKind::Mul, kSrcI); break;
    case 0x120: set(UKind::Add, kSrcQ); break;
    case 0x121: set(UKind::Madd, kSrcQ); break;
    case 0x122: set(UKind::Add, kSrcI); break;
    case 0x123: set(UKind::Madd, kSrcI); break;
    case 0x124: set(UKind::Sub, kSrcQ); break;
    case 0x125: set(UKind::Msub, kSrcQ); break;
    case 0x126: set(UKind::Sub, kSrcI); break;
    case 0x127: set(UKind::Msub, kSrcI); break;
    case 0x128: set(UKind::Add, kSrcVec); break;
    case 0x129: set(UKind::Madd, kSrcVec); break;
    case 0x12A: set(UKind::Mul, kSrcVec); break;
    case 0x12C: set(UKind::Sub, kSrcVec); break;
    case 0x12D: set(UKind::Msub, kSrcVec); break;
    case 0x12F: d.kind = UKind::Nop; break;
    default: break;   // OPMULA/OPMSUB, ITOF/FTOI, ABS, CLIP, unknown: upper()
    }
    return d;
}

// vu_float() on four lanes: denormals to signed zero, Inf/NaN to signed max.
inline __m128 vu_float4(__m128 x) {
    const __m128i u = _mm_castps_si128(x);
    const __m128i e = _mm_and_si128(u, _mm_set1_epi32(0x7F800000));
    const __m128i sign = _mm_and_si128(u, _mm_set1_epi32((int)0x80000000u));
    const __m128i den = _mm_cmpeq_epi32(e, _mm_setzero_si128());
    const __m128i inf = _mm_cmpeq_epi32(e, _mm_set1_epi32(0x7F800000));
    __m128i r = _mm_or_si128(_mm_andnot_si128(den, u), _mm_and_si128(den, sign));
    r = _mm_or_si128(_mm_andnot_si128(inf, r), _mm_and_si128(inf, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF))));
    return _mm_castsi128_ps(r);
}

// movemask order (bit 0 = x) to MAC order (bit 3 = x)
constexpr uint8_t kRev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

__forceinline void upper_fast(const UDec& d, Upper* out, bool push = true) {
    const __m128 a = vu_float4(_mm_loadu_ps(U.vf[d.fs].f));
    __m128 b;
    switch (d.src) {
    case kSrcVec: b = _mm_loadu_ps(U.vf[d.ft].f); break;
    case kSrcQ: b = _mm_set1_ps(qv()); break;
    case kSrcI: b = _mm_set1_ps(U.i); break;
    default: b = _mm_set1_ps(U.vf[d.ft].f[d.src]); break;
    }
    b = vu_float4(b);
    __m128 r;
    bool flags = true;
    switch (d.kind) {
    case UKind::Add: r = _mm_add_ps(a, b); break;
    case UKind::Sub: r = _mm_sub_ps(a, b); break;
    case UKind::Mul: r = _mm_mul_ps(a, b); break;
    case UKind::Madd: r = _mm_add_ps(_mm_loadu_ps(U.acc.f), _mm_mul_ps(a, b)); break;
    case UKind::Msub: r = _mm_sub_ps(_mm_loadu_ps(U.acc.f), _mm_mul_ps(a, b)); break;
    case UKind::Max: r = _mm_max_ps(b, a); flags = false; break;   // std::max(a, b) = a < b ? b : a
    default: r = _mm_min_ps(b, a); flags = false; break;           // std::min(a, b) = b < a ? b : a
    }
    r = vu_float4(r);
    _mm_store_ps(out->res, r);
    out->write = true; out->fd = d.target; out->dest = d.dest;
    if (!flags) return;
    const uint32_t zero = kRev4[_mm_movemask_ps(_mm_cmpeq_ps(r, _mm_setzero_ps()))];
    const uint32_t neg = kRev4[_mm_movemask_ps(r)];
    const uint32_t mac = (zero & d.dest) | ((neg & d.dest) << 4);
    U.mac = mac;
    uint32_t st = U.status & 0xFC0;   // sticky bits
    if (mac & 0x000F) st |= 0x001 | 0x040;
    if (mac & 0x00F0) st |= 0x002 | 0x080;
    U.status = (U.status & 0x030) | st;   // keep I/D (set by DIV)
    if (push) push_flags(U.cycle + 4, U.mac, U.status, 0, false);
}

// ---- lower ------------------------------------------------------------------------------
bool g_replaying;   // re-running a runaway program for its trace: no side effects
FILE* g_replay_log;

bool g_bench_mode;   // replaying a benchmark snapshot: no XGKICK output
uint32_t g_bench_top_v, g_bench_itop_v;

void xgkick(Context* ctx, uint32_t addr) {
    if (g_replaying || g_bench_mode) return;
    // walk GIF tags in VU1 memory until EOP, then hand the packet to PATH1
    uint32_t a = addr & 0x3FF;
    for (int guard = 0; guard < 1024; ++guard) {
        const uint64_t* tag = reinterpret_cast<const uint64_t*>(mem() + a * 16);
        const uint32_t nloop = (uint32_t)(tag[0] & 0x7FFF), flg = (uint32_t)((tag[0] >> 58) & 3);
        uint32_t nreg = (uint32_t)((tag[0] >> 60) & 0xF);
        if (!nreg) nreg = 16;
        const bool eop = (tag[0] >> 15) & 1;
        const uint32_t qw = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        for (uint32_t i = 0; i <= qw;) {   // the tag and its data, in runs up to where memory wraps
            const uint32_t at = (a + i) & 0x3FF, run = std::min(qw + 1 - i, 0x400 - at);
            gif_write(Path::P1, mem() + at * 16, run);
            i += run;
        }
        a = (a + qw + 1) & 0x3FF;
        if (eop) break;
    }
    (void)ctx;
}

void set_vi(int r, uint16_t v) {
    if (r == 0) return;
    U.last_vi = r;
    U.last_vi_old = U.vi[r];
    U.vi[r] = v;
}

// Flag reads (FMAND, FCAND, ...) are visible to a branch in the very next pair:
// microcode tests a flag and branches on it straight away (as PCSX2's microVU
// models it, the old-value delay applies to integer ALU and load results only).
void set_vi_flag(int r, uint16_t v) {
    if (r == 0) return;
    U.vi[r] = v;
}

// Returns the branch target, or ~0u when the lower op is not a taken branch.
uint32_t lower(Context* ctx, uint32_t w, uint32_t pc, int prev_vi, uint16_t prev_old) {
    const uint32_t op = w >> 25, dest = (w >> 21) & 0xF, it = (w >> 16) & 31, is = (w >> 11) & 31;
    const int32_t imm11 = ((int32_t)(w << 21)) >> 21;
    const uint32_t target = (uint32_t)((int32_t)pc + 8 + imm11 * 8) & (kVu1MicroSize - 1);
    auto vi = [&](uint32_t r) -> uint16_t { return (uint16_t)(r & 15 ? U.vi[r & 15] : 0); };
    auto bvi = [&](uint32_t r) -> uint16_t {   // branch read: old value if the previous pair wrote it
        r &= 15;
        if (!r) return 0;
        return (int)r == prev_vi ? prev_old : U.vi[r];
    };
    auto store = [&](uint32_t qaddr, const Vf& v) {
        float* m = memq(qaddr);
        for (int c = 0; c < 4; ++c) if (dest & (1u << comp_bit(c))) m[c] = v.f[c];
    };
    auto load = [&](uint32_t reg, uint32_t qaddr) {
        if (!reg) return;
        const float* m = memq(qaddr);
        for (int c = 0; c < 4; ++c) if (dest & (1u << comp_bit(c))) U.vf[reg].f[c] = m[c];
    };
    auto comp_of = [&](uint32_t d) { return d & 8 ? 0 : d & 4 ? 1 : d & 2 ? 2 : 3; };
    if (op >= 0x10 && op <= 0x1C) sync_flags();   // FC*/FS*/FM*: read (or set) the visible flags
    switch (op) {
    case 0x00: load(it, vi(is) + imm11); return ~0u;                                        // LQ
    case 0x01: store(vi(it) + imm11, U.vf[is]); return ~0u;                                // SQ
    case 0x04: set_vi((int)(it & 15), (uint16_t)memw(vi(is) + imm11)[comp_of(dest)]); return ~0u;   // ILW
    case 0x05: {                                                                           // ISW
        uint32_t* m = memw(vi(is) + imm11);
        for (int c = 0; c < 4; ++c) if (dest & (1u << comp_bit(c))) m[c] = vi(it);
        return ~0u;
    }
    case 0x08: set_vi((int)(it & 15), (uint16_t)(vi(is) + ((w & 0x7FF) | ((w >> 10) & 0x7800)))); return ~0u;   // IADDIU
    case 0x09: set_vi((int)(it & 15), (uint16_t)(vi(is) - ((w & 0x7FF) | ((w >> 10) & 0x7800)))); return ~0u;   // ISUBIU
    case 0x10: set_vi_flag(1, (U.vclip & 0xFFFFFF) == (w & 0xFFFFFF)); return ~0u;               // FCEQ
    case 0x11:                                                                               // FCSET
        // sets the flag outright: a CLIP still in flight must not land on top of it
        U.clip = U.vclip = w & 0xFFFFFF;
        U.flag_q.each([](FlagSnap& f) { f.has_clip = false; });
        return ~0u;
    case 0x12: set_vi_flag(1, (U.vclip & w & 0xFFFFFF) != 0); return ~0u;                        // FCAND
    case 0x13: set_vi_flag(1, ((U.vclip | w) & 0xFFFFFF) == 0xFFFFFF); return ~0u;               // FCOR
    case 0x14: set_vi_flag((int)(it & 15), (U.vstatus & 0xFFF) == ((w & 0x7FF) | ((w >> 10) & 0x800))); return ~0u;   // FSEQ
    case 0x15: U.status = (U.status & 0x3F) | (((w & 0x7FF) | ((w >> 10) & 0x800)) & 0xFC0); return ~0u;          // FSSET
    case 0x16: set_vi_flag((int)(it & 15), (uint16_t)(U.vstatus & ((w & 0x7FF) | ((w >> 10) & 0x800)))); return ~0u;  // FSAND
    case 0x17: set_vi_flag((int)(it & 15), (uint16_t)(U.vstatus | ((w & 0x7FF) | ((w >> 10) & 0x800)))); return ~0u;  // FSOR
    case 0x18: set_vi_flag((int)(it & 15), (U.vmac & 0xFFFF) == vi(is)); return ~0u;            // FMEQ
    case 0x1A: set_vi_flag((int)(it & 15), (uint16_t)(U.vmac & vi(is))); return ~0u;             // FMAND
    case 0x1B: set_vi_flag((int)(it & 15), (uint16_t)(U.vmac | vi(is))); return ~0u;             // FMOR
    case 0x1C: set_vi_flag((int)(it & 15), (uint16_t)(U.vclip & 0xFFF)); return ~0u;             // FCGET
    case 0x20: return target;                                                               // B
    case 0x21: set_vi((int)(it & 15), (uint16_t)((pc + 16) / 8)); return target;            // BAL
    case 0x24: return (uint32_t)(bvi(is) * 8) & (kVu1MicroSize - 1);                        // JR
    case 0x25: {                                                                            // JALR
        const uint32_t t = (uint32_t)(bvi(is) * 8) & (kVu1MicroSize - 1);
        set_vi((int)(it & 15), (uint16_t)((pc + 16) / 8));
        return t;
    }
    case 0x28: return bvi(it) == bvi(is) ? target : ~0u;                                    // IBEQ
    case 0x29: return bvi(it) != bvi(is) ? target : ~0u;                                    // IBNE
    case 0x2C: return (int16_t)bvi(is) < 0 ? target : ~0u;                                  // IBLTZ
    case 0x2D: return (int16_t)bvi(is) > 0 ? target : ~0u;                                  // IBGTZ
    case 0x2E: return (int16_t)bvi(is) <= 0 ? target : ~0u;                                 // IBLEZ
    case 0x2F: return (int16_t)bvi(is) >= 0 ? target : ~0u;                                 // IBGEZ
    case 0x40: break;
    default: {
        static unsigned warned;
        if (warned++ < 8) log(Log::Gpu, "VU1: unknown lower op %08x at %04x", w, pc);
        return ~0u;
    }
    }
    // LowerOP
    const uint32_t funct = w & 0x3F, id = (w >> 6) & 31;
    switch (funct) {
    case 0x30: set_vi((int)(id & 15), (uint16_t)(vi(is) + vi(it))); return ~0u;   // IADD
    case 0x31: set_vi((int)(id & 15), (uint16_t)(vi(is) - vi(it))); return ~0u;   // ISUB
    case 0x32: set_vi((int)(it & 15), (uint16_t)(vi(is) + (((int32_t)(id << 27)) >> 27))); return ~0u;   // IADDI
    case 0x34: set_vi((int)(id & 15), (uint16_t)(vi(is) & vi(it))); return ~0u;   // IAND
    case 0x35: set_vi((int)(id & 15), (uint16_t)(vi(is) | vi(it))); return ~0u;   // IOR
    default: break;
    }
    if (funct < 0x3C) {
        static unsigned warned;
        if (warned++ < 8) log(Log::Gpu, "VU1: unknown lower op %08x at %04x", w, pc);
        return ~0u;
    }
    const uint32_t sop = (id << 2) | (funct & 3);
    const uint32_t fsf = (w >> 21) & 3, ftf = (w >> 23) & 3;
    const Vf& S = U.vf[is];
    switch (sop) {
    case 0x30: if (it) for (int c = 0; c < 4; ++c) if (dest & (1u << comp_bit(c))) U.vf[it].f[c] = S.f[c]; return ~0u;   // MOVE
    case 0x31: if (it) { const Vf s = S; for (int c = 0; c < 4; ++c) if (dest & (1u << comp_bit(c))) U.vf[it].f[c] = s.f[(c + 1) & 3]; } return ~0u;   // MR32
    case 0x34: load(it, vi(is)); set_vi((int)(is & 15), (uint16_t)(vi(is) + 1)); return ~0u;   // LQI
    case 0x35: store(vi(it), S); set_vi((int)(it & 15), (uint16_t)(vi(it) + 1)); return ~0u;   // SQI
    case 0x36: set_vi((int)(is & 15), (uint16_t)(vi(is) - 1)); load(it, vi(is)); return ~0u;   // LQD
    case 0x37: set_vi((int)(it & 15), (uint16_t)(vi(it) - 1)); store(vi(it), S); return ~0u;   // SQD
    case 0x38: {                                                                             // DIV
        const float a = S.f[fsf], b = U.vf[it].f[ftf];
        U.status &= ~0x30u;
        if (b == 0.0f) { U.status |= 0x820; start_q(std::copysign(3.4028235e38f, a) * (std::signbit(b) ? -1.0f : 1.0f), 7); }
        else start_q(a / b, 7);
        return ~0u;
    }
    case 0x39: start_q(std::sqrt(std::fabs(U.vf[it].f[ftf])), 7); return ~0u;               // SQRT
    case 0x3A: {                                                                             // RSQRT
        const float b = std::sqrt(std::fabs(U.vf[it].f[ftf]));
        start_q(b == 0.0f ? 3.4028235e38f : S.f[fsf] / b, 13);
        return ~0u;
    }
    case 0x3B: finish_q(); return ~0u;                                                       // WAITQ
    case 0x3C: set_vi((int)(it & 15), (uint16_t)fbits(S.f[fsf])); return ~0u;               // MTIR
    case 0x3D: if (it) for (int c = 0; c < 4; ++c) if (dest & (1u << comp_bit(c))) U.vf[it].f[c] = ffrom((uint32_t)(int32_t)(int16_t)vi(is)); return ~0u;   // MFIR
    case 0x3E: set_vi((int)(it & 15), (uint16_t)memw(vi(is))[comp_of(dest)]); return ~0u;  // ILWR
    case 0x3F: {                                                                             // ISWR
        uint32_t* m = memw(vi(is));
        for (int c = 0; c < 4; ++c) if (dest & (1u << comp_bit(c))) m[c] = vi(it);
        return ~0u;
    }
    case 0x40: case 0x41: {                                                                  // RNEXT / RGET
        if (sop == 0x40) {
            const uint32_t x = U.r;
            const uint32_t bit = ((x >> 4) ^ (x >> 22)) & 1;
            U.r = 0x3F800000u | (((x << 1) | bit) & 0x7FFFFF);
        }
        if (it) for (int c = 0; c < 4; ++c) if (dest & (1u << comp_bit(c))) U.vf[it].f[c] = ffrom(U.r);
        return ~0u;
    }
    case 0x42: U.r = 0x3F800000u | (fbits(S.f[fsf]) & 0x7FFFFF); return ~0u;               // RINIT
    case 0x43: U.r = 0x3F800000u | ((U.r ^ fbits(S.f[fsf])) & 0x7FFFFF); return ~0u;       // RXOR
    case 0x64: if (it) for (int c = 0; c < 4; ++c) if (dest & (1u << comp_bit(c))) U.vf[it].f[c] = U.p; return ~0u;   // MFP
    case 0x68: set_vi((int)(it & 15), (uint16_t)(g_bench_mode ? g_bench_top_v : vif1_top())); return ~0u;                    // XTOP
    case 0x69: set_vi((int)(it & 15), (uint16_t)(g_bench_mode ? g_bench_itop_v : vif1_itop())); return ~0u;                   // XITOP
    case 0x6C: xgkick(ctx, vi(is)); return ~0u;                                              // XGKICK
    case 0x70: U.p = vu_float(S.f[0] * S.f[0] + S.f[1] * S.f[1] + S.f[2] * S.f[2]); return ~0u;   // ESADD
    case 0x71: { const float v = S.f[0] * S.f[0] + S.f[1] * S.f[1] + S.f[2] * S.f[2]; U.p = vu_float(1.0f / v); return ~0u; }   // ERSADD
    case 0x72: U.p = std::sqrt(S.f[0] * S.f[0] + S.f[1] * S.f[1] + S.f[2] * S.f[2]); return ~0u;   // ELENG
    case 0x73: U.p = vu_float(1.0f / std::sqrt(S.f[0] * S.f[0] + S.f[1] * S.f[1] + S.f[2] * S.f[2])); return ~0u;   // ERLENG
    case 0x74: U.p = std::atan2(S.f[1], S.f[0]); return ~0u;                                // EATANxy
    case 0x75: U.p = std::atan2(S.f[2], S.f[0]); return ~0u;                                // EATANxz
    case 0x76: U.p = vu_float(S.f[0] + S.f[1] + S.f[2] + S.f[3]); return ~0u;               // ESUM
    case 0x78: U.p = std::sqrt(std::fabs(S.f[fsf])); return ~0u;                            // ESQRT
    case 0x79: U.p = vu_float(1.0f / std::sqrt(std::fabs(S.f[fsf]))); return ~0u;           // ERSQRT
    case 0x7A: U.p = vu_float(1.0f / S.f[fsf]); return ~0u;                                 // ERCPR
    case 0x7B: return ~0u;                                                                   // WAITP
    case 0x7C: U.p = std::sin(S.f[fsf]); return ~0u;                                        // ESIN
    case 0x7D: U.p = std::atan(S.f[fsf]); return ~0u;                                       // EATAN
    case 0x7E: U.p = std::exp(-S.f[fsf]); return ~0u;                                       // EEXP
    default: {
        static unsigned warned;
        if (warned++ < 8) log(Log::Gpu, "VU1: unknown lower op %08x at %04x", w, pc);
        return ~0u;
    }
    }
}

// ---- register stalls -----------------------------------------------------------------
// A VF result (FMAC op, or a lower load/move) can be read 4 cycles after the pair
// that writes it; a pair that reads it sooner stalls. Stalls matter beyond timing:
// flags become visible 4 cycles after their op, so without them FM*/FC* tests that
// follow a stall read stale flags (RenderWare's clipper then loops forever).
constexpr int kFmacLatency = 4;

// VF registers the upper instruction `w` reads, as a bit mask.
uint32_t upper_reads(uint32_t w) {
    const uint32_t ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, funct = w & 0x3F;
    const uint32_t op = funct >= 0x3C ? (0x100 | (fd << 2) | (funct & 3)) : funct;
    if (op == 0x12F || op >= 0x130) return 0;                          // NOP / unknown
    bool t = true;
    if ((op >= 0x1C && op <= 0x27) || (op >= 0x11C && op <= 0x127 && op != 0x11F)) t = false;   // Q/I forms
    if ((op >= 0x110 && op <= 0x117) || op == 0x11D) t = false;       // ITOF/FTOI/ABS read fs only
    return (1u << fs) | (t ? 1u << ft : 0u);
}

// VF registers the lower instruction `w` reads.
uint32_t lower_reads(uint32_t w) {
    const uint32_t op = w >> 25, it = (w >> 16) & 31, is = (w >> 11) & 31;
    if (op == 0x01) return 1u << is;                                   // SQ
    if (op != 0x40 || (w & 0x3F) < 0x3C) return 0;
    const uint32_t sop = (((w >> 6) & 31) << 2) | (w & 3);
    switch (sop) {
    case 0x30: case 0x31: case 0x35: case 0x37: case 0x3C: return 1u << is;   // MOVE MR32 SQI SQD MTIR
    case 0x38: case 0x3A: return (1u << is) | (1u << it);                     // DIV RSQRT
    case 0x39: return 1u << it;                                               // SQRT
    default: return (sop >= 0x70 && sop <= 0x7E) ? 1u << is : 0u;             // EFU ops
    }
}

// VF register the lower instruction `w` writes, or 0.
uint32_t lower_writes(uint32_t w) {
    const uint32_t op = w >> 25, it = (w >> 16) & 31;
    if (op == 0x00) return it;                                         // LQ
    if (op != 0x40 || (w & 0x3F) < 0x3C) return 0;
    const uint32_t sop = (((w >> 6) & 31) << 2) | (w & 3);
    switch (sop) {
    case 0x30: case 0x31: case 0x34: case 0x36: case 0x3D: case 0x40: case 0x41: case 0x64: return it;
    default: return 0;
    }
}

// Read/write sets per pair, cached on the pair's words (micro memory can be rewritten).
struct PairInfo { uint32_t lo, hi, reads, writes; bool valid; UDec up; };
PairInfo g_pair_info[kVu1MicroSize / 8];

const PairInfo& pair_info(uint32_t pc, uint32_t lo, uint32_t hi) {
    PairInfo& p = g_pair_info[(pc & (kVu1MicroSize - 1)) / 8];
    if (!p.valid || p.lo != lo || p.hi != hi) {
        const bool ibit = (hi & 0x80000000u) != 0;
        p = {lo, hi, upper_reads(hi) | (ibit ? 0u : lower_reads(lo)), ibit ? 0u : lower_writes(lo), true, decode_upper(hi)};
    }
    return p;
}

void stall_for(uint32_t reads) {
    reads &= ~1u;   // vf0 is constant
    uint64_t need = 0;
    for (uint32_t m = reads; m; m &= m - 1) need = std::max(need, U.vf_ready[ctz32(m)]);
    if (U.cycle >= need) return;
    U.cycle = need - 1;   // the flag queue and Q only depend on the cycle reached
    advance_cycle();
}

// $TWONK_VU_TRACE=<start pc hex>: snapshot VU1 at each start of that program; on
// its first runaway, replay it from the snapshot and log every pair to vu1_replay.txt.
struct Snapshot { bool valid = false; Vu u; uint8_t mem[kVu1MemSize]; };
Snapshot* g_snap;
uint32_t g_trace_pc = ~0u;
uint32_t g_micro_gen = 1;   // counts micro memory changes (vu1_aot)
const bool g_vu_dump = std::getenv("TWONK_VU_DUMP") != nullptr;
constexpr uint32_t kJumpTarget = 0x10000;   // dump_note(): a JR/JALR target, not a program start
void dump_note(uint32_t v);
void aot_micro_changed(uint32_t first_pair, uint32_t npairs);   // vu1_aot: forget lookups there

void run(Context* ctx);

void replay_runaway(Context* ctx) {
    static bool done;
    if (done || !g_snap || !g_snap->valid) return;
    done = true;
    const Vu end_state = U;
    std::vector<uint8_t> end_mem(vu1().mem, vu1().mem + kVu1MemSize);
    U = g_snap->u;
    std::memcpy(vu1().mem, g_snap->mem, kVu1MemSize);
    g_replay_log = std::fopen("vu1_replay.txt", "w");
    g_replaying = true;
    run(ctx);
    g_replaying = false;
    if (g_replay_log) std::fclose(g_replay_log);
    if (FILE* f = std::fopen("vu1_replay.bin", "wb")) {   // micro, then data memory at the start
        std::fwrite(vu1().micro, 1, kVu1MicroSize, f);
        std::fwrite(g_snap->mem, 1, kVu1MemSize, f);
        std::fclose(f);
    }
    U = end_state;
    std::memcpy(vu1().mem, end_mem.data(), kVu1MemSize);
    std::fprintf(stderr, "[gpu] VU1: replayed the runaway into vu1_replay.txt\n");
}

// ---- decoded pairs (the fast path) ---------------------------------------------------------
// Each pair is decoded once and cached by address; the cache entry is checked against
// the pair's words, since MPG rewrites micro memory. The common lower ops run inline
// here; the rest go through lower(). Results, flags and cycle counts are identical to
// run_ref() (the original loop, kept for TWONK_VU_SLOW=1); `--vu-bench` checks that.
enum class LK : uint8_t {
    Slow, Nop, LQ, SQ, ILW, ISW, IADDIU, ISUBIU, FCEQ, FCSET, FCAND, FCOR, FMEQ, FMAND, FMOR, FCGET,
    B, BAL, JR, JALR, IBEQ, IBNE, IBLTZ, IBGTZ, IBLEZ, IBGEZ, IADD, ISUB, IADDI, IAND, IOR,
    MOVE, MR32, LQI, SQI, LQD, SQD,
};

struct Op {
    uint32_t lo, hi;
    bool valid, ibit, ebit;
    LK lk;
    UDec up;
    uint8_t it, is, id, dest, comp;
    uint16_t uimm;       // IADDIU/ISUBIU immediate, or IADDI's (sign-extended 5 bits)
    int32_t imm11;
    uint32_t target;     // branch target
    uint32_t fimm;       // FC* 24-bit immediate
    uint32_t reads;      // VF read mask (stalls)
    uint32_t writes;     // VF register the lower op writes, or 0
    bool flags_dead;     // this upper op's MAC/status result can never be observed (see decode)
    uint8_t nreads;      // VF registers read (vf0 excluded), for the stall check
    uint8_t rd[4];
    __m128 (*upfn)(const Op&);   // specialised FMAC op (UDec kinds other than Slow/Nop)
    const float* bsrc;           // its broadcast operand: a VF lane or I
};
Op g_ops[kVu1MicroSize / 8];
const uint8_t* g_micro;   // vu1().micro, cached by run()

inline bool writes_mac(UKind k) { return k == UKind::Add || k == UKind::Sub || k == UKind::Mul || k == UKind::Madd || k == UKind::Msub; }

// Whether the flag result of the FMAC op in the pair at `pc` is dead. It lands 4 cycles
// after issue and stays visible until the next flag result lands. Cycles grow by at
// least one per pair, so if the next pair writes flags, only lower ops in the next 4
// pairs could ever see this result. When those 4 pairs run in straight line (no
// branch or E bit before them, and this pair is not a delay slot) and none reads MAC or
// status (FM*, FS*), nothing can
// observe it, and the push is skipped. CLIP results carry clip flags and are not
// affected. Skipping a chain of such pushes is safe for the same reason: whatever a
// reader sees is the latest landed result, and that one was always pushed.
const bool g_no_dead = std::getenv("TWONK_VU_NODEAD") != nullptr;
bool flags_dead_at(uint32_t pc, uint32_t hi) {
    if (g_no_dead || !writes_mac(decode_upper(hi).kind)) return false;
    for (int j = -1; j <= 4; ++j) {   // j = -1: a branch there makes this pair its delay slot
        uint32_t l, h;
        const uint8_t* p = g_micro + ((pc + 8u * (uint32_t)j) & (kVu1MicroSize - 8));
        std::memcpy(&l, p, 4);
        std::memcpy(&h, p + 4, 4);
        if (j == 1 && !writes_mac(decode_upper(h).kind)) return false;
        if (j <= 3 && (h & 0x40000000u)) return false;               // E bit: what follows is unknown
        if (j == -1 && !(h & 0x80000000u) && (l >> 25) >= 0x20 && (l >> 25) <= 0x2F) return false;
        if (j == -1) continue;
        if (h & 0x80000000u) continue;                                 // I bit: the lower word is data
        const uint32_t op = l >> 25;
        if (j <= 3 && op >= 0x20 && op <= 0x2F) return false;         // branch or jump
        if (j >= 1 && op >= 0x14 && op <= 0x1B) return false;         // reads (or sets) MAC/status
    }
    return true;
}


// FMAC ops specialised on operation, operand form and whether the flag result is
// pushed: no per-op switches at run time. Same arithmetic as upper_fast()/upper().
enum : int { kBVec, kBBcast, kBQ };

template <UKind K, int B, bool Push>
__m128 up_impl(const Op& o) {
    const __m128 a = vu_float4(_mm_loadu_ps(U.vf[o.up.fs].f));
    __m128 b;
    if constexpr (B == kBVec) b = _mm_loadu_ps(U.vf[o.up.ft].f);
    else if constexpr (B == kBQ) b = _mm_set1_ps(qv());
    else b = _mm_set1_ps(*o.bsrc);
    b = vu_float4(b);
    __m128 r;
    if constexpr (K == UKind::Add) r = _mm_add_ps(a, b);
    else if constexpr (K == UKind::Sub) r = _mm_sub_ps(a, b);
    else if constexpr (K == UKind::Mul) r = _mm_mul_ps(a, b);
    else if constexpr (K == UKind::Madd) r = _mm_add_ps(_mm_loadu_ps(U.acc.f), _mm_mul_ps(a, b));
    else if constexpr (K == UKind::Msub) r = _mm_sub_ps(_mm_loadu_ps(U.acc.f), _mm_mul_ps(a, b));
    else if constexpr (K == UKind::Max) r = _mm_max_ps(b, a);
    else r = _mm_min_ps(b, a);
    r = vu_float4(r);
    if constexpr (K != UKind::Max && K != UKind::Min) {
        const __m128 rev = _mm_shuffle_ps(r, r, _MM_SHUFFLE(0, 1, 2, 3));   // lane 0 = w: movemask bit 3 = x, as MAC
        const uint32_t zero = (uint32_t)_mm_movemask_ps(_mm_cmpeq_ps(rev, _mm_setzero_ps()));
        const uint32_t neg = (uint32_t)_mm_movemask_ps(rev);
        const uint32_t mac = (zero & o.up.dest) | ((neg & o.up.dest) << 4);
        U.mac = mac;
        uint32_t st = U.status & 0xFC0;   // sticky bits
        if (mac & 0x000F) st |= 0x001 | 0x040;
        if (mac & 0x00F0) st |= 0x002 | 0x080;
        U.status = (U.status & 0x030) | st;   // keep I/D (set by DIV)
        if constexpr (Push) push_flags(U.cycle + 4, U.mac, U.status, 0, false);
    }
    return r;
}

template <UKind K, int B>
__m128 (*pick_push(bool push))(const Op&) { return push ? &up_impl<K, B, true> : &up_impl<K, B, false>; }

template <UKind K>
__m128 (*pick_src(int b, bool push))(const Op&) {
    return b == kBVec ? pick_push<K, kBVec>(push) : b == kBQ ? pick_push<K, kBQ>(push) : pick_push<K, kBBcast>(push);
}

void set_upfn(Op& o) {
    o.upfn = nullptr;
    const UDec& d = o.up;
    int b = kBBcast;
    if (d.src == kSrcVec) b = kBVec;
    else if (d.src == kSrcQ) b = kBQ;
    else o.bsrc = d.src == kSrcI ? &U.i : &U.vf[d.ft].f[d.src];
    const bool push = !o.flags_dead;
    switch (d.kind) {
    case UKind::Add: o.upfn = pick_src<UKind::Add>(b, push); break;
    case UKind::Sub: o.upfn = pick_src<UKind::Sub>(b, push); break;
    case UKind::Mul: o.upfn = pick_src<UKind::Mul>(b, push); break;
    case UKind::Madd: o.upfn = pick_src<UKind::Madd>(b, push); break;
    case UKind::Msub: o.upfn = pick_src<UKind::Msub>(b, push); break;
    case UKind::Max: o.upfn = pick_src<UKind::Max>(b, push); break;
    case UKind::Min: o.upfn = pick_src<UKind::Min>(b, push); break;
    default: break;
    }
}
}   // namespace

// MPG changed micro memory: decoded pairs there, and the 4 before them (whose flag
// liveness looks ahead), must be decoded again.
void vu1_micro_changed(uint32_t at, uint32_t bytes) {
    const uint32_t n = kVu1MicroSize / 8;
    const uint32_t first = (at / 8 + n - 4) % n, count = std::min(n, (bytes + 7) / 8 + 5);
    for (uint32_t k = 0; k < count; ++k) g_ops[(first + k) % n].valid = false;
    ++g_micro_gen;
    aot_micro_changed(at / 8 % n, std::min(n, (at % 8 + bytes + 7) / 8));
}

namespace {

void decode(Op& o, uint32_t lo, uint32_t hi, uint32_t pc) {
    o = Op{};
    o.lo = lo; o.hi = hi; o.valid = true;
    o.ibit = (hi & 0x80000000u) != 0;
    o.ebit = (hi & 0x40000000u) != 0;
    o.up = decode_upper(hi);
    o.reads = upper_reads(hi) | (o.ibit ? 0u : lower_reads(lo));
    o.writes = o.ibit ? 0u : lower_writes(lo);
    o.flags_dead = flags_dead_at(pc, hi);
    set_upfn(o);
    for (uint32_t m = o.reads & ~1u; m && o.nreads < 4; m &= m - 1) o.rd[o.nreads++] = (uint8_t)ctz32(m);
    if (o.ibit) { o.lk = LK::Nop; return; }
    const uint32_t w = lo;
    const uint32_t op = w >> 25, dest = (w >> 21) & 0xF, it = (w >> 16) & 31, is = (w >> 11) & 31, id = (w >> 6) & 31;
    o.dest = (uint8_t)dest; o.it = (uint8_t)it; o.is = (uint8_t)is; o.id = (uint8_t)id;
    o.comp = (uint8_t)(dest & 8 ? 0 : dest & 4 ? 1 : dest & 2 ? 2 : 3);
    o.imm11 = ((int32_t)(w << 21)) >> 21;
    o.target = (uint32_t)((int32_t)pc + 8 + o.imm11 * 8) & (kVu1MicroSize - 1);
    o.uimm = (uint16_t)((w & 0x7FF) | ((w >> 10) & 0x7800));
    o.fimm = w & 0xFFFFFF;
    LK k = LK::Slow;
    switch (op) {
    case 0x00: k = LK::LQ; break;
    case 0x01: k = LK::SQ; break;
    case 0x04: k = LK::ILW; break;
    case 0x05: k = LK::ISW; break;
    case 0x08: k = LK::IADDIU; break;
    case 0x09: k = LK::ISUBIU; break;
    case 0x10: k = LK::FCEQ; break;
    case 0x11: k = LK::FCSET; break;
    case 0x12: k = LK::FCAND; break;
    case 0x13: k = LK::FCOR; break;
    case 0x18: k = LK::FMEQ; break;
    case 0x1A: k = LK::FMAND; break;
    case 0x1B: k = LK::FMOR; break;
    case 0x1C: k = LK::FCGET; break;
    case 0x20: k = LK::B; break;
    case 0x21: k = LK::BAL; break;
    case 0x24: k = LK::JR; break;
    case 0x25: k = LK::JALR; break;
    case 0x28: k = LK::IBEQ; break;
    case 0x29: k = LK::IBNE; break;
    case 0x2C: k = LK::IBLTZ; break;
    case 0x2D: k = LK::IBGTZ; break;
    case 0x2E: k = LK::IBLEZ; break;
    case 0x2F: k = LK::IBGEZ; break;
    case 0x40: {
        const uint32_t funct = w & 0x3F;
        switch (funct) {
        case 0x30: k = LK::IADD; break;
        case 0x31: k = LK::ISUB; break;
        case 0x32: k = LK::IADDI; o.uimm = (uint16_t)(((int32_t)(id << 27)) >> 27); break;
        case 0x34: k = LK::IAND; break;
        case 0x35: k = LK::IOR; break;
        default:
            if (funct >= 0x3C) {
                switch ((id << 2) | (funct & 3)) {
                case 0x30: k = (it && dest) ? LK::MOVE : LK::Nop; break;
                case 0x31: k = (it && dest) ? LK::MR32 : LK::Nop; break;
                case 0x34: k = LK::LQI; break;
                case 0x35: k = LK::SQI; break;
                case 0x36: k = LK::LQD; break;
                case 0x37: k = LK::SQD; break;
                default: break;
                }
            }
            break;
        }
        break;
    }
    default: break;
    }
    o.lk = k;
}

inline uint16_t rvi(uint32_t r) { return (uint16_t)(r & 15 ? U.vi[r & 15] : 0); }

inline void load_masked(uint32_t reg, uint32_t qaddr, uint32_t dest) {
    if (!reg) return;
    const __m128 m = dest_mask(dest);
    float* d = U.vf[reg].f;
    _mm_storeu_ps(d, _mm_or_ps(_mm_and_ps(m, _mm_load_ps(memq(qaddr))), _mm_andnot_ps(m, _mm_loadu_ps(d))));
}

inline void store_masked(uint32_t qaddr, __m128 v, uint32_t dest) {
    const __m128 m = dest_mask(dest);
    float* d = memq(qaddr);
    _mm_store_ps(d, _mm_or_ps(_mm_and_ps(m, v), _mm_andnot_ps(m, _mm_load_ps(d))));
}

void run_ref(Context* ctx);
const bool g_slow = std::getenv("TWONK_VU_SLOW") != nullptr;

constexpr uint64_t kMaxPairs = 200000;   // see run_ref()

// The decoded-pair interpreter from pair `pc` on, with a branch pending from the pair
// before (`branch_to`), the E bit seen (`ending`), and `n0` pairs already run. Compiled
// microprograms (vu1_aot) hand over here when they reach something they do not cover.
void run_from(Context* ctx, uint32_t pc, uint32_t branch_to, bool ending, uint64_t n0) {
    const uint8_t* const micro = vu1().micro;
    g_micro = micro;
    for (uint64_t n = n0; n < kMaxPairs; ++n) {
        const uint8_t* p = micro + (pc & (kVu1MicroSize - 8));
        uint32_t lo, hi;
        std::memcpy(&lo, p, 4);
        std::memcpy(&hi, p + 4, 4);
        Op& o = g_ops[(pc & (kVu1MicroSize - 1)) / 8];
        if (!o.valid || o.lo != lo || o.hi != hi) decode(o, lo, hi, pc & (kVu1MicroSize - 8));
        U.pc = pc;
        const int prev_vi = U.last_vi;
        const uint16_t prev_old = U.last_vi_old;
        U.last_vi = -1;
        if (o.nreads) {   // stall until the registers this pair reads are written (see stall_for)
            uint64_t need = U.vf_ready[o.rd[0]];
            for (uint32_t k = 1; k < o.nreads; ++k) need = std::max(need, U.vf_ready[o.rd[k]]);
            if (U.cycle < need) U.cycle = need;
        }
        // upper: computed now, committed after the lower op (both read the old registers)
        Upper slow;
        slow.write = false;
        __m128 ur = _mm_setzero_ps();
        bool uwrite = false;
        if (o.up.kind == UKind::Slow) upper(hi, &slow);
        else if (o.upfn) {
            ur = o.upfn(o);
            uwrite = true;
        }
        uint32_t taken = ~0u;
        if (o.ibit) U.i = ffrom(lo);
        else {
            auto bvi = [&](uint32_t r) -> uint16_t {
                r &= 15;
                if (!r) return 0;
                return (int)r == prev_vi ? prev_old : U.vi[r];
            };
            switch (o.lk) {
            case LK::Nop: break;
            case LK::LQ: load_masked(o.it, rvi(o.is) + o.imm11, o.dest); break;
            case LK::SQ: store_masked(rvi(o.it) + o.imm11, _mm_loadu_ps(U.vf[o.is].f), o.dest); break;
            case LK::ILW: set_vi(o.it & 15, (uint16_t)memw(rvi(o.is) + o.imm11)[o.comp]); break;
            case LK::ISW: {
                uint32_t* m = memw(rvi(o.is) + o.imm11);
                const uint32_t v = rvi(o.it);
                for (int c = 0; c < 4; ++c) if (o.dest & (1u << comp_bit(c))) m[c] = v;
                break;
            }
            case LK::IADDIU: set_vi(o.it & 15, (uint16_t)(rvi(o.is) + o.uimm)); break;
            case LK::ISUBIU: set_vi(o.it & 15, (uint16_t)(rvi(o.is) - o.uimm)); break;
            case LK::FCEQ: sync_flags(); set_vi_flag(1, (U.vclip & 0xFFFFFF) == o.fimm); break;
            case LK::FCSET:
                sync_flags();
                U.clip = U.vclip = o.fimm;
                U.flag_q.each([](FlagSnap& f) { f.has_clip = false; });
                break;
            case LK::FCAND: sync_flags(); set_vi_flag(1, (U.vclip & o.fimm) != 0); break;
            case LK::FCOR: sync_flags(); set_vi_flag(1, ((U.vclip | o.fimm) & 0xFFFFFF) == 0xFFFFFF); break;
            case LK::FMEQ: sync_flags(); set_vi_flag(o.it & 15, (U.vmac & 0xFFFF) == rvi(o.is)); break;
            case LK::FMAND: sync_flags(); set_vi_flag(o.it & 15, (uint16_t)(U.vmac & rvi(o.is))); break;
            case LK::FMOR: sync_flags(); set_vi_flag(o.it & 15, (uint16_t)(U.vmac | rvi(o.is))); break;
            case LK::FCGET: sync_flags(); set_vi_flag(o.it & 15, (uint16_t)(U.vclip & 0xFFF)); break;
            case LK::B: taken = o.target; break;
            case LK::BAL: set_vi(o.it & 15, (uint16_t)((pc + 16) / 8)); taken = o.target; break;
            case LK::JR:
                taken = (uint32_t)(bvi(o.is) * 8) & (kVu1MicroSize - 1);
                if (g_vu_dump) dump_note(kJumpTarget | taken);
                break;
            case LK::JALR:
                taken = (uint32_t)(bvi(o.is) * 8) & (kVu1MicroSize - 1);
                if (g_vu_dump) dump_note(kJumpTarget | taken);
                set_vi(o.it & 15, (uint16_t)((pc + 16) / 8));
                break;
            case LK::IBEQ: if (bvi(o.it) == bvi(o.is)) taken = o.target; break;
            case LK::IBNE: if (bvi(o.it) != bvi(o.is)) taken = o.target; break;
            case LK::IBLTZ: if ((int16_t)bvi(o.is) < 0) taken = o.target; break;
            case LK::IBGTZ: if ((int16_t)bvi(o.is) > 0) taken = o.target; break;
            case LK::IBLEZ: if ((int16_t)bvi(o.is) <= 0) taken = o.target; break;
            case LK::IBGEZ: if ((int16_t)bvi(o.is) >= 0) taken = o.target; break;
            case LK::IADD: set_vi(o.id & 15, (uint16_t)(rvi(o.is) + rvi(o.it))); break;
            case LK::ISUB: set_vi(o.id & 15, (uint16_t)(rvi(o.is) - rvi(o.it))); break;
            case LK::IADDI: set_vi(o.it & 15, (uint16_t)(rvi(o.is) + o.uimm)); break;
            case LK::IAND: set_vi(o.id & 15, (uint16_t)(rvi(o.is) & rvi(o.it))); break;
            case LK::IOR: set_vi(o.id & 15, (uint16_t)(rvi(o.is) | rvi(o.it))); break;
            case LK::MOVE: {
                const __m128 m = dest_mask(o.dest);
                float* d = U.vf[o.it].f;
                _mm_storeu_ps(d, _mm_or_ps(_mm_and_ps(m, _mm_loadu_ps(U.vf[o.is].f)), _mm_andnot_ps(m, _mm_loadu_ps(d))));
                break;
            }
            case LK::MR32: {
                const __m128 m = dest_mask(o.dest);
                const __m128 s = _mm_loadu_ps(U.vf[o.is].f);
                const __m128 r = _mm_shuffle_ps(s, s, _MM_SHUFFLE(0, 3, 2, 1));   // (y, z, w, x)
                float* d = U.vf[o.it].f;
                _mm_storeu_ps(d, _mm_or_ps(_mm_and_ps(m, r), _mm_andnot_ps(m, _mm_loadu_ps(d))));
                break;
            }
            case LK::LQI: load_masked(o.it, rvi(o.is), o.dest); set_vi(o.is & 15, (uint16_t)(rvi(o.is) + 1)); break;
            case LK::SQI: store_masked(rvi(o.it), _mm_loadu_ps(U.vf[o.is].f), o.dest); set_vi(o.it & 15, (uint16_t)(rvi(o.it) + 1)); break;
            case LK::LQD: set_vi(o.is & 15, (uint16_t)(rvi(o.is) - 1)); load_masked(o.it, rvi(o.is), o.dest); break;
            case LK::SQD: set_vi(o.it & 15, (uint16_t)(rvi(o.it) - 1)); store_masked(rvi(o.it), _mm_loadu_ps(U.vf[o.is].f), o.dest); break;
            default: taken = lower(ctx, lo, pc, prev_vi, prev_old); break;
            }
        }
        if (uwrite) {
            const int fd = o.up.target;
            if (fd != 0) {
                float* d = (fd == 32 ? U.acc : U.vf[fd]).f;
                const __m128 m = dest_mask(o.up.dest);
                _mm_storeu_ps(d, _mm_or_ps(_mm_and_ps(m, ur), _mm_andnot_ps(m, _mm_loadu_ps(d))));
                if (fd < 32) U.vf_ready[fd] = U.cycle + kFmacLatency;
            }
        } else if (slow.write) {
            commit(slow);
            if (slow.fd > 0 && slow.fd < 32) U.vf_ready[slow.fd] = U.cycle + kFmacLatency;
        }
        if (o.writes) U.vf_ready[o.writes] = U.cycle + kFmacLatency;
        // vf0 and vi0 need no reset here: no fast-path op writes register 0, and the
        // slow ones (commit, set_vi, lower()'s loads and moves) skip it too.
        advance_cycle();
        uint32_t next = (pc + 8) & (kVu1MicroSize - 1);
        if (branch_to != ~0u) { next = branch_to; branch_to = ~0u; }
        if (taken != ~0u) branch_to = taken;
        if (ending) {
            U.pc = next;
            g_pairs += n + 1;
            perf_add(kPerfVu1Pairs, n + 1);
            g_prog_pairs[g_last_start] += n + 1;
            if (n > g_longest) g_longest = n;
            return;
        }
        if (o.ebit) ending = true;   // E bit: one more pair, then stop
        pc = next;
    }
    ++g_runaways;
    g_aborted = true;
    replay_runaway(ctx);
    static unsigned reported;
    if (reported++ < 4) {
        std::fprintf(stderr, "[gpu] VU1: program at %04x did not end, now at %04x (longest normal run %llu pairs)\n",
                     g_last_start, U.pc, (unsigned long long)g_longest);
    }
}

// ---- compiled microprograms (vu1_aot) ------------------------------------------------------------
// `twonk --vu-aot <dir> <out>` turns the micro memory images saved with TWONK_VU_DUMP=<dir>
// into C++ (vu1_aot_gen.inc, in the generated-code directory). Each pair becomes the
// code run_from() would execute for it, with its registers, masks and operation kinds
// as constants, and branches become gotos. The cycle, stall, flag and Q model is the
// same code, so results are identical (TWONK_VU_VERIFY=1 and --vu-bench check it).
// Whatever a compiled program does not cover (a jump to a pair it did not compile, a
// branch in a delay slot, an E bit next to a branch) continues in run_from().
constexpr uint32_t kNoBranch = ~0u;

void aot_finish(uint32_t next, uint64_t n) {
    U.pc = next;
    g_pairs += n;
    perf_add(kPerfVu1Pairs, n);
    g_prog_pairs[g_last_start] += n;
    if (n - 1 > g_longest) g_longest = n - 1;
}

// up_impl() on register values the compiled block holds in locals. CA/CB: the operand is
// known to be a vu_float4() result already (vu_float4 is idempotent, so skipping it there
// changes nothing). The cycle, MAC and status are the block's locals too.
template <UKind K, bool Push, bool CA, bool CB>
__forceinline __m128 aot_up(uint64_t cyc, uint32_t& st, uint32_t& mc, __m128 a, __m128 b, __m128 acc, uint32_t dest) {
    if constexpr (!CA) a = vu_float4(a);
    if constexpr (!CB) b = vu_float4(b);
    __m128 r;
    if constexpr (K == UKind::Add) r = _mm_add_ps(a, b);
    else if constexpr (K == UKind::Sub) r = _mm_sub_ps(a, b);
    else if constexpr (K == UKind::Mul) r = _mm_mul_ps(a, b);
    else if constexpr (K == UKind::Madd) r = _mm_add_ps(acc, _mm_mul_ps(a, b));
    else if constexpr (K == UKind::Msub) r = _mm_sub_ps(acc, _mm_mul_ps(a, b));
    else if constexpr (K == UKind::Max) r = _mm_max_ps(b, a);
    else r = _mm_min_ps(b, a);
    r = vu_float4(r);
    if constexpr (K != UKind::Max && K != UKind::Min) {
        const __m128 rev = _mm_shuffle_ps(r, r, _MM_SHUFFLE(0, 1, 2, 3));
        const uint32_t zero = (uint32_t)_mm_movemask_ps(_mm_cmpeq_ps(rev, _mm_setzero_ps()));
        const uint32_t neg = (uint32_t)_mm_movemask_ps(rev);
        const uint32_t mac = (zero & dest) | ((neg & dest) << 4);
        mc = mac;
        uint32_t s2 = st & 0xFC0;
        if (mac & 0x000F) s2 |= 0x001 | 0x040;
        if (mac & 0x00F0) s2 |= 0x002 | 0x080;
        st = (st & 0x030) | s2;
        if constexpr (Push) push_flags_at(cyc, cyc + 4, mac, st, 0, false);
    }
    return r;
}

template <int L>
__forceinline __m128 aot_bc(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(L, L, L, L)); }
template <int L>
__forceinline float aot_lane(__m128 v) { return _mm_cvtss_f32(aot_bc<L>(v)); }

// ITOF0/4/12/15: dividing by a power of two is exact, so the multiply gives upper()'s result.
template <int M>
__forceinline __m128 aot_itof(__m128 s) {
    constexpr float kScale[4] = {1.0f, 1.0f / 16.0f, 1.0f / 4096.0f, 1.0f / 32768.0f};
    const __m128 r = _mm_cvtepi32_ps(_mm_castps_si128(s));
    if constexpr (M == 0) return r;
    else return _mm_mul_ps(r, _mm_set1_ps(kScale[M]));
}

// FTOI0/4/12/15: the scaled value is exact in float (or overflows to an infinity, which
// clamps the same way); no float lies between 2^31 - 1 and 2^31, and cvtt gives INT32_MIN
// for NaN and anything at or below -2^31, as the double conversion in upper() does.
template <int M>
__forceinline __m128 aot_ftoi(__m128 v) {
    constexpr float kScale[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
    if constexpr (M != 0) v = _mm_mul_ps(v, _mm_set1_ps(kScale[M]));
    const __m128i i = _mm_cvttps_epi32(v);
    const __m128i big = _mm_castps_si128(_mm_cmpge_ps(v, _mm_set1_ps(2147483648.0f)));
    return _mm_castsi128_ps(_mm_or_si128(_mm_andnot_si128(big, i), _mm_and_si128(big, _mm_set1_epi32(0x7FFFFFFF))));
}

__forceinline __m128 aot_abs(__m128 s) { return _mm_and_ps(s, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))); }

// CLIP: as upper() does it, four lanes at a time.
__forceinline void aot_clip(uint64_t cyc, uint32_t st, uint32_t mc, __m128 s, __m128 t) {
    static constexpr uint8_t kSpread[8] = {0, 1, 4, 5, 16, 17, 20, 21};   // bit c to bit 2c
    const float wv = std::fabs(aot_lane<3>(t));
    const uint32_t gt = (uint32_t)_mm_movemask_ps(_mm_cmpgt_ps(s, _mm_set1_ps(wv))) & 7;
    const uint32_t lt = (uint32_t)_mm_movemask_ps(_mm_cmplt_ps(s, _mm_set1_ps(-wv))) & 7;
    U.clip = ((U.clip << 6) | kSpread[gt] | (uint32_t)(kSpread[lt] << 1)) & 0xFFFFFF;
    push_flags_at(cyc, cyc + 4, mc, st, U.clip, true);
}

// DIV, SQRT, RSQRT: as lower() does them.
__forceinline void aot_div(uint64_t cyc, uint32_t& st, float a, float b) {
    st &= ~0x30u;
    if (b == 0.0f) { st |= 0x820; start_q_at(cyc, std::copysign(3.4028235e38f, a) * (std::signbit(b) ? -1.0f : 1.0f), 7); }
    else start_q_at(cyc, a / b, 7);
}
__forceinline void aot_sqrt(uint64_t cyc, float t) { start_q_at(cyc, std::sqrt(std::fabs(t)), 7); }
__forceinline void aot_rsqrt(uint64_t cyc, float s, float t) {
    const float b = std::sqrt(std::fabs(t));
    start_q_at(cyc, b == 0.0f ? 3.4028235e38f : s / b, 13);
}

__forceinline uint32_t aot_lane_bits(__m128 v, int lane) {
    alignas(16) uint32_t t[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(t), _mm_castps_si128(v));
    return t[lane];
}

__forceinline void aot_store(uint32_t qaddr, __m128 v) { _mm_store_ps(memq(qaddr), v); }

__forceinline uint16_t aot_bvi(int r, int pv, uint16_t po) { return r == 0 ? 0 : r == pv ? po : U.vi[r]; }

uint64_t micro_hash(const uint8_t* m) {
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < kVu1MicroSize; i += 8) {
        uint64_t v;
        std::memcpy(&v, m + i, 8);
        x = (x ^ v) * 0xFF51AFD7ED558CCDull;
        x ^= x >> 32;
    }
    return x;
}

// A compiled block: the pairs from `start` to the end of the delay slot of its first
// branch (or to its E bit's last pair). It returns the pair to continue at, or kAotDone
// when the program ended or was handed to run_from(). `ranges` (first pair index and
// pair count, `nranges` of them) are the pairs its code depends on, with their hash: it
// is used whenever micro memory holds those same pairs, whatever is loaded around them.
using AotFn = uint32_t (*)(Context*, uint64_t*);
constexpr uint32_t kAotDone = ~0u;
struct AotBlock { uint32_t start; uint64_t hash; const uint16_t* ranges; uint32_t nranges; AotFn fn; };

uint64_t block_hash(const uint8_t* m, uint32_t start, const uint16_t* r, uint32_t nr) {
    uint64_t x = 0x9E3779B97F4A7C15ull ^ start;
    for (uint32_t k = 0; k < nr; ++k) {
        const uint8_t* p = m + (size_t)r[2 * k] * 8;
        for (uint32_t i = 0; i < r[2 * k + 1]; ++i) {
            uint64_t v;
            std::memcpy(&v, p + i * 8, 8);
            x = (x ^ v) * 0xFF51AFD7ED558CCDull;
            x ^= x >> 32;
        }
        x ^= (uint64_t)r[2 * k] << 48 | r[2 * k + 1];
    }
    return x;
}

}   // namespace
}   // namespace ps2::gpu

#if __has_include("vu1_aot_gen.inc")
namespace ps2::gpu {
namespace {
#include "vu1_aot_gen.inc"
}
}
#else
namespace ps2::gpu {
namespace {
const AotBlock kAotBlocks[] = {{~0u, 0, nullptr, 0, nullptr}};
}
}
#endif

namespace ps2::gpu {
namespace {

const bool g_aot_off = [] { const char* e = std::getenv("TWONK_VU_AOT"); return e && e[0] == '0'; }();
uint64_t g_aot_hits, g_aot_misses;   // compiled blocks run / hand-overs to the interpreter

// The compiled block starting at `pc` for micro memory as it is now, if any: found by
// hashing the candidates' pairs, then cached per start pair. A change to micro memory
// forgets only the cached lookups whose candidates depend on the pairs that changed
// (`span`: the pairs any candidate's ranges cover), so swapping one microprogram does
// not make every other start hash its code again.
struct AotSlot { bool valid = false; AotFn fn = nullptr; uint16_t span_lo = 0, span_n = 0; };
AotSlot g_aot_slots[kVu1MicroSize / 8];
const std::vector<const AotBlock*>* g_aot_index;

void aot_build_index() {
    auto* v = new std::vector<const AotBlock*>[kVu1MicroSize / 8];
    for (const AotBlock& b : kAotBlocks) {
        if (!b.fn) continue;
        const uint32_t i = (b.start & (kVu1MicroSize - 8)) / 8;
        v[i].push_back(&b);
        AotSlot& s = g_aot_slots[i];
        uint32_t lo = s.span_n ? s.span_lo : ~0u, hi = s.span_n ? s.span_lo + s.span_n : 0;
        for (uint32_t k = 0; k < b.nranges; ++k) {
            lo = std::min<uint32_t>(lo, b.ranges[2 * k]);
            hi = std::max<uint32_t>(hi, b.ranges[2 * k] + b.ranges[2 * k + 1]);
        }
        if (hi > lo) { s.span_lo = (uint16_t)lo; s.span_n = (uint16_t)(hi - lo); }
    }
    g_aot_index = v;
}

void aot_micro_changed(uint32_t first, uint32_t count) {
    constexpr uint32_t N = kVu1MicroSize / 8;
    auto forget = [](uint32_t a, uint32_t e) {   // pairs [a, e), no wrap
        for (AotSlot& s : g_aot_slots)
            if (s.valid && s.span_n && s.span_lo < e && a < (uint32_t)s.span_lo + s.span_n) s.valid = false;
    };
    if (first + count <= N) forget(first, first + count);
    else { forget(first, N); forget(0, first + count - N); }
}

void aot_forget_all() { for (AotSlot& s : g_aot_slots) s.valid = false; }

__declspec(noinline) AotFn aot_resolve(uint32_t pc) {
    if (!g_aot_index) aot_build_index();
    if (g_aot_off || g_no_dead || (pc & 7)) return nullptr;
    AotSlot& s = g_aot_slots[(pc & (kVu1MicroSize - 8)) / 8];
    s.valid = true;
    s.fn = nullptr;
    for (const AotBlock* b : g_aot_index[(pc & (kVu1MicroSize - 8)) / 8])
        if (block_hash(vu1().micro, b->start, b->ranges, b->nranges) == b->hash) { s.fn = b->fn; break; }
    return s.fn;
}

// An unaligned pc's slot is never marked valid by aot_resolve().
__forceinline AotFn aot_lookup(uint32_t pc) {
    const AotSlot& s = g_aot_slots[(pc & (kVu1MicroSize - 8)) / 8];
    if (s.valid && !(pc & 7)) return s.fn;
    return aot_resolve(pc);
}

void run(Context* ctx) {
    g_vumem = vu1().mem;
    if (g_slow || g_replaying || g_hist_on) { run_ref(ctx); return; }
    g_micro = vu1().micro;
    _mm_storeu_ps(U.vf[0].f, _mm_setr_ps(0, 0, 0, 1));
    U.vi[0] = 0;
    uint32_t pc = U.pc;
    uint64_t n = 0;
    for (AotFn fn = aot_lookup(pc); fn; fn = aot_lookup(pc)) {
        ++g_aot_hits;
        pc = fn(ctx, &n);
        if (pc == kAotDone) return;
        if (n >= kMaxPairs) break;
    }
    ++g_aot_misses;
    if (g_vu_dump && n) dump_note(kJumpTarget | pc);
    run_from(ctx, pc, kNoBranch, false, n);
}

// TWONK_VU_DUMP=<dir>: save each micro memory image programs start in (vu1_<hash>.bin),
// the addresses they start at and the targets their JR/JALR jumps reached
// (vu1_<hash>.txt: "2d78" a start, "j 2e40" a jump target), for --vu-aot.
void dump_note(uint32_t v) {
    static const char* dir = std::getenv("TWONK_VU_DUMP");
    if (!dir) return;
    static std::map<uint64_t, std::vector<uint32_t>> seen;
    static uint32_t gen;
    static uint64_t image;
    if (gen != g_micro_gen) { gen = g_micro_gen; image = micro_hash(vu1().micro); }
    auto [it, fresh] = seen.try_emplace(image);
    char base[600];
    std::snprintf(base, sizeof base, "%s/vu1_%016llx", dir, (unsigned long long)image);
    if (fresh) {
        if (FILE* f = std::fopen((std::string(base) + ".bin").c_str(), "wb")) {
            std::fwrite(vu1().micro, 1, kVu1MicroSize, f);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((std::string(base) + ".txt").c_str(), "r")) {   // from earlier runs
            char line[64];
            while (std::fgets(line, sizeof line, f)) {
                unsigned x;
                if (line[0] == 'j' && std::sscanf(line + 1, "%x", &x) == 1) it->second.push_back(kJumpTarget | x);
                else if (std::sscanf(line, "%x", &x) == 1) it->second.push_back(x);
            }
            std::fclose(f);
        }
    }
    if (std::find(it->second.begin(), it->second.end(), v) != it->second.end()) return;
    it->second.push_back(v);
    if (FILE* f = std::fopen((std::string(base) + ".txt").c_str(), "a")) {
        std::fprintf(f, (v & kJumpTarget) ? "j %04x\n" : "%04x\n", v & 0xFFFF);
        std::fclose(f);
    }
    if (!(v & kJumpTarget) && !aot_lookup(v)) {
        static int n;
        if (n++ < 50) std::fprintf(stderr, "[vu-aot] program %04x in image %016llx is not compiled\n", v, (unsigned long long)image);
    }
}
void dump_image(uint32_t pc) { dump_note(pc); }

// ---- the generator --------------------------------------------------------------------------
bool lk_branch(LK k) { return k == LK::B || k == LK::BAL || k == LK::JR || k == LK::JALR || (k >= LK::IBEQ && k <= LK::IBGEZ); }
bool lk_dynamic(LK k) { return k == LK::JR || k == LK::JALR; }
bool lk_always(LK k) { return k == LK::B || k == LK::BAL || k == LK::JR || k == LK::JALR; }

struct AotWriter {
    std::string s;
    void put(const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        s += buf;
    }
};

const char* ukind_name(UKind k) {
    switch (k) {
    case UKind::Add: return "Add"; case UKind::Sub: return "Sub"; case UKind::Mul: return "Mul";
    case UKind::Madd: return "Madd"; case UKind::Msub: return "Msub"; case UKind::Max: return "Max";
    default: return "Min";
    }
}

// The compiled code keeps the cycle, MAC and status in locals (cyc, mc, st); they go
// back to U before anything that reads them there (the interpreter, returns).
const char* const kAotSpill = "U.cycle = cyc; U.status = st; U.mac = mc;";
const char* const kAotReload = "cyc = U.cycle; st = U.status; mc = U.mac;";

// What the generator knows about register timing inside a block. Every VF write sets
// vf_ready to (its cycle + 4), and the cycle goes up by at least one per pair, so at any
// pair boundary every vf_ready is at most 3 ahead of the cycle. A pair k pairs into the
// block therefore cannot stall on a register written before the block once k >= 3, nor
// on one written in the block at pair j once k - j >= 4; when it can, the in-block write's
// ready cycle is a local (no store-to-load round trip through U.vf_ready).
struct AotTiming {
    int k = 0;         // pairs since the block's start (its loop label)
    int barrier = 0;   // first pair after a write the generator cannot place
    int wrote[33];     // in-block pair index of each VF register's last write, or -1
    uint32_t wrote_pc[33];
    AotTiming() { for (int& x : wrote) x = -1; }
};

// The VF registers and ACC (32) as locals v0..v32 of the compiled block: loaded from U
// on first use, stored back before the interpreter runs and at every exit. `clean`: the
// value is known to be a vu_float4() result (an FMAC result, or vf0), so an FMAC op
// reading it needs no clamp.
struct AotRegs {
    uint8_t state[33] = {};   // 0: only in U; 1: local, same as U; 2: local, newer than U
    bool clean[33] = {};
    bool used[33] = {};
    AotRegs() { clean[0] = true; }
    static std::string mem(int r) { return r == 32 ? std::string("U.acc.f") : "U.vf[" + std::to_string(r) + "].f"; }
    static int blend_imm(unsigned dest) { return (int)(((dest >> 3) & 1) | (((dest >> 2) & 1) << 1) | (((dest >> 1) & 1) << 2) | ((dest & 1) << 3)); }
    std::string get(AotWriter& w, int r) {
        used[r] = true;
        if (!state[r]) { w.put("        v%d = _mm_loadu_ps(%s);\n", r, mem(r).c_str()); state[r] = 1; }
        return "v" + std::to_string(r);
    }
    void set(AotWriter& w, int r, const std::string& val, unsigned dest, bool val_clean) {
        dest &= 15;
        if (dest == 15) {
            used[r] = true;
            w.put("        v%d = %s;\n", r, val.c_str());
            clean[r] = val_clean;
        } else {
            get(w, r);
            w.put("        v%d = _mm_blend_ps(v%d, %s, %d);\n", r, r, val.c_str(), blend_imm(dest));
            clean[r] = val_clean && clean[r];
        }
        state[r] = 2;
    }
    void spill(AotWriter& w) {
        for (int r = 1; r <= 32; ++r)
            if (state[r] == 2) { w.put("        _mm_storeu_ps(%s, v%d);\n", mem(r).c_str(), r); state[r] = 1; }
    }
    void forget() { for (int r = 0; r <= 32; ++r) { state[r] = 0; clean[r] = r == 0; } }   // U may have changed
    std::string decl() const {
        std::string s;
        for (int r = 0; r <= 32; ++r) if (used[r]) s += (s.empty() ? "    __m128 v" : ", v") + std::to_string(r);
        return s.empty() ? s : s + ";\n";
    }
};

// The ITOF/FTOI/ABS/CLIP upper ops (UKind::Slow) compiled inline: 1 ITOF, 2 FTOI, 3 ABS, 4 CLIP.
int aot_misc_upper(uint32_t hi, int* mode) {
    const uint32_t funct = hi & 0x3F;
    if (funct < 0x3C) return 0;
    const uint32_t op = (((hi >> 6) & 31) << 2) | (funct & 3);
    *mode = (int)(op & 3);
    if (op >= 0x10 && op <= 0x13) return 1;
    if (op >= 0x14 && op <= 0x17) return 2;
    if (op == 0x1D) return 3;
    if (op == 0x1F) return 4;
    return 0;
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// One pair, exactly as run_from() executes it (without the move to the next pair).
void aot_pair(AotWriter& w, const Op& o, uint32_t pc, AotTiming& t, AotRegs& rg) {
    w.put("    { // %04x  %08x %08x\n", pc, o.hi, o.lo);
    {
        std::string need;
        for (uint32_t k = 0; k < o.nreads; ++k) {
            const uint32_t r = o.rd[k];
            char term[48];
            if (t.wrote[r] >= t.barrier) {
                if (t.k - t.wrote[r] >= kFmacLatency) continue;
                std::snprintf(term, sizeof term, "rdy_%04x_%u", t.wrote_pc[r], r);
            } else {
                if (t.k - t.barrier >= kFmacLatency - 1) continue;
                std::snprintf(term, sizeof term, "U.vf_ready[%u]", r);
            }
            need = need.empty() ? std::string(term) : "std::max<uint64_t>(" + need + ", " + term + ")";
        }
        if (!need.empty()) w.put("        { const uint64_t need = %s; if (cyc < need) cyc = need; }\n", need.c_str());
    }
    if ((lk_branch(o.lk) || o.lk == LK::Slow) && !o.ibit) w.put("        const int pv = U.last_vi; const uint16_t po = U.last_vi_old;\n");
    w.put("        U.last_vi = -1;\n");
    int mmode = 0;
    const int misc = o.up.kind == UKind::Slow ? aot_misc_upper(o.hi, &mmode) : 0;
    const bool slow_up = o.up.kind == UKind::Slow && !misc;
    const int ufs = (int)((o.hi >> 11) & 31), uft = (int)((o.hi >> 16) & 31);
    const unsigned udest = (o.hi >> 21) & 15;
    int up_writes = 0;       // VF register the upper op writes (1..31), for the timing
    bool ur_clean = false;   // the upper result needs no clamp
    if (slow_up) {
        rg.spill(w);
        w.put("        Upper su; su.write = false; U.pc = 0x%x; %s upper(0x%08xu, &su); %s\n", pc, kAotSpill, o.hi, kAotReload);
    } else if (misc == 1) {
        w.put("        const __m128 ur = aot_itof<%d>(%s);\n", mmode, rg.get(w, ufs).c_str());
        ur_clean = true;
    } else if (misc == 2) {
        w.put("        const __m128 ur = aot_ftoi<%d>(%s);\n", mmode, rg.get(w, ufs).c_str());
    } else if (misc == 3) {
        const std::string s = rg.get(w, ufs);
        ur_clean = rg.clean[ufs];
        w.put("        const __m128 ur = aot_abs(%s);\n", s.c_str());
    } else if (misc == 4) {
        const std::string s = rg.get(w, ufs), tt = rg.get(w, uft);
        w.put("        aot_clip(cyc, st, mc, %s, %s);\n", s.c_str(), tt.c_str());
    } else if (o.upfn) {
        const std::string a = rg.get(w, o.up.fs);
        std::string b;
        bool cb = false;
        if (o.up.src == kSrcVec) { b = rg.get(w, o.up.ft); cb = rg.clean[o.up.ft]; }
        else if (o.up.src == kSrcQ) b = "_mm_set1_ps(qv_at(cyc))";
        else if (o.up.src == kSrcI) b = "_mm_set1_ps(U.i)";
        else { b = fmt("aot_bc<%u>(%s)", o.up.src, rg.get(w, o.up.ft).c_str()); cb = rg.clean[o.up.ft]; }
        const bool acc = o.up.kind == UKind::Madd || o.up.kind == UKind::Msub;
        const std::string av = acc ? rg.get(w, 32) : std::string("_mm_setzero_ps()");
        w.put("        const __m128 ur = aot_up<UKind::%s, %s, %s, %s>(cyc, st, mc, %s, %s, %s, %u);\n", ukind_name(o.up.kind),
              o.flags_dead ? "false" : "true", rg.clean[o.up.fs] ? "true" : "false", cb ? "true" : "false", a.c_str(), b.c_str(),
              av.c_str(), o.up.dest);
        ur_clean = true;
    }
    const int it = o.it, is = o.is, id = o.id;
    const unsigned dest = o.dest;
    const unsigned fsf = (o.lo >> 21) & 3, ftf = (o.lo >> 23) & 3;
    const uint32_t sop = (o.lo >> 25) == 0x40 && (o.lo & 0x3F) >= 0x3C ? ((((o.lo >> 6) & 31) << 2) | (o.lo & 3)) : ~0u;
    auto store = [&](const std::string& addr, int reg) {
        const std::string v = rg.get(w, reg);
        if (dest == 15) w.put("        aot_store(%s, %s);\n", addr.c_str(), v.c_str());
        else w.put("        store_masked(%s, %s, %u);\n", addr.c_str(), v.c_str(), dest);
    };
    auto load = [&](const std::string& addr) { if (it) rg.set(w, it, "_mm_load_ps(memq(" + addr + "))", dest, false); };
    if (o.ibit) w.put("        U.i = ffrom(0x%08xu);\n", o.lo);
    else switch (o.lk) {
    case LK::Nop: break;
    case LK::LQ: load(fmt("rvi(%d) + (%d)", is, o.imm11)); break;
    case LK::SQ: store(fmt("rvi(%d) + (%d)", it, o.imm11), is); break;
    case LK::ILW: w.put("        set_vi(%d, (uint16_t)memw(rvi(%d) + (%d))[%u]);\n", it & 15, is, o.imm11, o.comp); break;
    case LK::ISW:
        w.put("        { uint32_t* m = memw(rvi(%d) + (%d)); const uint32_t v = rvi(%d);", is, o.imm11, it);
        for (int c = 0; c < 4; ++c) if (dest & (1u << comp_bit(c))) w.put(" m[%d] = v;", c);
        w.put(" }\n");
        break;
    case LK::IADDIU: w.put("        set_vi(%d, (uint16_t)(rvi(%d) + %uu));\n", it & 15, is, o.uimm); break;
    case LK::ISUBIU: w.put("        set_vi(%d, (uint16_t)(rvi(%d) - %uu));\n", it & 15, is, o.uimm); break;
    case LK::FCEQ: w.put("        sync_flags_at(cyc); set_vi_flag(1, (U.vclip & 0xFFFFFF) == 0x%xu);\n", o.fimm); break;
    case LK::FCSET:
        w.put("        sync_flags_at(cyc); U.clip = U.vclip = 0x%xu; U.flag_q.each([](FlagSnap& f) { f.has_clip = false; });\n", o.fimm);
        break;
    case LK::FCAND: w.put("        sync_flags_at(cyc); set_vi_flag(1, (U.vclip & 0x%xu) != 0);\n", o.fimm); break;
    case LK::FCOR: w.put("        sync_flags_at(cyc); set_vi_flag(1, ((U.vclip | 0x%xu) & 0xFFFFFF) == 0xFFFFFF);\n", o.fimm); break;
    case LK::FMEQ: w.put("        sync_flags_at(cyc); set_vi_flag(%d, (U.vmac & 0xFFFF) == rvi(%d));\n", it & 15, is); break;
    case LK::FMAND: w.put("        sync_flags_at(cyc); set_vi_flag(%d, (uint16_t)(U.vmac & rvi(%d)));\n", it & 15, is); break;
    case LK::FMOR: w.put("        sync_flags_at(cyc); set_vi_flag(%d, (uint16_t)(U.vmac | rvi(%d)));\n", it & 15, is); break;
    case LK::FCGET: w.put("        sync_flags_at(cyc); set_vi_flag(%d, (uint16_t)(U.vclip & 0xFFF));\n", it & 15); break;
    case LK::B: w.put("        bt = 0x%xu;\n", o.target); break;
    case LK::BAL: w.put("        set_vi(%d, (uint16_t)%uu); bt = 0x%xu;\n", it & 15, (pc + 16) / 8, o.target); break;
    case LK::JR: w.put("        bt = (uint32_t)(aot_bvi(%d, pv, po) * 8) & (kVu1MicroSize - 1);\n", is & 15); break;
    case LK::JALR:
        w.put("        { const uint32_t t = (uint32_t)(aot_bvi(%d, pv, po) * 8) & (kVu1MicroSize - 1); set_vi(%d, (uint16_t)%uu); bt = t; }\n",
              is & 15, it & 15, (pc + 16) / 8);
        break;
    case LK::IBEQ: w.put("        if (aot_bvi(%d, pv, po) == aot_bvi(%d, pv, po)) bt = 0x%xu;\n", it & 15, is & 15, o.target); break;
    case LK::IBNE: w.put("        if (aot_bvi(%d, pv, po) != aot_bvi(%d, pv, po)) bt = 0x%xu;\n", it & 15, is & 15, o.target); break;
    case LK::IBLTZ: w.put("        if ((int16_t)aot_bvi(%d, pv, po) < 0) bt = 0x%xu;\n", is & 15, o.target); break;
    case LK::IBGTZ: w.put("        if ((int16_t)aot_bvi(%d, pv, po) > 0) bt = 0x%xu;\n", is & 15, o.target); break;
    case LK::IBLEZ: w.put("        if ((int16_t)aot_bvi(%d, pv, po) <= 0) bt = 0x%xu;\n", is & 15, o.target); break;
    case LK::IBGEZ: w.put("        if ((int16_t)aot_bvi(%d, pv, po) >= 0) bt = 0x%xu;\n", is & 15, o.target); break;
    case LK::IADD: w.put("        set_vi(%d, (uint16_t)(rvi(%d) + rvi(%d)));\n", id & 15, is, it); break;
    case LK::ISUB: w.put("        set_vi(%d, (uint16_t)(rvi(%d) - rvi(%d)));\n", id & 15, is, it); break;
    case LK::IADDI: w.put("        set_vi(%d, (uint16_t)(rvi(%d) + %uu));\n", it & 15, is, o.uimm); break;
    case LK::IAND: w.put("        set_vi(%d, (uint16_t)(rvi(%d) & rvi(%d)));\n", id & 15, is, it); break;
    case LK::IOR: w.put("        set_vi(%d, (uint16_t)(rvi(%d) | rvi(%d)));\n", id & 15, is, it); break;
    case LK::MOVE: {   // decode() makes it a Nop when it is 0
        const std::string s = rg.get(w, is);
        rg.set(w, it, s, dest, rg.clean[is]);
        break;
    }
    case LK::MR32: {
        const std::string s = rg.get(w, is);
        rg.set(w, it, "_mm_shuffle_ps(" + s + ", " + s + ", _MM_SHUFFLE(0, 3, 2, 1))", dest, rg.clean[is]);
        break;
    }
    case LK::LQI: load(fmt("rvi(%d)", is)); w.put("        set_vi(%d, (uint16_t)(rvi(%d) + 1));\n", is & 15, is); break;
    case LK::SQI: store(fmt("rvi(%d)", it), is); w.put("        set_vi(%d, (uint16_t)(rvi(%d) + 1));\n", it & 15, it); break;
    case LK::LQD: w.put("        set_vi(%d, (uint16_t)(rvi(%d) - 1));\n", is & 15, is); load(fmt("rvi(%d)", is)); break;
    case LK::SQD: w.put("        set_vi(%d, (uint16_t)(rvi(%d) - 1));\n", it & 15, it); store(fmt("rvi(%d)", it), is); break;
    default:
        switch (sop) {   // lower() ops compiled inline (same code as there)
        case 0x38: {     // DIV
            const std::string s = rg.get(w, is), tt = rg.get(w, it);
            w.put("        aot_div(cyc, st, aot_lane<%u>(%s), aot_lane<%u>(%s));\n", fsf, s.c_str(), ftf, tt.c_str());
            break;
        }
        case 0x39: w.put("        aot_sqrt(cyc, aot_lane<%u>(%s));\n", ftf, rg.get(w, it).c_str()); break;   // SQRT
        case 0x3A: {     // RSQRT
            const std::string s = rg.get(w, is), tt = rg.get(w, it);
            w.put("        aot_rsqrt(cyc, aot_lane<%u>(%s), aot_lane<%u>(%s));\n", fsf, s.c_str(), ftf, tt.c_str());
            break;
        }
        case 0x3B: w.put("        finish_q();\n"); break;                                         // WAITQ
        case 0x3C: w.put("        set_vi(%d, (uint16_t)aot_lane_bits(%s, %u));\n", it & 15, rg.get(w, is).c_str(), fsf); break;   // MTIR
        case 0x3D:                                                                                // MFIR
            if (it) rg.set(w, it, fmt("_mm_castsi128_ps(_mm_set1_epi32((int32_t)(int16_t)rvi(%d)))", is), dest, false);
            break;
        case 0x3E: w.put("        set_vi(%d, (uint16_t)memw(rvi(%d))[%u]);\n", it & 15, is, o.comp); break;   // ILWR
        case 0x3F:                                                                                // ISWR
            w.put("        { uint32_t* m = memw(rvi(%d)); const uint32_t v = rvi(%d);", is, it);
            for (int c = 0; c < 4; ++c) if (dest & (1u << comp_bit(c))) w.put(" m[%d] = v;", c);
            w.put(" }\n");
            break;
        case 0x64: if (it) rg.set(w, it, "_mm_set1_ps(U.p)", dest, false); break;                 // MFP
        case 0x68: w.put("        set_vi(%d, (uint16_t)(g_bench_mode ? g_bench_top_v : vif1_top()));\n", it & 15); break;   // XTOP
        case 0x69: w.put("        set_vi(%d, (uint16_t)(g_bench_mode ? g_bench_itop_v : vif1_itop()));\n", it & 15); break;   // XITOP
        case 0x6C: w.put("        xgkick(ctx, rvi(%d));\n", is); break;                          // XGKICK (reads data memory only)
        default:
            rg.spill(w);
            w.put("        U.pc = 0x%xu; %s lower(ctx, 0x%08xu, 0x%xu, pv, po); %s\n", pc, kAotSpill, o.lo, pc, kAotReload);
            rg.forget();
            break;
        }
        break;
    }
    if (o.upfn && o.up.target != 0) {
        rg.set(w, o.up.target, "ur", o.up.dest, ur_clean);
        if (o.up.target < 32) up_writes = o.up.target;
    }
    if ((misc >= 1 && misc <= 3) && uft != 0) {
        rg.set(w, uft, "ur", udest, ur_clean);
        up_writes = uft;
    }
    if (slow_up) {   // commit() writes U: the lower op's results go there first
        rg.spill(w);
        w.put("        commit(su); if (su.write && su.fd > 0 && su.fd < 32) U.vf_ready[su.fd] = cyc + kFmacLatency;\n");
        rg.forget();
    }
    if (up_writes) w.put("        U.vf_ready[%d] = cyc + kFmacLatency;\n", up_writes);
    if (o.writes) w.put("        U.vf_ready[%u] = cyc + kFmacLatency;\n", o.writes);
    w.put("    }\n");
    for (const int r : {up_writes, (int)o.writes}) {
        if (r <= 0 || r >= 32) continue;
        if (t.wrote[r] == t.k && t.wrote_pc[r] == pc) continue;   // both units wrote it: one local
        w.put("    const uint64_t rdy_%04x_%d = cyc + kFmacLatency;\n", pc, r);
        t.wrote[r] = t.k;
        t.wrote_pc[r] = pc;
    }
    if (slow_up) t.barrier = t.k + 1;   // its target is only known at run time
    w.put("    ++cyc; ++n;\n");
    ++t.k;
}

// Compiles the code in one micro memory image into blocks (see AotBlock).
struct AotGen {
    static constexpr uint32_t N = kVu1MicroSize / 8, kMask = kVu1MicroSize - 8;
    std::vector<Op> ops;
    explicit AotGen(const uint8_t* m) : ops(N) {
        g_micro = m;
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t lo, hi;
            std::memcpy(&lo, m + i * 8, 4);
            std::memcpy(&hi, m + i * 8 + 4, 4);
            decode(ops[i], lo, hi, i * 8);
        }
    }
    const Op& op(uint32_t pc) const { return ops[(pc & kMask) / 8]; }
    bool branch(uint32_t pc) const { const Op& o = op(pc); return !o.ibit && lk_branch(o.lk); }

    // The block starting at `s`, as a function named @NAME@. Adds the pairs it runs to
    // `pcs` and the blocks it can continue at to `next` (`targets`: where JR/JALR went).
    void block(AotWriter& w, uint32_t s, std::vector<uint32_t>& pcs, std::vector<uint32_t>& next,
               const std::vector<uint32_t>& targets) const {
        w.put("uint32_t @NAME@(Context* ctx, uint64_t* pn) {\n    uint64_t n = *pn;\n    uint32_t bt = kNoBranch;\n"
              "    uint64_t cyc = U.cycle; uint32_t st = U.status, mc = U.mac;\n@VDECL@L_top:\n");
        AotTiming tm;
        AotRegs rg;
        uint32_t p = s;
        bool prevb = false;   // the pair before ran in this block and is a branch
        for (uint32_t k = 0;; ++k) {
            pcs.push_back(p);
            const Op& o = op(p);
            const bool b = branch(p);
            if ((o.ebit && (b || prevb)) || (b && prevb)) {   // left to the interpreter
                rg.spill(w);
                w.put("    %s run_from(ctx, 0x%xu, bt, false, n); return kAotDone;\n", kAotSpill, p);
                break;
            }
            aot_pair(w, o, p, tm, rg);
            const uint32_t nx = (p + 8) & kMask;
            if (o.ebit) {   // E bit: one more pair, then stop
                pcs.push_back(nx);
                aot_pair(w, op(nx), nx, tm, rg);
                rg.spill(w);
                w.put("    %s aot_finish(0x%xu, n); return kAotDone;\n", kAotSpill, (p + 16) & kMask);
                break;
            }
            if (prevb) {   // the delay slot ran: the branch before it may be taken
                rg.spill(w);   // every way out (and the loop back, which starts with nothing loaded)
                const Op& br = op(p - 8);
                if (lk_dynamic(br.lk)) {
                    w.put("    if (bt != kNoBranch) { %s *pn = n; return bt; }\n", kAotSpill);
                    next.insert(next.end(), targets.begin(), targets.end());
                } else {
                    next.push_back(br.target);
                    if (br.target == s)   // a loop back to this block's start
                        w.put("    if (bt != kNoBranch) { if (n < kMaxPairs) { bt = kNoBranch; goto L_top; } %s *pn = n; return bt; }\n", kAotSpill);
                    else
                        w.put("    if (bt != kNoBranch) { %s *pn = n; return bt; }\n", kAotSpill);
                }
                if (!lk_always(br.lk)) next.push_back(nx);
                w.put("    %s *pn = n; return 0x%xu;\n", kAotSpill, nx);
                break;
            }
            if (b && (o.lk == LK::BAL || o.lk == LK::JALR)) next.push_back((p + 16) & kMask);   // the return address
            prevb = b;
            p = nx;
            if (k + 1 >= 256 && !b) {   // long straight-line code: continue in another block
                rg.spill(w);
                w.put("    %s *pn = n; return 0x%xu;\n", kAotSpill, p);
                next.push_back(p);
                break;
            }
        }
        w.put("}\n\n");
        const size_t at = w.s.rfind("@VDECL@");
        w.s.replace(at, 7, rg.decl());
    }

    // What a block's code depends on: each pair it runs, the one before (whether it is a
    // delay slot) and the 5 after (flag liveness looks ahead).
    static std::vector<uint16_t> ranges(const std::vector<uint32_t>& pcs) {
        std::vector<bool> dep(N);
        for (uint32_t pc : pcs)
            for (int d = -1; d <= 5; ++d) dep[(pc / 8 + N + d) % N] = true;
        std::vector<uint16_t> r;
        for (uint32_t i = 0; i < N;) {
            if (!dep[i]) { ++i; continue; }
            uint32_t j = i;
            while (j < N && dep[j]) ++j;
            r.push_back((uint16_t)i);
            r.push_back((uint16_t)(j - i));
            i = j;
        }
        return r;
    }
};

} // namespace

int vu1_aot_generate(const char* dir, const char* out) {
    namespace fs = std::filesystem;
    AotWriter w;
    w.put("// Generated by `twonk --vu-aot` from VU1 micro memory images: do not edit.\n");
    w.put("#pragma warning(push)\n#pragma warning(disable : 4102 4189 4100 4700 4701 4703)\n");
    struct Blk { uint32_t start; uint64_t hash; std::vector<uint16_t> ranges; };
    std::vector<Blk> blocks;
    std::set<std::pair<uint32_t, uint64_t>> seen;
    size_t images = 0, total = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() != 24 || name.rfind("vu1_", 0) != 0 || e.path().extension() != ".bin") continue;
        std::vector<uint8_t> micro(kVu1MicroSize);
        FILE* f = std::fopen(e.path().string().c_str(), "rb");
        if (!f) continue;
        const bool ok = std::fread(micro.data(), 1, kVu1MicroSize, f) == kVu1MicroSize;
        std::fclose(f);
        if (!ok) { std::fprintf(stderr, "vu-aot: %s is damaged\n", name.c_str()); continue; }
        std::vector<uint32_t> entries, targets;
        fs::path txt = e.path();
        txt.replace_extension(".txt");
        if (FILE* t = std::fopen(txt.string().c_str(), "r")) {
            char line[64];
            while (std::fgets(line, sizeof line, t)) {
                unsigned x;
                if (line[0] == 'j' && std::sscanf(line + 1, "%x", &x) == 1) targets.push_back(x & (kVu1MicroSize - 8));
                else if (std::sscanf(line, "%x", &x) == 1) entries.push_back(x & (kVu1MicroSize - 8));
            }
            std::fclose(t);
        }
        if (entries.empty()) continue;
        ++images;
        const AotGen gen(micro.data());
        std::vector<bool> done(AotGen::N);
        std::vector<uint32_t> work = entries;
        work.insert(work.end(), targets.begin(), targets.end());
        while (!work.empty()) {
            const uint32_t s = work.back() & AotGen::kMask;
            work.pop_back();
            if (done[s / 8]) continue;
            done[s / 8] = true;
            AotWriter bw;
            std::vector<uint32_t> pcs, next;
            gen.block(bw, s, pcs, next, targets);
            work.insert(work.end(), next.begin(), next.end());
            std::vector<uint16_t> r = AotGen::ranges(pcs);
            const uint64_t h = block_hash(micro.data(), s, r.data(), (uint32_t)r.size() / 2);
            if (!seen.insert({s, h}).second) continue;
            char fn[48];
            std::snprintf(fn, sizeof fn, "aot_%04x_%016llx", s, (unsigned long long)h);
            const size_t at = bw.s.find("@NAME@");
            bw.s.replace(at, 6, fn);
            w.s += bw.s;
            total += pcs.size();
            blocks.push_back({s, h, std::move(r)});
        }
    }
    std::sort(blocks.begin(), blocks.end(), [](const Blk& a, const Blk& b) { return a.start != b.start ? a.start < b.start : a.hash < b.hash; });
    for (const Blk& b : blocks) {
        w.put("const uint16_t kR_%04x_%016llx[] = {", b.start, (unsigned long long)b.hash);
        for (size_t k = 0; k < b.ranges.size(); ++k) w.put("%s%u", k ? ", " : "", b.ranges[k]);
        w.put("};\n");
    }
    w.put("const AotBlock kAotBlocks[] = {\n");
    for (const Blk& b : blocks)
        w.put("    {0x%x, 0x%016llxull, kR_%04x_%016llx, %zu, &aot_%04x_%016llx},\n", b.start, (unsigned long long)b.hash, b.start,
              (unsigned long long)b.hash, b.ranges.size() / 2, b.start, (unsigned long long)b.hash);
    w.put("    {~0u, 0, nullptr, 0, nullptr},\n};\n#pragma warning(pop)\n");
    FILE* f = std::fopen(out, "wb");
    if (!f) { std::fprintf(stderr, "vu-aot: cannot write %s\n", out); return 1; }
    std::fwrite(w.s.data(), 1, w.s.size(), f);
    std::fclose(f);
    std::printf("%zu images, %zu blocks, %zu pairs compiled -> %s\n", images, blocks.size(), total, out);
    return 0;
}

namespace {

void run_ref(Context* ctx) {
    uint32_t pc = U.pc;
    uint32_t branch_to = ~0u;   // pending (delay slot) branch
    bool ending = false;
    // A RenderWare microprogram walks at most VU memory (1024 qwords), so it ends
    // within a few hundred thousand pairs. One that runs longer is looping on bad
    // state; stop it rather than stall the game drawing what it kicks.
    const uint64_t max_pairs = g_replaying ? 6000 : kMaxPairs;
    for (uint64_t n = 0; n < max_pairs; ++n) {
        const uint8_t* p = vu1().micro + (pc & (kVu1MicroSize - 8));
        uint32_t lo, hi;
        std::memcpy(&lo, p, 4);
        std::memcpy(&hi, p + 4, 4);
        U.pc = pc;
        if (g_hist_on) ++g_pc_hist[(pc & (kVu1MicroSize - 1)) / 8];
        const int prev_vi = U.last_vi;
        const uint16_t prev_old = U.last_vi_old;
        U.last_vi = -1;
        const bool ibit = (hi & 0x80000000u) != 0;
        const PairInfo& info = pair_info(pc, lo, hi);
        stall_for(info.reads);
        Upper up;
        if (info.up.kind == UKind::Slow) upper(hi, &up);
        else if (info.up.kind != UKind::Nop) {
            static const bool check = std::getenv("TWONK_VU_CHECK") != nullptr;
            if (check) {
                const uint32_t m0 = U.mac, s0 = U.status, t0 = U.flag_q.tail;
                Upper ref;
                upper(hi, &ref);
                const uint32_t m1 = U.mac, s1 = U.status;
                U.mac = m0; U.status = s0; U.flag_q.tail = t0;
                upper_fast(info.up, &up);
                static unsigned bad;
                if ((ref.write != up.write || ref.fd != up.fd || ref.dest != up.dest || std::memcmp(ref.res, up.res, 16) || m1 != U.mac || s1 != U.status) && bad++ < 20)
                    std::fprintf(stderr, "[vucheck] %08x: ref %08x %08x %08x %08x mac %x st %x | fast %08x %08x %08x %08x mac %x st %x\n", hi,
                                 fbits(ref.res[0]), fbits(ref.res[1]), fbits(ref.res[2]), fbits(ref.res[3]), m1, s1,
                                 fbits(up.res[0]), fbits(up.res[1]), fbits(up.res[2]), fbits(up.res[3]), U.mac, U.status);
            } else {
                upper_fast(info.up, &up);
            }
        }
        uint32_t taken = ~0u;
        if (ibit) U.i = ffrom(lo);   // I bit: lower word is an immediate
        else taken = lower(ctx, lo, pc, prev_vi, prev_old);
        commit(up);
        if (up.write && up.fd > 0 && up.fd < 32) U.vf_ready[up.fd] = U.cycle + kFmacLatency;
        if (info.writes) U.vf_ready[info.writes] = U.cycle + kFmacLatency;
        if (g_replaying && g_replay_log) {
            sync_flags();
            std::fprintf(g_replay_log, "%04x %08x %08x cyc %llu mac %04x clip %06x |", pc, hi, lo,
                         (unsigned long long)U.cycle, U.vmac & 0xFFFF, U.vclip & 0xFFFFFF);
            for (int i = 1; i < 16; ++i) std::fprintf(g_replay_log, " %04x", U.vi[i]);
            std::fputc('\n', g_replay_log);
        }
        U.vf[0].f[0] = U.vf[0].f[1] = U.vf[0].f[2] = 0; U.vf[0].f[3] = 1;
        U.vi[0] = 0;
        advance_cycle();
        uint32_t next = (pc + 8) & (kVu1MicroSize - 1);
        if (branch_to != ~0u) { next = branch_to; branch_to = ~0u; }
        if (taken != ~0u) branch_to = taken;
        if (ending) {
            U.pc = next;
            g_pairs += n + 1;
            perf_add(kPerfVu1Pairs, n + 1);
            g_prog_pairs[g_last_start] += n + 1;
            if (n > g_longest) g_longest = n;
            return;
        }
        if (hi & 0x40000000u) ending = true;   // E bit: one more pair, then stop
        pc = next;
    }
    ++g_runaways;
    g_aborted = true;
    if (g_replaying) return;
    replay_runaway(ctx);
    static unsigned reported;
    if (reported++ < 4) {   // a runaway microprogram: say where it loops and on what
        std::fprintf(stderr, "[gpu] VU1: program at %04x did not end, now at %04x (longest normal run %llu pairs); vi:",
                     g_last_start, U.pc, (unsigned long long)g_longest);
        for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %04x", U.vi[i]);
        std::fprintf(stderr, "\n[gpu]   vf1 %g %g %g %g, top %04x itop %04x\n", U.vf[1].f[0], U.vf[1].f[1],
                     U.vf[1].f[2], U.vf[1].f[3], vif1_top(), vif1_itop());
        if (reported == 1)
            if (FILE* f = std::fopen("vu1_runaway.bin", "wb")) {   // micro memory, then data memory
                std::fwrite(vu1().micro, 1, kVu1MicroSize, f);
                std::fwrite(vu1().mem, 1, kVu1MemSize, f);
                std::fclose(f);
            }
    }
}

} // namespace

// ---- benchmark snapshots ---------------------------------------------------------------
// TWONK_VU_SNAP=<pc>:<n>[:<file>] writes the architectural VU1 state at the n-th start of
// the program at <pc>; `twonk --vu-bench <file> [runs]` replays it and reports the
// speed and a checksum of the end state (an optimisation must keep the checksum).
namespace {
struct SnapFile {
    char magic[8];
    uint32_t pc, top, itop, pad;
    Vf vf[32], acc;
    uint16_t vi[16];
    float q, p, i;
    uint32_t r, mac, status, clip, vmac, vstatus, vclip;
    uint8_t mem[kVu1MemSize], micro[kVu1MicroSize];
};
bool g_snap_parsed = false;
std::vector<std::pair<uint32_t, uint32_t>> g_snap_list;

void snap_write(uint32_t pc, const char* file) {
    auto* s = new SnapFile{};
    std::memcpy(s->magic, "VU1SNAP1", 8);
    sync_flags();
    s->pc = pc; s->top = vif1_top(); s->itop = vif1_itop();
    std::memcpy(s->vf, U.vf, sizeof s->vf); s->acc = U.acc;
    std::memcpy(s->vi, U.vi, sizeof s->vi);
    s->q = U.q; s->p = U.p; s->i = U.i; s->r = U.r;
    s->mac = U.mac; s->status = U.status; s->clip = U.clip; s->vmac = U.vmac; s->vstatus = U.vstatus; s->vclip = U.vclip;
    std::memcpy(s->mem, vu1().mem, kVu1MemSize);
    std::memcpy(s->micro, vu1().micro, kVu1MicroSize);
    if (FILE* f = std::fopen(file, "wb")) {
        std::fwrite(s, sizeof *s, 1, f);
        std::fclose(f);
        std::fprintf(stderr, "[gpu] VU1 snapshot of program %04x written to %s\n", pc, file);
    }
    delete s;
}

void snap_load(const SnapFile& s) {
    U = Vu{};
    std::memcpy(U.vf, s.vf, sizeof s.vf); U.acc = s.acc;
    std::memcpy(U.vi, s.vi, sizeof s.vi);
    U.q = s.q; U.p = s.p; U.i = s.i; U.r = s.r;
    U.mac = s.mac; U.status = s.status; U.clip = s.clip; U.vmac = s.vmac; U.vstatus = s.vstatus; U.vclip = s.vclip;
    U.pc = s.pc;
    std::memcpy(vu1().mem, s.mem, kVu1MemSize);
    if (std::memcmp(vu1().micro, s.micro, kVu1MicroSize)) {   // as MPG: only a real change counts
        std::memcpy(vu1().micro, s.micro, kVu1MicroSize);
        ++g_micro_gen;
        aot_forget_all();
    }
    g_bench_top_v = s.top; g_bench_itop_v = s.itop;
}

uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t k = 0; k < n; ++k) h = (h ^ b[k]) * 0x100000001B3ull;
    return h;
}
} // namespace

int vu1_bench(const char* path, int runs) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "vu-bench: cannot open %s\n", path); return 1; }
    auto* s = new SnapFile{};
    const bool ok = std::fread(s, sizeof *s, 1, f) == 1 && !std::memcmp(s->magic, "VU1SNAP1", 8);
    std::fclose(f);
    if (!ok) { std::fprintf(stderr, "vu-bench: %s is not a VU1 snapshot\n", path); return 1; }
    g_bench_mode = true;
    for (auto& o : g_ops) o.valid = false;   // decoded once, then reused by every run, as in the game
    uint64_t sum = 0, pairs = 0;
    double secs = 0;
    for (int k = 0; k < runs; ++k) {
        snap_load(*s);
        if (k == 0) dump_image(s->pc);
        const uint64_t p0 = g_pairs;
        const auto t0 = std::chrono::steady_clock::now();
        run(nullptr);
        finish_q();
        secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        pairs += g_pairs - p0;
        if (k == 0) {
            uint64_t h = 0xCBF29CE484222325ull;
            h = fnv(h, U.vf, sizeof U.vf); h = fnv(h, &U.acc, sizeof U.acc); h = fnv(h, U.vi, sizeof U.vi);
            h = fnv(h, &U.q, 4); h = fnv(h, &U.p, 4); h = fnv(h, &U.r, 4); h = fnv(h, &U.mac, 4); h = fnv(h, &U.status, 4);
            h = fnv(h, &U.clip, 4); h = fnv(h, &U.pc, 4); h = fnv(h, &U.cycle, 8);
            U.cycle += 8;   // let every flag result land, then include the visible flags
            sync_flags();
            U.cycle -= 8;
            h = fnv(h, &U.vmac, 4); h = fnv(h, &U.vstatus, 4); h = fnv(h, &U.vclip, 4);
            if (std::getenv("TWONK_VU_BENCH_FLAGS")) std::printf("vmac %x vstatus %x vclip %x mac %x status %x\n", U.vmac, U.vstatus, U.vclip, U.mac, U.status);
            h = fnv(h, vu1().mem, kVu1MemSize);
            sum = h;
        }
    }
    std::printf("%s: program %04x, %llu pairs/run, %.2f ns/pair, %.1f us/run, checksum %016llx\n", path, s->pc,
                (unsigned long long)(pairs / runs), secs * 1e9 / (double)pairs, secs * 1e6 / runs, (unsigned long long)sum);
    delete s;
    return 0;
}


// TWONK_VU_VERIFY=1: run every program on the fast path, then again from the same
// starting state on the reference path (run_ref, without XGKICK output), and compare
// the whole end state. The fast path's state is kept.
struct VerifyCopy { Vu u; alignas(16) uint8_t mem[kVu1MemSize]; };

uint64_t effective_flags_hash(Vu u) {   // visible flags once everything lands, and what is still in flight
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = u.flag_q.head; i != u.flag_q.tail; ++i) {
        const FlagSnap& f = u.flag_q.e[i & 15];
        if (f.at <= u.cycle) { u.vmac = f.mac; u.vstatus = f.status; if (f.has_clip) u.vclip = f.clip; continue; }
        h = fnv(h, &f.at, 8); h = fnv(h, &f.mac, 4); h = fnv(h, &f.status, 4); h = fnv(h, &f.has_clip, 1);
        if (f.has_clip) h = fnv(h, &f.clip, 4);
    }
    h = fnv(h, &u.vmac, 4); h = fnv(h, &u.vstatus, 4); h = fnv(h, &u.vclip, 4);
    return h;
}

void run_verified(Context* ctx) {
    static VerifyCopy* start = new VerifyCopy;
    static VerifyCopy* fast = new VerifyCopy;
    static uint64_t checked, bad;
    const uint32_t pc0 = U.pc;
    start->u = U;
    std::memcpy(start->mem, vu1().mem, kVu1MemSize);
    run(ctx);
    finish_q();
    fast->u = U;
    std::memcpy(fast->mem, vu1().mem, kVu1MemSize);
    U = start->u;
    std::memcpy(vu1().mem, start->mem, kVu1MemSize);
    g_bench_mode = true;   // no second XGKICK; XTOP/XITOP as the fast run saw them
    g_bench_top_v = vif1_top(); g_bench_itop_v = vif1_itop();
    g_vumem = vu1().mem;
    run_ref(ctx);
    finish_q();
    g_bench_mode = false;
    const Vu& a = fast->u;
    const Vu& b = U;
    const char* what = nullptr;
    if (std::memcmp(a.vf, b.vf, sizeof a.vf)) what = "vf";
    else if (std::memcmp(a.vi, b.vi, sizeof a.vi)) what = "vi";
    else if (std::memcmp(&a.acc, &b.acc, sizeof a.acc)) what = "acc";
    else if (a.q != b.q || a.p != b.p || a.i != b.i || a.r != b.r) what = "q/p/i/r";
    else if (a.mac != b.mac || a.status != b.status || a.clip != b.clip) what = "mac/status/clip";
    else if (a.cycle != b.cycle || a.pc != b.pc) what = "cycle/pc";
    else if (std::memcmp(a.vf_ready, b.vf_ready, sizeof a.vf_ready)) what = "vf_ready";
    else if (a.last_vi != b.last_vi || a.last_vi_old != b.last_vi_old) what = "last_vi";
    else if (effective_flags_hash(a) != effective_flags_hash(b)) what = "visible flags";
    else if (std::memcmp(fast->mem, vu1().mem, kVu1MemSize)) what = "data memory";
    ++checked;
    if (what && bad++ < 20)
        std::fprintf(stderr, "[vu-verify] program %04x: fast and reference paths differ in %s\n", pc0, what);
    if (checked % 200000 == 0)
        std::fprintf(stderr, "[vu-verify] %llu programs checked, %llu differ\n", (unsigned long long)checked, (unsigned long long)bad);
    U = fast->u;   // continue from the fast path's result
    std::memcpy(vu1().mem, fast->mem, kVu1MemSize);
}

// `twonk --vu-fuzz <TWONK_VU_DUMP dir> [seeds]`: for every dumped micro memory image and
// each pair its notes list, run from random register and data memory contents on the
// compiled path (run()) and on the reference path (run_ref()), and compare the whole end
// state as TWONK_VU_VERIFY does. Programs that hit the pair limit on either path are
// skipped (the two may stop at different pairs then). No XGKICK output (bench mode).
int vu1_fuzz(const char* dir, int seeds) {
    namespace fs = std::filesystem;
    uint64_t rng = 0x243F6A8885A308D3ull;
    auto next = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    auto rfloat = [&]() -> uint32_t {
        const uint64_t k = next() % 100, v = next();
        if (k < 60) return fbits((float)((double)(v % 2000001) / 10000.0 - 100.0));   // -100..100
        if (k < 72) return fbits((float)((int64_t)(v % 64) - 32));                     // small integers
        if (k < 77) return (v & 1) ? 0x80000000u : 0u;                                   // zeros
        if (k < 82) return (uint32_t)(v & 0x807FFFFFu);                                  // denormals
        if (k < 87) return (uint32_t)((v & 0x807FFFFFu) | 0x7F800000u);                  // Inf / NaN
        if (k < 92) return (uint32_t)(v % 1024);                                         // small int bits
        return (uint32_t)v;                                                              // anything
    };
    auto* start = new VerifyCopy;
    auto* fast = new VerifyCopy;
    uint64_t runs = 0, skipped = 0, bad = 0;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".bin" && e.path().filename().string().rfind("vu1_", 0) == 0) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    g_bench_mode = true;
    for (const fs::path& bin : files) {
        std::vector<uint8_t> micro(kVu1MicroSize);
        FILE* f = std::fopen(bin.string().c_str(), "rb");
        if (!f) continue;
        const bool ok = std::fread(micro.data(), 1, kVu1MicroSize, f) == kVu1MicroSize;
        std::fclose(f);
        if (!ok) continue;
        std::vector<uint32_t> entries;
        fs::path txt = bin;
        txt.replace_extension(".txt");
        if (FILE* t = std::fopen(txt.string().c_str(), "r")) {
            char line[64];
            unsigned x;
            while (std::fgets(line, sizeof line, t))
                if (line[0] != 'j' && std::sscanf(line, "%x", &x) == 1) entries.push_back(x & (kVu1MicroSize - 8));
            std::fclose(t);
        }
        std::sort(entries.begin(), entries.end());
        entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
        if (entries.empty()) continue;
        std::memcpy(vu1().micro, micro.data(), kVu1MicroSize);
        ++g_micro_gen;
        aot_forget_all();
        for (auto& o : g_ops) o.valid = false;
        for (const uint32_t pc : entries)
            for (int sd = 0; sd < seeds; ++sd) {
                U = Vu{};
                for (int r = 1; r < 32; ++r) for (int c = 0; c < 4; ++c) U.vf[r].f[c] = ffrom(rfloat());
                for (int c = 0; c < 4; ++c) U.acc.f[c] = ffrom(rfloat());
                for (int r = 1; r < 16; ++r) U.vi[r] = (uint16_t)(next() % 16 == 0 ? next() : next() % 2 ? next() % 24 : next() % 1024);
                U.q = vu_float(ffrom(rfloat())); U.p = ffrom(rfloat()); U.i = ffrom(rfloat());
                U.r = 0x3F800000u | (uint32_t)(next() & 0x7FFFFF);
                U.mac = (uint32_t)(next() & 0xFFFF); U.status = (uint32_t)(next() & 0xFFF); U.clip = (uint32_t)(next() & 0xFFFFFF);
                U.vmac = U.mac; U.vstatus = U.status; U.vclip = U.clip;
                U.cycle = 1000 + next() % 1000;
                for (int r = 1; r < 32; ++r) U.vf_ready[r] = U.cycle - 4 + next() % 8;   // at most 3 ahead
                if (next() % 2) { U.q_busy = true; U.q_pending = vu_float(ffrom(rfloat())); U.q_ready = U.cycle + next() % 14; }
                for (int k = (int)(next() % 4); k > 0; --k)
                    U.flag_q.push_back({U.cycle + (uint64_t)(4 - k), (uint32_t)(next() & 0xFF), (uint32_t)(next() & 0xFFF),
                                        (uint32_t)(next() & 0xFFFFFF), (next() & 1) != 0});
                U.last_vi = next() % 3 ? -1 : (int)(1 + next() % 15);
                U.last_vi_old = (uint16_t)next();
                U.pc = pc;
                U.vf[0].f[0] = U.vf[0].f[1] = U.vf[0].f[2] = 0; U.vf[0].f[3] = 1;
                for (uint32_t i = 0; i < kVu1MemSize / 4; ++i) {   // loop counts come from here: keep most small
                    const uint32_t v = next() % 3 == 0 ? (uint32_t)(next() % 24) : rfloat();
                    std::memcpy(vu1().mem + i * 4, &v, 4);
                }
                g_bench_top_v = (uint32_t)(next() % 1024); g_bench_itop_v = (uint32_t)(next() % 1024);
                start->u = U;
                std::memcpy(start->mem, vu1().mem, kVu1MemSize);
                const uint64_t ra = g_runaways;
                run(nullptr);
                finish_q();
                fast->u = U;
                std::memcpy(fast->mem, vu1().mem, kVu1MemSize);
                U = start->u;
                std::memcpy(vu1().mem, start->mem, kVu1MemSize);
                g_vumem = vu1().mem;
                run_ref(nullptr);
                finish_q();
                ++runs;
                if (g_runaways != ra) { ++skipped; continue; }
                const Vu& a = fast->u;
                const Vu& b = U;
                const char* what = nullptr;
                if (std::memcmp(a.vf, b.vf, sizeof a.vf)) what = "vf";
                else if (std::memcmp(a.vi, b.vi, sizeof a.vi)) what = "vi";
                else if (std::memcmp(&a.acc, &b.acc, sizeof a.acc)) what = "acc";
                else if (fbits(a.q) != fbits(b.q) || fbits(a.p) != fbits(b.p) || fbits(a.i) != fbits(b.i) || a.r != b.r) what = "q/p/i/r";
                else if (a.mac != b.mac || a.status != b.status || a.clip != b.clip) what = "mac/status/clip";
                else if (a.cycle != b.cycle || a.pc != b.pc) what = "cycle/pc";
                else if (std::memcmp(a.vf_ready, b.vf_ready, sizeof a.vf_ready)) what = "vf_ready";
                else if (a.last_vi != b.last_vi || a.last_vi_old != b.last_vi_old) what = "last_vi";
                else if (effective_flags_hash(a) != effective_flags_hash(b)) what = "visible flags";
                else if (std::memcmp(fast->mem, vu1().mem, kVu1MemSize)) what = "data memory";
                if (what && bad++ < 30)
                    std::printf("%s: start %04x seed %d: compiled and reference paths differ in %s\n", bin.filename().string().c_str(), pc, sd, what);
            }
    }
    g_bench_mode = false;
    std::printf("vu-fuzz: %llu runs, %llu skipped (pair limit), %llu differ; %llu compiled starts, %llu interpreted\n",
                (unsigned long long)runs, (unsigned long long)skipped, (unsigned long long)bad, (unsigned long long)g_aot_hits,
                (unsigned long long)g_aot_misses);
    return bad ? 1 : 0;
}

void vu1_start(Context* ctx, uint32_t pc) {
    gpu::ProfScope prof_scope(gpu::prof().vu1);
    if (pc == 0xFFFFFFFFu && g_aborted) return;   // nothing to continue
    g_aborted = false;
    if (pc != 0xFFFFFFFFu) U.pc = pc & (kVu1MicroSize - 1);   // MSCNT continues at TPC
    if (!g_snap_parsed) {   // TWONK_VU_SNAP=<pc>:<n>[,<pc>:<n>...]
        g_snap_parsed = true;
        char buf[512]; size_t n = 0;
        if (!getenv_s(&n, buf, sizeof buf, "TWONK_VU_SNAP") && n)
            for (char* e = buf; *e;) {
                const uint32_t spc = (uint32_t)std::strtoul(e, &e, 16);
                uint32_t sn = 1;
                if (*e == ':') sn = (uint32_t)std::strtoul(e + 1, &e, 10);
                g_snap_list.push_back({spc, sn});
                if (*e != ',') break;
                ++e;
            }
    }
    for (const auto& sp : g_snap_list)
        if (U.pc == sp.first && g_starts[U.pc] + 1 == sp.second) {
            char name[64];
            std::snprintf(name, sizeof name, "vu1_snap_%04x_%u.bin", sp.first, sp.second);
            snap_write(U.pc, name);
        }
    ++g_starts[U.pc];
    g_last_start = U.pc;
    if (g_trace_pc == ~0u) {
        char buf[16]; size_t n = 0;
        g_trace_pc = (getenv_s(&n, buf, sizeof buf, "TWONK_VU_TRACE") || !n) ? 0xFFFFFFFEu : (uint32_t)std::strtoul(buf, nullptr, 16);
    }
    if (U.pc == g_trace_pc) {
        if (!g_snap) g_snap = new Snapshot;
        g_snap->valid = true;
        g_snap->u = U;
        std::memcpy(g_snap->mem, vu1().mem, kVu1MemSize);
    }
    U.vf[0].f[3] = 1.0f;
    dump_image(U.pc);
    static const bool verify = std::getenv("TWONK_VU_VERIFY") != nullptr;
    if (verify) { run_verified(ctx); return; }
    run(ctx);
    finish_q();
}

void vu1_print_stats() {
    log(Log::Gpu, "VU1 %llu pairs; %llu program starts compiled, %llu interpreted", (unsigned long long)g_pairs, (unsigned long long)g_aot_hits, (unsigned long long)g_aot_misses);
    if (g_hist_on) {
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (uint32_t i = 0; i < kVu1MicroSize / 8; ++i) if (g_pc_hist[i]) v.push_back({g_pc_hist[i], i * 8});
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second < b.second; });
        if (FILE* f = std::fopen("vu1_hist.txt", "w")) {
            for (auto& e : v) {
                uint32_t lo, hi;
                std::memcpy(&lo, vu1().micro + e.second, 4); std::memcpy(&hi, vu1().micro + e.second + 4, 4);
                std::fprintf(f, "%04x %08x %08x %llu\n", e.second, hi, lo, (unsigned long long)e.first);
            }
            std::fclose(f);
        }
    }
    for (const auto& kv : g_starts) log(Log::Gpu, "  VU1 program %04x started %llu times, %llu pairs", kv.first, (unsigned long long)kv.second, (unsigned long long)g_prog_pairs[kv.first]);
}

} // namespace ps2::gpu
