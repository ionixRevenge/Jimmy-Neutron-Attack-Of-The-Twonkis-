// GS: register state, local memory transfers and a software rasterizer.
//
// Coverage follows the GS rules (12.4 fixed-point vertices, pixel centres at
// integer coordinates, top-left fill). The pixel pipeline implements texture
// lookup (all storage modes, CLUT, TEXA), TFX/TCC, fog, alpha test, depth test,
// alpha blending, FBA/PABE, COLCLAMP and frame/z masks.
// Not yet: mipmapping, dithering, destination alpha test, anti-aliasing.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gpu.hpp"
#include "gs_mem.hpp"
#include "ps2/runtime.hpp"

namespace ps2::gpu {

uint32_t g_gsmem[kGsMemWords];

static SwizzleTables build_swizzle() {
    SwizzleTables t{};
    for (uint32_t z = 0; z < 2; ++z)
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 64; ++x) t.t32[z][y][x] = (uint16_t)addr32_formula(0, 1, x, y, z != 0);
    for (uint32_t m = 0; m < 4; ++m)
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x) t.t16[m][y][x] = (uint16_t)addr16_formula(0, 1, x, y, (m & 1) != 0, (m & 2) != 0);
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 128; ++x) t.t8[y][x] = (uint16_t)addr8_formula(0, 2, x, y);
    for (uint32_t y = 0; y < 128; ++y)
        for (uint32_t x = 0; x < 128; ++x) t.t4[y][x] = (uint16_t)addr4_formula(0, 2, x, y);
    return t;
}
const SwizzleTables g_swz = build_swizzle();

void vu1_print_stats();

