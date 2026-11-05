// VIF1: decodes the VIF code stream that DMA channel 1 delivers.
//
// UNPACK writes VU1 data memory, MPG writes VU1 micro memory, DIRECT/DIRECTHL
// forward GIF packets (PATH2) and MSCAL/MSCNT start VU1 microprograms.
// Command data may span DMA packets, so this is a word-at-a-time state machine.
// (Behaviour follows the EE User's Manual, VIF chapter.)
#include <algorithm>
#include <cstring>
#include <vector>

#include "gpu.hpp"
#include "ps2/runtime.hpp"

namespace ps2 {
void gif_path3_resume(Context* ctx);   // hw.cpp
}

namespace ps2::gpu {

void vu1_start(Context* ctx, uint32_t pc);   // vu1.cpp

namespace {

Vu1 g_vu1;

struct Vif {
    // registers
    uint32_t cl = 1, wl = 1, mode = 0, mask = 0, row[4] = {}, col[4] = {};
    uint32_t base = 0, ofst = 0, tops = 0, top = 0, itop = 0, itops = 0, mark = 0;
    bool dbf = false, mskpath3 = false;
    // command in progress
    uint32_t code = 0, need = 0;            // words still expected for this command
    std::vector<uint32_t> buf;              // its data so far
    uint32_t direct_qw_left = 0;            // DIRECT: quadwords left
    uint32_t direct_word = 0;               // DIRECT: words of the current quadword
    alignas(16) uint32_t direct_q[4] = {};
} V;

uint32_t unpack_words(uint32_t code, uint32_t num) {
    const uint32_t vl = code & 3, vn = (code >> 2) & 3;
    const uint32_t vec_bits = (vn == 3 && vl == 3) ? 16 : (32u >> vl) * (vn + 1);
    const uint32_t n = V.cl >= V.wl ? num : V.cl * (num / V.wl) + std::min(num % V.wl, V.cl);
    return (n * vec_bits + 31) / 32;
}

uint32_t cmd_words(uint32_t code) {
    const uint32_t cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
    switch (cmd) {
    case 0x20: return 1;                                   // STMASK
    case 0x30: case 0x31: return 4;                        // STROW / STCOL
    case 0x4A: return (num ? num : 256) * 2;               // MPG
    case 0x50: case 0x51: return 0;                        // DIRECT(HL): streamed separately
    default:
        if (cmd >= 0x60) return unpack_words(cmd, num ? num : 256);
        (void)imm;
        return 0;
    }
}

// Reads element `i` (component c) of an UNPACK stream.
uint32_t element(const std::vector<uint32_t>& d, uint32_t bits, uint32_t i, bool usn) {
    const uint32_t bit = i * bits;
    const uint32_t w = d[bit / 32] >> (bit % 32);
    switch (bits) {
    case 8: return usn ? (w & 0xFF) : (uint32_t)(int32_t)(int8_t)(w & 0xFF);
    case 16: return usn ? (w & 0xFFFF) : (uint32_t)(int32_t)(int16_t)(w & 0xFFFF);
    default: return d[bit / 32];
    }
}

void do_unpack() {
    const uint32_t code = V.code, cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF;
    const uint32_t num = ((code >> 16) & 0xFF) ? (code >> 16) & 0xFF : 256;
    const uint32_t vl = cmd & 3, vn = (cmd >> 2) & 3;
    const bool masked = (cmd & 0x10) != 0, usn = (imm >> 14) & 1, flg = (imm >> 15) & 1;
    const bool v45 = vn == 3 && vl == 3;
    const uint32_t bits = 32u >> vl, comps = vn + 1;
    uint32_t addr = ((imm & 0x3FF) + (flg ? V.tops : 0)) & 0x3FF;
    uint32_t in = 0;   // input vectors consumed
    ++stats().vif1_unpack;
    for (uint32_t i = 0, cyc = 0; i < num; ++i) {
        const bool fill = cyc >= V.cl;   // filling write (WL > CL): no input data
        uint32_t v[4] = {};
        if (!fill) {
            if (v45) {
                const uint32_t x = element(V.buf, 16, in, true);
                v[0] = (x & 0x1F) << 3; v[1] = ((x >> 5) & 0x1F) << 3; v[2] = ((x >> 10) & 0x1F) << 3; v[3] = ((x >> 15) & 1) << 7;
            } else {
                for (uint32_t c = 0; c < comps; ++c) v[c] = element(V.buf, bits, in * comps + c, usn);
                if (comps == 1) v[1] = v[2] = v[3] = v[0];
            }
            ++in;
        }
        uint32_t* dst = reinterpret_cast<uint32_t*>(g_vu1.mem + addr * 16);
        const uint32_t mrow = std::min(cyc, 3u);
        for (uint32_t c = 0; c < 4; ++c) {
            if (!v45 && comps > 1 && c >= comps && !masked) continue;   // V2/V3 leave the rest alone
            uint32_t m = masked ? (V.mask >> (mrow * 8 + c * 2)) & 3 : 0;
            if (fill && !masked) m = 1;
            uint32_t val = v[c];
            switch (m) {
            case 0:
                if (V.mode == 1) val += V.row[c];
                else if (V.mode == 2) { V.row[c] += val; val = V.row[c]; }
                break;
            case 1: val = V.row[c]; break;
            case 2: val = V.col[mrow]; break;
            default: continue;   // write-protected
            }
            dst[c] = val;
        }
        addr = (addr + 1) & 0x3FF;
        // a block is WL writes; with CL > WL the next CL - WL quadwords are skipped
        if (++cyc >= std::max(V.wl, 1u)) {
            if (V.cl > V.wl) addr = (addr + V.cl - V.wl) & 0x3FF;
            cyc = 0;
        }
    }
}

void mscal(Context* ctx, uint32_t pc) {
    ++stats().vif1_mscal;
    V.top = V.tops;
    V.itop = V.itops;
    V.dbf = !V.dbf;
    V.tops = V.dbf ? V.base + V.ofst : V.base;
    vu1_start(ctx, pc);
}

uint32_t g_last_pc = 0;

void finish(Context* ctx) {
    const uint32_t cmd = (V.code >> 24) & 0x7F, imm = V.code & 0xFFFF;
    switch (cmd) {
    case 0x20: V.mask = V.buf[0]; break;
    case 0x30: std::memcpy(V.row, V.buf.data(), 16); break;
    case 0x31: std::memcpy(V.col, V.buf.data(), 16); break;
    case 0x4A: {
        const uint32_t at = (imm * 8) % kVu1MicroSize;
        const uint32_t bytes = (uint32_t)V.buf.size() * 4;
        for (uint32_t i = 0; i < bytes; ++i) g_vu1.micro[(at + i) % kVu1MicroSize] = reinterpret_cast<const uint8_t*>(V.buf.data())[i];
        stats().vif1_mpg_words += V.buf.size();
        break;
    }
    default:
        if (cmd >= 0x60) do_unpack();
        break;
    }
    (void)ctx;
    V.buf.clear();
}

void command(Context* ctx, uint32_t code) {
    const uint32_t cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF;
    V.code = code;
    switch (cmd) {
    case 0x00: return;                                                     // NOP
    case 0x01: V.cl = imm & 0xFF; V.wl = (imm >> 8) & 0xFF; return;        // STCYCL
    case 0x02: V.ofst = imm & 0x3FF; V.dbf = false; V.tops = V.base; return;   // OFFSET
    case 0x03: V.base = imm & 0x3FF; return;                               // BASE
    case 0x04: V.itops = imm & 0x3FF; return;                              // ITOP
    case 0x05: V.mode = imm & 3; return;                                   // STMOD
    case 0x06:                                                             // MSKPATH3
        V.mskpath3 = (imm & 0x8000) != 0;
        if (!V.mskpath3) gif_path3_resume(ctx);
        return;
    case 0x07: V.mark = imm; return;                                       // MARK
    case 0x10: case 0x11: case 0x13: return;                               // FLUSHE/FLUSH/FLUSHA: VU1 runs synchronously
    case 0x14: case 0x15: g_last_pc = imm * 8; mscal(ctx, g_last_pc); return;   // MSCAL/MSCALF
    case 0x17: mscal(ctx, 0xFFFFFFFFu); return;                            // MSCNT: continue where it stopped
    case 0x50: case 0x51:                                                  // DIRECT / DIRECTHL
        V.direct_qw_left = imm ? imm : 65536;
        V.direct_word = 0;
        return;
    default: break;
    }
    V.need = cmd_words(code);
    if (V.need == 0 && cmd != 0x20 && cmd != 0x30 && cmd != 0x31 && cmd != 0x4A && cmd < 0x60) {
        static unsigned warned;
        if (warned++ < 16) log(Log::Gpu, "VIF1: unknown command %02x (code %08x)", cmd, code);
        return;
    }
    V.buf.clear();
    V.buf.reserve(V.need);
}

void word(Context* ctx, uint32_t w) {
    if (V.direct_qw_left) {
        V.direct_q[V.direct_word++] = w;
        if (V.direct_word == 4) {
            V.direct_word = 0;
            --V.direct_qw_left;
            ++stats().vif1_direct_qw;
            gif_write(Path::P2, reinterpret_cast<const uint8_t*>(V.direct_q), 1);
        }
        return;
    }
    if (V.need) {
        V.buf.push_back(w);
        if (--V.need == 0) finish(ctx);
        return;
    }
    command(ctx, w);
}

} // namespace

Vu1& vu1() { return g_vu1; }
bool vif1_mskpath3() { return V.mskpath3; }
uint32_t vif1_top() { return V.top; }
uint32_t vif1_itop() { return V.itop; }

void vif1_write(Context* ctx, const uint8_t* data, uint32_t qwc) {
    ProfScope prof_scope(prof().vif);
    stats().vif1_qw += qwc;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(data);
    for (uint32_t i = 0; i < qwc * 4; ++i) word(ctx, w[i]);
}

void vif1_fifo_words(Context* ctx, const uint32_t* words, int count) {
    for (int i = 0; i < count; ++i) word(ctx, words[i]);
}

} // namespace ps2::gpu
