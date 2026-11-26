// EE hardware registers (MMIO). The game's own code rarely touches hardware;
// most accesses come from Sony libraries that will be replaced by HLE. This
// layer keeps those accesses well-defined meanwhile:
//   - timers count in real time,
//   - registers with known reset/idle values report them,
//   - everything else is a logged, stored register,
//   - a watchdog stops tight polling loops on a register that never changes.
#include <chrono>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu.hpp"
#include "ipu.hpp"
#include "perf.hpp"
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

// The raw tick count at which interrupt event k (1-based) of the timer fires:
// COUNT reaching COMP with CMPE, or a 16-bit wrap with OVFE.
bool timer_compare_mode(const Timer& t) { return (t.mode & 0x100) && t.comp; }
uint64_t timer_event_raw(const Timer& t, uint64_t k) {
    return timer_compare_mode(t) ? t.comp + (k - 1) * timer_period(t) : k * 0x10000ull;
}

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
// D1/D2 CHCR..ASR1 live outside the register map: the GIF/VIF worker owns them
// while it runs a transfer, and must never touch the (non-thread-safe) map.
uint32_t g_gfx_regs[2][6];
bool gfx_reg(uint32_t a) {
    return !(a & 0xF) && ((a >= 0x10009000 && a < 0x10009060) || (a >= 0x1000A000 && a < 0x1000A060));
}
uint32_t& R(uint32_t a) { return gfx_reg(a) ? g_gfx_regs[a >= 0x1000A000][(a & 0xFF) >> 4] : g_regs[a]; }
std::atomic<uint32_t> g_gfx_done{0};   // channels the worker finished; the game thread raises their interrupts
std::atomic<uint64_t> g_gfx_done_at{0};   // when (perf_now) the oldest undelivered one finished (TWONK_PERF_LOG)

// VIF1 DMA snapshots. RenderWare starts one VIF1 chain per frame and waits for it
// to finish before going on, while the transfer runs for most of a frame on the
// gfx thread: game and gfx took turns instead of overlapping. When the game starts
// a VIF1 transfer, the chain is walked on the game thread and everything it sends
// is copied; the transfer is then complete as far as the game can tell (registers,
// interrupt), and the gfx thread runs it from the copy. TWONK_DMA_SNAPSHOT=0 turns
// it off.
struct DmaSnap {
    struct Seg { uint32_t addr; bool spr; uint32_t off, len; };
    std::vector<Seg> segs;
    std::vector<uint8_t> data;
    size_t cursor = 0;
    uint32_t regs[6];   // D1 CHCR (with STR), MADR, QWC, TADR, ASR0, ASR1 at the start
};
thread_local DmaSnap* g_snap = nullptr;   // the gfx thread, while it runs a snapshotted transfer
std::atomic<int> g_snaps_pending{0};
uint32_t g_d1_shadow[6];       // game thread: D1 registers as the game sees them
bool g_d1_shadow_live = true;  // they are current (every VIF1 transfer since the last sync was snapshotted)

bool dma_snapshot_on() {
    static const bool on = [] { const char* e = std::getenv("TWONK_DMA_SNAPSHOT"); return !(e && e[0] == '0'); }();
    return on;
}

const uint8_t* snap_find(uint32_t addr, bool spr, uint32_t len) {
    const auto& v = g_snap->segs;
    for (size_t k = 0; k < v.size(); ++k) {   // usually the next one: reads come in the order they were recorded
        const size_t i = (g_snap->cursor + k) % v.size();
        const DmaSnap::Seg& sg = v[i];
        if (sg.spr == spr && addr >= sg.addr && addr + len <= sg.addr + sg.len) {
            g_snap->cursor = i;
            return g_snap->data.data() + sg.off + (addr - sg.addr);
        }
    }
    return nullptr;
}

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

