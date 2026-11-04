// EE hardware registers (MMIO). The game's own code rarely touches hardware;
// most accesses come from Sony libraries that will be replaced by HLE. This
// layer keeps those accesses well-defined meanwhile:
//   - timers count in real time,
//   - registers with known reset/idle values report them,
//   - everything else is a logged, stored register,
//   - a watchdog stops tight polling loops on a register that never changes.
#include <chrono>
#include <cstdio>
#include <unordered_map>

#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2 {

namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point kStart = Clock::now();
constexpr double kBusHz = 147456000.0;
constexpr double kHblankHz = 15734.0;

struct Timer { uint32_t count_base = 0, mode = 0, comp = 0, hold = 0; double t_base = 0; };
Timer g_timer[4];
std::unordered_map<uint32_t, uint32_t> g_regs;
std::unordered_map<uint32_t, uint64_t> g_unknown_reads;
uint32_t g_last_read_addr = 0;
uint64_t g_same_read_count = 0;

double now_s() { return std::chrono::duration<double>(Clock::now() - kStart).count(); }

double timer_rate(uint32_t mode) {
    switch (mode & 3) {
    case 0: return kBusHz;
    case 1: return kBusHz / 16;
    case 2: return kBusHz / 256;
    default: return kHblankHz;
    }
}

uint32_t timer_count(const Timer& t) {
    if (!(t.mode & 0x80)) return t.count_base & 0xFFFF;   // CUE: count enable
    return (uint32_t)(t.count_base + (uint64_t)((now_s() - t.t_base) * timer_rate(t.mode))) & 0xFFFF;
}

bool timer_reg(uint32_t a, int* idx, int* reg) {
    if (a < 0x10000000 || a >= 0x10001840) return false;
    *idx = (int)((a - 0x10000000) >> 11);
    *reg = (int)((a >> 4) & 3);   // 0 COUNT, 1 MODE, 2 COMP, 3 HOLD
    return (a & 0x7FF) < 0x40;
}

const char* region_name(uint32_t a) {
    if (a < 0x10002000) return "timer";
    if (a < 0x10004000) return "ipu";
    if (a < 0x10008000) return "gif/vif fifo";
    if (a < 0x1000E000) return "dma channel";
    if (a < 0x1000F000) return "dmac";
    if (a < 0x1000F200) return "intc";
    if (a < 0x1000F300) return "sif";
    if (a < 0x10010000) return "misc";
    if (a >= 0x12000000 && a < 0x12002000) return "gs priv";
    if (a >= 0x11000000 && a < 0x11010000) return "vu mem";
    return "?";
}

} // namespace

uint32_t mmio_read(Context* ctx, uint32_t addr, int size) {
    const uint32_t a = addr & 0x1FFFFFFF;   // strip kseg0/kseg1

    // watchdog: the same register read millions of times in a row is a stuck poll
    if (a == g_last_read_addr) {
        if (++g_same_read_count == 20000000ull) {
            char msg[128];
            std::snprintf(msg, sizeof msg, "stuck polling %s register %08x (value %08x)", region_name(a), a, g_regs[a]);
            fatal(ctx, addr, msg);
        }
    } else {
        g_last_read_addr = a;
        g_same_read_count = 0;
    }

    int ti, tr;
    if (timer_reg(a, &ti, &tr)) {
        const Timer& t = g_timer[ti];
        switch (tr) {
        case 0: return timer_count(t);
        case 1: return t.mode | (ti == 3 ? 0x100u : 0u);   // T3 CMPE: tells libkernl the alarm patch is present
        case 2: return t.comp;
        default: return t.hold;
        }
    }
    switch (a) {
    case 0x12001000: {  // GS CSR: report VSINT/HSINT pending and the current field
        const uint64_t frames = (uint64_t)(now_s() * 59.94);
        return 0x0000000Cu | ((uint32_t)(frames & 1) << 13) | (g_regs[a] & ~0x200Fu);
    }
    case 0x1000E010: return g_regs[a];   // D_STAT
    case 0x1000F000: return g_regs[a];   // INTC_STAT
    case 0x1000F010: return g_regs[a];   // INTC_MASK
    default: break;
    }
    auto it = g_regs.find(a);
    const uint32_t v = it == g_regs.end() ? 0u : it->second;
    if (g_unknown_reads[a]++ == 0 || log_enabled(Log::Mem))   // first read of each register is always shown
        std::fprintf(stderr, "[mem] mmio read%d %s %08x -> %08x\n", size * 8, region_name(a), a, v);
    return v;
}

void mmio_write(Context*, uint32_t addr, uint64_t value, int size) {
    const uint32_t a = addr & 0x1FFFFFFF;
    const uint32_t v = (uint32_t)value;
    g_last_read_addr = 0;   // a write breaks a polling streak

    int ti, tr;
    if (timer_reg(a, &ti, &tr)) {
        Timer& t = g_timer[ti];
        switch (tr) {
        case 0: t.count_base = v & 0xFFFF; t.t_base = now_s(); break;
        case 1: t.count_base = timer_count(t); t.t_base = now_s(); t.mode = v & 0x3FF; break;   // flag bits are write-1-to-clear
        case 2: t.comp = v & 0xFFFF; break;
        default: t.hold = v & 0xFFFF; break;
        }
        return;
    }
    switch (a) {
    case 0x1000F000: g_regs[a] &= ~v; return;   // INTC_STAT: write 1 to clear
    case 0x1000F010: g_regs[a] ^= v; return;    // INTC_MASK: write 1 to toggle
    case 0x1000E010: {                          // D_STAT: low bits clear, high bits toggle
        uint32_t& s = g_regs[a];
        s = (s & ~(v & 0xFFFF)) ^ (v & 0xFFFF0000u);
        return;
    }
    case 0x12001000: g_regs[a] = v & ~0xFu; return;   // GS CSR: low bits are write-1-to-clear
    default: break;
    }
    g_regs[a] = v;
    if (log_enabled(Log::Mem)) log(Log::Mem, "mmio write%d %s %08x <- %08x", size * 8, region_name(a), a, v);
}

} // namespace ps2
