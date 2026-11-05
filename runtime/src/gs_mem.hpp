// GS local memory (4 MB) and its swizzled pixel layouts.
//
// Pixels are grouped into pages (8 KB), blocks (256 bytes) and columns; the
// arrangement depends on the pixel storage mode (PSM). The bit-interleave
// formulas below reproduce the block/column tables of the GS User's Manual.
#pragma once
#include <cstdint>

namespace ps2::gpu {

enum Psm : uint32_t {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A,
};

constexpr uint32_t kGsMemWords = 1u << 20;   // 4 MB
extern uint32_t g_gsmem[kGsMemWords];

// word index inside a 32-bit block (8x8 pixels); y is 0..7
inline uint32_t col32(uint32_t x, uint32_t y) { return (y >> 1) * 16 + (x >> 1) * 4 + (y & 1) * 2 + (x & 1); }

inline uint32_t block32(uint32_t bx, uint32_t by) {   // bx 0..7, by 0..3
    return (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2) | ((bx & 4) << 2);
}
inline uint32_t block16(uint32_t bx, uint32_t by) {   // bx 0..3, by 0..7
    return (by & 1) | ((bx & 1) << 1) | ((by & 2) << 1) | ((bx & 2) << 2) | ((by & 4) << 2);
}
inline uint32_t block16s(uint32_t bx, uint32_t by) {
    return (by & 1) | ((bx & 1) << 1) | (by & 4) | ((by & 2) << 2) | ((bx & 2) << 3);
}

// bp: base pointer in 256-byte blocks; bw: buffer width in 64-pixel units.
inline uint32_t addr32_formula(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, bool z = false) {
    const uint32_t page = (y >> 5) * (bw ? bw : 1) + (x >> 6);
    uint32_t blk = block32((x >> 3) & 7, (y >> 3) & 3);
    if (z) blk ^= 24;
    return ((bp * 64) + page * 2048 + blk * 64 + col32(x & 7, y & 7)) & (kGsMemWords - 1);
}

// halfword index
inline uint32_t addr16_formula(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, bool s, bool z = false) {
    const uint32_t page = (y >> 6) * (bw ? bw : 1) + (x >> 6);
    uint32_t blk = s ? block16s((x >> 4) & 3, (y >> 3) & 7) : block16((x >> 4) & 3, (y >> 3) & 7);
    if (z) blk ^= 24;
    return ((bp * 128) + page * 4096 + blk * 128 + 2 * col32(x & 7, y & 7) + ((x >> 3) & 1)) & (kGsMemWords * 2 - 1);
}

// byte index
inline uint32_t addr8_formula(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    const uint32_t pages_per_row = bw >= 2 ? bw / 2 : 1;
    const uint32_t page = (y >> 6) * pages_per_row + (x >> 7);
    const uint32_t blk = block32((x >> 4) & 7, (y >> 4) & 3);
    const uint32_t ly = y & 3, column = (y >> 2) & 3;
    const uint32_t swap = (((ly >> 1) ^ (column & 1)) & 1) << 2;
    const uint32_t word = column * 16 + col32((x ^ swap) & 7, ly & 1);
    const uint32_t byte = (ly >> 1) + 2 * ((x >> 3) & 1);
    return ((bp * 256) + page * 8192 + blk * 256 + word * 4 + byte) & (kGsMemWords * 4 - 1);
}

// nibble index
inline uint32_t addr4_formula(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    const uint32_t pages_per_row = bw >= 2 ? bw / 2 : 1;
    const uint32_t page = (y >> 7) * pages_per_row + (x >> 7);
    const uint32_t blk = block16((x >> 5) & 3, (y >> 4) & 7);
    const uint32_t ly = y & 3, column = (y >> 2) & 3;
    const uint32_t swap = (((ly >> 1) ^ (column & 1)) & 1) << 2;
    const uint32_t word = column * 16 + col32((x ^ swap) & 7, ly & 1);
    const uint32_t nib = (ly >> 1) + 2 * ((x >> 3) & 3);
    return ((bp * 512) + page * 16384 + blk * 512 + word * 8 + nib) & (kGsMemWords * 8 - 1);
}

// The formulas above are evaluated once into per-page tables (offset of each
// pixel inside its page); addressing is then a table lookup.
struct SwizzleTables {
    uint16_t t32[2][32][64];    // [z] word in a 64x32 page
    uint16_t t16[4][64][64];    // [s + 2z] halfword in a 64x64 page
    uint16_t t8[64][128];       // byte in a 128x64 page
    uint16_t t4[128][128];      // nibble in a 128x128 page
};
extern const SwizzleTables g_swz;

inline uint32_t addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, bool z = false) {
    const uint32_t page = (y >> 5) * (bw ? bw : 1) + (x >> 6);
    return (bp * 64 + page * 2048 + g_swz.t32[z][y & 31][x & 63]) & (kGsMemWords - 1);
}
inline uint32_t addr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, bool s, bool z = false) {
    const uint32_t page = (y >> 6) * (bw ? bw : 1) + (x >> 6);
    return (bp * 128 + page * 4096 + g_swz.t16[(s ? 1 : 0) + (z ? 2 : 0)][y & 63][x & 63]) & (kGsMemWords * 2 - 1);
}
inline uint32_t addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    const uint32_t page = (y >> 6) * (bw >= 2 ? bw / 2 : 1) + (x >> 7);
    return (bp * 256 + page * 8192 + g_swz.t8[y & 63][x & 127]) & (kGsMemWords * 4 - 1);
}
inline uint32_t addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    const uint32_t page = (y >> 7) * (bw >= 2 ? bw / 2 : 1) + (x >> 7);
    return (bp * 512 + page * 16384 + g_swz.t4[y & 127][x & 127]) & (kGsMemWords * 8 - 1);
}