namespace {

void flush();   // draw queued primitives (below)

uint64_t g_reg[0x100];
uint64_t g_priv[0x20];   // 0x12000000 + 16*i

inline uint32_t bf(uint64_t v, int lo, int n) { return (uint32_t)((v >> lo) & ((1ull << n) - 1)); }

// ---- registers ------------------------------------------------------------------
enum Reg : uint32_t {
    PRIM = 0x00, RGBAQ = 0x01, ST = 0x02, UV = 0x03, XYZF2 = 0x04, XYZ2 = 0x05, TEX0_1 = 0x06, TEX0_2 = 0x07,
    CLAMP_1 = 0x08, CLAMP_2 = 0x09, FOG = 0x0A, XYZF3 = 0x0C, XYZ3 = 0x0D, TEX1_1 = 0x14, TEX1_2 = 0x15,
    TEX2_1 = 0x16, TEX2_2 = 0x17, XYOFFSET_1 = 0x18, XYOFFSET_2 = 0x19, PRMODECONT = 0x1A, PRMODE = 0x1B,
    TEXCLUT = 0x1C, TEXA = 0x3B, FOGCOL = 0x3D, TEXFLUSH = 0x3F, SCISSOR_1 = 0x40, SCISSOR_2 = 0x41,
    ALPHA_1 = 0x42, ALPHA_2 = 0x43, DTHE = 0x45, COLCLAMP = 0x46, TEST_1 = 0x47, TEST_2 = 0x48, PABE = 0x49,
    FBA_1 = 0x4A, FBA_2 = 0x4B, FRAME_1 = 0x4C, FRAME_2 = 0x4D, ZBUF_1 = 0x4E, ZBUF_2 = 0x4F,
    BITBLTBUF = 0x50, TRXPOS = 0x51, TRXREG = 0x52, TRXDIR = 0x53, HWREG = 0x54,
    SIGNAL = 0x60, FINISH = 0x61, LABEL = 0x62,
};

// ---- CLUT -------------------------------------------------------------------------
uint32_t g_clut[256];   // entries as stored (CT32, or CT16 in the low half)
uint32_t g_clut_psm = PSMCT32;
uint32_t g_cbp0 = ~0u, g_cbp1 = ~0u;

void load_clut(uint64_t tex0) {
    const uint32_t psm = bf(tex0, 20, 6), cbp = bf(tex0, 37, 14), cpsm = bf(tex0, 51, 4);
    const uint32_t csm = bf(tex0, 55, 1), csa = bf(tex0, 56, 5), cld = bf(tex0, 61, 3);
    bool load = false;
    switch (cld) {
    case 1: load = true; break;
    case 2: load = true; g_cbp0 = cbp; break;
    case 3: load = true; g_cbp1 = cbp; break;
    case 4: load = cbp != g_cbp0; g_cbp0 = cbp; break;
    case 5: load = cbp != g_cbp1; g_cbp1 = cbp; break;
    default: break;
    }
    if (!load) return;
    flush();   // queued draws still use the old CLUT
    const bool eight = psm == PSMT8 || psm == PSMT8H;
    const uint32_t n = eight ? 256 : 16;
    const uint32_t cpsm_mem = cpsm == 0 ? PSMCT32 : cpsm == 2 ? PSMCT16 : PSMCT16S;
    g_clut_psm = cpsm_mem;
    if (csm) {   // CSM2: a straight line of entries at TEXCLUT
        const uint64_t tc = g_reg[TEXCLUT];
        const uint32_t cbw = bf(tc, 0, 6), cou = bf(tc, 6, 6) * 16, cov = bf(tc, 12, 10);
        for (uint32_t i = 0; i < n; ++i) g_clut[i] = read_pixel(cpsm_mem, cbp, cbw, cou + i, cov);
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t x, y;
        if (eight) {   // 16x16 with entries 8-15 and 16-23 of every 32 swapped
            const uint32_t j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
            x = j & 15; y = j >> 4;
        } else {       // 8x2
            x = i & 7; y = i >> 3;
        }
        const uint32_t slot = eight ? i : (cpsm_mem == PSMCT32 ? (csa & 15) * 16 + i : csa * 16 + i) & 255;
        if (eight) g_clut[i] = read_pixel(cpsm_mem, cbp, 1, x, y);
        else g_clut[slot] = read_pixel(cpsm_mem, cbp, 1, x, y);
    }
}

// ---- transfers ------------------------------------------------------------------------
struct Transfer {
    bool active = false;
    uint32_t psm = 0, bp = 0, bw = 0, x0 = 0, y0 = 0, w = 0, h = 0, x = 0, y = 0;
    uint64_t acc = 0;
    uint32_t acc_bits = 0;
} T;

void start_transfer() {
    flush();
    const uint64_t bb = g_reg[BITBLTBUF], pos = g_reg[TRXPOS], rg = g_reg[TRXREG];
    const uint32_t dir = bf(g_reg[TRXDIR], 0, 2);
    if (dir == 0) {   // host -> local
        T = Transfer{};
        T.active = true;
        T.bp = bf(bb, 32, 14); T.bw = bf(bb, 48, 6); T.psm = bf(bb, 56, 6);
        T.x0 = bf(pos, 32, 11); T.y0 = bf(pos, 48, 11);
        T.w = bf(rg, 0, 12); T.h = bf(rg, 32, 12);
        T.x = 0; T.y = 0;
    } else if (dir == 2) {   // local -> local
        const uint32_t sbp = bf(bb, 0, 14), sbw = bf(bb, 16, 6), spsm = bf(bb, 24, 6);
        const uint32_t dbp = bf(bb, 32, 14), dbw = bf(bb, 48, 6), dpsm = bf(bb, 56, 6);
        const uint32_t sx = bf(pos, 0, 11), sy = bf(pos, 16, 11), dx = bf(pos, 32, 11), dy = bf(pos, 48, 11);
        const uint32_t w = bf(rg, 0, 12), h = bf(rg, 32, 12);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                write_pixel(dpsm, dbp, dbw, (dx + x) & 2047, (dy + y) & 2047, read_pixel(spsm, sbp, sbw, (sx + x) & 2047, (sy + y) & 2047));
    } else if (dir == 1) {
        static unsigned warned;
        if (warned++ < 4) log(Log::Gpu, "GS local->host transfer not implemented");
    }
}

void transfer_pixel(uint32_t v) {
    if (T.y >= T.h) return;
    write_pixel(T.psm, T.bp, T.bw, (T.x0 + T.x) & 2047, (T.y0 + T.y) & 2047, v);
    if (++T.x == T.w) { T.x = 0; ++T.y; }
}

// ---- vertices and primitives ---------------------------------------------------------------
struct Vertex {
    float x, y;        // pixels (offset removed)
    uint32_t z;
    float r, g, b, a;
    float s, t, q;     // STQ
    float u, v;        // texels (FST)
    float f;           // fog
};
Vertex g_q[3];
uint32_t g_nq = 0;
uint32_t g_strip_count = 0;   // vertices since PRIM (for fans)
Vertex g_fan0;

struct Ctx {   // drawing state of the active context, decoded at draw time
    uint64_t tex0, tex1, clamp, alpha, test, frame, zbuf, scissor, fba;
    int ofx, ofy;
};

uint64_t prim_attr() { return bf(g_reg[PRMODECONT], 0, 1) ? g_reg[PRIM] : g_reg[PRMODE]; }

Ctx context() {
    const int c = bf(prim_attr(), 9, 1);
    Ctx k;
    k.tex0 = g_reg[TEX0_1 + c]; k.tex1 = g_reg[TEX1_1 + c]; k.clamp = g_reg[CLAMP_1 + c];
    k.alpha = g_reg[ALPHA_1 + c]; k.test = g_reg[TEST_1 + c]; k.frame = g_reg[FRAME_1 + c];
    k.zbuf = g_reg[ZBUF_1 + c]; k.scissor = g_reg[SCISSOR_1 + c]; k.fba = g_reg[FBA_1 + c];
    k.ofx = (int)bf(g_reg[XYOFFSET_1 + c], 0, 16); k.ofy = (int)bf(g_reg[XYOFFSET_1 + c], 32, 16);
    return k;
}

// ---- pixel pipeline ---------------------------------------------------------------------------
struct Rgba { int r, g, b, a; };

Rgba from16(uint32_t v, uint64_t texa, bool tex) {
    Rgba c{(int)((v & 0x1F) << 3), (int)(((v >> 5) & 0x1F) << 3), (int)(((v >> 10) & 0x1F) << 3), 0};
    if (tex) {
        const bool aem = bf(texa, 15, 1);
        c.a = (v & 0x8000) ? (int)bf(texa, 32, 8) : ((aem && !(v & 0x7FFF)) ? 0 : (int)bf(texa, 0, 8));
    } else {
        c.a = (v & 0x8000) ? 0x80 : 0;
    }
    return c;
}
Rgba from32(uint32_t v) { return Rgba{(int)(v & 0xFF), (int)((v >> 8) & 0xFF), (int)((v >> 16) & 0xFF), (int)(v >> 24)}; }

struct Texture {
    uint32_t psm, bp, bw, tw, th, tcc, tfx;
    uint32_t wms, wmt, minu, maxu, minv, maxv;
    bool bilinear;
    uint64_t texa;
};

int wrap(int c, uint32_t mode, uint32_t size, uint32_t mn, uint32_t mx) {
    switch (mode) {
    case 0: return c & (int)(size - 1);
    case 1: return std::clamp(c, 0, (int)size - 1);
    case 2: return std::clamp(c, (int)mn, (int)mx);
    default: return (int)((c & (int)mn) | mx);   // region repeat: UMSK/UFIX
    }
}

Rgba texel(const Texture& t, int u, int v) {
    u = wrap(u, t.wms, t.tw, t.minu, t.maxu);
    v = wrap(v, t.wmt, t.th, t.minv, t.maxv);
    const uint32_t raw = read_pixel(t.psm, t.bp, t.bw, (uint32_t)u, (uint32_t)v);
    switch (t.psm) {
    case PSMCT32: return from32(raw);
    case PSMCT24: {
        Rgba c = from32(raw);
        const bool aem = bf(t.texa, 15, 1);
        c.a = (aem && !(raw & 0xFFFFFF)) ? 0 : (int)bf(t.texa, 0, 8);
        return c;
    }
    case PSMCT16: case PSMCT16S: return from16(raw, t.texa, true);
    default: {   // indexed
        const uint32_t e = g_clut[raw & 255];
        return g_clut_psm == PSMCT32 ? from32(e) : from16(e & 0xFFFF, t.texa, true);
    }
    }
}

Rgba sample(const Texture& t, float u, float v) {
    if (!t.bilinear) return texel(t, (int)std::floor(u), (int)std::floor(v));
    u -= 0.5f; v -= 0.5f;
    const int iu = (int)std::floor(u), iv = (int)std::floor(v);
    const float fu = u - iu, fv = v - iv;
    const Rgba a = texel(t, iu, iv), b = texel(t, iu + 1, iv), c = texel(t, iu, iv + 1), d = texel(t, iu + 1, iv + 1);
    auto mix = [&](int p, int q, int r, int s) {
        return (int)((p * (1 - fu) + q * fu) * (1 - fv) + (r * (1 - fu) + s * fu) * fv + 0.5f);
    };
    return Rgba{mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g), mix(a.b, b.b, c.b, d.b), mix(a.a, b.a, c.a, d.a)};
}

