// Runtime services called from generated code.
// Indirect control flow is real; hardware/OS services are placeholders until the
// HLE layers (kernel, IOP, GS/RenderWare) land.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
constexpr unsigned kTraceSize = 1u << 16;
static uint32_t g_trace[kTraceSize];
static unsigned g_trace_pos;
void trace_enter(Context*, uint32_t addr) { g_trace[g_trace_pos++ & (kTraceSize - 1)] = addr; }

// Whole ring to the file named by TWONK_TRACE_FILE (one address per line, oldest first).
void write_trace_file() {
    char path[512];
    size_t n = 0;
    if (getenv_s(&n, path, sizeof path, "TWONK_TRACE_FILE") || !n) return;
    std::FILE* f = std::fopen(path, "w");
    if (!f) return;
    const unsigned pos = g_trace_pos, cnt = pos < kTraceSize ? pos : kTraceSize;
    for (unsigned i = 0; i < cnt; ++i) std::fprintf(f, "%08x\n", g_trace[(pos - cnt + i) & (kTraceSize - 1)]);
    std::fclose(f);
    std::fprintf(stderr, "[ps2] wrote %u trace entries to %s\n", cnt, path);
}

// Snapshot of recent function entries; safe to call from another host thread (values may be torn).
void dump_trace() {
    const unsigned pos = g_trace_pos;
    const unsigned n = pos < 64 ? pos : 64;
    std::fprintf(stderr, "[ps2] last function entries (oldest first):");
    for (unsigned i = 0; i < n; ++i)
        std::fprintf(stderr, "%s%08x", (i % 8) ? " " : "\n   ", g_trace[(pos - n + i) & (kTraceSize - 1)]);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

void fatal(Context* ctx, uint32_t pc, const char* what) {
    if (ctx) std::fprintf(stderr, "[ps2] fatal at %08x: %s (ra=%08x sp=%08x)\n", pc, what, ctx->r[31].u32[0], ctx->r[29].u32[0]);
    else std::fprintf(stderr, "[ps2] fatal at %08x: %s\n", pc, what);
    if (g_trace_pos) {
        std::fprintf(stderr, "[ps2] last function entries (oldest first):");
        const unsigned n = g_trace_pos < 64 ? g_trace_pos : 64;
        for (unsigned i = 0; i < n; ++i)
            std::fprintf(stderr, "%s%08x", (i % 8) ? " " : "\n   ", g_trace[(g_trace_pos - n + i) & (kTraceSize - 1)]);
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
    write_trace_file();
    std::abort();
}

void unimplemented(Context* ctx, uint32_t pc, uint32_t raw, const char* name) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "unimplemented instruction %08x (%s)", raw, name);
    fatal(ctx, pc, msg);
}

// Cutscene skip. The game has none, so START during a cutscene fast-forwards it:
// the frame loop (f_00142870) updates the active cutscene player through a
// virtual call update(this, dt) returning to 0x001429e4; while skipping, that
// update runs many extra steps per frame, so the timeline and its script events
// still play in order, just quickly. Player state +84 is 5 while it plays.
static void cutscene_update(Context* ctx, RecompFn fn) {
    static bool skipping = false, start_was_down = true;
    const uint32_t player = ctx->r[4].u32[0];
    const auto a0 = ctx->r[4];
    const float dt = ctx->f[12];
    const bool start = (g_pad0_buttons & 0x8) != 0;
    if (start && !start_was_down) skipping = true;
    start_was_down = start;
    if (skipping) {
        uint32_t state = 0;
        std::memcpy(&state, guest_ptr(ctx, player + 84, 4), 4);
        if (state != 5) skipping = false;
    }
    if (skipping) {
        for (int i = 0; i < 40; ++i) {   // 40 x 50 ms of cutscene per frame
            ctx->r[4] = a0;
            ctx->f[12] = 0.05f;
            ctx->r[31].u32[0] = 0x001429e4u;
            fn(ctx);
            uint32_t state = 0;
            std::memcpy(&state, guest_ptr(ctx, player + 84, 4), 4);
            if (state != 5) { skipping = false; return; }
        }
        ctx->r[4] = a0;
        ctx->f[12] = dt;
        ctx->r[31].u32[0] = 0x001429e4u;
    }
    fn(ctx);
}

void call_indirect(Context* ctx, uint32_t target) {
    if (ctx->r[31].u32[0] == 0x001429e4u) {
        if (RecompFn fn = lookup(target)) { cutscene_update(ctx, fn); return; }
    }
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
