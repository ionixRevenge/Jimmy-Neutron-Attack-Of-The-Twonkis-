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
#include <deque>
#include <map>

#include "gpu.hpp"
#include "ps2/runtime.hpp"

namespace ps2::gpu {

uint32_t vif1_top();
uint32_t vif1_itop();

namespace {

std::map<uint32_t, uint64_t> g_starts;

struct Vf { float f[4]; };

struct FlagSnap { uint64_t at; uint32_t mac, status, clip; };

struct Vu {
    Vf vf[32];
    uint16_t vi[16];
    Vf acc;
    float q = 1, p = 0, i = 0;
    uint32_t r = 0x3F800000;
    uint32_t mac = 0, status = 0, clip = 0;           // values as written by the FMAC
    uint32_t vmac = 0, vstatus = 0, vclip = 0;        // values the lower unit can see
    std::deque<FlagSnap> flag_q;
    float q_pending = 0; uint64_t q_ready = 0; bool q_busy = false;
    uint64_t cycle = 0;
    uint32_t pc = 0;
    // previous pair's integer write (for the branch hazard)
    int last_vi = -1; uint16_t last_vi_old = 0;
} U;

inline uint8_t* mem() { return vu1().mem; }
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
    U.flag_q.push_back({U.cycle + 4, U.mac, U.status, U.clip});
}

void advance_cycle() {
    ++U.cycle;
    while (!U.flag_q.empty() && U.flag_q.front().at <= U.cycle) {
        U.vmac = U.flag_q.front().mac;
        U.vstatus = U.flag_q.front().status;
        U.vclip = U.flag_q.front().clip;
        U.flag_q.pop_front();
    }
    if (U.q_busy && U.cycle >= U.q_ready) { U.q = U.q_pending; U.q_busy = false; }
}

void finish_q() {
    if (U.q_busy) { U.q = U.q_pending; U.q_busy = false; }
}

void start_q(float v, int latency) {
    finish_q();   // a second divide waits for the first
    U.q_pending = vu_float(v);
    U.q_ready = U.cycle + latency;
    U.q_busy = true;
}

// ---- upper (FMAC) ------------------------------------------------------------------------
struct Upper {
    bool write = false;      // has a register result to commit
    int fd = 0;              // 0..31, or 32 = ACC
    uint32_t dest = 0;
    float res[4];
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
    case 0x1C: scalar(U.q); kind = Mul; break;
    case 0x1D: scalar(U.i); kind = Max; break;
    case 0x1E: scalar(U.i); kind = Mul; break;
    case 0x1F: scalar(U.i); kind = Min; break;
    case 0x20: scalar(U.q); kind = Add; break;
    case 0x21: scalar(U.q); kind = Madd; break;
    case 0x22: scalar(U.i); kind = Add; break;
    case 0x23: scalar(U.i); kind = Madd; break;
    case 0x24: scalar(U.q); kind = Sub; break;
    case 0x25: scalar(U.q); kind = Msub; break;
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
    case 0x11C: scalar(U.q); kind = Mul; break;                                            // MULAq
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
        U.flag_q.push_back({U.cycle + 4, U.mac, U.status, U.clip});
        return;
    }
    case 0x120: scalar(U.q); kind = Add; break;   // ADDAq
    case 0x121: scalar(U.q); kind = Madd; break;  // MADDAq
    case 0x122: scalar(U.i); kind = Add; break;   // ADDAi
    case 0x123: scalar(U.i); kind = Madd; break;  // MADDAi
    case 0x124: scalar(U.q); kind = Sub; break;   // SUBAq
    case 0x125: scalar(U.q); kind = Msub; break;  // MSUBAq
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

void commit(const Upper& u) {
    if (!u.write) return;
    if (u.fd == 0) return;   // vf0 is constant
    Vf& d = u.fd == 32 ? U.acc : U.vf[u.fd];
    for (int c = 0; c < 4; ++c)
        if (u.dest & (1u << comp_bit(c))) d.f[c] = u.res[c];
}

// ---- lower ------------------------------------------------------------------------------
void xgkick(Context* ctx, uint32_t addr) {
    // walk GIF tags in VU1 memory until EOP, then hand the packet to PATH1
    uint32_t a = addr & 0x3FF;
    for (int guard = 0; guard < 1024; ++guard) {
        const uint64_t* tag = reinterpret_cast<const uint64_t*>(mem() + a * 16);
        const uint32_t nloop = (uint32_t)(tag[0] & 0x7FFF), flg = (uint32_t)((tag[0] >> 58) & 3);
        uint32_t nreg = (uint32_t)((tag[0] >> 60) & 0xF);
        if (!nreg) nreg = 16;
        const bool eop = (tag[0] >> 15) & 1;
        const uint32_t qw = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        for (uint32_t i = 0; i <= qw; ++i) gif_write(Path::P1, mem() + ((a + i) & 0x3FF) * 16, 1);
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
    case 0x10: set_vi(1, (U.vclip & 0xFFFFFF) == (w & 0xFFFFFF)); return ~0u;               // FCEQ
    case 0x11: U.clip = U.vclip = w & 0xFFFFFF; return ~0u;                                // FCSET
    case 0x12: set_vi(1, (U.vclip & w & 0xFFFFFF) != 0); return ~0u;                        // FCAND
    case 0x13: set_vi(1, ((U.vclip | w) & 0xFFFFFF) == 0xFFFFFF); return ~0u;               // FCOR
    case 0x14: set_vi((int)(it & 15), (U.vstatus & 0xFFF) == ((w & 0x7FF) | ((w >> 10) & 0x800))); return ~0u;   // FSEQ
    case 0x15: U.status = (U.status & 0x3F) | (((w & 0x7FF) | ((w >> 10) & 0x800)) & 0xFC0); return ~0u;          // FSSET
    case 0x16: set_vi((int)(it & 15), (uint16_t)(U.vstatus & ((w & 0x7FF) | ((w >> 10) & 0x800)))); return ~0u;  // FSAND
    case 0x17: set_vi((int)(it & 15), (uint16_t)(U.vstatus | ((w & 0x7FF) | ((w >> 10) & 0x800)))); return ~0u;  // FSOR
    case 0x18: set_vi((int)(it & 15), (U.vmac & 0xFFFF) == vi(is)); return ~0u;            // FMEQ
    case 0x1A: set_vi((int)(it & 15), (uint16_t)(U.vmac & vi(is))); return ~0u;             // FMAND
    case 0x1B: set_vi((int)(it & 15), (uint16_t)(U.vmac | vi(is))); return ~0u;             // FMOR
    case 0x1C: set_vi((int)(it & 15), (uint16_t)(U.vclip & 0xFFF)); return ~0u;             // FCGET
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
    case 0x68: set_vi((int)(it & 15), (uint16_t)vif1_top()); return ~0u;                    // XTOP
    case 0x69: set_vi((int)(it & 15), (uint16_t)vif1_itop()); return ~0u;                   // XITOP
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

void run(Context* ctx) {
    uint32_t pc = U.pc;
    uint32_t branch_to = ~0u;   // pending (delay slot) branch
    bool ending = false;
    for (uint64_t n = 0; n < 50000000ull; ++n) {
        const uint8_t* p = vu1().micro + (pc & (kVu1MicroSize - 8));
        uint32_t lo, hi;
        std::memcpy(&lo, p, 4);
        std::memcpy(&hi, p + 4, 4);
        U.pc = pc;
        const int prev_vi = U.last_vi;
        const uint16_t prev_old = U.last_vi_old;
        U.last_vi = -1;
        Upper up;
        upper(hi, &up);
        uint32_t taken = ~0u;
        if (hi & 0x80000000u) U.i = ffrom(lo);   // I bit: lower word is an immediate
        else taken = lower(ctx, lo, pc, prev_vi, prev_old);
        commit(up);
        U.vf[0].f[0] = U.vf[0].f[1] = U.vf[0].f[2] = 0; U.vf[0].f[3] = 1;
        U.vi[0] = 0;
        advance_cycle();
        uint32_t next = (pc + 8) & (kVu1MicroSize - 1);
        if (branch_to != ~0u) { next = branch_to; branch_to = ~0u; }
        if (taken != ~0u) branch_to = taken;
        if (ending) { U.pc = next; return; }
        if (hi & 0x40000000u) ending = true;   // E bit: one more pair, then stop
        pc = next;
    }
    log(Log::Gpu, "VU1: program at %04x did not end", U.pc);
}

} // namespace

void vu1_start(Context* ctx, uint32_t pc) {
    gpu::ProfScope prof_scope(gpu::prof().vu1);
    if (pc != 0xFFFFFFFFu) U.pc = pc & (kVu1MicroSize - 1);   // MSCNT continues at TPC
    ++g_starts[U.pc];
    U.vf[0].f[3] = 1.0f;
    run(ctx);
    finish_q();
}

void vu1_print_stats() {
    for (const auto& kv : g_starts) log(Log::Gpu, "  VU1 program %04x started %llu times", kv.first, (unsigned long long)kv.second);
}

} // namespace ps2::gpu