struct DrawState {
    Ctx k;
    uint64_t attr;
    bool tme, fge, abe, fst, iip;
    Texture tex;
    uint32_t fpsm, fbp, fbw, fbmsk;
    uint32_t zpsm, zbp; bool zmsk, zte; uint32_t ztst;
    bool ate; uint32_t atst, aref, afail;
    uint32_t a_a, a_b, a_c, a_d, a_fix;
    bool pabe, colclamp, fba;
    int scax0, scax1, scay0, scay1;
    Rgba fogcol;
};
DrawState g_cur;   // state of the primitive being queued

void setup_draw() {
    DrawState& D = g_cur;
    D = DrawState{};   // zeroes padding too: states are compared with memcmp
    D.k = context();
    D.attr = prim_attr();
    D.iip = bf(D.attr, 3, 1); D.tme = bf(D.attr, 4, 1); D.fge = bf(D.attr, 5, 1); D.abe = bf(D.attr, 6, 1);
    D.fst = bf(D.attr, 8, 1);
    const uint64_t t0 = D.k.tex0, cl = D.k.clamp;
    D.tex.bp = bf(t0, 0, 14); D.tex.bw = bf(t0, 14, 6); D.tex.psm = bf(t0, 20, 6);
    D.tex.tw = 1u << std::min(bf(t0, 26, 4), 10u); D.tex.th = 1u << std::min(bf(t0, 30, 4), 10u);
    D.tex.tcc = bf(t0, 34, 1); D.tex.tfx = bf(t0, 35, 2);
    D.tex.wms = bf(cl, 0, 2); D.tex.wmt = bf(cl, 2, 2);
    D.tex.minu = bf(cl, 4, 10); D.tex.maxu = bf(cl, 14, 10); D.tex.minv = bf(cl, 24, 10); D.tex.maxv = bf(cl, 34, 10);
    D.tex.bilinear = bf(D.k.tex1, 5, 1);   // MMAG (LOD 0 assumed)
    D.tex.texa = g_reg[TEXA];
    D.fbp = bf(D.k.frame, 0, 9) * 32; D.fbw = bf(D.k.frame, 16, 6); D.fpsm = bf(D.k.frame, 24, 6); D.fbmsk = bf(D.k.frame, 32, 32);
    D.zbp = bf(D.k.zbuf, 0, 9) * 32; D.zpsm = bf(D.k.zbuf, 24, 4) | 0x30; D.zmsk = bf(D.k.zbuf, 32, 1);
    const uint64_t te = D.k.test;
    D.ate = bf(te, 0, 1); D.atst = bf(te, 1, 3); D.aref = bf(te, 4, 8); D.afail = bf(te, 12, 2);
    D.zte = bf(te, 16, 1); D.ztst = bf(te, 17, 2);
    const uint64_t al = D.k.alpha;
    D.a_a = bf(al, 0, 2); D.a_b = bf(al, 2, 2); D.a_c = bf(al, 4, 2); D.a_d = bf(al, 6, 2); D.a_fix = bf(al, 32, 8);
    D.pabe = bf(g_reg[PABE], 0, 1); D.colclamp = bf(g_reg[COLCLAMP], 0, 1); D.fba = bf(D.k.fba, 0, 1);
    const uint64_t sc = D.k.scissor;
    D.scax0 = (int)bf(sc, 0, 11); D.scax1 = (int)bf(sc, 16, 11); D.scay0 = (int)bf(sc, 32, 11); D.scay1 = (int)bf(sc, 48, 11);
    const uint64_t fc = g_reg[FOGCOL];
    D.fogcol = Rgba{(int)bf(fc, 0, 8), (int)bf(fc, 8, 8), (int)bf(fc, 16, 8), 0};
}

Rgba read_frame(const DrawState& D, int x, int y) {
    const uint32_t raw = read_pixel(D.fpsm, D.fbp, D.fbw, (uint32_t)x, (uint32_t)y);
    if (D.fpsm == PSMCT16 || D.fpsm == PSMCT16S) return from16(raw, 0, false);
    Rgba c = from32(raw);
    if (D.fpsm == PSMCT24) c.a = 0x80;
    return c;
}

