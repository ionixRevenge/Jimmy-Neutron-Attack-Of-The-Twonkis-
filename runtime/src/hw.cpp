// EE hardware registers (MMIO). The game's own code rarely touches hardware;
// most accesses come from Sony libraries that will be replaced by HLE. This
// layer keeps those accesses well-defined meanwhile:
//   - timers count in real time,
//   - registers with known reset/idle values report them,
//   - everything else is a logged, stored register,
//   - a watchdog stops tight polling loops on a register that never changes.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "gpu.hpp"
#include "ipu.hpp"
#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2 {

void gif_path3_resume(Context* ctx);   // below: VIF1 lifted the PATH3 mask

namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point kStart = Clock::now();
constexpr double kBusHz = 147456000.0;
constexpr double kHblankHz = 15734.0;

struct Timer { uint32_t count_base = 0, mode = 0, comp = 0, hold = 0; double t_base = 0; uint64_t events = 0; };
Timer g_timer[4];
std::unordered_map<uint32_t, uint32_t> g_regs;
std::unordered_map<uint32_t, uint64_t> g_unknown_reads;
uint32_t g_last_read_addr = 0;
uint64_t g_same_read_count = 0;

double now_s() { return std::chrono::duration<double>(Clock::now() - kStart).count(); }

// INTC_STAT latches VBLANK_S (bit 2) and VBLANK_E (bit 3) once per frame until written back.
uint64_t g_stat_frame = 0;
void latch_vblank() {
    const uint64_t frame = (uint64_t)(now_s() * 59.94);
    if (frame != g_stat_frame) {
        g_stat_frame = frame;
        g_regs[0x1000F000] |= 0xCu;
    }
}

double timer_rate(uint32_t mode) {
    switch (mode & 3) {
    case 0: return kBusHz;
    case 1: return kBusHz / 16;
    case 2: return kBusHz / 256;
    default: return kHblankHz;
    }
}

// Ticks since the last rebase (COUNT or MODE write), plus the count at that point.
uint64_t timer_raw(const Timer& t) {
    if (!(t.mode & 0x80)) return t.count_base;   // CUE: count enable
    return t.count_base + (uint64_t)((now_s() - t.t_base) * timer_rate(t.mode));
}

// ZRET restarts the count at COMP; otherwise it wraps at 16 bits.
uint32_t timer_period(const Timer& t) { return ((t.mode & 0x40) && t.comp) ? t.comp : 0x10000u; }

uint32_t timer_count(const Timer& t) { return (uint32_t)(timer_raw(t) % timer_period(t)) & 0xFFFF; }

void timer_rebase(Timer& t, uint32_t count) {
    t.count_base = count & 0xFFFF;
    t.t_base = now_s();
    t.events = 0;
}

bool timer_reg(uint32_t a, int* idx, int* reg) {
    if (a < 0x10000000 || a >= 0x10001840) return false;
    *idx = (int)((a - 0x10000000) >> 11);
    *reg = (int)((a >> 4) & 3);   // 0 COUNT, 1 MODE, 2 COMP, 3 HOLD
    return (a & 0x7FF) < 0x40;
}

const char* region_name(uint32_t a) {
    if (a < 0x10002000) return "timer";
    if (a < 0x10003000) return "ipu";
    if (a < 0x10003800) return "gif";
    if (a < 0x10004000) return "vif";
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

// ---- DMA controller ------------------------------------------------------------------
// Transfers complete the moment STR is written. Scratchpad transfers copy memory;
// GIF/VIF/IPU transfers are counted (their consumers are not implemented yet).
constexpr uint32_t kDmaBase[10] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                   0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};
const char* const kDmaName[10] = {"VIF0", "VIF1", "GIF", "fromIPU", "toIPU", "SIF0", "SIF1", "SIF2", "fromSPR", "toSPR"};
uint64_t g_dma_count[10];

// PATH3 masking: a GIF transfer started while VIF1's MSKPATH3 is on waits (its
// data sits in the GIF FIFO, FQC != 0) until VIF1 lifts the mask.
//
// VIF1 runs synchronously here, so PATH3 cannot really run beside it. RenderWare
// opens a short MSKPATH3 off/on window before each texture's draws, and the
// hardware lets one GIF packet (up to an EOP tag) through it. So, while a VIF1
// transfer is in progress, PATH3 pauses after every EOP packet. Each MSKPATH3-off
// edge releases the next packet, and the end of the VIF1 transfer releases the rest.
bool g_gif_stalled = false;
bool g_vif1_busy = false;
bool g_p3_pause = false;          // PATH3 stopped at a packet boundary
bool g_chain_end_pending[10];     // the paused tag was the chain's last

