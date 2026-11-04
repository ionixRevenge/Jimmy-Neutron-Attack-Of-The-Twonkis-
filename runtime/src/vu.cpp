// VU0 control registers accessed with CFC2/CTC2 (vi16..vi31).
#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2 {

uint32_t vu0_ctrl_read(Context* ctx, unsigned reg) {
    switch (reg) {
    case 20: return ctx->vR;
    case 21: return as_u32(ctx->vI);
    case 22: return as_u32(ctx->vq);
    case 23: return as_u32(ctx->vp);
    case 28: return 0;          // FBRST
    case 29: return 0;          // VPU-STAT: VU0 and VU1 idle
    default: return reg < 32 ? ctx->vi[reg] : 0;
    }
}

void vu0_ctrl_write(Context* ctx, unsigned reg, uint32_t v) {
    switch (reg) {
    case 0: return;                                   // vi0 is hard-wired to zero
    case 20: ctx->vR = 0x3F800000u | (v & 0x007FFFFFu); return;
    case 21: ctx->vI = as_float(v); return;
    case 22: ctx->vq = as_float(v); return;
    case 28: return;                                  // FBRST (reset/force-break): nothing to reset
    case 27: case 31: {                               // CMSAR0 / CMSAR1: start a VU microprogram
        static unsigned warned;
        if (warned++ < 8)
            log(Log::Cpu, "%s write %08x ignored (VU microprograms are replaced by HLE)", reg == 27 ? "CMSAR0" : "CMSAR1", v);
        ctx->vi[reg] = v;
        return;
    }
    default:
        if (reg < 16) ctx->vi[reg] = v & 0xFFFF;
        else if (reg < 32) ctx->vi[reg] = v;
        return;
    }
}

} // namespace ps2