bool alpha_pass(const DrawState& D, int a) {
    switch (D.atst) {
    case 0: return false;
    case 1: return true;
    case 2: return a < (int)D.aref;
    case 3: return a <= (int)D.aref;
    case 4: return a == (int)D.aref;
    case 5: return a >= (int)D.aref;
    case 6: return a > (int)D.aref;
    default: return a != (int)D.aref;
    }
}

// One fragment: colour (0-255, alpha 0-255 with 0x80 = 1.0), depth, texture coords.
inline void fragment(const DrawState& D, int x, int y, uint32_t z, Rgba c, float u, float v, float fog) {
    if (x < D.scax0 || x > D.scax1 || y < D.scay0 || y > D.scay1) return;
    if (D.tme) {
        const Rgba t = sample(D.tex, u, v);
        switch (D.tex.tfx) {
        case 0:   // modulate
            c.r = std::min(255, (c.r * t.r) >> 7); c.g = std::min(255, (c.g * t.g) >> 7); c.b = std::min(255, (c.b * t.b) >> 7);
            c.a = D.tex.tcc ? std::min(255, (c.a * t.a) >> 7) : c.a;
            break;
        case 1: c.r = t.r; c.g = t.g; c.b = t.b; c.a = D.tex.tcc ? t.a : c.a; break;   // decal
        case 2:   // highlight
            c.r = std::min(255, ((c.r * t.r) >> 7) + c.a); c.g = std::min(255, ((c.g * t.g) >> 7) + c.a);
            c.b = std::min(255, ((c.b * t.b) >> 7) + c.a); c.a = D.tex.tcc ? std::min(255, c.a + t.a) : c.a;
            break;
        default:  // highlight2
            c.r = std::min(255, ((c.r * t.r) >> 7) + c.a); c.g = std::min(255, ((c.g * t.g) >> 7) + c.a);
            c.b = std::min(255, ((c.b * t.b) >> 7) + c.a); c.a = D.tex.tcc ? t.a : c.a;
            break;
        }
    }
    if (D.fge) {
        const int f = std::clamp((int)fog, 0, 255);
        c.r = (c.r * f + D.fogcol.r * (255 - f)) >> 8; c.g = (c.g * f + D.fogcol.g * (255 - f)) >> 8; c.b = (c.b * f + D.fogcol.b * (255 - f)) >> 8;
    }
    bool write_fb = true, write_z = !D.zmsk, write_alpha = true;
    if (D.ate && !alpha_pass(D, c.a)) {
        switch (D.afail) {
        case 0: return;
        case 1: write_z = false; break;
        case 2: write_fb = false; break;
        default: write_z = false; write_alpha = false; break;
        }
    }
    if (D.zte) {
        const uint32_t zmax = D.zpsm == PSMZ32 ? 0xFFFFFFFFu : D.zpsm == PSMZ24 ? 0xFFFFFFu : 0xFFFFu;
        z = std::min(z, zmax);
        const uint32_t zd = read_pixel(D.zpsm, D.zbp, D.fbw, (uint32_t)x, (uint32_t)y);
        switch (D.ztst) {
        case 0: return;
        case 1: break;
        case 2: if (z < zd) return; break;
        default: if (z <= zd) return; break;
        }
        if (write_z) write_pixel(D.zpsm, D.zbp, D.fbw, (uint32_t)x, (uint32_t)y, z);
    }
    if (!write_fb) return;
    const Rgba d = read_frame(D, x, y);
    if (D.abe && !(D.pabe && c.a < 0x80)) {
        auto pick = [&](uint32_t sel, int s, int dd) { return sel == 0 ? s : sel == 1 ? dd : 0; };
        const int ca = D.a_c == 0 ? c.a : D.a_c == 1 ? d.a : (int)D.a_fix;
        c.r = (((pick(D.a_a, c.r, d.r) - pick(D.a_b, c.r, d.r)) * ca) >> 7) + pick(D.a_d, c.r, d.r);
        c.g = (((pick(D.a_a, c.g, d.g) - pick(D.a_b, c.g, d.g)) * ca) >> 7) + pick(D.a_d, c.g, d.g);
        c.b = (((pick(D.a_a, c.b, d.b) - pick(D.a_b, c.b, d.b)) * ca) >> 7) + pick(D.a_d, c.b, d.b);
    }
    if (D.colclamp) { c.r = std::clamp(c.r, 0, 255); c.g = std::clamp(c.g, 0, 255); c.b = std::clamp(c.b, 0, 255); }
    else { c.r &= 255; c.g &= 255; c.b &= 255; }
    int a = write_alpha ? (c.a & 255) : d.a;
    if (D.fba) a |= 0x80;
    uint32_t out;
    if (D.fpsm == PSMCT16 || D.fpsm == PSMCT16S)
        out = (uint32_t)((c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10) | ((a & 0x80) ? 0x8000 : 0));
    else
        out = (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | ((uint32_t)a << 24);
    if (D.fbmsk) {
        const uint32_t old = read_pixel(D.fpsm, D.fbp, D.fbw, (uint32_t)x, (uint32_t)y);
        uint32_t msk = D.fbmsk;
        if (D.fpsm == PSMCT16 || D.fpsm == PSMCT16S)
            msk = ((msk >> 3) & 0x1F) | (((msk >> 11) & 0x1F) << 5) | (((msk >> 19) & 0x1F) << 10) | ((msk >> 31) << 15);
        out = (out & ~msk) | (old & msk);
    }
    write_pixel(D.fpsm, D.fbp, D.fbw, (uint32_t)x, (uint32_t)y, out);
}

// Texture coordinates of a vertex in texels.
void tex_coords(const DrawState& D, const Vertex& v, float* u, float* t, float* q) {
    if (D.fst) { *u = v.u; *t = v.v; *q = 1.0f; }
    else { *u = v.s * D.tex.tw; *t = v.t * D.tex.th; *q = v.q; }
}

// Rows are split between threads in 8-row bands: thread `bi` of `bn` draws the rows
// whose band index modulo bn is bi, so each pixel is always written by one thread.
inline bool my_row(int y, int bn, int bi) { return bn == 1 || ((y >> 3) % bn) == bi; }

void log_sprite(const DrawState& D, const Vertex& a, const Vertex& b) {
    static unsigned logged;
    if (logged < 40 && log_enabled(Log::Gpu)) {
        ++logged;
        log(Log::Gpu, "sprite (%.1f,%.1f)-(%.1f,%.1f) rgba %d,%d,%d,%d fbp %u fbw %u psm %x tme %d tex bp %u psm %x %ux%u uv (%.1f,%.1f)-(%.1f,%.1f) abe %d scissor %d-%d,%d-%d",
            a.x, a.y, b.x, b.y, (int)b.r, (int)b.g, (int)b.b, (int)b.a, D.fbp, D.fbw, D.fpsm, (int)D.tme, D.tex.bp, D.tex.psm,
            D.tex.tw, D.tex.th, a.u, a.v, b.u, b.v, (int)D.abe, D.scax0, D.scax1, D.scay0, D.scay1);
    }
}

void draw_sprite(const DrawState& D, const Vertex& a, const Vertex& b, int bn, int bi) {
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    const int ix0 = std::max((int)std::ceil(x0), D.scax0), ix1 = std::min((int)std::ceil(x1) - 1, D.scax1);
    const int iy0 = std::max((int)std::ceil(y0), D.scay0), iy1 = std::min((int)std::ceil(y1) - 1, D.scay1);
    if (ix0 > ix1 || iy0 > iy1) return;
    float ua, va, qa, ub, vb, qb;
    tex_coords(D, a, &ua, &va, &qa);
    tex_coords(D, b, &ub, &vb, &qb);
    if (!D.fst) { ua /= qb; va /= qb; ub /= qb; vb /= qb; }   // STQ: Q of the second vertex
    const float dx = b.x - a.x, dy = b.y - a.y;
    const Rgba c{(int)b.r, (int)b.g, (int)b.b, (int)b.a};
    for (int y = iy0; y <= iy1; ++y) {
        if (!my_row(y, bn, bi)) continue;
        const float fy = dy != 0 ? (y - a.y) / dy : 0;
        const float v = va + (vb - va) * fy;
        for (int x = ix0; x <= ix1; ++x) {
            const float fx = dx != 0 ? (x - a.x) / dx : 0;
            fragment(D, x, y, b.z, c, ua + (ub - ua) * fx, v, b.f);
        }
    }
}

void draw_triangle(const DrawState& D, const Vertex& v0, const Vertex& v1, const Vertex& v2, int bn, int bi) {
    const float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    if (area == 0) return;
    const int ix0 = std::max((int)std::ceil(std::min({v0.x, v1.x, v2.x})), D.scax0);
    const int ix1 = std::min((int)std::ceil(std::max({v0.x, v1.x, v2.x})) - 1, D.scax1);
    const int iy0 = std::max((int)std::ceil(std::min({v0.y, v1.y, v2.y})), D.scay0);
    const int iy1 = std::min((int)std::ceil(std::max({v0.y, v1.y, v2.y})) - 1, D.scay1);
    if (ix0 > ix1 || iy0 > iy1) return;
    float s0, t0, q0, s1, t1, q1, s2, t2, q2;
    tex_coords(D, v0, &s0, &t0, &q0); tex_coords(D, v1, &s1, &t1, &q1); tex_coords(D, v2, &s2, &t2, &q2);
    const float inv = 1.0f / area;
    // top-left rule: an edge owns its pixels when it is a top or left edge
    auto edge_bias = [&](const Vertex& a, const Vertex& b) {
        const float ex = b.x - a.x, ey = b.y - a.y;
        const bool top_left = area > 0 ? (ey < 0 || (ey == 0 && ex > 0)) : (ey > 0 || (ey == 0 && ex < 0));
        return top_left ? 0.0f : 1e-6f;
    };
    const float bias0 = edge_bias(v1, v2), bias1 = edge_bias(v2, v0), bias2 = edge_bias(v0, v1);
    const Rgba flat{(int)v2.r, (int)v2.g, (int)v2.b, (int)v2.a};
    for (int y = iy0; y <= iy1; ++y) {
        if (!my_row(y, bn, bi)) continue;
        for (int x = ix0; x <= ix1; ++x) {
            const float px = (float)x, py = (float)y;
            float w0 = ((v2.x - v1.x) * (py - v1.y) - (v2.y - v1.y) * (px - v1.x)) * inv;
            float w1 = ((v0.x - v2.x) * (py - v2.y) - (v0.y - v2.y) * (px - v2.x)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < bias0 || w1 < bias1 || w2 < bias2) continue;
            const uint32_t z = (uint32_t)std::clamp((double)v0.z * w0 + (double)v1.z * w1 + (double)v2.z * w2, 0.0, 4294967295.0);
            Rgba c = flat;
            if (D.iip) c = Rgba{(int)(v0.r * w0 + v1.r * w1 + v2.r * w2 + 0.5f), (int)(v0.g * w0 + v1.g * w1 + v2.g * w2 + 0.5f),
                                (int)(v0.b * w0 + v1.b * w1 + v2.b * w2 + 0.5f), (int)(v0.a * w0 + v1.a * w1 + v2.a * w2 + 0.5f)};
            float u = s0 * w0 + s1 * w1 + s2 * w2, v = t0 * w0 + t1 * w1 + t2 * w2;
            if (!D.fst) { const float q = q0 * w0 + q1 * w1 + q2 * w2; if (q != 0) { u /= q; v /= q; } }
            fragment(D, x, y, z, c, u, v, v0.f * w0 + v1.f * w1 + v2.f * w2);
        }
    }
}

void draw_line(const DrawState& D, const Vertex& a, const Vertex& b, int bn, int bi) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const int n = (int)std::ceil(std::max(std::fabs(dx), std::fabs(dy)));
    float sa, ta, qa, sb, tb, qb;
    tex_coords(D, a, &sa, &ta, &qa); tex_coords(D, b, &sb, &tb, &qb);
    for (int i = 0; i < std::max(n, 1); ++i) {
        const float f = n ? (float)i / n : 0;
        const Vertex& cv = D.iip ? a : b;
        Rgba c{(int)(cv.r + (D.iip ? (b.r - a.r) * f : 0)), (int)(cv.g + (D.iip ? (b.g - a.g) * f : 0)),
               (int)(cv.b + (D.iip ? (b.b - a.b) * f : 0)), (int)(cv.a + (D.iip ? (b.a - a.a) * f : 0))};
        float u = sa + (sb - sa) * f, v = ta + (tb - ta) * f;
        if (!D.fst) { const float q = qa + (qb - qa) * f; if (q != 0) { u /= q; v /= q; } }
        const int py = (int)std::floor(a.y + dy * f + 0.5f);
        if (!my_row(py, bn, bi)) continue;
        fragment(D, (int)std::floor(a.x + dx * f + 0.5f), py, (uint32_t)(a.z + (double)((int64_t)b.z - a.z) * f), c, u, v, a.f + (b.f - a.f) * f);
    }
}

