// IRX loader, R3000 interpreter and the IOP kernel services one module needs.
// See iop_irx.hpp.
#include "iop_irx.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "disc.hpp"
#include "iop.hpp"
#include "ps2/runtime.hpp"
#include "spu2.hpp"

namespace ps2::iop::irx {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagicRet = 0x1FFFFFF0u;   // a call returning here is finished

// ---- module image ---------------------------------------------------------------------
struct Module {
    std::string name;
    uint32_t base = 0, size = 0, gp = 0;
    uint32_t stub_lo = 0, stub_hi = 0;   // import stubs: [lo, hi)
    std::unordered_map<uint32_t, std::string> imports;
    std::map<std::string, uint32_t> symbols;
    bool loaded = false;
} M;

uint8_t* g_ram = nullptr;
Context* g_ctx = nullptr;

inline uint32_t phys(uint32_t a) { return a & 0x1FFFFFFFu; }
inline bool in_ram(uint32_t a, uint32_t n) { return (uint64_t)phys(a) + n <= kRamSize; }

void bad_access(uint32_t a, const char* what) {
    static unsigned n;
    if (n++ < 16) std::fprintf(stderr, "[irx] %s: %s outside IOP RAM at %08x\n", M.name.c_str(), what, a);
}
inline uint32_t r32(uint32_t a) { if (!in_ram(a, 4)) { bad_access(a, "read"); return 0; } uint32_t v; std::memcpy(&v, g_ram + phys(a), 4); return v; }
inline uint16_t r16(uint32_t a) { if (!in_ram(a, 2)) { bad_access(a, "read"); return 0; } uint16_t v; std::memcpy(&v, g_ram + phys(a), 2); return v; }
inline uint8_t r8(uint32_t a) { if (!in_ram(a, 1)) { bad_access(a, "read"); return 0; } return g_ram[phys(a)]; }
inline void w32(uint32_t a, uint32_t v) { if (!in_ram(a, 4)) { bad_access(a, "write"); return; } std::memcpy(g_ram + phys(a), &v, 4); }
inline void w16(uint32_t a, uint16_t v) { if (!in_ram(a, 2)) { bad_access(a, "write"); return; } std::memcpy(g_ram + phys(a), &v, 2); }
inline void w8(uint32_t a, uint8_t v) { if (!in_ram(a, 1)) { bad_access(a, "write"); return; } g_ram[phys(a)] = v; }
std::string rstr(uint32_t a, size_t max = 256) {
    std::string s;
    for (size_t i = 0; i < max; ++i) { const uint8_t c = r8(a + (uint32_t)i); if (!c) break; s += (char)c; }
    return s;
}

// ---- CPU -------------------------------------------------------------------------------
struct Cpu { uint32_t r[32] = {}, hi = 0, lo = 0, pc = 0; };

// ---- kernel objects --------------------------------------------------------------------------
enum class Wait { None, Event, Sema, Delay, Forever };
struct Thread {
    int id = 0;
    Cpu cpu;
    int prio = 64;
    bool started = false, done = false;
    Wait wait = Wait::None;
    int obj = 0;
    uint32_t bits = 0, mode = 0, res_ptr = 0;
    Clock::time_point wake;
    uint64_t seq = 0;            // round robin within a priority: lowest runs first
    uint32_t entry = 0, stack_size = 0, stack = 0;
};
struct EventFlag { uint32_t bits = 0; };
struct Sema { int count = 0, max = 1; };

std::vector<std::unique_ptr<Thread>> g_threads;   // index = id - 1
std::vector<EventFlag> g_events;
std::vector<Sema> g_semas;
Thread* g_cur = nullptr;
bool g_in_irq = false;
uint64_t g_seq = 0;
uint64_t g_insns = 0;

struct Irq { uint32_t func, a0, a1; };
std::mutex g_irq_mutex;
std::vector<Irq> g_irqs;
std::atomic<bool> g_irq_pending{false};   // g_irqs is not empty (read without the lock)
// A running thread gives way to a pending interrupt callback only once it is this many
// instructions old (about 0.5 ms of IOP time). Real completions come that late at the
// earliest: HIS starts a CD read and then sets up its wait, and a callback run right
// after sceCdRead returns would land before that wait and be lost (the game then
// waits for the read forever). With every thread blocked it is delivered at once.
constexpr uint64_t kIrqLatency = 20000;
std::atomic<uint64_t> g_irq_due{~0ull};   // g_insns at which the oldest pending one may preempt
const bool g_irq_preempt = [] { char b[8]; size_t n = 0; return getenv_s(&n, b, sizeof b, "TWONK_IRX_PREEMPT") || !n || b[0] != '0'; }();

// TWONK_IRX_PROF=1: where the module spends its instructions (one sample per 1024),
// printed every 10 s with the nearest symbol.
const bool g_irx_prof = [] { char b[8]; size_t n = 0; return !getenv_s(&n, b, sizeof b, "TWONK_IRX_PROF") && n && b[0] == '1'; }();
std::unordered_map<uint32_t, uint64_t> g_pc_samples;
void irx_prof_report(uint32_t pc);
uint32_t g_cd_callback = 0;
uint32_t g_rpc_func = 0, g_rpc_buf = 0;

Thread* thread(int id) { return id >= 1 && id <= (int)g_threads.size() ? g_threads[id - 1].get() : nullptr; }

// Can a waiting thread go on? Applies the wait's side effects when it can.
bool try_wake(Thread& t) {
    switch (t.wait) {
    case Wait::None: return true;   // preempted or yielded: its registers stand as they are
    case Wait::Forever: return false;
    case Wait::Delay: if (Clock::now() < t.wake) return false; break;
    case Wait::Sema: {
        Sema& s = g_semas[t.obj];
        if (s.count <= 0) return false;
        --s.count;
        break;
    }
    case Wait::Event: {
        EventFlag& e = g_events[t.obj];
        const bool ok = (t.mode & 1) ? (e.bits & t.bits) != 0 : (e.bits & t.bits) == t.bits;
        if (!ok) return false;
        if (t.res_ptr) w32(t.res_ptr, e.bits);
        if (t.mode & 0x20) e.bits = 0;              // WEF_CLEARALL
        else if (t.mode & 0x10) e.bits &= ~t.bits;   // WEF_CLEAR
        break;
    }
    }
    t.wait = Wait::None;
    t.cpu.r[2] = 0;
    return true;
}

// ---- imports ---------------------------------------------------------------------------------
// Each returns v0. A blocking call sets g_cur->wait; the thread resumes after the call.
uint32_t arg(const Cpu& c, int i) { return i < 4 ? c.r[4 + i] : r32(c.r[29] + 16 + 4 * (i - 4)); }

// printf-style formatting from IOP memory and the call's arguments (from argument `first`).
std::string format(const Cpu& c, uint32_t fmt, int first) {
    const std::string f = rstr(fmt, 512);
    std::string out;
    int ai = first;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') { out += f[i]; continue; }
        std::string spec = "%";
        while (++i < f.size() && std::strchr("-+ #0123456789.lh", f[i])) if (f[i] != 'l' && f[i] != 'h') spec += f[i];
        if (i >= f.size()) break;
        const char conv = f[i];
        char buf[512];
        if (conv == '%') { out += '%'; continue; }
        if (conv == 's') { spec += 's'; std::snprintf(buf, sizeof buf, spec.c_str(), rstr(arg(c, ai++)).c_str()); }
        else if (conv == 'c') { spec += 'c'; std::snprintf(buf, sizeof buf, spec.c_str(), (int)arg(c, ai++)); }
        else if (strchr("diuxXop", conv)) { spec += conv == 'p' ? 'x' : conv; std::snprintf(buf, sizeof buf, spec.c_str(), arg(c, ai++)); }
        else { buf[0] = conv; buf[1] = 0; }
        out += buf;
    }
    return out;
}