void finish_dma(Context*, int ch) {
    R(kDmaBase[ch]) &= ~0x100u;             // STR: transfer finished
    if (ch == 1 && g_snap) return;          // the game was told when the copy was taken
    if (ch == 1 || ch == 2) {   // on the worker
        if (perf_enabled()) { uint64_t z = 0; g_gfx_done_at.compare_exchange_strong(z, perf_now()); }
        g_gfx_done.fetch_or(1u << ch);
        return;
    }
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
    if (g_snap) {
        const bool in_spr = spr || (addr & 0x80000000u);
        const uint32_t a = in_spr ? (addr & (kSprSize - 1) & ~15u) : (addr & 0x1FFFFFF0u);
        if (const uint8_t* p = snap_find(a, in_spr, std::max(qwc, 1u) * 16)) return const_cast<uint8_t*>(p);
        static int missed;
        if (missed++ < 5) std::fprintf(stderr, "[dma] snapshot miss at %08x (%u qw): reading live memory\n", addr, qwc);
    }
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
        return dma_mem(ctx, addr + i * 16, in_spr, 1);
    };
    if (ch == 2 && g_vif1_busy) {   // stop at the first packet boundary
        for (uint32_t i = 0; i < qwc; ++i) {
            gpu::gif_write(gpu::Path::P3, qword(i), 1);
            if (gpu::gif_p3_packet_end()) { g_p3_pause = true; return i + 1; }
        }
        return qwc;
    }
    if (in_spr && g_snap) {
        for (uint32_t i = 0; i < qwc; ++i) {
            if (ch == 1) gpu::vif1_write(ctx, qword(i), 1); else gpu::gif_write(gpu::Path::P3, qword(i), 1);
        }
    } else if (in_spr) {   // runs up to where the scratchpad wraps
        for (uint32_t i = 0; i < qwc;) {
            const uint32_t off = (addr + i * 16) & (kSprSize - 1) & ~15u;
            const uint32_t run = std::min(qwc - i, (kSprSize - off) / 16);
            const uint8_t* q = ctx->spr + off;
            if (ch == 1) gpu::vif1_write(ctx, q, run); else gpu::gif_write(gpu::Path::P3, q, run);
            i += run;
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
    uint32_t& chcr = R(base);
    uint32_t& madr = R(base + 0x10);
    uint32_t& qwc = R(base + 0x20);
    uint32_t& tadr = R(base + 0x30);
    const bool tte = (chcr >> 6) & 1;
    uint32_t stack[2] = {R(base + 0x40), R(base + 0x50)};
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
    R(base + 0x40) = stack[0];
    R(base + 0x50) = stack[1];
    return !paused;
}

// Game thread: copy what a VIF1 transfer starting with `r` (CHCR..ASR1) will send,
// following the chain as run_chain does, and leave in `r` the registers it ends
// with. False when it cannot (the transfer then runs the old way).
bool snap_walk(Context* ctx, DmaSnap& sn, uint32_t r[6]) {
    auto copy = [&](uint32_t addr, bool spr, uint32_t n) -> bool {
        if (!n) return true;
        const bool in_spr = spr || (addr & 0x80000000u);
        if (!in_spr) {
            const uint32_t a = addr & 0x1FFFFFF0u;
            if ((uint64_t)a + n * 16ull > kRamSize) return false;
            sn.segs.push_back({a, false, (uint32_t)sn.data.size(), n * 16});
            sn.data.insert(sn.data.end(), ctx->ram + a, ctx->ram + a + n * 16);
            return true;
        }
        for (uint32_t i = 0; i < n; ++i) {   // scratchpad wraps: contiguous runs
            const uint32_t a = (addr + i * 16) & (kSprSize - 1) & ~15u;
            DmaSnap::Seg* last = sn.segs.empty() ? nullptr : &sn.segs.back();
            if (last && last->spr && last->addr + last->len == a && last->off + last->len == sn.data.size()) last->len += 16;
            else sn.segs.push_back({a, true, (uint32_t)sn.data.size(), 16});
            sn.data.insert(sn.data.end(), ctx->spr + a, ctx->spr + a + 16);
        }
        return true;
    };
    uint32_t& chcr = r[0];
    uint32_t& madr = r[1];
    uint32_t& qwc = r[2];
    uint32_t& tadr = r[3];
    const uint32_t mode = (chcr >> 2) & 3;
    if (!(chcr & 1) || mode > 1 || g_chain_end_pending[1]) return false;
    if (mode == 0) {
        if (!copy(madr, false, qwc)) return false;
        madr += qwc * 16;
        qwc = 0;
        chcr &= ~0x100u;
        return true;
    }
    const bool tte = (chcr >> 6) & 1;
    (void)tte;
    uint32_t stack[2] = {r[4], r[5]};
    uint32_t asp = (chcr >> 4) & 3;
    bool done = false;
    auto send = [&](uint32_t from, bool from_spr, uint32_t n, bool last) -> bool {
        if (!copy(from, from_spr, n)) return false;
        madr = from + n * 16;
        qwc = 0;
        if (last) done = true;
        return true;
    };
    if (qwc && !send(madr, false, qwc, false)) return false;
    for (int guard = 0; !done; ++guard) {
        if (guard >= 1 << 20) return false;
        if ((uint64_t)(tadr & 0x1FFFFFF0u) + 16 > kRamSize) return false;
        if (!copy(tadr, false, 1)) return false;   // the tag (TTE sends its upper half)
        uint64_t tag;
        std::memcpy(&tag, ctx->ram + (tadr & 0x1FFFFFF0u), 8);
        const uint32_t n = (uint32_t)(tag & 0xFFFF), id = (uint32_t)((tag >> 28) & 7);
        const uint32_t addr = (uint32_t)((tag >> 32) & 0x7FFFFFF0u);
        const bool spr = (tag >> 63) & 1, irq = (tag >> 31) & 1;
        const bool stop_irq = irq && (chcr & 0x80);
        chcr = (chcr & 0xFFFF) | (uint32_t)(tag & 0xFFFF0000u);
        const uint32_t data = tadr + 16;
        bool ok = true;
        switch (id) {
        case 0:  tadr += 16; ok = send(addr, spr, n, true); break;
        case 1:  tadr += 16 + n * 16; ok = send(data, false, n, stop_irq); break;
        case 2:  tadr = addr; ok = send(data, false, n, stop_irq); break;
        case 3: case 4: tadr += 16; ok = send(addr, spr, n, stop_irq); break;
        case 5:
            if (asp >= 2) return false;
            stack[asp++] = data + n * 16;
            tadr = addr;
            ok = send(data, false, n, stop_irq);
            break;
        case 6:
            if (asp) { tadr = stack[--asp]; ok = send(data, false, n, stop_irq); }
            else { tadr += 16 + n * 16; ok = send(data, false, n, true); }
            break;
        default: tadr += 16 + n * 16; ok = send(data, false, n, true); break;
        }
        if (!ok) return false;
    }
    chcr = ((chcr & ~0x30u) | (asp << 4)) & ~0x100u;
    r[4] = stack[0];
    r[5] = stack[1];
    return true;
}

void run_dma(Context* ctx, int ch) {
    // Only VIF1 transfers read a snapshot; PATH3 resuming in the middle of one reads live memory.
    struct SnapScope {
        DmaSnap* saved;
        explicit SnapScope(bool keep) : saved(g_snap) { if (!keep) g_snap = nullptr; }
        ~SnapScope() { g_snap = saved; }
    } snap_scope(ch == 1);
    const uint32_t base = kDmaBase[ch];
    uint32_t& chcr = R(base);
    const uint32_t mode = (chcr >> 2) & 3;   // 0 normal, 1 chain, 2 interleave
    uint32_t& madr = R(base + 0x10);
    uint32_t& qwc = R(base + 0x20);
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

// For the hang watchdog: the DMA/PATH3 state a RenderWare sync loop waits on.
void dump_hw_state() {
    std::fprintf(stderr, "[ps2] D1 chcr %08x madr %08x qwc %x tadr %08x | D2 chcr %08x madr %08x qwc %x tadr %08x\n",
                 R(0x10009000), R(0x10009010), R(0x10009020), R(0x10009030), R(0x1000A000),
                 R(0x1000A010), R(0x1000A020), R(0x1000A030));
    std::fprintf(stderr, "[ps2] PATH3 stalled %d paused %d mskpath3 %d vif1 busy %d, VIF1_STAT %08x\n", g_gif_stalled,
                 g_p3_pause, gpu::vif1_mskpath3(), g_vif1_busy, g_regs[0x10003C00]);
}
void dma_complete(Context* ctx, int ch) { finish_dma(ctx, ch); }

// VIF1 lifted the PATH3 mask: a GIF transfer that was waiting runs now.
void gif_path3_resume(Context* ctx) {
    if (!g_gif_stalled) return;
    g_gif_stalled = false;
    run_dma(ctx, 2);
}


// ---- GIF/VIF worker ----------------------------------------------------------------------
// VIF1 (with the VU1 microprograms it starts), PATH3 and everything they feed the
// GS run on their own thread, as the real DMAC runs beside the EE. The game thread
// queues, in order: writes to D1/D2 registers (a CHCR write with STR starts the
// transfer), VIF1 FIFO writes, GS privileged register writes and the vblank
// present. A finished transfer raises its DMAC interrupt on the game thread at the
// next delivery point. Reads of D2 and GIF_STAT wait for the worker to go idle, so
// the game never sees a transfer half-done; VIF1 transfers are snapshotted (see
// DmaSnap), so D1 and D_STAT reads answer at once. TWONK_GFX_SYNC=1 runs it all
// inline instead.
namespace {

enum class GfxCmd : uint8_t { Reg, Fifo, GsPriv, Present, SnapDma };
struct Gfx { GfxCmd kind; uint32_t a; uint64_t b; };

struct GfxWorker {
    std::mutex m;
    std::condition_variable cv_work, cv_idle;
    std::vector<Gfx> q;
    uint64_t issued = 0, done = 0;   // guarded by m
    std::atomic<bool> idle{true};
    bool sync = false;
    Context* ctx = nullptr;

    GfxWorker() {
        char buf[8]; size_t n = 0;
        sync = !getenv_s(&n, buf, sizeof buf, "TWONK_GFX_SYNC") && n && buf[0] == '1';
        if (!sync) std::thread([this] { profiler_name_thread("gfx"); loop(); }).detach();
    }

    void exec(const Gfx& c) {
        switch (c.kind) {
        case GfxCmd::Reg: {
            R(c.a) = (uint32_t)c.b;
            if (const int ch = dma_channel(c.a); ch >= 0 && (c.b & 0x100u)) run_dma(ctx, ch);
            break;
        }
        case GfxCmd::Fifo: {
            const uint32_t w[2] = {(uint32_t)c.b, (uint32_t)(c.b >> 32)};
            gpu::vif1_fifo_words(ctx, w, (int)c.a);
            break;
        }
        case GfxCmd::GsPriv: gpu::gs_priv_write(c.a, c.b); break;
        case GfxCmd::Present: gpu::gs_present(c.b); break;
        case GfxCmd::SnapDma: {   // a VIF1 transfer, from its snapshot
            DmaSnap* sn = reinterpret_cast<DmaSnap*>(c.b);
            for (int i = 0; i < 6; ++i) R(0x10009000 + i * 0x10) = sn->regs[i];
            g_snap = sn;
            run_dma(ctx, 1);
            g_snap = nullptr;
            delete sn;
            g_snaps_pending.fetch_sub(1, std::memory_order_release);
            break;
        }
        }
    }

    void push(Context* c, const Gfx& cmd) {
        if (c) ctx = c;
        if (sync) { exec(cmd); return; }
        {
            std::lock_guard<std::mutex> lock(m);
            q.push_back(cmd);
            ++issued;
            idle.store(false, std::memory_order_relaxed);
        }
        cv_work.notify_one();
    }

    void wait_idle() {
        if (sync || idle.load(std::memory_order_acquire)) return;
        PerfScope wait(kPerfGameWaitGfx);
        std::unique_lock<std::mutex> lock(m);
        cv_idle.wait(lock, [&] { return done == issued; });
    }

    void loop() {
        std::vector<Gfx> batch;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m);
                if (done == issued) { idle.store(true, std::memory_order_release); cv_idle.notify_all(); }
                cv_work.wait(lock, [&] { return !q.empty(); });
                batch.swap(q);
            }
            {
                PerfScope busy(kPerfGfxBusy);
                for (const Gfx& c : batch) exec(c);
            }
            std::lock_guard<std::mutex> lock(m);
            done += batch.size();
            batch.clear();
        }
    }
};