// ---- deferred, multithreaded drawing ------------------------------------------------------
// Primitives are queued with a snapshot of their drawing state and rasterized in
// parallel (row bands) when something needs GS memory to be current: uploads,
// transfers, CLUT loads, the display, or a draw that samples what queued
// draws are rendering.
struct Job { uint8_t kind; uint32_t state; Vertex v[3]; };   // kind: 0 line/point, 1 triangle, 2 sprite
std::vector<Job> g_jobs;
std::vector<DrawState> g_states;
struct Range { uint32_t lo, hi; };   // GS memory written by queued jobs (256-byte blocks)
std::vector<Range> g_writes;
uint64_t g_job_area = 0;

void run_jobs(int bn, int bi) {
    for (const Job& j : g_jobs) {
        const DrawState& D = g_states[j.state];
        switch (j.kind) {
        case 0: draw_line(D, j.v[0], j.v[1], bn, bi); break;
        case 1: draw_triangle(D, j.v[0], j.v[1], j.v[2], bn, bi); break;
        default: draw_sprite(D, j.v[0], j.v[1], bn, bi); break;
        }
    }
}

class Pool {
public:
    Pool() {
        n_ = std::clamp((int)std::thread::hardware_concurrency(), 1, 16);
        for (int i = 1; i < n_; ++i) std::thread([this, i] { worker(i); }).detach();
    }
    int size() const { return n_; }
    void run() {   // every thread draws its bands; returns when all are done
        pending_.store(n_ - 1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_);
            ++gen_;
        }
        cv_.notify_all();
        run_jobs(n_, 0);
        while (pending_.load(std::memory_order_acquire) != 0) std::this_thread::yield();
    }