void block(Wait w) { if (g_cur && !g_in_irq) { g_cur->wait = w; g_cur->seq = ++g_seq; } }

uint32_t imp_call(Cpu& c, const std::string& n) {
    // ---- threads ----
    if (n == "CreateThread") {
        const uint32_t p = c.r[4];
        auto t = std::make_unique<Thread>();
        t->id = (int)g_threads.size() + 1;
        t->entry = r32(p + 8); t->stack_size = r32(p + 12); t->prio = (int)r32(p + 16);
        g_threads.push_back(std::move(t));
        log(Log::Iop, "[irx] CreateThread entry %05x prio %d stack %x -> %d", g_threads.back()->entry - M.base,
            g_threads.back()->prio, g_threads.back()->stack_size, (int)g_threads.size());
        return (uint32_t)g_threads.size();
    }
    if (n == "StartThread") {
        Thread* t = thread((int)c.r[4]);
        if (!t) return (uint32_t)-1;
        t->stack = alloc(std::max(t->stack_size, 0x800u), 16);
        t->cpu = Cpu{};
        t->cpu.r[4] = c.r[5];
        t->cpu.r[28] = M.gp;
        t->cpu.r[29] = t->stack + std::max(t->stack_size, 0x800u) - 16;
        t->cpu.r[31] = kMagicRet;
        t->cpu.pc = t->entry;
        t->started = true;
        t->seq = ++g_seq;
        return 0;
    }
    if (n == "GetThreadId") return g_cur ? (uint32_t)g_cur->id : 1u;
    if (n == "RotateThreadReadyQueue") { block(Wait::None); return 0; }
    if (n == "DelayThread") {
        if (g_cur && !g_in_irq) { g_cur->wake = Clock::now() + std::chrono::microseconds(c.r[4]); block(Wait::Delay); }
        return 0;
    }
    // ---- event flags ----
    if (n == "CreateEventFlag") { g_events.push_back({r32(c.r[4] + 8)}); return (uint32_t)g_events.size() - 1 + 1; }
    if (n == "SetEventFlag" || n == "iSetEventFlag") {
        if (c.r[4] - 1 < g_events.size()) g_events[c.r[4] - 1].bits |= c.r[5];
        return 0;
    }
    if (n == "ClearEventFlag") { if (c.r[4] - 1 < g_events.size()) g_events[c.r[4] - 1].bits &= c.r[5]; return 0; }
    if (n == "WaitEventFlag") {
        if (c.r[4] - 1 >= g_events.size() || !g_cur || g_in_irq) return (uint32_t)-1;
        g_cur->obj = (int)c.r[4] - 1; g_cur->bits = c.r[5]; g_cur->mode = c.r[6]; g_cur->res_ptr = c.r[7];
        g_cur->wait = Wait::Event;
        if (try_wake(*g_cur)) return 0;
        g_cur->wait = Wait::Event;
        g_cur->seq = ++g_seq;
        return 0;
    }
    // ---- semaphores ----
    if (n == "CreateSema") {
        Sema s; s.count = (int)r32(c.r[4] + 8); s.max = std::max(1, (int)r32(c.r[4] + 12));
        g_semas.push_back(s);
        return (uint32_t)g_semas.size();
    }
    if (n == "SignalSema") {
        if (c.r[4] - 1 < g_semas.size()) { Sema& s = g_semas[c.r[4] - 1]; s.count = std::min(s.count + 1, s.max); }
        return 0;
    }
    if (n == "WaitSema") {
        if (c.r[4] - 1 >= g_semas.size()) return (uint32_t)-1;
        Sema& s = g_semas[c.r[4] - 1];
        if (s.count > 0) { --s.count; return 0; }
        if (g_cur && !g_in_irq) { g_cur->obj = (int)c.r[4] - 1; block(Wait::Sema); }
        return 0;
    }
    // ---- memory, interrupts ----
    if (n == "AllocSysMemory") { const uint32_t a = alloc(c.r[5], 64); std::memset(g_ram + a, 0, c.r[5]); return a; }
    if (n == "FreeSysMemory") return 0;
    if (n == "CpuSuspendIntr") { if (c.r[4]) w32(c.r[4], 0); return 0; }
    if (n == "CpuResumeIntr" || n == "FlushDcache") return 0;
    // ---- SIF ----
    if (n == "sceSifCheckInit") return 1;
    if (n == "sceSifInit" || n == "sceSifInitRpc" || n == "sceSifSetRpcQueue") return 0;
    if (n == "sceSifRegisterRpc") { g_rpc_func = c.r[6]; g_rpc_buf = c.r[7]; return 0; }
    if (n == "sceSifRpcLoop") { block(Wait::Forever); return 0; }
    if (n == "sceSifSetDmaIntr") {   // { src (IOP), dest (EE), size, attr } x count
        for (uint32_t i = 0; i < c.r[5]; ++i) {
            const uint32_t d = c.r[4] + i * 16, src = r32(d), dst = r32(d + 4), size = r32(d + 8);
            if (size && in_ram(src, size)) std::memcpy(guest_ptr(g_ctx, dst & 0x1FFFFFFFu, size), g_ram + phys(src), size);
        }
        if (c.r[6]) raise(c.r[6], c.r[7], 0);
        return 1;
    }
    if (n == "sceSifGetOtherData") {   // (rd, src EE, dest IOP, size, mode)
        const uint32_t src = c.r[5], dst = c.r[6], size = c.r[7];
        if (size && in_ram(dst, size)) std::memcpy(g_ram + phys(dst), guest_ptr(g_ctx, src & 0x1FFFFFFFu, size), size);
        return 0;
    }
    // ---- CD ----
    if (n == "sceCdInit" || n == "sceCdMmode") return 1;
    if (n == "sceCdDiskReady") return 2;   // SCECdComplete
    if (n == "sceCdGetError" || n == "sceCdSync") return 0;
    if (n == "sceCdCallback") { const uint32_t old = g_cd_callback; g_cd_callback = c.r[4]; return old; }
    if (n == "sceCdRead") {
        const uint32_t lsn = c.r[4], count = c.r[5], buf = c.r[6];
        if (count && in_ram(buf, count * 2048)) disc::read(lsn, count, g_ram + phys(buf));
        else bad_access(buf, "sceCdRead");
        log(Log::Iop, "[irx] cd read lsn %u x%u -> %06x", lsn, count, buf);
        if (g_cd_callback) raise(g_cd_callback, 1, 0);   // SCECdFuncRead
        return 1;
    }
    // ---- host files (only for the host: media type) ----
    if (n == "open" || n == "close" || n == "read" || n == "lseek") return (uint32_t)-1;
    // ---- C library ----
    if (n == "printf") { if (log_enabled(Log::Iop)) std::fprintf(stderr, "[irx] %s", format(c, c.r[4], 1).c_str()); return 0; }
    if (n == "sprintf") {
        const std::string s = format(c, c.r[5], 2);
        for (size_t i = 0; i <= s.size(); ++i) w8(c.r[4] + (uint32_t)i, i < s.size() ? (uint8_t)s[i] : 0);
        return (uint32_t)s.size();
    }
    if (n == "toupper") return (uint32_t)std::toupper((int)(c.r[4] & 0xFF));
    if (n == "strlen") return (uint32_t)rstr(c.r[4], 0x10000).size();
    if (n == "strcpy" || n == "strcat") {
        const std::string s = rstr(c.r[5], 0x10000);
        const uint32_t d = c.r[4] + (n == "strcat" ? (uint32_t)rstr(c.r[4], 0x10000).size() : 0u);
        for (size_t i = 0; i <= s.size(); ++i) w8(d + (uint32_t)i, i < s.size() ? (uint8_t)s[i] : 0);
        return c.r[4];
    }
    if (n == "memset") { if (in_ram(c.r[4], c.r[6])) std::memset(g_ram + phys(c.r[4]), (int)(c.r[5] & 0xFF), c.r[6]); return c.r[4]; }
    if (n == "memcmp") {
        if (!in_ram(c.r[4], c.r[6]) || !in_ram(c.r[5], c.r[6])) return 0;
        const int r = std::memcmp(g_ram + phys(c.r[4]), g_ram + phys(c.r[5]), c.r[6]);
        return (uint32_t)(r < 0 ? -1 : r > 0 ? 1 : 0);
    }
    // ---- libsd ----
    if (n == "sceSdInit") { spu2::sd_init((int)c.r[4]); return 0; }
    if (n == "sceSdSetParam") { spu2::set_param((uint16_t)c.r[4], (uint16_t)c.r[5]); return 0; }
    if (n == "sceSdGetParam") return spu2::get_param((uint16_t)c.r[4]);
    if (n == "sceSdSetSwitch") { spu2::set_switch((uint16_t)c.r[4], c.r[5]); return 0; }
    if (n == "sceSdGetSwitch") return spu2::get_switch((uint16_t)c.r[4]);
    if (n == "sceSdSetAddr") { spu2::set_addr((uint16_t)c.r[4], c.r[5]); return 0; }
    if (n == "sceSdGetAddr") return spu2::get_addr((uint16_t)c.r[4]);
    if (n == "sceSdVoiceTrans") return (uint32_t)spu2::voice_trans((int16_t)c.r[4], (uint16_t)c.r[5], c.r[6], c.r[7], arg(c, 4));
    if (n == "sceSdBlockTrans") return (uint32_t)spu2::block_trans((int16_t)c.r[4], (uint16_t)c.r[5], c.r[6], c.r[7], arg(c, 4));
    if (n == "sceSdBlockTransStatus") return spu2::block_trans_status((int16_t)c.r[4], (int16_t)c.r[5]);
    if (n == "sceSdSetTransIntrHandler") { spu2::set_trans_handler((int)c.r[4], c.r[5], c.r[6]); return 0; }

    static std::map<std::string, int> warned;
    if (warned[n]++ == 0) std::fprintf(stderr, "[irx] %s: unhandled import %s\n", M.name.c_str(), n.c_str());
    return 0;
}

