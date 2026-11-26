// Performance counters for the TWONK_PERF_LOG monitor (perf.cpp).
#pragma once
#include <atomic>
#include <cstdint>

namespace ps2 {

enum PerfCounter {
    kPerfGfxBusy,      // ns the gfx thread (VIF1/VU1/PATH3) spent working
    kPerfGsBusy,       // ns the GS thread spent rendering
    kPerfGameWaitGfx,  // ns the game thread waited for the gfx thread
    kPerfGfxWaitGs,    // ns the gfx thread waited for room in the GS queue
    kPerfDiscBytes,    // bytes read from the disc image
    kPerfDiscNs,       // ns spent reading it
    kPerfVu1Pairs,     // VU1 instruction pairs run
    kPerfDmaLatency,   // ns from a gfx DMA finishing to the game thread raising its interrupt
    kPerfDmaDone,      // gfx DMA completions delivered
    kPerfCount
};

extern std::atomic<uint64_t> g_perf[kPerfCount];
inline void perf_add(PerfCounter c, uint64_t n) { g_perf[c].fetch_add(n, std::memory_order_relaxed); }
uint64_t perf_now();   // steady clock, ns
bool perf_enabled();
void perf_flip();      // a game frame was shown (DISPFB changed)
// Once a second from the vblank: write the line for the last `seconds`.
void perf_second(double fps, double vblank_hz, double seconds, uint64_t frame, uint32_t game_ms);

// Times a scope into a counter when the monitor is on.
struct PerfScope {
    PerfCounter c;
    uint64_t t0;
    explicit PerfScope(PerfCounter counter) : c(counter), t0(perf_enabled() ? perf_now() : 0) {}
    ~PerfScope() { if (t0) perf_add(c, perf_now() - t0); }
};

} // namespace ps2
