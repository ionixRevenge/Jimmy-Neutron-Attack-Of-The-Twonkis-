// IPU: MPEG-2 macroblock decoder (EE User's Manual, IPU chapter; ISO/IEC 13818-2
// for the bitstream syntax and VLC tables).
//
// Commands run on a dedicated fiber. A command that needs more input than the
// FIFO holds, or that has more output than the out FIFO can take, switches back
// to the guest and continues when toIPU/fromIPU DMA or a FIFO access makes
// progress possible. Everything happens on the EE thread, so it is
// deterministic.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gpu.hpp"
#include "ipu.hpp"
#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2::ipu {

std::atomic<uint64_t> g_last_picture_ms{0};   // GetTickCount64 at the last IDEC/BDEC (movie_playing)

namespace {

// ---- IPU_CTRL fields ---------------------------------------------------------------
constexpr uint32_t kEcd = 1u << 14, kScd = 1u << 15;
constexpr uint32_t kAs = 1u << 20, kIvf = 1u << 21, kQst = 1u << 22, kMp1 = 1u << 23;

Context* g_ctx = nullptr;
uint32_t g_ctrl = 0;          // ECD/SCD + the 0x3FFF0000 settings
uint32_t g_result = 0;        // VDEC/FDEC result
uint32_t g_last_cmd = 0xFF;
uint16_t g_th0 = 0, g_th1 = 0;
uint8_t g_iq[2][64];          // [0] intra, [1] non-intra, raster order
uint16_t g_vqclut[16];
int16_t g_dc_pred[3];

struct Vlc;   // forward

// ---- fiber -------------------------------------------------------------------------
struct Abort {};
struct VlcError {};
LPVOID g_fiber = nullptr, g_caller = nullptr;
bool g_busy = false, g_abort = false;
uint32_t g_cmd = 0;

void yield_wait() {
    SwitchToFiber(g_caller);
    if (g_abort) throw Abort{};
}

void resume() {
    if (!g_busy) return;
    if (!IsThreadAFiber()) ConvertThreadToFiber(nullptr);
    g_caller = GetCurrentFiber();
    gpu::ProfScope prof_scope(gpu::prof().ipu);
    SwitchToFiber(g_fiber);
}

// ---- toIPU DMA (channel 4): pulled one quadword at a time as the FIFO drains ---------
constexpr uint32_t kD4 = 0x1000B400, kD3 = 0x1000B000;
bool g_d4_active = false, g_d4_last = false;
bool g_d3_active = false;

uint8_t* guest_mem(uint32_t addr) {
    if (addr & 0x80000000u) return g_ctx->spr + (addr & (kSprSize - 1) & ~15u);
    return g_ctx->ram + (addr & (kRamSize - 1) & ~15u);
}

bool d4_pop(uint8_t* q) {
    // hw registers live in a node-based map, so references stay valid
    static uint32_t& chcr = hw_reg(kD4);
    static uint32_t& madr = hw_reg(kD4 + 0x10);
    static uint32_t& qwc = hw_reg(kD4 + 0x20);
    static uint32_t& tadr = hw_reg(kD4 + 0x30);
    if (!(chcr & 0x100u)) g_d4_active = false;   // the game stopped the channel
    while (g_d4_active) {
        const bool chain = ((chcr >> 2) & 3) == 1;
        if (qwc) {
            std::memcpy(q, guest_mem(madr), 16);
            madr += 16;
            if (--qwc == 0 && (!chain || g_d4_last)) { g_d4_active = false; dma_complete(g_ctx, 4); }
            return true;
        }
        if (!chain || g_d4_last) { g_d4_active = false; dma_complete(g_ctx, 4); return false; }
        uint64_t tag;
        std::memcpy(&tag, guest_mem(tadr), 8);
        const uint32_t n = (uint32_t)(tag & 0xFFFF), id = (uint32_t)((tag >> 28) & 7);
        const uint32_t addr = (uint32_t)((tag >> 32) & 0xFFFFFFF0u);
        chcr = (chcr & 0xFFFF) | (uint32_t)(tag & 0xFFFF0000u);
        qwc = n;
        switch (id) {
        case 0: madr = addr; tadr += 16; g_d4_last = true; break;           // refe
        case 1: madr = tadr + 16; tadr += 16 + n * 16; break;               // cnt
        case 2: madr = tadr + 16; tadr = addr; break;                       // next
        case 3: case 4: madr = addr; tadr += 16; break;                     // ref / refs
        case 5: {                                                           // call
            const uint32_t asp = (chcr >> 4) & 3;
            if (asp >= 2) fatal(g_ctx, tadr, "toIPU DMA call stack overflow");
            hw_reg(kD4 + 0x40 + asp * 0x10) = tadr + 16 + n * 16;
            chcr = (chcr & ~0x30u) | ((asp + 1) << 4);
            madr = tadr + 16; tadr = addr;
            break;
        }
        case 6: {                                                           // ret
            const uint32_t asp = (chcr >> 4) & 3;
            madr = tadr + 16;
            if (asp) { tadr = hw_reg(kD4 + 0x40 + (asp - 1) * 0x10); chcr = (chcr & ~0x30u) | ((asp - 1) << 4); }
            else { tadr += 16 + n * 16; g_d4_last = true; }
            break;
        }
        default: madr = tadr + 16; tadr += 16 + n * 16; g_d4_last = true; break;   // end
        }
        if (((tag >> 31) & 1) && (chcr & 0x80)) g_d4_last = true;              // TIE
    }
    return false;
}

// ---- input FIFO (8 quadwords, read MSB-first in byte order) -------------------------
constexpr uint32_t kFifoBytes = 8 * 16;
uint8_t g_in[kFifoBytes + 16];
uint32_t g_in_size = 0;   // bytes
uint32_t g_bp = 0;        // bit position from g_in[0]

void in_push(const uint8_t* p, uint32_t n) {
    n = std::min(n, kFifoBytes - g_in_size);
    std::memcpy(g_in + g_in_size, p, n);
    g_in_size += n;
}

void in_normalize() {
    while (g_bp >= 128 && g_in_size >= 16) {
        std::memmove(g_in, g_in + 16, g_in_size - 16);
        g_in_size -= 16;
        g_bp -= 128;
    }
}

void fill() {
    in_normalize();
    uint8_t q[16];
    while (g_in_size + 16 <= kFifoBytes && d4_pop(q)) in_push(q, 16);
}

uint32_t avail() { return g_in_size * 8 > g_bp ? g_in_size * 8 - g_bp : 0; }

uint32_t peek_raw(uint32_t n) {   // n <= 32; bits past the end read as 0
    if (!n) return 0;
    const uint32_t byte = g_bp >> 3, s = g_bp & 7;
    uint64_t w = 0;
    for (uint32_t i = 0; i < 5; ++i) w = (w << 8) | (byte + i < g_in_size ? g_in[byte + i] : 0);
    return (uint32_t)((w >> (40 - s - n)) & ((1ull << n) - 1));
}

void need(uint32_t n) {
    while (avail() < n) {
        fill();
        if (avail() >= n) break;
        yield_wait();
    }
}

uint32_t peek(uint32_t n) { need(n); return peek_raw(n); }
void advance(uint32_t n) {
    while (n) {   // FB can exceed what one FIFO fill holds only in theory; step anyway
        const uint32_t k = std::min(n, 32u);
        need(k);
        g_bp += k;
        n -= k;
        in_normalize();
    }
}
uint32_t get_bits8();
// n whole bytes; fast when the read position is byte aligned (it is for CSC input)
void get_bytes(uint8_t* dst, uint32_t n) {
    while (n) {
        if (g_bp & 7) { *dst++ = (uint8_t)get_bits8(); --n; continue; }
        need(8);
        in_normalize();
        const uint32_t at = g_bp >> 3, k = std::min(n, g_in_size - at);
        std::memcpy(dst, g_in + at, k);
        dst += k;
        n -= k;
        g_bp += k * 8;
        in_normalize();
    }
}

uint32_t get(uint32_t n) { const uint32_t v = peek(n); g_bp += n; in_normalize(); return v; }
uint32_t get_bits8() { return get(8); }
int32_t get_signed(uint32_t n) { const uint32_t v = get(n); return (int32_t)(v << (32 - n)) >> (32 - n); }

// ---- output FIFO / fromIPU DMA (channel 3) -------------------------------------------
std::vector<uint8_t> g_out;
size_t g_out_rd = 0;

size_t out_pending() { return g_out.size() - g_out_rd; }

void drain() {
    static uint32_t& chcr = hw_reg(kD3);
    static uint32_t& madr = hw_reg(kD3 + 0x10);
    static uint32_t& qwc = hw_reg(kD3 + 0x20);
    if (!(chcr & 0x100u)) g_d3_active = false;
    if (g_d3_active) {
        while (qwc && out_pending() >= 16) {
            std::memcpy(guest_mem(madr), g_out.data() + g_out_rd, 16);
            g_out_rd += 16;
            madr += 16;
            --qwc;
        }
        if (!qwc) { g_d3_active = false; dma_complete(g_ctx, 3); }
    }
    if (g_out_rd == g_out.size()) { g_out.clear(); g_out_rd = 0; }
}

void out_push(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    g_out.insert(g_out.end(), b, b + n);
    drain();
    while (out_pending() > kFifoBytes) { yield_wait(); drain(); }
}

// ---- VLC tables ------------------------------------------------------------------------
// Lookup indexed by the next `bits` bits; each slot holds the code length and value.
struct Vlc {
    uint32_t bits = 0;
    std::vector<uint8_t> len;
    std::vector<int32_t> val;
};

struct Code { const char* bits; int32_t value; };

void vlc_add(Vlc& t, const char* code, int32_t value, const char* name) {
    uint32_t c = 0, n = 0;
    for (const char* p = code; *p; ++p) {
        if (*p == ' ') continue;
        c = (c << 1) | (uint32_t)(*p == '1');
        ++n;
    }
    if (n > t.bits) { std::fprintf(stderr, "[ipu] %s: code %s longer than the table\n", name, code); std::abort(); }
    const uint32_t shift = t.bits - n;
    for (uint32_t i = 0; i < (1u << shift); ++i) {
        const uint32_t slot = (c << shift) | i;
        if (t.len[slot]) { std::fprintf(stderr, "[ipu] %s: code %s overlaps another code\n", name, code); std::abort(); }
        t.len[slot] = (uint8_t)n;
        t.val[slot] = value;
    }
}

Vlc vlc_build(uint32_t bits, std::initializer_list<Code> codes, const char* name) {
    Vlc t;
    t.bits = bits;
    t.len.assign(size_t{1} << bits, 0);
    t.val.assign(size_t{1} << bits, 0);
    for (const Code& c : codes) vlc_add(t, c.bits, c.value, name);
    return t;
}

// Small tables: ISO 13818-2 tables B.1-B.4, B.9-B.13. Values follow the IPU's
// VDEC result format (macroblock_type flags: quant 0x10, forward 0x08,
// backward 0x04, pattern 0x02, intra 0x01; MBA 34 = stuffing, 35 = escape).
const Vlc& mba_table() {
    static const Vlc t = vlc_build(11, {
        {"1", 1}, {"011", 2}, {"010", 3}, {"0011", 4}, {"0010", 5}, {"00011", 6}, {"00010", 7},
        {"0000111", 8}, {"0000110", 9}, {"00001011", 10}, {"00001010", 11}, {"00001001", 12},
        {"00001000", 13}, {"00000111", 14}, {"00000110", 15}, {"0000010111", 16}, {"0000010110", 17},
        {"0000010101", 18}, {"0000010100", 19}, {"0000010011", 20}, {"0000010010", 21},
        {"00000100011", 22}, {"00000100010", 23}, {"00000100001", 24}, {"00000100000", 25},
        {"00000011111", 26}, {"00000011110", 27}, {"00000011101", 28}, {"00000011100", 29},
        {"00000011011", 30}, {"00000011010", 31}, {"00000011001", 32}, {"00000011000", 33},
        {"00000001111", 34}, {"00000001000", 35},
    }, "macroblock_address_increment");
    return t;
}

const Vlc& mbtype_table(uint32_t picture_type) {
    static const Vlc i = vlc_build(2, {{"1", 0x01}, {"01", 0x11}}, "macroblock_type I");
    static const Vlc p = vlc_build(6, {
        {"1", 0x0A}, {"01", 0x02}, {"001", 0x08}, {"00011", 0x01}, {"00010", 0x1A}, {"00001", 0x12}, {"000001", 0x11},
    }, "macroblock_type P");
    static const Vlc b = vlc_build(6, {
        {"10", 0x0C}, {"11", 0x0E}, {"010", 0x04}, {"011", 0x06}, {"0010", 0x08}, {"0011", 0x0A},
        {"00011", 0x01}, {"00010", 0x1E}, {"000011", 0x1A}, {"000010", 0x16}, {"000001", 0x11},
    }, "macroblock_type B");
    return picture_type == 2 ? p : picture_type == 3 ? b : i;
}

const Vlc& motion_table() {
    static const Vlc t = vlc_build(11, {
        {"1", 0}, {"010", 1}, {"011", -1}, {"0010", 2}, {"0011", -2}, {"00010", 3}, {"00011", -3},
        {"0000110", 4}, {"0000111", -4}, {"00001010", 5}, {"00001011", -5}, {"00001000", 6},
        {"00001001", -6}, {"00000110", 7}, {"00000111", -7}, {"0000010110", 8}, {"0000010111", -8},
        {"0000010100", 9}, {"0000010101", -9}, {"0000010010", 10}, {"0000010011", -10},
        {"00000100010", 11}, {"00000100011", -11}, {"00000100000", 12}, {"00000100001", -12},
        {"00000011110", 13}, {"00000011111", -13}, {"00000011100", 14}, {"00000011101", -14},
        {"00000011010", 15}, {"00000011011", -15}, {"00000011000", 16}, {"00000011001", -16},
    }, "motion_code");
    return t;
}

const Vlc& dmv_table() {
    static const Vlc t = vlc_build(2, {{"0", 0}, {"10", 1}, {"11", -1}}, "dmvector");
    return t;
}

const Vlc& cbp_table() {
    static const Vlc t = vlc_build(9, {
        {"111", 60}, {"1101", 4}, {"1100", 8}, {"1011", 16}, {"1010", 32}, {"10011", 12}, {"10010", 48},
        {"10001", 20}, {"10000", 40}, {"01111", 28}, {"01110", 44}, {"01101", 52}, {"01100", 56},
        {"01011", 1}, {"01010", 61}, {"01001", 2}, {"01000", 62}, {"001111", 24}, {"001110", 36},
        {"001101", 3}, {"001100", 63}, {"0010111", 5}, {"0010110", 9}, {"0010101", 17}, {"0010100", 33},
        {"0010011", 6}, {"0010010", 10}, {"0010001", 18}, {"0010000", 34}, {"00011111", 7},
        {"00011110", 11}, {"00011101", 19}, {"00011100", 35}, {"00011011", 13}, {"00011010", 49},
        {"00011001", 21}, {"00011000", 41}, {"00010111", 14}, {"00010110", 50}, {"00010101", 22},
        {"00010100", 42}, {"00010011", 15}, {"00010010", 51}, {"00010001", 23}, {"00010000", 43},
        {"00001111", 25}, {"00001110", 37}, {"00001101", 26}, {"00001100", 38}, {"00001011", 29},
        {"00001010", 45}, {"00001001", 53}, {"00001000", 57}, {"00000111", 30}, {"00000110", 46},
        {"00000101", 54}, {"00000100", 58}, {"000000111", 31}, {"000000110", 47}, {"000000101", 55},
        {"000000100", 59}, {"000000011", 27}, {"000000010", 39}, {"000000001", 0},
    }, "coded_block_pattern");
    return t;
}

const Vlc& dc_size_table(bool chroma) {
    static const Vlc lum = vlc_build(9, {
        {"100", 0}, {"00", 1}, {"01", 2}, {"101", 3}, {"110", 4}, {"1110", 5}, {"11110", 6},
        {"111110", 7}, {"1111110", 8}, {"11111110", 9}, {"111111110", 10}, {"111111111", 11},
    }, "dct_dc_size_luminance");
    static const Vlc chr = vlc_build(10, {
        {"00", 0}, {"01", 1}, {"10", 2}, {"110", 3}, {"1110", 4}, {"11110", 5}, {"111110", 6},
        {"1111110", 7}, {"11111110", 8}, {"111111110", 9}, {"1111111110", 10}, {"1111111111", 11},
    }, "dct_dc_size_chrominance");
    return chroma ? chr : lum;
}

// DCT coefficient tables B.14 (zero) and B.15 (one), codes without the sign bit.
// Values: run << 8 | level; kEob / kEscape mark the special codes.
constexpr int32_t kEob = -1, kEscape = -2;
#define RL(r, l) ((r) << 8 | (l))

// Codes shared by both tables (12 bits and longer).
#define DCT_LONG_CODES     {"0000 0001 1100", RL(3, 3)}, {"0000 0001 0010", RL(4, 3)}, {"0000 0001 1110", RL(6, 2)},     {"0000 0001 0101", RL(7, 2)}, {"0000 0001 0001", RL(8, 2)}, {"0000 0001 1111", RL(17, 1)},     {"0000 0001 1010", RL(18, 1)}, {"0000 0001 1001", RL(19, 1)}, {"0000 0001 0111", RL(20, 1)},     {"0000 0001 0110", RL(21, 1)},     {"0000 0000 1011 0", RL(1, 6)}, {"0000 0000 1010 1", RL(1, 7)}, {"0000 0000 1010 0", RL(2, 5)},     {"0000 0000 1001 1", RL(3, 4)}, {"0000 0000 1001 0", RL(5, 3)}, {"0000 0000 1000 1", RL(9, 2)},     {"0000 0000 1000 0", RL(10, 2)}, {"0000 0000 1111 1", RL(22, 1)}, {"0000 0000 1111 0", RL(23, 1)},     {"0000 0000 1110 1", RL(24, 1)}, {"0000 0000 1110 0", RL(25, 1)}, {"0000 0000 1101 1", RL(26, 1)},     {"0000 0000 0111 11", RL(0, 16)}, {"0000 0000 0111 10", RL(0, 17)}, {"0000 0000 0111 01", RL(0, 18)},     {"0000 0000 0111 00", RL(0, 19)}, {"0000 0000 0110 11", RL(0, 20)}, {"0000 0000 0110 10", RL(0, 21)},     {"0000 0000 0110 01", RL(0, 22)}, {"0000 0000 0110 00", RL(0, 23)}, {"0000 0000 0101 11", RL(0, 24)},     {"0000 0000 0101 10", RL(0, 25)}, {"0000 0000 0101 01", RL(0, 26)}, {"0000 0000 0101 00", RL(0, 27)},     {"0000 0000 0100 11", RL(0, 28)}, {"0000 0000 0100 10", RL(0, 29)}, {"0000 0000 0100 01", RL(0, 30)},     {"0000 0000 0100 00", RL(0, 31)},     {"0000 0000 0011 000", RL(0, 32)}, {"0000 0000 0010 111", RL(0, 33)}, {"0000 0000 0010 110", RL(0, 34)},     {"0000 0000 0010 101", RL(0, 35)}, {"0000 0000 0010 100", RL(0, 36)}, {"0000 0000 0010 011", RL(0, 37)},     {"0000 0000 0010 010", RL(0, 38)}, {"0000 0000 0010 001", RL(0, 39)}, {"0000 0000 0010 000", RL(0, 40)},     {"0000 0000 0011 111", RL(1, 8)}, {"0000 0000 0011 110", RL(1, 9)}, {"0000 0000 0011 101", RL(1, 10)},     {"0000 0000 0011 100", RL(1, 11)}, {"0000 0000 0011 011", RL(1, 12)}, {"0000 0000 0011 010", RL(1, 13)},     {"0000 0000 0011 001", RL(1, 14)},     {"0000 0000 0001 0011", RL(1, 15)}, {"0000 0000 0001 0010", RL(1, 16)}, {"0000 0000 0001 0001", RL(1, 17)},     {"0000 0000 0001 0000", RL(1, 18)}, {"0000 0000 0001 0100", RL(6, 3)}, {"0000 0000 0001 1010", RL(11, 2)},     {"0000 0000 0001 1001", RL(12, 2)}, {"0000 0000 0001 1000", RL(13, 2)}, {"0000 0000 0001 0111", RL(14, 2)},     {"0000 0000 0001 0110", RL(15, 2)}, {"0000 0000 0001 0101", RL(16, 2)}, {"0000 0000 0001 1111", RL(27, 1)},     {"0000 0000 0001 1110", RL(28, 1)}, {"0000 0000 0001 1101", RL(29, 1)}, {"0000 0000 0001 1100", RL(30, 1)},     {"0000 0000 0001 1011", RL(31, 1)}

const Vlc& dct_table(bool one) {
    static const Vlc zero = vlc_build(16, {
        {"10", kEob}, {"000001", kEscape},
        {"11", RL(0, 1)}, {"011", RL(1, 1)}, {"0100", RL(0, 2)}, {"0101", RL(2, 1)}, {"00101", RL(0, 3)},
        {"00111", RL(3, 1)}, {"00110", RL(4, 1)}, {"000110", RL(1, 2)}, {"000111", RL(5, 1)},
        {"000101", RL(6, 1)}, {"000100", RL(7, 1)}, {"0000110", RL(0, 4)}, {"0000100", RL(2, 2)},
        {"0000111", RL(8, 1)}, {"0000101", RL(9, 1)}, {"00100110", RL(0, 5)}, {"00100001", RL(0, 6)},
        {"00100101", RL(1, 3)}, {"00100100", RL(3, 2)}, {"00100111", RL(10, 1)}, {"00100011", RL(11, 1)},
        {"00100010", RL(12, 1)}, {"00100000", RL(13, 1)}, {"0000001010", RL(0, 7)}, {"0000001100", RL(1, 4)},
        {"0000001011", RL(2, 3)}, {"0000001111", RL(4, 2)}, {"0000001001", RL(5, 2)}, {"0000001110", RL(14, 1)},
        {"0000001101", RL(15, 1)}, {"0000001000", RL(16, 1)},
        {"0000 0001 1101", RL(0, 8)}, {"0000 0001 1000", RL(0, 9)}, {"0000 0001 0011", RL(0, 10)},
        {"0000 0001 0000", RL(0, 11)}, {"0000 0001 1011", RL(1, 5)}, {"0000 0001 0100", RL(2, 4)},
        {"0000 0000 1101 0", RL(0, 12)}, {"0000 0000 1100 1", RL(0, 13)}, {"0000 0000 1100 0", RL(0, 14)},
        {"0000 0000 1011 1", RL(0, 15)},
        DCT_LONG_CODES,
    }, "dct_coefficients table zero");
    static const Vlc one_t = vlc_build(16, {
        {"0110", kEob}, {"000001", kEscape},
        {"10", RL(0, 1)}, {"010", RL(1, 1)}, {"110", RL(0, 2)}, {"00101", RL(2, 1)}, {"0111", RL(0, 3)},
        {"00111", RL(3, 1)}, {"000110", RL(4, 1)}, {"00110", RL(1, 2)}, {"000111", RL(5, 1)},
        {"0000110", RL(6, 1)}, {"0000100", RL(7, 1)}, {"11100", RL(0, 4)}, {"0000111", RL(2, 2)},
        {"0000101", RL(8, 1)}, {"1111000", RL(9, 1)}, {"11101", RL(0, 5)}, {"000101", RL(0, 6)},
        {"1111001", RL(1, 3)}, {"00100110", RL(3, 2)}, {"1111010", RL(10, 1)}, {"00100001", RL(11, 1)},
        {"00100101", RL(12, 1)}, {"00100100", RL(13, 1)}, {"000100", RL(0, 7)}, {"00100111", RL(1, 4)},
        {"11111100", RL(2, 3)}, {"11111101", RL(4, 2)}, {"000000100", RL(5, 2)}, {"000000101", RL(14, 1)},
        {"000000111", RL(15, 1)}, {"0000001101", RL(16, 1)}, {"1111011", RL(0, 8)}, {"1111100", RL(0, 9)},
        {"00100011", RL(0, 10)}, {"00100010", RL(0, 11)}, {"00100000", RL(1, 5)}, {"0000001100", RL(2, 4)},
        {"11111010", RL(0, 12)}, {"11111011", RL(0, 13)}, {"11111110", RL(0, 14)}, {"11111111", RL(0, 15)},
        DCT_LONG_CODES,
    }, "dct_coefficients table one");
    static const bool checked = [] {
        // every slot is covered except the forbidden 0000 0000 0000 prefix (and, in
        // table one, the codes table zero uses for (0,8)-(0,15), (1,5), (2,4))
        auto filled = [](const Vlc& t) { return (int)std::count_if(t.len.begin(), t.len.end(), [](uint8_t n) { return n != 0; }); };
        if (filled(zero) != 65536 - 16 || filled(one_t) != 65536 - 16 - 96 - 32) {
            std::fprintf(stderr, "[ipu] DCT table coverage wrong: %d / %d\n", filled(zero), filled(one_t));
            std::abort();
        }
        return true;
    }();
    (void)checked;
    return one ? one_t : zero;
}
#undef RL

// Decodes one symbol; returns its value and stores the code length in *len.
int32_t vlc_get(const Vlc& t, uint32_t* len = nullptr) {
    for (;;) {
        fill();
        const uint32_t have = std::min(avail(), t.bits);
        const uint32_t slot = peek_raw(t.bits);
        const uint32_t n = t.len[slot];
        if (n && n <= have) {
            g_bp += n;
            in_normalize();
            if (len) *len = n;
            return t.val[slot];
        }
        if (have >= t.bits) throw VlcError{};
        yield_wait();
    }
}

// ---- block decoding -----------------------------------------------------------------
const uint8_t kZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
const uint8_t kAlternate[64] = {
    0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45, 53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
const uint8_t kNonLinearQ[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22,
                                  24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

// Separable 8x8 inverse DCT in single precision (well within IEEE 1180 limits
// for 12-bit input). Rows holding only a DC term take a shortcut.
void idct(int16_t* blk) {
    static float c[8][8];
    static bool init = false;
    if (!init) {
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                c[u][x] = (float)((u ? 1.0 : std::sqrt(0.5)) * 0.5 * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
        init = true;
    }
    float tmp[64];
    for (int y = 0; y < 8; ++y) {          // rows
        const int16_t* in = blk + y * 8;
        float* out = tmp + y * 8;
        if (!(in[1] | in[2] | in[3] | in[4] | in[5] | in[6] | in[7])) {
            const float dc = in[0] * c[0][0];
            for (int x = 0; x < 8; ++x) out[x] = dc;
            continue;
        }
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int u = 0; u < 8; ++u) s += c[u][x] * in[u];
            out[x] = s;
        }
    }
    for (int x = 0; x < 8; ++x)            // columns
        for (int y = 0; y < 8; ++y) {
            float s = 0;
            for (int v = 0; v < 8; ++v) s += c[v][y] * tmp[v * 8 + x];
            const int r = (int)(s + (s >= 0 ? 0.5f : -0.5f));
            blk[y * 8 + x] = (int16_t)std::clamp(r, -256, 255);
        }
}

// Reads one block's coefficients into raster order, dequantised and inverse transformed.
void read_block(int16_t* blk, int channel, bool intra, int qsc) {
    int16_t coef[64] = {};   // scan order
    const bool mpeg1 = g_ctrl & kMp1;
    int i = 0;
    if (intra) {
        const int32_t size = vlc_get(dc_size_table(channel != 0));
        int32_t diff = 0;
        if (size) {
            const int32_t v = (int32_t)get((uint32_t)size);
            diff = v < (1 << (size - 1)) ? v + 1 - (1 << size) : v;
        }
        g_dc_pred[channel] = (int16_t)(g_dc_pred[channel] + diff);
        coef[0] = g_dc_pred[channel];
        i = 1;
    }
    const Vlc& tab = dct_table(intra && (g_ctrl & kIvf));
    for (;;) {
        int32_t run, level;
        if (i == 0 && peek(1) == 1) {   // first coefficient of a non-intra block: '1s' = run 0, level 1
            g_bp += 1; in_normalize();
            run = 0; level = 1;
            if (get(1)) level = -1;
        } else {
            const int32_t v = vlc_get(tab);
            if (v == kEob) break;
            if (v == kEscape) {
                run = (int32_t)get(6);
                if (mpeg1) {
                    level = get_signed(8);
                    if (level == 0) level = (int32_t)get(8);
                    else if (level == -128) level = (int32_t)get(8) - 256;
                } else {
                    level = get_signed(12);
                }
            } else {
                run = v >> 8;
                level = v & 0xFF;
                if (get(1)) level = -level;
            }
        }
        i += run;
        if (i >= 64) throw VlcError{};
        coef[i++] = (int16_t)level;
    }

    // dequantise (7.4) into raster order
    const uint8_t* scan = (g_ctrl & kAs) ? kAlternate : kZigzag;
    const int qs = (g_ctrl & kQst) ? kNonLinearQ[qsc & 31] : 2 * (qsc & 31);
    const uint8_t* w = g_iq[intra ? 0 : 1];
    int32_t sum = 0;
    std::memset(blk, 0, 64 * sizeof(int16_t));
    for (int k = 0; k < 64; ++k) {
        const int32_t q = coef[k];
        if (!q && !(intra && k == 0)) continue;
        const int pos = scan[k];
        int32_t f;
        if (intra && k == 0) {
            f = q * (8 >> ((g_ctrl >> 16) & 3));
        } else if (intra) {
            f = (q * w[pos] * qs * 2) / 32;
        } else {
            f = ((2 * q + (q > 0 ? 1 : -1)) * w[pos] * qs) / 32;
        }
        if (mpeg1 && !(intra && k == 0) && f && !(f & 1)) f -= f > 0 ? 1 : -1;   // MPEG-1 oddification
        f = std::clamp(f, -2048, 2047);
        blk[pos] = (int16_t)f;
        sum += f;
    }
    if (!mpeg1 && !(sum & 1)) blk[63] ^= 1;   // mismatch control
    idct(blk);
}

// BDEC core: one macroblock into 384 RAW16 values (Y 16x16, Cb 8x8, Cr 8x8).
void decode_macroblock(bool intra, bool dcr, bool dt, int qsc, int16_t* out) {
    const int cbp = intra ? 0x3F : vlc_get(cbp_table());
    if (dcr) {
        const int16_t reset = (int16_t)(128 << ((g_ctrl >> 16) & 3));
        g_dc_pred[0] = g_dc_pred[1] = g_dc_pred[2] = reset;
    }
    int16_t blk[6][64];
    for (int b = 0; b < 6; ++b) {
        if (cbp & (1 << (5 - b))) read_block(blk[b], b < 4 ? 0 : b - 3, intra, qsc);
        else std::memset(blk[b], 0, sizeof blk[b]);
    }
    for (int y = 0; y < 16; ++y) {
        // frame DCT: rows 0-7 from blocks 0/1, 8-15 from 2/3; field DCT interleaves them
        const int top = dt ? (y & 1) * 2 : (y >> 3) * 2;
        const int row = dt ? y >> 1 : y & 7;
        std::memcpy(out + y * 16, blk[top] + row * 8, 16);
        std::memcpy(out + y * 16 + 8, blk[top + 1] + row * 8, 16);
    }
    std::memcpy(out + 256, blk[4], 128);
    std::memcpy(out + 320, blk[5], 128);
}

// CSC: one macroblock of RAW8 (Y 256, Cb 64, Cr 64) to RGB32 or RGB16.
void color_convert(const uint8_t* mb, bool rgb16) {
    uint32_t px[256];
    const uint32_t th0 = g_th0, th1 = g_th1;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const int c = (y >> 1) * 8 + (x >> 1);
            // 16.16 fixed point; truncation matches the float formula it replaces
            const int Y = mb[y * 16 + x] << 16, cb = mb[256 + c] - 128, cr = mb[320 + c] - 128;
            const uint32_t r = (uint32_t)(std::clamp(Y + 91881 * cr, 0, 255 << 16) >> 16);
            const uint32_t g = (uint32_t)(std::clamp(Y - 22554 * cb - 46802 * cr, 0, 255 << 16) >> 16);
            const uint32_t b = (uint32_t)(std::clamp(Y + 116130 * cb, 0, 255 << 16) >> 16);
            uint32_t a = 0x80;
            if (r < th0 && g < th0 && b < th0) a = 0;
            else if (r < th1 && g < th1 && b < th1) a = 0x40;
            px[y * 16 + x] = a << 24 | b << 16 | g << 8 | r;
        }
    if (rgb16) {
        uint16_t h[256];
        for (int i = 0; i < 256; ++i) {
            const uint32_t p = px[i];
            h[i] = (uint16_t)(((p >> 3) & 0x1F) | ((p >> 11) & 0x1F) << 5 | ((p >> 19) & 0x1F) << 10 | (p >> 31) << 15);
        }
        out_push(h, sizeof h);
    } else {
        out_push(px, sizeof px);
    }
}

void raw16_to_raw8(const int16_t* in, uint8_t* out) {
    for (int i = 0; i < 384; ++i) out[i] = (uint8_t)std::clamp<int>(in[i], 0, 255);
}

// ---- commands -------------------------------------------------------------------------
void cmd_idec(uint32_t c) {
    g_last_picture_ms.store(GetTickCount64(), std::memory_order_relaxed);
    advance(c & 0x3F);
    const bool rgb16 = (c >> 27) & 1, dtd = (c >> 24) & 1;
    int qsc = (int)((c >> 16) & 0x1F);
    for (int mb = 0;; ++mb) {
        const int32_t type = vlc_get(mbtype_table(1));
        const bool dt = dtd ? get(1) != 0 : false;
        if (type & 0x10) qsc = (int)get(5);
        int16_t raw[384];
        uint8_t raw8[384];
        decode_macroblock(true, mb == 0, dt, qsc, raw);
        raw16_to_raw8(raw, raw8);
        color_convert(raw8, rgb16);
        // a start code ends the slice: eight zero bits, then 00 00 01 at a byte boundary
        if (peek(8) == 0) {
            g_bp = (g_bp + 7) & ~7u;
            in_normalize();
            for (;;) {
                fill();
                if (avail() < 24) break;   // the packet ends here: treat it as the start code
                const uint32_t sc = peek_raw(24);
                if (sc == 1) break;
                if (sc != 0) throw VlcError{};
                g_bp += 8;
                in_normalize();
            }
            g_ctrl |= kScd;
            return;
        }
        int32_t inc;
        while ((inc = vlc_get(mba_table())) == 34) {}   // stuffing
        if (inc != 1) {
            static int warned;
            if (warned++ < 4) log(Log::Gpu, "IDEC: macroblock_address_increment %d (skipped macroblocks)", inc);
        }
    }
}

void cmd_bdec(uint32_t c) {
    g_last_picture_ms.store(GetTickCount64(), std::memory_order_relaxed);
    advance(c & 0x3F);
    int16_t raw[384];
    decode_macroblock((c >> 27) & 1, (c >> 26) & 1, (c >> 25) & 1, (int)((c >> 16) & 0x1F), raw);
    static int dumped;
    if (false && dumped < 12) {
        ++dumped;
        log(Log::Gpu, "BDEC %08x: Y %d %d %d %d  Cb %d %d  Cr %d %d  dc %d %d %d", c, raw[0], raw[8], raw[128], raw[255],
            raw[256], raw[319], raw[320], raw[383], g_dc_pred[0], g_dc_pred[1], g_dc_pred[2]);
    }
    out_push(raw, sizeof raw);
    // start code detection needs the next 8 bits: wait for them like the hardware
    // does (the stream always continues with a start code at the latest)
    if (peek(8) == 0) g_ctrl |= kScd;
}

void cmd_vdec(uint32_t c) {
    advance(c & 0x3F);
    const Vlc* t;
    switch ((c >> 26) & 3) {
    case 0: t = &mba_table(); break;
    case 1: t = &mbtype_table((g_ctrl >> 24) & 7); break;
    case 2: t = &motion_table(); break;
    default: t = &dmv_table(); break;
    }
    uint32_t len = 0;
    const int32_t v = vlc_get(*t, &len);
    g_result = len << 16 | ((uint32_t)v & 0xFFFF);
}

uint64_t g_csc_mbs = 0;   // macroblocks colour-converted (statistics)

void cmd_csc(uint32_t c) {
    const uint32_t count = c & 0x7FF;
    g_csc_mbs += count;
    if (log_enabled(Log::Gpu)) {
        static uint64_t last_mbs = 0;
        static ULONGLONG last_t = GetTickCount64();
        const ULONGLONG t = GetTickCount64();
        if (t - last_t >= 5000) {
            log(Log::Gpu, "IPU: %.1f pictures/s (640x448)", (double)(g_csc_mbs - last_mbs) / 1120.0 / ((double)(t - last_t) / 1000.0));
            last_mbs = g_csc_mbs;
            last_t = t;
        }
    }
    const bool rgb16 = (c >> 27) & 1;
    for (uint32_t m = 0; m < count; ++m) {
        uint8_t mb[384];
        get_bytes(mb, sizeof mb);
        color_convert(mb, rgb16);
    }
}

void execute(uint32_t c) {
    switch (c >> 28) {
    case 0: g_in_size = 0; g_bp = c & 0x7F; break;   // BCLR
    case 1: cmd_idec(c); break;
    case 2: cmd_bdec(c); break;
    case 3: cmd_vdec(c); break;
    case 4: advance(c & 0x3F); g_result = peek(32); break;   // FDEC
    case 5: {                                                // SETIQ (bitstream order = zigzag)
        advance(c & 0x3F);
        uint8_t* m = g_iq[(c >> 27) & 1];
        for (int i = 0; i < 64; ++i) m[kZigzag[i]] = (uint8_t)get(8);
        break;
    }
    case 6:                                                  // SETVQ
        for (uint16_t& e : g_vqclut) { const uint32_t lo = get(8); e = (uint16_t)(lo | get(8) << 8); }
        break;
    case 7: cmd_csc(c); break;
    case 9: g_th0 = (uint16_t)(c & 0x1FF); g_th1 = (uint16_t)((c >> 16) & 0x1FF); break;   // SETTH
    default: {
        static int warned;
        if (warned++ < 8) log(Log::Gpu, "IPU command %u (%08x) is not implemented", c >> 28, c);
        break;
    }
    }
}

VOID CALLBACK fiber_main(LPVOID) {
    for (;;) {
        bool aborted = false;
        try {
            execute(g_cmd);
        } catch (const VlcError&) {
            g_ctrl |= kEcd;
            static int warned;
            if (warned++ < 8) {
                in_normalize();
                const uint32_t at = g_bp >> 3;
                log(Log::Gpu, "IPU: VLC error in command %08x (bp %u, fifo %u bytes, dma madr %08x qwc %u) next %02x %02x %02x %02x %02x %02x %02x %02x",
                    g_cmd, g_bp, g_in_size, hw_reg(kD4 + 0x10), hw_reg(kD4 + 0x20), g_in[at], g_in[at + 1], g_in[at + 2], g_in[at + 3],
                    g_in[at + 4], g_in[at + 5], g_in[at + 6], g_in[at + 7]);
            }
        } catch (const Abort&) {
            aborted = true;
        }
        g_busy = false;
        g_abort = false;
        if (!aborted) kernel_raise_intc(8);   // INTC_IPU
        SwitchToFiber(g_caller);
    }
}

uint32_t ifc_fp_bp(uint32_t* fp, uint32_t* bp);

void start_command(uint32_t c) {
    if (g_busy) {   // a new command replaces one still waiting for data
        g_abort = true;
        resume();
    }
    if (!g_fiber) {
        g_fiber = CreateFiber(1 << 20, fiber_main, nullptr);
        if (!g_fiber) fatal(g_ctx, 0x10002000, "CreateFiber failed for the IPU");
    }
    g_ctrl &= ~(kEcd | kScd);
    static int logged;
    if (log_enabled(Log::Gpu) && logged < 40) {
        ++logged;
        uint32_t fp, bp;
        const uint32_t ifc = ifc_fp_bp(&fp, &bp);
        log(Log::Gpu, "IPU cmd %08x ctrl %08x ifc %u fp %u bp %u d4 qwc %u d3 qwc %u", c, g_ctrl, ifc, fp, bp,
            hw_reg(kD4 + 0x20), hw_reg(kD3 + 0x20));
    }
    g_cmd = c;
    g_last_cmd = c >> 28;
    g_busy = true;
    resume();
}

void reset() {
    if (g_busy) { g_abort = true; resume(); }
    g_th0 = g_th1 = 0;
    g_in_size = 0;
    g_bp = 0;
    g_out.clear();
    g_out_rd = 0;
    g_last_cmd = 0xFF;
}

uint32_t ifc_fp_bp(uint32_t* fp, uint32_t* bp) {
    in_normalize();
    uint32_t ifc = g_in_size / 16, f = 0, b = g_bp;
    if (b && ifc) { f = 1; --ifc; }
    *fp = f; *bp = b;
    return ifc;
}

uint32_t peek_top() {
    const uint32_t n = std::min(avail(), 32u);
    return n ? peek_raw(n) << (32 - n) : 0;
}

} // namespace