GfxWorker& gfx() { static GfxWorker w; return w; }

} // namespace

// Game thread: DMAC interrupts for the transfers the worker finished.
void hw_collect_gfx() {
    const uint32_t d = g_gfx_done.exchange(0, std::memory_order_acq_rel);
    if (!d) return;
    if (const uint64_t at = g_gfx_done_at.exchange(0)) {
        perf_add(kPerfDmaLatency, perf_now() - at);
        perf_add(kPerfDmaDone, 1);
    }
    for (int ch = 1; ch <= 2; ++ch)
        if (d & (1u << ch)) {
            g_regs[0x1000E010] |= 1u << ch;   // D_STAT CIS
            kernel_raise_dmac(ch);
        }
}

void gfx_sync() {
    gfx().wait_idle();
    hw_collect_gfx();
    for (int i = 0; i < 6; ++i) g_d1_shadow[i] = R(0x10009000 + i * 0x10);   // the worker is idle: D1 is the game's again
    g_d1_shadow_live = true;
}

// For the perf log: which register reads made the game wait for the gfx thread.
std::map<uint32_t, std::pair<uint32_t, uint64_t>> g_sync_by_reg;   // address -> (waits, ns); game thread only
void gfx_sync_reg(uint32_t a) {
    if (!perf_enabled()) { gfx_sync(); return; }
    const bool was_idle = gfx().idle.load(std::memory_order_acquire);
    const uint64_t t0 = perf_now();
    gfx_sync();
    if (!was_idle) {
        auto& e = g_sync_by_reg[a];
        ++e.first;
        e.second += perf_now() - t0;
    }
}