void finish_dma(Context* ctx, int ch) {
    g_regs[kDmaBase[ch]] &= ~0x100u;        // STR: transfer finished
    g_regs[0x1000E010] |= 1u << ch;         // D_STAT CIS
    kernel_raise_dmac(ch);
}

int dma_channel(uint32_t a) {
    for (int c = 0; c < 10; ++c)
        if (a == kDmaBase[c]) return c;
    return -1;
}

// Physical DMA address -> host memory (bit 31 of MADR / bit 63 of a tag selects scratchpad).
uint8_t* dma_mem(Context* ctx, uint32_t addr, bool spr, uint32_t qwc) {
    if (spr || (addr & 0x80000000u)) return ctx->spr + (addr & (kSprSize - 1) & ~15u);
    const uint32_t a = addr & 0x1FFFFFF0u;
    if (a + qwc * 16 > kRamSize) fatal(ctx, addr, "DMA transfer outside RAM");
    return ctx->ram + a;
}

// Sends `qwc` quadwords to channel 1 (VIF1) or 2 (GIF PATH3); returns how many were
// sent (fewer when PATH3 pauses at a packet boundary). Scratchpad sources wrap.
uint32_t dma_send(Context* ctx, int ch, uint32_t addr, bool spr, uint32_t qwc) {
    if (!qwc) return 0;
    if (ch == 9) {   // toSPR: memory -> scratchpad at SADR
        uint32_t& sadr = g_regs[kDmaBase[9] + 0x80];
        for (uint32_t i = 0; i < qwc; ++i) {
            std::memcpy(ctx->spr + (sadr & (kSprSize - 1) & ~15u), dma_mem(ctx, addr + i * 16, spr, 1), 16);
            sadr += 16;
        }
        return qwc;
    }
    const bool in_spr = spr || (addr & 0x80000000u);
    auto qword = [&](uint32_t i) -> const uint8_t* {
        return in_spr ? ctx->spr + ((addr + i * 16) & (kSprSize - 1) & ~15u) : dma_mem(ctx, addr + i * 16, false, 1);
    };
    if (ch == 2 && g_vif1_busy) {   // stop at the first packet boundary
        for (uint32_t i = 0; i < qwc; ++i) {
            gpu::gif_write(gpu::Path::P3, qword(i), 1);
            if (gpu::gif_p3_packet_end()) { g_p3_pause = true; return i + 1; }
        }
        return qwc;
    }
    if (in_spr) {
        for (uint32_t i = 0; i < qwc; ++i) {
            if (ch == 1) gpu::vif1_write(ctx, qword(i), 1); else gpu::gif_write(gpu::Path::P3, qword(i), 1);
        }
    } else {
        const uint8_t* p = dma_mem(ctx, addr, false, qwc);
        if (ch == 1) gpu::vif1_write(ctx, p, qwc); else gpu::gif_write(gpu::Path::P3, p, qwc);
    }
    if (ch == 2) gpu::gif_p3_packet_end();   // not pausing: drop the flag
    return qwc;
}