private:
    void worker(int i) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [&] { return gen_ != seen; });
                seen = gen_;
            }
            run_jobs(n_, i);
            pending_.fetch_sub(1, std::memory_order_release);
        }
    }
    int n_ = 1;
    std::mutex m_;
    std::condition_variable cv_;
    uint64_t gen_ = 0;
    std::atomic<int> pending_{0};
};

void flush() {
    if (g_jobs.empty()) return;
    static Pool pool;
    if (g_job_area < 4096 || pool.size() == 1) run_jobs(1, 0);
    else pool.run();
    g_jobs.clear();
    g_states.clear();
    g_writes.clear();
    g_job_area = 0;
}

uint32_t span_blocks(uint32_t bw, uint32_t rows, uint32_t bits) {   // + slack for page rounding
    return (uint32_t)(((uint64_t)(bw ? bw : 1) * 64 * rows * bits / 8 + 255) / 256) + 64;
}

void queue(uint8_t kind, const Vertex& a, const Vertex& b, const Vertex& c) {
    const DrawState& D = g_cur;
    if (kind == 2) log_sprite(D, a, b);
    if (g_states.empty() || std::memcmp(&g_states.back(), &D, sizeof D) != 0) {
        if (D.tme) {   // sampling something a queued draw renders: draw that first
            const uint32_t lo = D.tex.bp, hi = lo + span_blocks(D.tex.bw, D.tex.th, psm_bits(D.tex.psm));
            for (const Range& r : g_writes)
                if (lo < r.hi && r.lo < hi) { flush(); break; }
        }
        g_states.push_back(D);
        const uint32_t rows = (uint32_t)D.scay1 + 1;
        g_writes.push_back({D.fbp, D.fbp + span_blocks(D.fbw, rows, psm_bits(D.fpsm))});
        if (D.zte && !D.zmsk) g_writes.push_back({D.zbp, D.zbp + span_blocks(D.fbw, rows, psm_bits(D.zpsm))});
    }
    Job j;
    j.kind = kind;
    j.state = (uint32_t)g_states.size() - 1;
    j.v[0] = a; j.v[1] = b; j.v[2] = c;
    g_jobs.push_back(j);
    const float x0 = std::min({a.x, b.x, c.x}), x1 = std::max({a.x, b.x, c.x});
    const float y0 = std::min({a.y, b.y, c.y}), y1 = std::max({a.y, b.y, c.y});
    g_job_area += (uint64_t)(std::clamp(x1 - x0, 0.0f, 2048.0f) + 1) * (uint64_t)(std::clamp(y1 - y0, 0.0f, 2048.0f) + 1);
    if (g_jobs.size() >= 16384) flush();
}

