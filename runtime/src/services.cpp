// Runtime services called from generated code.
// Indirect control flow is real; hardware/OS services are placeholders until the
// HLE layers (kernel, IOP, GS/RenderWare) land.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "ps2/recomp.hpp"

extern const ps2::FuncEntry g_func_table[];
extern const unsigned g_func_table_size;

namespace ps2 {

static RecompFn lookup(uint32_t addr) {
    const FuncEntry* b = g_func_table;
    const FuncEntry* e = g_func_table + g_func_table_size;
    const FuncEntry* it = std::lower_bound(b, e, addr, [](const FuncEntry& f, uint32_t a) { return f.addr < a; });
    return (it != e && it->addr == addr) ? it->fn : nullptr;
}

void fatal(Context* ctx, uint32_t pc, const char* what) {
    std::fprintf(stderr, "[ps2] fatal at %08x: %s (ra=%08x sp=%08x)\n", pc, what, ctx->r[31].u32[0], ctx->r[29].u32[0]);
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

void syscall(Context* ctx, uint32_t pc) { fatal(ctx, pc, "syscall (kernel HLE not implemented yet)"); }
void breakpoint(Context* ctx, uint32_t pc) { fatal(ctx, pc, "break"); }
bool cop0_condition(Context*) { return true; }   // DMA always idle
bool vu0_busy(Context*) { return false; }        // VU0 macro ops complete synchronously

uint32_t mmio_read(Context* ctx, uint32_t addr, int) { fatal(ctx, addr, "mmio read"); }
void mmio_write(Context* ctx, uint32_t addr, uint64_t, int) { fatal(ctx, addr, "mmio write"); }

} // namespace ps2