void irx_prof_report(uint32_t at) {
    ++g_pc_samples[at];
    static auto last = Clock::now();
    if (Clock::now() - last < std::chrono::seconds(10)) return;
    last = Clock::now();
    std::vector<std::pair<uint64_t, uint32_t>> v;
    uint64_t total = 0;
    for (auto& [pc, k] : g_pc_samples) { v.push_back({k, pc}); total += k; }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "[irx] %s: %llu instructions in all; last 10 s, by 1024-instruction samples:\n", M.name.c_str(),
                 (unsigned long long)g_insns);
    for (size_t i = 0; i < v.size() && i < 24; ++i) {
        const uint32_t pc = v[i].second;
        std::string best;
        uint32_t best_at = 0;
        for (auto& [name, at] : M.symbols)
            if (at <= pc && at >= best_at && pc - at < 0x10000) { best = name; best_at = at; }
        std::fprintf(stderr, "[irx]   %5.1f%%  %05x  %s+%x\n", 100.0 * (double)v[i].first / (double)total, pc - M.base, best.c_str(), pc - best_at);
    }
    g_pc_samples.clear();
}

// ---- interpreter ---------------------------------------------------------------------------------
// Preempt: an interrupt callback is pending; the thread stopped between instructions
// and goes on once it has run (on the IOP it would run at once).
enum class Stop { Budget, Blocked, Returned, Fault, Preempt };

