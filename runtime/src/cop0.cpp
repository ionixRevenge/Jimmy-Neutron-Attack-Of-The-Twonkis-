// COP0 system control registers.
#include <chrono>

#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2 {

namespace {
// EE core clock: 294.912 MHz. Count increments once per cycle.
uint32_t cycle_count() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    const auto ns = duration_cast<nanoseconds>(steady_clock::now() - t0).count();
    return (uint32_t)((uint64_t)ns * 294912ull / 1000000ull);
}
} // namespace

uint32_t cop0_read(Context* ctx, unsigned reg) {
    switch (reg) {
    case 9:  return cycle_count();          // Count
    case 15: return 0x00002E20u;            // PRid: R5900 rev 2.0
    case 16: return 0x73003u;               // Config (caches enabled; value as seen on hardware)
    default: return ctx->cop0[reg];
    }
}

void cop0_write(Context* ctx, unsigned reg, uint32_t value) {
    log(Log::Cpu, "mtc0 $%u <- %08x", reg, value);
    ctx->cop0[reg] = value;
}

} // namespace ps2