bool owns(uint32_t a) { return (a >= 0x10002000 && a < 0x10002040) || (a >= 0x10007000 && a < 0x10007020); }

uint64_t mmio_read(Context* ctx, uint32_t a) {
    g_ctx = ctx;
    if (g_busy) resume(); else fill();
    const uint64_t busy = g_busy ? 1ull : 0ull;
    uint32_t fp, bp;
    switch (a) {
    case 0x10002000: {   // IPU_CMD
        const uint32_t data = (g_last_cmd == 3 || g_last_cmd == 4) ? g_result : peek_top();
        return busy << 63 | data;
    }
    case 0x10002004: return busy << 31;
    case 0x10002010: {   // IPU_CTRL
        const uint32_t ifc = ifc_fp_bp(&fp, &bp);
        const uint32_t ofc = (uint32_t)std::min<size_t>(out_pending() / 16, 8);
        return (uint32_t)(g_ctrl | busy << 31 | ofc << 4 | ifc);
    }
    case 0x10002020: {   // IPU_BP
        const uint32_t ifc = ifc_fp_bp(&fp, &bp);
        return fp << 16 | ifc << 8 | bp;
    }
    case 0x10002030: {   // IPU_TOP
        const bool b = g_busy || avail() < 32;
        return (uint64_t)b << 63 | (g_busy ? 0u : peek_top());
    }
    case 0x10002034: return (g_busy || avail() < 32) ? 0x80000000u : 0u;
    case 0x10007000: case 0x10007008: {   // OUT FIFO
        uint64_t v = 0;
        if (out_pending() >= 8) { std::memcpy(&v, g_out.data() + g_out_rd, 8); g_out_rd += 8; }
        drain();
        if (g_busy) resume();
        return v;
    }
    default: return 0;
    }
}

