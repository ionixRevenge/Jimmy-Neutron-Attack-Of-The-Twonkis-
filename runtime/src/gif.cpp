// GIF: decodes GIF packets (tag + data) from PATH1/2/3 into GS register writes
// and image transfers. PACKED data is converted to the GS register format here,
// so the GS only ever sees plain 64-bit register writes.
#include <cstring>

#include "gpu.hpp"
#include "ps2/runtime.hpp"

namespace ps2::gpu {

namespace {

enum Fmt { kPacked = 0, kReglist = 1, kImage = 2 };

struct PathState {
    uint32_t nloop = 0, nreg = 0, reg = 0, fmt = 0;
    uint64_t regs = 0;   // 4-bit register descriptors
    bool eop = false;
    bool reglist_half = false;
};
PathState g_path[4];
float g_q = 1.0f;        // Q from the last PACKED ST, used by PACKED RGBAQ

uint64_t bits(const uint64_t* q, int lo, int n) {   // n <= 32, within one 64-bit half
    return (q[lo / 64] >> (lo % 64)) & ((1ull << n) - 1);
}

void packed(uint32_t desc, const uint64_t* q) {
    switch (desc) {
    case 0x0: gs_write_reg(0x00, q[0] & 0x7FF); break;   // PRIM
    case 0x1: {                                          // RGBAQ
        uint32_t qbits;
        std::memcpy(&qbits, &g_q, 4);
        gs_write_reg(0x01, bits(q, 0, 8) | bits(q, 32, 8) << 8 | bits(q, 64, 8) << 16 | bits(q, 96, 8) << 24 | (uint64_t)qbits << 32);
        break;
    }
    case 0x2: {                                          // ST (+Q)
        const uint32_t qb = (uint32_t)bits(q, 64, 32);
        std::memcpy(&g_q, &qb, 4);
        gs_write_reg(0x02, q[0]);
        break;
    }
    case 0x3: gs_write_reg(0x03, bits(q, 0, 14) | bits(q, 32, 14) << 16); break;   // UV
    case 0x4: {                                          // XYZF2 (ADC -> XYZF3)
        const uint64_t v = bits(q, 0, 16) | bits(q, 32, 16) << 16 | bits(q, 68, 24) << 32 | bits(q, 100, 8) << 56;
        gs_write_reg(bits(q, 111, 1) ? 0x0C : 0x04, v);
        break;
    }
    case 0x5: {                                          // XYZ2 (ADC -> XYZ3)
        const uint64_t v = bits(q, 0, 16) | bits(q, 32, 16) << 16 | bits(q, 64, 32) << 32;
        gs_write_reg(bits(q, 111, 1) ? 0x0D : 0x05, v);
        break;
    }
    case 0xA: gs_write_reg(0x0A, bits(q, 100, 8) << 56); break;   // FOG
    case 0xE: gs_write_reg((uint32_t)(q[1] & 0xFF), q[0]); break; // A+D
    case 0xF: break;                                              // NOP
    default: gs_write_reg(desc, q[0]); break;   // TEX0/CLAMP (6-9), XYZF3/XYZ3 (12/13) raw
    }
}

void start_tag(Path p, PathState& s, const uint64_t* q) {
    ++stats().gif_tags;
    s.nloop = (uint32_t)(q[0] & 0x7FFF);
    s.eop = (q[0] >> 15) & 1;
    s.fmt = (uint32_t)((q[0] >> 58) & 3);
    if (s.fmt == 3) s.fmt = kImage;
    s.nreg = (uint32_t)((q[0] >> 60) & 0xF);
    if (s.nreg == 0) s.nreg = 16;
    s.regs = q[1];
    s.reg = 0;
    s.reglist_half = false;
    if (((q[0] >> 46) & 1) && s.fmt == kPacked) gs_write_reg(0x00, (q[0] >> 47) & 0x7FF);   // PRE: PRIM
    (void)p;
}

} // namespace

void gif_write(Path p, const uint8_t* data, uint32_t qwc) {
    ProfScope prof_scope(prof().gif);
    PathState& s = g_path[(int)p];
    stats().gif_qw[(int)p] += qwc;
    for (uint32_t i = 0; i < qwc; ++i) {
        const uint64_t* q = reinterpret_cast<const uint64_t*>(data + i * 16);
        if (s.nloop == 0) {
            start_tag(p, s, q);
            continue;
        }
        switch (s.fmt) {
        case kPacked:
            packed((uint32_t)((s.regs >> (s.reg * 4)) & 0xF), q);
            if (++s.reg == s.nreg) { s.reg = 0; --s.nloop; }
            break;
        case kReglist:
            for (int h = 0; h < 2 && s.nloop; ++h) {
                const uint32_t desc = (uint32_t)((s.regs >> (s.reg * 4)) & 0xF);
                if (desc != 0xE && desc != 0xF) gs_write_reg(desc, q[h]);
                if (++s.reg == s.nreg) { s.reg = 0; --s.nloop; }
            }
            break;
        default:
            gs_image_data(reinterpret_cast<const uint8_t*>(q), 1);
            --s.nloop;
            break;
        }
    }
}

} // namespace ps2::gpu