inline uint16_t* mem16() { return reinterpret_cast<uint16_t*>(g_gsmem); }
inline uint8_t* mem8() { return reinterpret_cast<uint8_t*>(g_gsmem); }

// Raw pixel value in the given format (no colour conversion).
inline uint32_t read_pixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    switch (psm) {
    case PSMCT32: case PSMZ32: return g_gsmem[addr32(bp, bw, x, y, psm == PSMZ32)];
    case PSMCT24: case PSMZ24: return g_gsmem[addr32(bp, bw, x, y, psm == PSMZ24)] & 0xFFFFFF;
    case PSMCT16: case PSMZ16: return mem16()[addr16(bp, bw, x, y, false, psm == PSMZ16)];
    case PSMCT16S: case PSMZ16S: return mem16()[addr16(bp, bw, x, y, true, psm == PSMZ16S)];
    case PSMT8: return mem8()[addr8(bp, bw, x, y)];
    case PSMT4: { const uint32_t n = addr4(bp, bw, x, y); return (mem8()[n >> 1] >> ((n & 1) * 4)) & 0xF; }
    case PSMT8H: return g_gsmem[addr32(bp, bw, x, y)] >> 24;
    case PSMT4HL: return (g_gsmem[addr32(bp, bw, x, y)] >> 24) & 0xF;
    case PSMT4HH: return g_gsmem[addr32(bp, bw, x, y)] >> 28;
    default: return 0;
    }
}

inline void write_pixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) {
    switch (psm) {
    case PSMCT32: case PSMZ32: g_gsmem[addr32(bp, bw, x, y, psm == PSMZ32)] = v; break;
    case PSMCT24: case PSMZ24: { uint32_t& w = g_gsmem[addr32(bp, bw, x, y, psm == PSMZ24)]; w = (w & 0xFF000000u) | (v & 0xFFFFFF); break; }
    case PSMCT16: case PSMZ16: mem16()[addr16(bp, bw, x, y, false, psm == PSMZ16)] = (uint16_t)v; break;
    case PSMCT16S: case PSMZ16S: mem16()[addr16(bp, bw, x, y, true, psm == PSMZ16S)] = (uint16_t)v; break;
    case PSMT8: mem8()[addr8(bp, bw, x, y)] = (uint8_t)v; break;
    case PSMT4: {
        const uint32_t n = addr4(bp, bw, x, y);
        uint8_t& b = mem8()[n >> 1];
        b = (n & 1) ? (uint8_t)((b & 0x0F) | ((v & 0xF) << 4)) : (uint8_t)((b & 0xF0) | (v & 0xF));
        break;
    }
    case PSMT8H: { uint32_t& w = g_gsmem[addr32(bp, bw, x, y)]; w = (w & 0x00FFFFFFu) | (v << 24); break; }
    case PSMT4HL: { uint32_t& w = g_gsmem[addr32(bp, bw, x, y)]; w = (w & 0xF0FFFFFFu) | ((v & 0xF) << 24); break; }
    case PSMT4HH: { uint32_t& w = g_gsmem[addr32(bp, bw, x, y)]; w = (w & 0x0FFFFFFFu) | ((v & 0xF) << 28); break; }
    default: break;
    }
}

inline uint32_t psm_bits(uint32_t psm) {
    switch (psm) {
    case PSMCT32: case PSMZ32: return 32;
    case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: case PSMT8H: return 8;
    default: return 4;
    }
}

} // namespace ps2::gpu