std::string take_sync_reasons() {
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& [a, e] : g_sync_by_reg) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%ux reg %08x, %.1f ms", e.first, a, e.second / 1e6);
        v.emplace_back(e.second, buf);
    }
    g_sync_by_reg.clear();
    std::sort(v.rbegin(), v.rend());
    std::string out;
    for (size_t i = 0; i < v.size() && i < 4; ++i) out += "    " + v[i].second + "\n";
    return out;
}

// A spinning guest: let the worker finish (up to `usec`), or yield the core if it is idle.
void gfx_wait_progress(int usec) {
    GfxWorker& w = gfx();
    if (w.sync || w.idle.load(std::memory_order_acquire)) { SwitchToThread(); return; }
    PerfScope wait(kPerfGameWaitGfx);
    std::unique_lock<std::mutex> lock(w.m);
    w.cv_idle.wait_for(lock, std::chrono::microseconds(usec), [&] { return w.done == w.issued; });
}

void gfx_present(uint64_t frame) { gfx().push(nullptr, {GfxCmd::Present, 0, frame}); }

// One compare/overflow event of timer i (0-2), if one is due: sets EQUF/OVFF and
// returns true, so the kernel runs the INTC 9+i handler once per event. With the
// cause masked, events are only counted.
uint32_t g_timer_irq_mask;   // INTC causes enabled, as the kernel last said
int g_timer_in_handler = -1;   // timer whose INTC handler is running, else -1
uint64_t g_timer_delivered[3], g_timer_dropped[3];   // statistics (TWONK_TIME_DEBUG)

