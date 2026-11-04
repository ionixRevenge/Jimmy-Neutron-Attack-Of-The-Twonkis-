// Runtime services called from generated code.
// Indirect control flow is real; hardware/OS services are placeholders until the
// HLE layers (kernel, IOP, GS/RenderWare) land.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

extern const ps2::FuncEntry g_func_table[];
extern const unsigned g_func_table_size;

namespace ps2 {

static RecompFn lookup(uint32_t addr) {
    const FuncEntry* b = g_func_table;
    const FuncEntry* e = g_func_table + g_func_table_size;
    const FuncEntry* it = std::lower_bound(b, e, addr, [](const FuncEntry& f, uint32_t a) { return f.addr < a; });
    return (it != e && it->addr == addr) ? it->fn : nullptr;
}

// Ring buffer of recent function entries (filled only when the game is built with PS2_TRACE=1).
static uint32_t g_trace[64];
static unsigned g_trace_pos;
void trace_enter(Context*, uint32_t addr) { g_trace[g_trace_pos++ & 63] = addr; }

void fatal(Context* ctx, uint32_t pc, const char* what) {
    std::fprintf(stderr, "[ps2] fatal at %08x: %s (ra=%08x sp=%08x)\n", pc, what, ctx->r[31].u32[0], ctx->r[29].u32[0]);
    if (g_trace_pos) {
        std::fprintf(stderr, "[ps2] last function entries (oldest first):");
        const unsigned n = g_trace_pos < 64 ? g_trace_pos : 64;
        for (unsigned i = 0; i < n; ++i)
            std::fprintf(stderr, "%s%08x", (i % 8) ? " " : "\n   ", g_trace[(g_trace_pos - n + i) & 63]);
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
    std::abort();
}

void unimplemented(Context* ctx, uint32_t pc, uint32_t raw, const char* name) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "unimplemented instruction %08x (%s)", raw, name);
    fatal(ctx, pc, msg);
}

void call_indirect(Context* ctx, uint32_t target) {
    if (RecompFn fn = lookup(target)) { fn(ctx); return; }
    fatal(ctx, target, "call to address that is not a known function");
}

void jump_indirect(Context* ctx, uint32_t target) {
    if (RecompFn fn = lookup(target)) { fn(ctx); return; }
    fatal(ctx, target, "jump to address that is not a known function");
}

void syscall(Context* ctx, uint32_t pc) { kernel_syscall(ctx, pc); }
void breakpoint(Context* ctx, uint32_t pc) { fatal(ctx, pc, "break"); }
bool cop0_condition(Context*) { return true; }   // DMA always idle
bool vu0_busy(Context*) { return false; }        // VU0 macro ops complete synchronously


} // namespace ps2