inline int32_t sx16(uint32_t v) { return (int32_t)(int16_t)(v & 0xFFFF); }

// Runs `c` for up to `budget` instructions.
Stop run(Cpu& c, uint64_t budget) {
    uint32_t* R = c.r;
    for (uint64_t n = 0; n < budget; ++n) {
        if (c.pc == kMagicRet) return Stop::Returned;
        // (a budget-1 run is a delay slot: n is 0 there, so it is never cut short)
        if (n && (n & 4095) == 0 && g_cur && !g_in_irq && g_irq_preempt && g_irq_pending.load(std::memory_order_relaxed) &&
            g_insns >= g_irq_due.load(std::memory_order_relaxed))
            return Stop::Preempt;
        if (c.pc >= M.stub_lo && c.pc < M.stub_hi) {   // an import
            auto it = M.imports.find(c.pc);
            const uint32_t v0 = it == M.imports.end() ? 0u : imp_call(c, it->second);
            R[2] = v0;
            R[0] = 0;
            c.pc = R[31];
            if (g_cur && !g_in_irq && g_cur->wait != Wait::None) return Stop::Blocked;
            if (g_cur && !g_in_irq && it != M.imports.end() && it->second == "RotateThreadReadyQueue") return Stop::Blocked;
            continue;
        }
        if (!in_ram(c.pc, 4)) {
            std::fprintf(stderr, "[irx] %s: execution left IOP RAM (pc %08x, ra %08x)\n", M.name.c_str(), c.pc, R[31]);
            return Stop::Fault;
        }
        const uint32_t pc = c.pc;
        const uint32_t in = r32(pc);
        if ((++g_insns & 1023) == 0 && g_irx_prof) irx_prof_report(pc);
        const uint32_t op = in >> 26, rs = (in >> 21) & 31, rt = (in >> 16) & 31, rd = (in >> 11) & 31, sa = (in >> 6) & 31;
        const uint32_t imm = in & 0xFFFF;
        bool branch = false, taken = false;
        uint32_t target = 0;
        switch (op) {
        case 0x00:   // SPECIAL
            switch (in & 0x3F) {
            case 0x00: R[rd] = R[rt] << sa; break;
            case 0x02: R[rd] = R[rt] >> sa; break;
            case 0x03: R[rd] = (uint32_t)((int32_t)R[rt] >> sa); break;
            case 0x04: R[rd] = R[rt] << (R[rs] & 31); break;
            case 0x06: R[rd] = R[rt] >> (R[rs] & 31); break;
            case 0x07: R[rd] = (uint32_t)((int32_t)R[rt] >> (R[rs] & 31)); break;
            case 0x08: branch = taken = true; target = R[rs]; break;                        // JR
            case 0x09: branch = taken = true; target = R[rs]; R[rd ? rd : 31] = pc + 8; break;   // JALR
            case 0x0C: case 0x0D:
                std::fprintf(stderr, "[irx] %s: %s at %05x\n", M.name.c_str(), (in & 1) ? "break" : "syscall", pc - M.base);
                break;
            case 0x10: R[rd] = c.hi; break;
            case 0x11: c.hi = R[rs]; break;
            case 0x12: R[rd] = c.lo; break;
            case 0x13: c.lo = R[rs]; break;
            case 0x18: { const int64_t p = (int64_t)(int32_t)R[rs] * (int32_t)R[rt]; c.lo = (uint32_t)p; c.hi = (uint32_t)(p >> 32); break; }
            case 0x19: { const uint64_t p = (uint64_t)R[rs] * R[rt]; c.lo = (uint32_t)p; c.hi = (uint32_t)(p >> 32); break; }
            case 0x1A: {
                const int32_t a = (int32_t)R[rs], b = (int32_t)R[rt];
                if (b == 0) { c.lo = a < 0 ? 1u : 0xFFFFFFFFu; c.hi = (uint32_t)a; }
                else if (a == INT32_MIN && b == -1) { c.lo = (uint32_t)a; c.hi = 0; }
                else { c.lo = (uint32_t)(a / b); c.hi = (uint32_t)(a % b); }
                break;
            }
            case 0x1B:
                if (R[rt] == 0) { c.lo = 0xFFFFFFFFu; c.hi = R[rs]; }
                else { c.lo = R[rs] / R[rt]; c.hi = R[rs] % R[rt]; }
                break;
            case 0x20: case 0x21: R[rd] = R[rs] + R[rt]; break;
            case 0x22: case 0x23: R[rd] = R[rs] - R[rt]; break;
            case 0x24: R[rd] = R[rs] & R[rt]; break;
            case 0x25: R[rd] = R[rs] | R[rt]; break;
            case 0x26: R[rd] = R[rs] ^ R[rt]; break;
            case 0x27: R[rd] = ~(R[rs] | R[rt]); break;
            case 0x2A: R[rd] = (int32_t)R[rs] < (int32_t)R[rt]; break;
            case 0x2B: R[rd] = R[rs] < R[rt]; break;
            default: std::fprintf(stderr, "[irx] unknown SPECIAL %08x at %05x\n", in, pc - M.base); return Stop::Fault;
            }
            break;
        case 0x01: {   // REGIMM
            const bool ge = (rt & 1) != 0;
            taken = ge ? (int32_t)R[rs] >= 0 : (int32_t)R[rs] < 0;
            if (rt & 0x10) R[31] = pc + 8;   // BLTZAL/BGEZAL
            branch = true; target = pc + 4 + (uint32_t)(sx16(imm) << 2);
            break;
        }
        case 0x02: case 0x03:
            branch = taken = true;
            target = ((pc + 4) & 0xF0000000u) | ((in & 0x3FFFFFF) << 2);
            if (op == 3) R[31] = pc + 8;
            break;
        case 0x04: branch = true; taken = R[rs] == R[rt]; target = pc + 4 + (uint32_t)(sx16(imm) << 2); break;
        case 0x05: branch = true; taken = R[rs] != R[rt]; target = pc + 4 + (uint32_t)(sx16(imm) << 2); break;
        case 0x06: branch = true; taken = (int32_t)R[rs] <= 0; target = pc + 4 + (uint32_t)(sx16(imm) << 2); break;
        case 0x07: branch = true; taken = (int32_t)R[rs] > 0; target = pc + 4 + (uint32_t)(sx16(imm) << 2); break;
        case 0x08: case 0x09: R[rt] = R[rs] + (uint32_t)sx16(imm); break;
        case 0x0A: R[rt] = (int32_t)R[rs] < sx16(imm); break;
        case 0x0B: R[rt] = R[rs] < (uint32_t)sx16(imm); break;
        case 0x0C: R[rt] = R[rs] & imm; break;
        case 0x0D: R[rt] = R[rs] | imm; break;
        case 0x0E: R[rt] = R[rs] ^ imm; break;
        case 0x0F: R[rt] = imm << 16; break;
        case 0x10: break;   // COP0 (mfc0/mtc0/rfe): interrupt state is not modelled
        case 0x20: R[rt] = (uint32_t)(int8_t)r8(R[rs] + sx16(imm)); break;
        case 0x21: R[rt] = (uint32_t)(int16_t)r16(R[rs] + sx16(imm)); break;
        case 0x22: {   // LWL
            const uint32_t a = R[rs] + sx16(imm), sh = (a & 3) * 8;
            const uint32_t w = r32(a & ~3u);
            R[rt] = (R[rt] & (0x00FFFFFFu >> sh)) | (w << (24 - sh));
            break;
        }
        case 0x23: R[rt] = r32(R[rs] + sx16(imm)); break;
        case 0x24: R[rt] = r8(R[rs] + sx16(imm)); break;
        case 0x25: R[rt] = r16(R[rs] + sx16(imm)); break;
        case 0x26: {   // LWR
            const uint32_t a = R[rs] + sx16(imm), sh = (a & 3) * 8;
            const uint32_t w = r32(a & ~3u);
            R[rt] = (R[rt] & ~(0xFFFFFFFFu >> sh)) | (w >> sh);
            break;
        }
        case 0x28: w8(R[rs] + sx16(imm), (uint8_t)R[rt]); break;
        case 0x29: w16(R[rs] + sx16(imm), (uint16_t)R[rt]); break;
        case 0x2A: {   // SWL
            const uint32_t a = R[rs] + sx16(imm), sh = (a & 3) * 8;
            const uint32_t w = r32(a & ~3u);
            w32(a & ~3u, (w & ~(0xFFFFFFFFu >> (24 - sh))) | (R[rt] >> (24 - sh)));
            break;
        }
        case 0x2B: w32(R[rs] + sx16(imm), R[rt]); break;
        case 0x2E: {   // SWR
            const uint32_t a = R[rs] + sx16(imm), sh = (a & 3) * 8;
            const uint32_t w = r32(a & ~3u);
            w32(a & ~3u, (w & ~(0xFFFFFFFFu << sh)) | (R[rt] << sh));
            break;
        }
        default:
            std::fprintf(stderr, "[irx] %s: unknown instruction %08x at %05x\n", M.name.c_str(), in, pc - M.base);
            return Stop::Fault;
        }
        R[0] = 0;
        if (!branch) { c.pc = pc + 4; continue; }
        // The delay slot (plain ALU/memory work in compiled code) runs as one step.
        c.pc = pc + 4;
        if (run(c, 1) == Stop::Fault) return Stop::Fault;
        c.pc = taken ? target : pc + 8;
    }
    return Stop::Budget;
}