void kick(uint32_t reg) {
    const bool draw = reg == XYZ2 || reg == XYZF2;
    const uint64_t xyz = g_reg[reg], rgbaq = g_reg[RGBAQ], st = g_reg[ST], uv = g_reg[UV];
    const Ctx k = context();
    Vertex v;
    v.x = (float)((int)bf(xyz, 0, 16) - k.ofx) / 16.0f;
    v.y = (float)((int)bf(xyz, 16, 16) - k.ofy) / 16.0f;
    const bool fog_form = reg == XYZF2 || reg == XYZF3;
    v.z = fog_form ? bf(xyz, 32, 24) : bf(xyz, 32, 32);
    v.f = fog_form ? (float)bf(xyz, 56, 8) : (float)bf(g_reg[FOG], 56, 8);
    v.r = (float)bf(rgbaq, 0, 8); v.g = (float)bf(rgbaq, 8, 8); v.b = (float)bf(rgbaq, 16, 8); v.a = (float)bf(rgbaq, 24, 8);
    uint32_t qb = bf(rgbaq, 32, 32), sb = bf(st, 0, 32), tb = bf(st, 32, 32);
    std::memcpy(&v.q, &qb, 4); std::memcpy(&v.s, &sb, 4); std::memcpy(&v.t, &tb, 4);
    if (!std::isfinite(v.q) || v.q == 0) v.q = 1.0f;
    v.u = (float)bf(uv, 0, 14) / 16.0f; v.v = (float)bf(uv, 16, 14) / 16.0f;
    ++stats().gs_vertices;

    const uint32_t type = bf(g_reg[PRIM], 0, 3);
    if (g_strip_count == 0) g_fan0 = v;
    ++g_strip_count;
    auto push = [&](const Vertex& x) {
        if (g_nq == 3) { g_q[0] = g_q[1]; g_q[1] = g_q[2]; g_nq = 2; }
        g_q[g_nq++] = x;
    };
    push(v);
    if (draw) setup_draw();
    switch (type) {
    case 0: if (draw) queue(0, v, v, v); g_nq = 0; break;   // point
    case 1: if (g_nq == 2) { if (draw) queue(0, g_q[0], g_q[1], g_q[1]); g_nq = 0; } break;
    case 2: if (g_nq >= 2) { if (draw) queue(0, g_q[g_nq - 2], g_q[g_nq - 1], g_q[g_nq - 1]); } break;
    case 3: if (g_nq == 3) { if (draw) queue(1, g_q[0], g_q[1], g_q[2]); g_nq = 0; } break;
    case 4: if (g_nq == 3 && draw) queue(1, g_q[0], g_q[1], g_q[2]); break;
    case 5: if (g_nq == 3 && draw) queue(1, g_fan0, g_q[1], g_q[2]); break;
    case 6: if (g_nq == 2) { if (draw) queue(2, g_q[0], g_q[1], g_q[1]); g_nq = 0; } break;
    default: g_nq = 0; break;
    }
}

} // namespace

Stats& stats() {
    static Stats s;
    return s;
}

void gs_write_reg(uint32_t reg, uint64_t value) {
    reg &= 0xFF;
    g_reg[reg] = value;
    switch (reg) {
    case PRIM:
        ++stats().gs_prims;
        ++stats().prim_type[value & 7];
        g_nq = 0;
        g_strip_count = 0;
        break;
    case XYZ2: case XYZF2: case XYZ3: case XYZF3: kick(reg); break;
    case TEX0_1: case TEX0_2: load_clut(value); break;
    case TEX2_1: case TEX2_2: {   // TEX2 replaces TEX0's PSM and CLUT fields
        uint64_t& t0 = g_reg[reg - TEX2_1 + TEX0_1];
        const uint64_t keep = ~((0x3Full << 20) | (0x7FFFFFFFFull << 37));
        t0 = (t0 & keep) | (value & ~keep);
        load_clut(t0);
        break;
    }
    case TRXDIR: start_transfer(); break;
    case HWREG: gs_image_data(reinterpret_cast<const uint8_t*>(&value), 0); break;
    default: break;
    }
}

void gs_vertex_kick(bool) {}

// Image data: qwc quadwords (qwc == 0: one 64-bit HWREG word).
void gs_image_data(const uint8_t* data, uint32_t qwc) {
    ProfScope prof_scope(prof().image);
    stats().gs_image_qw += qwc;
    flush();
    if (!T.active) return;
    const uint32_t bytes = qwc ? qwc * 16 : 8;
    const uint32_t bits = psm_bits(T.psm);
    uint32_t i = 0;
    if (T.psm == PSMCT32 && T.acc_bits == 0) {   // fast path: whole 32-bit pixels
        for (; i + 4 <= bytes && T.y < T.h; i += 4) {
            uint32_t v;
            std::memcpy(&v, data + i, 4);
            g_gsmem[addr32(T.bp, T.bw, (T.x0 + T.x) & 2047, (T.y0 + T.y) & 2047)] = v;
            if (++T.x == T.w) { T.x = 0; ++T.y; }
        }
        if (T.y >= T.h) i = bytes;
    }
    for (; i < bytes; ++i) {
        T.acc |= (uint64_t)data[i] << T.acc_bits;
        T.acc_bits += 8;
        while (T.acc_bits >= bits) {
            transfer_pixel((uint32_t)(T.acc & ((1ull << bits) - 1)));
            T.acc >>= bits;
            T.acc_bits -= bits;
        }
    }
    if (T.y >= T.h) T.active = false;
}