// Source-chain DMA (EE manual, DMAC chapter): tags refe/cnt/next/ref/refs/call/ret/end.
// Returns false when PATH3 paused mid-chain; MADR/QWC/TADR then say where to resume.
bool run_chain(Context* ctx, int ch) {
    const uint32_t base = kDmaBase[ch];
    uint32_t& chcr = g_regs[base];
    uint32_t& madr = g_regs[base + 0x10];
    uint32_t& qwc = g_regs[base + 0x20];
    uint32_t& tadr = g_regs[base + 0x30];
    const bool tte = (chcr >> 6) & 1;
    uint32_t stack[2] = {g_regs[base + 0x40], g_regs[base + 0x50]};
    uint32_t asp = (chcr >> 4) & 3;
    bool done = false, paused = false;
    // send `n` qwords at `from`; on a pause, MADR/QWC keep the rest of this tag
    auto send = [&](uint32_t from, bool from_spr, uint32_t n, bool last) {
        const uint32_t sent = dma_send(ctx, ch, from, from_spr, n);
        madr = from + sent * 16;
        qwc = n - sent;
        if (g_p3_pause) { g_p3_pause = false; paused = true; g_chain_end_pending[ch] = last; }
        else if (last) done = true;
    };
    // QWC left over from a previous (suspended) transfer goes first
    if (qwc) send(madr, false, qwc, g_chain_end_pending[ch]);
    else if (g_chain_end_pending[ch]) done = true;
    g_chain_end_pending[ch] = false;
    for (int guard = 0; !done && !paused && guard < 1 << 20; ++guard) {
        const uint8_t* t = dma_mem(ctx, tadr, false, 1);
        uint64_t tag;
        std::memcpy(&tag, t, 8);
        const uint32_t n = (uint32_t)(tag & 0xFFFF), id = (uint32_t)((tag >> 28) & 7);
        const uint32_t addr = (uint32_t)((tag >> 32) & 0x7FFFFFF0u);
        const bool spr = (tag >> 63) & 1, irq = (tag >> 31) & 1;
        const bool stop_irq = irq && (chcr & 0x80);   // TIE: stop at an IRQ tag
        chcr = (chcr & 0xFFFF) | (uint32_t)(tag & 0xFFFF0000u);   // TAG field mirrors the tag's upper half
        if (tte && ch == 1) gpu::vif1_fifo_words(ctx, reinterpret_cast<const uint32_t*>(t + 8), 2);
        const uint32_t data = tadr + 16;
        switch (id) {
        case 0:  tadr += 16; send(addr, spr, n, true); break;                       // refe
        case 1:  tadr += 16 + n * 16; send(data, false, n, stop_irq); break;        // cnt
        case 2:  tadr = addr; send(data, false, n, stop_irq); break;                // next
        case 3: case 4: tadr += 16; send(addr, spr, n, stop_irq); break;            // ref / refs
        case 5:                                                                     // call
            if (asp >= 2) fatal(ctx, tadr, "DMA call stack overflow");
            stack[asp++] = data + n * 16;
            tadr = addr;
            send(data, false, n, stop_irq);
            break;
        case 6:                                                                     // ret
            if (asp) { tadr = stack[--asp]; send(data, false, n, stop_irq); }
            else { tadr += 16 + n * 16; send(data, false, n, true); }
            break;
        default: tadr += 16 + n * 16; send(data, false, n, true); break;            // end
        }
    }
    chcr = (chcr & ~0x30u) | (asp << 4);
    g_regs[base + 0x40] = stack[0];
    g_regs[base + 0x50] = stack[1];
    return !paused;
}

void run_dma(Context* ctx, int ch) {
    const uint32_t base = kDmaBase[ch];
    uint32_t& chcr = g_regs[base];
    const uint32_t mode = (chcr >> 2) & 3;   // 0 normal, 1 chain, 2 interleave
    uint32_t& madr = g_regs[base + 0x10];
    uint32_t& qwc = g_regs[base + 0x20];
    if (g_dma_count[ch]++ == 0) log(Log::Boot, "first %s DMA (mode %u)", kDmaName[ch], mode);
    if (ch == 2 && gpu::vif1_mskpath3()) {   // PATH3 masked: wait for VIF1
        g_gif_stalled = true;
        return;
    }
    if ((ch == 8 || ch == 9) && mode == 0) {
        uint32_t& sadr = g_regs[base + 0x80];
        for (uint32_t i = 0; i < qwc; ++i) {
            uint8_t* spr = ctx->spr + ((sadr + i * 16) & (kSprSize - 1));
            uint8_t* mem = dma_mem(ctx, madr + i * 16, false, 1);
            if (ch == 8) std::memcpy(mem, spr, 16);
            else std::memcpy(spr, mem, 16);
        }
        madr += qwc * 16;
        sadr += qwc * 16;
        qwc = 0;
    } else if (ch == 9 && mode == 1) {
        run_chain(ctx, ch);
    } else if (ch == 8 || ch == 9) {
        fatal(ctx, base, "SPR DMA in this mode is not implemented");
    } else if (ch == 1 || ch == 2) {
        if (ch == 1 && !(chcr & 1)) fatal(ctx, base, "VIF1 DMA towards memory is not implemented");
        const bool outer = ch == 1 && !g_vif1_busy;
        if (outer) g_vif1_busy = true;
        bool finished = true;
        if (mode == 1) finished = run_chain(ctx, ch);
        else {
            const uint32_t sent = dma_send(ctx, ch, madr, false, qwc);
            madr += sent * 16; qwc -= sent;
            if (g_p3_pause) { g_p3_pause = false; finished = false; }
        }
        if (outer) {
            g_vif1_busy = false;
            if (g_gif_stalled && !gpu::vif1_mskpath3()) gif_path3_resume(ctx);   // VIF1 done: PATH3 drains
        }
        if (!finished) { g_gif_stalled = true; return; }   // PATH3 waits for the next window
    } else if (ch == 3 || ch == 4) {
        ipu::dma_start(ctx, ch);   // the IPU completes the transfer as data moves
        return;
    } else if (ch == 5 || ch == 6 || ch == 7) {
        // SIF channels are driven by the SIF syscalls (iop.cpp), never directly
    } else {
        static unsigned warned;
        if (warned++ < 8) log(Log::Gpu, "%s DMA ignored (mode %u, qwc %u)", kDmaName[ch], mode, qwc);
        if (mode == 0) { madr += qwc * 16; qwc = 0; }
    }
    finish_dma(ctx, ch);
}

} // namespace