bool timer_take_event(int i, uint32_t intc_mask) {
    g_timer_irq_mask = intc_mask;
    Timer& t = g_timer[i];
    if (!(t.mode & 0x80) || !(t.mode & 0x300)) return false;   // counting, CMPE or OVFE
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
    if (ev <= t.events) return false;
    t.mode |= flag;
    if (!(intc_mask & (1u << (9 + i)))) { g_timer_dropped[i] += ev - t.events; t.events = ev; return false; }
    ++t.events;
    ++g_timer_delivered[i];
    return true;
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
    if (gfx_reg(a)) {
        if (a < 0x1000A000 && g_d1_shadow_live && dma_snapshot_on()) { hw_collect_gfx(); return g_d1_shadow[(a & 0xFF) >> 4]; }
        gfx_sync_reg(a);
        return R(a);
    }
    int ti, tr;
    if (timer_reg(a, &ti, &tr)) {
        const Timer& t = g_timer[ti];
        switch (tr) {
        case 0: {
            // While an overflow interrupt is still undelivered, the count holds at the
            // last value before the wrap. Otherwise the guest would read a wrapped count
            // with the old overflow total, and its clock would jump back.
            uint64_t raw = timer_raw(t);
            const bool irq = timer_compare_mode(t) ? (t.mode & 0x100) : (t.mode & 0x200);
            if ((g_timer_irq_mask & (1u << (9 + ti))) && (t.mode & 0x80) && irq) {
                const uint64_t limit = timer_event_raw(t, t.events + 1) - 1;
                if (raw > limit) raw = limit;
            }
            return (uint32_t)(raw % timer_period(t)) & 0xFFFF;
        }
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
        gfx_sync();
        return (gpu::vif1_mskpath3() ? 0x2u : 0u) | (g_gif_stalled ? 16u << 24 : 0u);
    case 0x1000E010:   // D_STAT (with snapshots, VIF1 completions are already in it)
        if (dma_snapshot_on()) { hw_collect_gfx(); return g_regs[a]; }
        gfx_sync_reg(a);
        return g_regs[a];
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
        gfx().push(ctx, {GfxCmd::Fifo, (uint32_t)(size / 4), value});
        return;
    }
    if (a >= 0x12000000 && a < 0x12001000) gfx().push(ctx, {GfxCmd::GsPriv, a, value});
    if (gfx_reg(a)) {   // D1/D2: in order on the worker; a CHCR write with STR starts the transfer
        if (log_enabled(Log::Mem)) log(Log::Mem, "mmio write%d %s %08x <- %08x", size * 8, region_name(a), a, v);
        if (a < 0x1000A000) {   // D1 (VIF1)
            const int i = (int)((a & 0xFF) >> 4);
            g_d1_shadow[i] = v;
            if (i == 0 && (v & 0x100u)) {   // STR: the transfer starts
                if (dma_snapshot_on() && g_d1_shadow_live) {
                    auto* sn = new DmaSnap;
                    uint32_t r[6];
                    std::memcpy(r, g_d1_shadow, sizeof r);
                    std::memcpy(sn->regs, g_d1_shadow, sizeof r);
                    if (snap_walk(ctx, *sn, r)) {
                        while (g_snaps_pending.load(std::memory_order_acquire) >= 2) gfx_wait_progress(2000);   // stay at most two transfers ahead
                        g_snaps_pending.fetch_add(1, std::memory_order_relaxed);
                        gfx().push(ctx, {GfxCmd::SnapDma, 0, reinterpret_cast<uint64_t>(sn)});
                        std::memcpy(g_d1_shadow, r, sizeof r);
                        g_regs[0x1000E010] |= 1u << 1;   // D_STAT CIS1, and the DMAC interrupt
                        kernel_raise_dmac(1);
                        return;
                    }
                    delete sn;
                }
                g_d1_shadow_live = false;   // runs on the worker the old way: reads must wait for it
            }
        }
        gfx().push(ctx, {GfxCmd::Reg, a, v});
        return;
    }
    if (ipu::owns(a)) { ipu::mmio_write(ctx, a, value); return; }

    int ti, tr;
    if (timer_reg(a, &ti, &tr)) {
        Timer& t = g_timer[ti];
        static const bool tlog = std::getenv("TWONK_TIMER_LOG") != nullptr;
        if (tlog && ti == 0) {
            static unsigned n;
            if (n++ < 60) std::fprintf(stderr, "[timer] T0 reg %d <- %08x (mode %08x count %04x events %llu)\n", tr, (unsigned)v, t.mode,
                                       timer_count(t), (unsigned long long)t.events);
        }
        switch (tr) {
        case 0:
            if (g_timer_in_handler == ti && t.events && (t.mode & 0x80)) {
                // The timer's interrupt handler resetting COUNT: on hardware it runs
                // right at the compare/wrap event. Here it runs at the next delivery point, often several ms
                // late, so restart the count from that event rather than from now.
                // (Twonkies' clock: COMP 0x8000, the handler adds 0x8000 and zeroes COUNT.)
                const double rate = timer_rate(t.mode);
                t.t_base += (double)(timer_event_raw(t, t.events) - t.count_base) / rate;
                t.count_base = v & 0xFFFF;
                t.events = 0;
            } else {
                timer_rebase(t, v);
            }
            break;
        case 1: {   // EQUF/OVFF (bits 10/11) are write-1-to-clear
            // Only a change of clock, ZRET or count enable restarts the count. An
            // interrupt handler acknowledging OVFF must not: that dropped every other
            // overflow still pending, and the game's clock (timer 0 plus an overflow
            // count) lost 7 ms per lost wrap, so the whole game ran at 35-60% speed.
            const bool restart = ((v ^ t.mode) & 0xC3) != 0;
            const uint32_t c = timer_count(t);
            t.mode = (v & 0x3FF) | (t.mode & 0xC00 & ~v);
            if (restart) timer_rebase(t, c);
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