void gs_priv_write(uint32_t addr, uint64_t value) {
    const uint32_t i = (addr & 0xFFF) >> 4;
    if (i < 0x20 && !(addr & 0x1000)) g_priv[i] = value;
}

uint64_t gs_priv_read(uint32_t addr) {
    const uint32_t i = (addr & 0xFFF) >> 4;
    return (i < 0x20 && !(addr & 0x1000)) ? g_priv[i] : 0;
}

// The displayed picture (read circuit 1, or 2 if only it is enabled) as 0x00RRGGBB.
bool gs_display(std::vector<uint32_t>* out, uint32_t* w, uint32_t* h) {
    ProfScope prof_scope(prof().display);
    flush();
    const uint64_t pmode = g_priv[0];
    const int circuit = (pmode & 1) ? 0 : (pmode & 2) ? 1 : -1;
    if (circuit < 0) return false;
    const uint64_t dispfb = g_priv[7 + circuit * 2], display = g_priv[8 + circuit * 2];
    const uint32_t fbp = bf(dispfb, 0, 9) * 32, fbw = bf(dispfb, 9, 6), psm = bf(dispfb, 15, 5);
    const uint32_t dbx = bf(dispfb, 32, 11), dby = bf(dispfb, 43, 11);
    const uint32_t magh = bf(display, 23, 4) + 1, magv = bf(display, 27, 2) + 1;
    *w = (bf(display, 32, 12) + 1) / magh;
    *h = (bf(display, 44, 11) + 1) / magv;
    if (*w == 0 || *h == 0 || *w > 2048 || *h > 2048) return false;
    out->resize((size_t)*w * *h);
    if (psm == PSMCT32 || psm == PSMCT24) {   // fast path
        for (uint32_t y = 0; y < *h; ++y) {
            uint32_t* row = out->data() + (size_t)y * *w;
            for (uint32_t x = 0; x < *w; ++x) {
                const uint32_t raw = g_gsmem[addr32(fbp, fbw, x + dbx, y + dby)];
                row[x] = ((raw & 0xFF) << 16) | (raw & 0xFF00) | ((raw >> 16) & 0xFF);
            }
        }
        return true;
    }
    for (uint32_t y = 0; y < *h; ++y)
        for (uint32_t x = 0; x < *w; ++x) {
            const uint32_t raw = read_pixel(psm, fbp, fbw, x + dbx, y + dby);
            uint32_t rgb;
            if (psm == PSMCT16 || psm == PSMCT16S) rgb = ((raw & 0x1F) << 19) | (((raw >> 5) & 0x1F) << 11) | (((raw >> 10) & 0x1F) << 3);
            else rgb = ((raw & 0xFF) << 16) | (raw & 0xFF00) | ((raw >> 16) & 0xFF);
            (*out)[(size_t)y * *w + x] = rgb;
        }
    return true;
}

Prof& prof() { static Prof p; return p; }

// Timing is only taken with `--log gpu`: clock reads are not free on hot paths.
ProfScope::ProfScope(double& s) : slot(s), t0(log_enabled(Log::Gpu) ? std::chrono::steady_clock::now().time_since_epoch().count() : 0) {}
ProfScope::~ProfScope() {
    if (!t0) return;
    using D = std::chrono::steady_clock::duration;
    slot += std::chrono::duration<double>(D(std::chrono::steady_clock::now().time_since_epoch().count() - t0)).count();
}

void print_stats() {
    {
        Prof& p = prof();
        std::fprintf(stderr, "[gpu] time: vif %.2fs vu1 %.2fs gif %.2fs image %.2fs display %.2fs ipu %.2fs\n", p.vif, p.vu1, p.gif,
                     p.image, p.display, p.ipu);
    }
    const Stats& s = stats();
    log(Log::Gpu, "VIF1 %llu qw: %llu unpacks, %llu MSCAL, %llu MPG words, %llu DIRECT qw",
        (unsigned long long)s.vif1_qw, (unsigned long long)s.vif1_unpack, (unsigned long long)s.vif1_mscal,
        (unsigned long long)s.vif1_mpg_words, (unsigned long long)s.vif1_direct_qw);
    log(Log::Gpu, "GIF qw path1 %llu path2 %llu path3 %llu, %llu tags; GS %llu PRIM, %llu vertices, %llu image qw",
        (unsigned long long)s.gif_qw[1], (unsigned long long)s.gif_qw[2], (unsigned long long)s.gif_qw[3],
        (unsigned long long)s.gif_tags, (unsigned long long)s.gs_prims, (unsigned long long)s.gs_vertices,
        (unsigned long long)s.gs_image_qw);
    log(Log::Gpu, "  prim types: pt %llu ln %llu lstrip %llu tri %llu tstrip %llu fan %llu sprite %llu",
        (unsigned long long)s.prim_type[0], (unsigned long long)s.prim_type[1], (unsigned long long)s.prim_type[2],
        (unsigned long long)s.prim_type[3], (unsigned long long)s.prim_type[4], (unsigned long long)s.prim_type[5],
        (unsigned long long)s.prim_type[6]);
    vu1_print_stats();
}

} // namespace ps2::gpu

namespace ps2 {
void gpu_vblank(Context*) {
    static uint64_t frames;
    static std::vector<uint32_t> pic;
    ++frames;
    if (frames % 600 == 0 && log_enabled(Log::Gpu)) gpu::print_stats();
    uint32_t w, h;
    if (gpu::gs_display(&pic, &w, &h)) display_present(pic.data(), w, h, frames);
}
} // namespace ps2