// ---- scheduling -------------------------------------------------------------------------------------
Cpu g_irq_cpu;
uint32_t g_irq_stack = 0;

void run_irq(const Irq& q) {
    if (!g_irq_stack) g_irq_stack = alloc(0x2000, 16);
    g_irq_cpu = Cpu{};
    g_irq_cpu.r[4] = q.a0;
    g_irq_cpu.r[5] = q.a1;
    g_irq_cpu.r[28] = M.gp;
    g_irq_cpu.r[29] = g_irq_stack + 0x2000 - 16;
    g_irq_cpu.r[31] = kMagicRet;
    g_irq_cpu.pc = q.func;
    const bool was = g_in_irq;
    g_in_irq = true;
    if (run(g_irq_cpu, 1000000) != Stop::Returned)
        std::fprintf(stderr, "[irx] %s: interrupt handler %05x did not return\n", M.name.c_str(), q.func - M.base);
    g_in_irq = was;
}

void deliver_irqs() {
    for (int round = 0; round < 8; ++round) {
        std::vector<Irq> q;
        {
            std::lock_guard<std::mutex> lock(g_irq_mutex);
            q.swap(g_irqs);
            g_irq_pending.store(false, std::memory_order_relaxed);
            g_irq_due.store(~0ull, std::memory_order_relaxed);
        }
        if (q.empty()) return;
        for (const Irq& i : q) run_irq(i);
    }
}