void mmio_write(Context* ctx, uint32_t a, uint64_t v) {
    g_ctx = ctx;
    switch (a) {
    case 0x10002000: start_command((uint32_t)v); break;
    case 0x10002010:
        if (v & (1u << 30)) reset();
        g_ctrl = (g_ctrl & ~0x3FFF0000u) | ((uint32_t)v & 0x3FFF0000u);
        break;
    case 0x10007010: case 0x10007018: {   // IN FIFO
        in_normalize();
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
        in_push(b, 8);
        if (g_busy) resume();
        break;
    }
    default: break;
    }
}

void dma_start(Context* ctx, int ch) {
    g_ctx = ctx;
    const uint32_t mode = (hw_reg(ch == 3 ? kD3 : kD4) >> 2) & 3;
    if (ch == 3) {
        if (mode != 0) fatal(ctx, kD3, "fromIPU DMA in chain mode is not implemented");
        g_d3_active = true;
        drain();
    } else {
        const uint32_t qwc = hw_reg(kD4 + 0x20);
        g_d4_active = true;
        g_d4_last = false;
        // End of stream: when a video's elementary stream is a whole number of quadwords,
        // the game's player skips the final sequence_end_code with an FDEC that wants 32
        // more bits, then kicks empty transfers until that command completes (libmpeg's
        // wait loop has no other exit). A starved IPU given an empty transfer reads zeros.
        if (mode == 0 && qwc == 0 && g_busy && g_in_size * 8 <= g_bp) {
            static const uint8_t zero[16] = {};
            in_normalize();
            in_push(zero, 16);
        }
        fill();
    }
    if (g_busy) resume();
}

} // namespace ps2::ipu

namespace ps2 {
bool movie_playing() { return GetTickCount64() - ipu::g_last_picture_ms.load(std::memory_order_relaxed) < 400; }
} // namespace ps2
