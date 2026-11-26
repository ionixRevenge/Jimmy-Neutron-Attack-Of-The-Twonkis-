// Performance monitor (TWONK_PERF_LOG=<file>): once a second, one line with the
// frame rate, the slowest frame, game time against real time, and where the
// threads spent their time. When the frame rate drops, a screenshot goes next to
// the log (TWONK_PERF_SHOTS=0 turns that off), so a slowdown can be placed.
#include "perf.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ps2/runtime.hpp"

namespace ps2 {
namespace gpu::hw { std::string take_download_reasons(); }
std::string take_sync_reasons();   // hw.cpp

std::atomic<uint64_t> g_perf[kPerfCount];

namespace {

uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<uint64_t> g_last_flip{0}, g_max_gap{0};

} // namespace

uint64_t perf_now() { return now_ns(); }

void perf_flip() {
    const uint64_t t = now_ns();
    const uint64_t last = g_last_flip.exchange(t, std::memory_order_relaxed);
    if (!last) return;
    const uint64_t gap = t - last;
    uint64_t m = g_max_gap.load(std::memory_order_relaxed);
    while (gap > m && !g_max_gap.compare_exchange_weak(m, gap, std::memory_order_relaxed)) {}
}

bool perf_enabled() {
    static const bool on = std::getenv("TWONK_PERF_LOG") != nullptr;
    return on;
}

void perf_second(double fps, double vblank_hz, double seconds, uint64_t frame, uint32_t game_ms) {
    static FILE* f = nullptr;
    static std::string dir;
    static bool shots = true;
    if (!f) {
        const char* path = std::getenv("TWONK_PERF_LOG");
        if (!path) return;
        f = std::fopen(path, "w");
        if (!f) return;
        dir = path;
        const size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
        const char* s = std::getenv("TWONK_PERF_SHOTS");
        shots = !(s && s[0] == '0');
        std::fprintf(f, "# time  frame  fps  worst_ms  game%%  vblank_hz | busy%% of each thread: gfx(vif/vu1) gs(render) | "
                        "game waiting on gfx%% | gfx waiting on gs%% | disc MB/s, disc ms | vu1 Mpairs/s | dma irqs/s, avg delay us, total delay %%\n");
    }
    static double t = 0;
    t += seconds;
    uint64_t v[kPerfCount];
    for (int i = 0; i < kPerfCount; ++i) v[i] = g_perf[i].exchange(0, std::memory_order_relaxed);
    const double ns = seconds * 1e9;
    const double worst = (double)g_max_gap.exchange(0, std::memory_order_relaxed) / 1e6;
    const double game_pct = 100.0 * game_ms / (seconds * 1000.0);
    const bool slow = fps < 45 || worst > 80;
    std::fprintf(f, "%6.1f %6llu %5.1f %7.1f %5.0f %5.1f | gfx %3.0f gs %3.0f | wait-gfx %3.0f | gfx-wait-gs %3.0f | disc %5.2f %4.0f | vu1 %6.1f | dma %5.0f %6.0f %3.0f%s",
                 t, (unsigned long long)frame, fps, worst, game_pct, vblank_hz, 100.0 * v[kPerfGfxBusy] / ns,
                 100.0 * v[kPerfGsBusy] / ns, 100.0 * v[kPerfGameWaitGfx] / ns, 100.0 * v[kPerfGfxWaitGs] / ns,
                 v[kPerfDiscBytes] / 1e6 / seconds, v[kPerfDiscNs] / 1e6, v[kPerfVu1Pairs] / 1e6 / seconds,
                 v[kPerfDmaDone] / seconds, v[kPerfDmaDone] ? v[kPerfDmaLatency] / 1e3 / v[kPerfDmaDone] : 0.0,
                 100.0 * v[kPerfDmaLatency] / ns, slow ? "  SLOW" : "");
    static double last_shot = -100;
    if (slow && shots && t - last_shot >= 5) {
        char name[64];
        std::snprintf(name, sizeof name, "/slow_%06llu.png", (unsigned long long)frame);
        display_request_shot((dir + name).c_str());
        std::fprintf(f, "  shot slow_%06llu.png", (unsigned long long)frame);
        last_shot = t;
    }
    static double last_prof = -100;
    if (slow && t - last_prof >= 20) {   // where the busiest thread spends its time
        const double gfx = (double)v[kPerfGfxBusy], gs = (double)v[kPerfGsBusy];
        const double game = ns - (double)v[kPerfGameWaitGfx];
        static int turn = 0;   // gfx and gs are usually both busy: take turns
        const char* who = game > gfx && game > gs ? "game" : (turn++ % 2 ? "gs" : "gfx");
        char tag[64];
        std::snprintf(tag, sizeof tag, "/profile_%06llu_%s.txt", (unsigned long long)frame, who);
        if (profiler_snapshot(who, 3.0, (dir + tag).c_str())) {
            std::fprintf(f, "  profiling %s", who);
            last_prof = t;
        }
    }
    std::fprintf(f, "\n");
    const std::string why = gpu::hw::take_download_reasons();   // GPU -> GS memory read-backs this second
    if (slow && !why.empty()) std::fprintf(f, "  read-backs:\n%s", why.c_str());
    const std::string sync = take_sync_reasons();   // register reads that waited for the gfx thread
    if (slow && !sync.empty()) std::fprintf(f, "  game waited for gfx on:\n%s", sync.c_str());
    std::fflush(f);
}

} // namespace ps2