// Runs ready threads (highest priority first) until none is ready or `slices` ran.
// Returns true if any thread ran.
bool schedule(int slices) {
    bool any = false;
    int preempts = 0;
    for (int s = 0; s < slices; ++s) {
        deliver_irqs();
        Thread* best = nullptr;
        for (auto& tp : g_threads) {
            Thread& t = *tp;
            if (!t.started || t.done) continue;
            if (t.wait != Wait::None && !try_wake(t)) continue;
            if (!best || t.prio < best->prio || (t.prio == best->prio && t.seq < best->seq)) best = &t;
        }
        if (!best) return any;
        any = true;
        g_cur = best;
        const Stop r = run(best->cpu, 2000000);
        g_cur = nullptr;
        if (r == Stop::Preempt) {   // deliver the interrupt and go on: not a slice, not a yield
            if (++preempts <= 4096) --s;
            continue;
        }
        best->seq = ++g_seq;
        if (r == Stop::Returned || r == Stop::Fault) best->done = true;
    }
    return any;
}

Thread* g_call = nullptr;

} // namespace

bool load(const std::vector<uint8_t>& b, const char* name) {
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &b[o], 2); return v; };
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &b[o], 4); return v; };
    if (b.size() < 0x34 || std::memcmp(b.data(), "\x7f" "ELF", 4) != 0 || u16(16) != 0xFF80) return false;
    M = Module{};
    M.name = name;
    g_ram = ram(0, kRamSize);
    // the one PT_LOAD segment holds .text/.data; memsz adds .bss
    const uint32_t phoff = u32(0x1C), phnum = u16(0x2C);
    uint32_t off = 0, filesz = 0, memsz = 0;
    for (uint32_t i = 0; i < phnum; ++i)
        if (u32(phoff + i * 32) == 1) { off = u32(phoff + i * 32 + 4); filesz = u32(phoff + i * 32 + 16); memsz = u32(phoff + i * 32 + 20); }
    if (!memsz || (size_t)off + filesz > b.size()) return false;
    M.size = memsz;
    M.base = alloc(memsz + 0x100, 0x10000);   // 64 KB aligned: LO16 relocations need no carry
    std::memcpy(g_ram + M.base, &b[off], filesz);
    std::memset(g_ram + M.base + filesz, 0, memsz - filesz);
    // sections: relocations and symbols
    const uint32_t shoff = u32(0x20), shnum = u16(0x30);
    uint32_t symtab = 0, symsize = 0, strtab = 0;
    for (uint32_t i = 0; i < shnum; ++i) {
        const size_t sh = shoff + i * 40;
        const uint32_t type = u32(sh + 4), soff = u32(sh + 16), ssize = u32(sh + 20), link = u32(sh + 24);
        if (type == 2) { symtab = soff; symsize = ssize; strtab = u32(shoff + link * 40 + 16); }
        if (type != 9) continue;   // SHT_REL
        for (uint32_t r = 0; r < ssize / 8; ++r) {
            const uint32_t at = u32(soff + r * 8), info = u32(soff + r * 8 + 4);
            uint32_t w = r32(M.base + at);
            switch (info & 0xFF) {
            case 2: w += M.base; break;                                                    // R_MIPS_32
            case 4: w = (w & 0xFC000000u) | ((w + (M.base >> 2)) & 0x03FFFFFFu); break;     // R_MIPS_26
            case 5: w = (w & 0xFFFF0000u) | ((w + (M.base >> 16)) & 0xFFFFu); break;         // R_MIPS_HI16
            case 6: break;                                                                 // R_MIPS_LO16 (base is 64 KB aligned)
            default: std::fprintf(stderr, "[irx] %s: relocation type %u not supported\n", name, info & 0xFF); return false;
            }
            w32(M.base + at, w);
        }
    }
    // symbols; imports are the untyped symbols between the first *_stub and _etext
    M.stub_lo = ~0u;
    std::vector<std::pair<uint32_t, std::string>> untyped;
    for (uint32_t i = 0; symtab && i < symsize / 16; ++i) {
        const size_t s = symtab + i * 16;
        const std::string n(reinterpret_cast<const char*>(&b[strtab + u32(s)]));
        const uint32_t val = u32(s + 4), type = b[s + 12] & 15;
        if (n.empty()) continue;
        M.symbols[n] = M.base + val;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, "_stub") == 0) M.stub_lo = std::min(M.stub_lo, M.base + val);
        if (type == 0) untyped.emplace_back(M.base + val, n);
    }
    M.stub_hi = M.symbols.count("_etext") ? M.symbols["_etext"] : M.base + filesz;
    for (auto& [a, n] : untyped)
        if (a >= M.stub_lo && a < M.stub_hi && n.find("_stub") == std::string::npos) M.imports[a] = n;
    M.gp = M.symbols.count("_gp") ? M.symbols["_gp"] : M.base + 0x8000;
    M.loaded = M.stub_lo != ~0u;
    std::fprintf(stderr, "[iop] %s: running the module itself (R3000 interpreter) at %06x, %zu imports\n", name, M.base, M.imports.size());
    return M.loaded;
}

