// Widescreen: native replacement for RenderWare's RwCameraSetViewWindow.
#include <cstdio>
#include <mutex>

#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace hle {

// 0x25cd40 RwCameraSetViewWindow(RwCamera* cam, const RwV2d* vw): stores the view
// window (tangents of the half fields of view) at cam+104 and its reciprocal at
// cam+112, then marks the camera's frame dirty (0x25e180) when it has one.
// RenderWare builds both the projection and the frustum-culling planes from these,
// so widening the X extent gives a true wider view (Hor+), culling included.
//
// Cameras drawing to the screen have a view window wider than it is tall (about
// 1.26-1.4 here); the square ones render to textures (shadows, reflections) and are
// left alone. The game's main camera recomputes its view window each frame and calls
// this whenever it differs from the stored one, so with widening on it calls every
// frame, and an aspect change in the F4 menu shows at once.
void rw_camera_set_view_window(ps2::Context* ctx) {
    using namespace ps2;
    const uint32_t cam = ctx->r[4].u32[0], vw = ctx->r[5].u32[0];
    float x = as_float(mem_read<uint32_t>(ctx, vw)), y = as_float(mem_read<uint32_t>(ctx, vw + 4));
    const float k = video_widescreen_factor();
    // (2D cameras have view windows in pixels, 320 x 224: those stay as they are)
    const bool widen = k != 1.0f && y > 0 && x > y * 1.15f && x < 4.0f;
    {   // a line per distinct caller and setting in the log, to see which cameras are widened
        static std::mutex m;
        static uint64_t seen[64];
        static int n = 0;
        const uint32_t ra = ctx->r[31].u32[0];
        const uint64_t key = (uint64_t)ra << 32 | (uint32_t)(k * 1000) << 1 | (widen ? 1u : 0u);
        std::lock_guard<std::mutex> lock(m);
        bool known = false;
        for (int i = 0; i < n; ++i) known |= seen[i] == key;
        if (!known && n < 64) {
            seen[n++] = key;
            std::fprintf(stderr, "[widescreen] camera %08x (called from %08x): view window %.3f x %.3f, %s\n", cam, ra, x, y,
                         widen ? "widened" : k == 1.0f ? "4:3 (not widened)" : "not a screen camera");
        }
    }
    if (widen) x *= k;
    // the guest function, step for step (same results in the same registers)
    const float one = 1.0f;
    mem_write<uint32_t>(ctx, cam + 104, as_u32(x));
    mem_write<uint32_t>(ctx, cam + 108, as_u32(y));
    ctx->f[2] = x;
    ctx->f[1] = one;
    ctx->f[0] = fdiv(ctx, one, x);
    mem_write<uint32_t>(ctx, cam + 112, as_u32(ctx->f[0]));
    ctx->f[0] = fdiv(ctx, one, y);
    mem_write<uint32_t>(ctx, cam + 116, as_u32(ctx->f[0]));
    const uint32_t frame = mem_read<uint32_t>(ctx, cam + 4);
    ctx->r[4].s64[0] = (int32_t)frame;
    if (frame) {
        const uint32_t sp = ctx->r[29].u32[0];
        const auto ra = ctx->r[31];
        ctx->r[29].s64[0] = (int32_t)(sp - 32);   // the guest's frame, so the callee's stack use lands where it did
        ctx->r[31].s64[0] = (int32_t)0x0025cd98u;
        call_indirect(ctx, 0x0025e180u);
        ctx->r[29].s64[0] = (int32_t)sp;
        ctx->r[31] = ra;   // the guest restores ra from its frame
    }
    ctx->r[2].s64[0] = (int32_t)cam;
}

} // namespace hle
