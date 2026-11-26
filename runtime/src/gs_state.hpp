// GS drawing state shared by the software rasterizer (gs.cpp) and the
// hardware renderer (gs_hw.cpp): decoded vertices and per-primitive state.
#pragma once
#include <cstdint>

namespace ps2::gpu {

struct Rgba { int r, g, b, a; };

struct Vertex {
    float x, y;        // pixels (offset removed)
    uint32_t z;
    float r, g, b, a;
    float s, t, q;     // STQ
    float u, v;        // texels (FST)
    float f;           // fog
};

struct Ctx {   // drawing state of the active context, decoded at draw time
    uint64_t tex0, tex1, clamp, alpha, test, frame, zbuf, scissor, fba;
    int ofx, ofy;
};

struct Texture {
    uint32_t psm, bp, bw, tw, th, tcc, tfx, clut_off, clut;
    uint32_t wms, wmt, minu, maxu, minv, maxv;
    bool bilinear;
    uint64_t texa;
};

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

// CLUT buffer entries (gs.cpp): 256 CT32 or 512 CT16 slots of palette `index`.
const uint32_t* gs_clut(uint32_t index, uint32_t* psm);

} // namespace ps2::gpu