uint32_t symbol(const char* name) {
    auto it = M.symbols.find(name);
    return it == M.symbols.end() ? 0 : it->second;
}

uint32_t call(Context* ctx, uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    g_ctx = ctx;
    if (!g_call) {
        auto t = std::make_unique<Thread>();
        t->id = (int)g_threads.size() + 1;
        t->prio = 40;   // HIS's RPC thread (MainLoop) priority
        t->stack_size = 0x4000;
        t->stack = alloc(t->stack_size, 16);
        g_call = t.get();
        g_threads.push_back(std::move(t));
    }
    Thread& t = *g_call;
    t.cpu = Cpu{};
    t.cpu.r[4] = a0; t.cpu.r[5] = a1; t.cpu.r[6] = a2; t.cpu.r[7] = a3;
    t.cpu.r[28] = M.gp;
    t.cpu.r[29] = t.stack + t.stack_size - 16;
    t.cpu.r[31] = kMagicRet;
    t.cpu.pc = func;
    t.started = true; t.done = false; t.wait = Wait::None;
    t.seq = 0;   // first in line at its priority
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!t.done) {
        if (!schedule(64)) {
            if (Clock::now() > deadline) {
                std::fprintf(stderr, "[irx] %s: call to %05x is stuck (waiting %d)\n", M.name.c_str(), func - M.base, (int)t.wait);
                t.done = true;
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));   // only timed waits are left
        }
    }
    return t.cpu.r[2];
}

void tick(Context* ctx) {
    if (!M.loaded) return;
    g_ctx = ctx;
    schedule(32);
}

void raise(uint32_t func, uint32_t a0, uint32_t a1) {
    std::lock_guard<std::mutex> lock(g_irq_mutex);
    g_irqs.push_back({func, a0, a1});
    if (!g_irq_pending.load(std::memory_order_relaxed)) g_irq_due.store(g_insns + kIrqLatency, std::memory_order_relaxed);
    g_irq_pending.store(true, std::memory_order_relaxed);
}

} // namespace ps2::iop::irx