uint32_t& hw_reg(uint32_t addr) { return g_regs[addr]; }
void dma_complete(Context* ctx, int ch) { finish_dma(ctx, ch); }

// VIF1 lifted the PATH3 mask: a GIF transfer that was waiting runs now.
void gif_path3_resume(Context* ctx) {
    if (!g_gif_stalled) return;
    g_gif_stalled = false;
    run_dma(ctx, 2);
}

// Compare/overflow events since the last call: sets EQUF/OVFF and returns the INTC
// causes (9 + timer) whose interrupt is enabled. Timer 3 belongs to the kernel alarms.
uint32_t timer_poll_irqs() {
    uint32_t causes = 0;
    for (int i = 0; i < 3; ++i) {
        Timer& t = g_timer[i];
        if (!(t.mode & 0x80) || !(t.mode & 0x300)) continue;   // counting, CMPE or OVFE
        const uint64_t raw = timer_raw(t);
        uint64_t ev;
        uint32_t flag;
        if ((t.mode & 0x100) && t.comp) {   // compare: COUNT reaches COMP
            ev = raw >= t.comp ? 1 + (raw - t.comp) / timer_period(t) : 0;
            flag = 0x400;
        } else {
            ev = raw / 0x10000u;
            flag = 0x800;
        }
        if (ev > t.events) {
            t.events = ev;
            t.mode |= flag;
            causes |= 1u << (9 + i);
        }
    }
    return causes;
}

uint64_t mmio_read(Context* ctx, uint32_t addr, int size) {
    g_poll_tick += 16;   // register polls reach the interrupt delivery point sooner
    if (g_poll_tick >= 4096u) poll_point(ctx);
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

    if (ipu::owns(a)) return ipu::mmio_read(ctx, a);
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
    case 0x10003020:   // GIF_STAT: M3P, and FQC = 16 while a masked transfer waits
        return (gpu::vif1_mskpath3() ? 0x2u : 0u) | (g_gif_stalled ? 16u << 24 : 0u);
    case 0x1000E010: return g_regs[a];   // D_STAT
    case 0x1000F000: latch_vblank(); return g_regs[a];   // INTC_STAT
    case 0x1000F010: return g_regs[a];   // INTC_MASK
    default: break;
    }
    auto it = g_regs.find(a);
    const uint32_t v = it == g_regs.end() ? 0u : it->second;
    if (g_unknown_reads[a]++ == 0 || log_enabled(Log::Mem))   // first read of each register is always shown
        std::fprintf(stderr, "[mem] mmio read%d %s %08x -> %08x\n", size * 8, region_name(a), a, v);
    return v;
}

void mmio_write(Context* ctx, uint32_t addr, uint64_t value, int size) {
    const uint32_t a = addr & 0x1FFFFFFF;
    const uint32_t v = (uint32_t)value;
    g_last_read_addr = 0;   // a write breaks a polling streak
    if (a >= 0x10005000 && a < 0x10006000) {   // VIF1 FIFO: VIF codes / data
        const uint32_t w[2] = {(uint32_t)value, (uint32_t)(value >> 32)};
        gpu::vif1_fifo_words(ctx, w, size / 4);
        return;
    }
    if (a >= 0x12000000 && a < 0x12001000) gpu::gs_priv_write(a, value);
    if (ipu::owns(a)) { ipu::mmio_write(ctx, a, value); return; }

    int ti, tr;
    if (timer_reg(a, &ti, &tr)) {
        Timer& t = g_timer[ti];
        switch (tr) {
        case 0: timer_rebase(t, v); break;
        case 1: {   // EQUF/OVFF (bits 10/11) are write-1-to-clear
            const uint32_t c = timer_count(t);
            t.mode = (v & 0x3FF) | (t.mode & 0xC00 & ~v);
            timer_rebase(t, c);
            break;
        }
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
    if (const int ch = dma_channel(a); ch >= 0 && (v & 0x100u)) run_dma(ctx, ch);
}

} // namespace ps2
