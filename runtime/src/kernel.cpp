// EE kernel HLE: syscalls issued with "addiu v1, zero, N; syscall".
// Negative N are the interrupt-context (i-prefixed) variants of the same call.
//
// Threads: every guest thread runs on its own Win32 fiber with its own register
// Context (memory pointers are shared). Scheduling follows the EE kernel: strict
// priority (lower number = higher priority), round-robin within a level, switches
// only on blocking calls, yields, or when an interrupt handler wakes a
// higher-priority thread at a syscall boundary.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2 {

namespace {

using Clock = std::chrono::steady_clock;

enum class TS { Dormant, Ready, Running, Waiting, WaitSuspended, Suspended };
enum class WaitKind { None, Sleep, Sema };

struct Thread {
    int id = 0;
    uint32_t entry = 0, stack = 0, stack_size = 0, gp = 0, init_priority = 0, attr = 0, option = 0;
    int priority = 0;
    TS status = TS::Dormant;
    WaitKind wait = WaitKind::None;
    int wait_id = 0;
    int wakeup_count = 0;
    uint32_t arg = 0;
    std::unique_ptr<Context> ctx;   // main thread uses the host-provided context
    Context* c = nullptr;
    LPVOID fiber = nullptr;
    bool fiber_finished = false;
};

struct Sema { int32_t count, max, init; uint32_t attr, option; std::deque<int> waiters; };

struct Handler { uint32_t cause, func, next, arg, gp; };
struct Alarm { int id; uint64_t due_hsync; uint32_t func, arg, gp; uint16_t time; };

constexpr double kVsyncHz = 59.94;
constexpr double kHsyncHz = 15734.0;
constexpr uint32_t kIrqStackTop = 0x0007FF00;  // interrupt handlers run on a stack in kernel RAM

struct Kernel {
    uint32_t heap_start = 0, heap_end = 0;
    std::map<int, Thread> threads;
    std::map<int, Sema> semas;
    std::vector<std::pair<int, Handler>> intc, dmac;   // registration order matters
    std::map<int, Alarm> alarms;
    uint32_t intc_mask = 0, dmac_mask = 0;
    uint32_t dmac_pending = 0, intc_pending = 0;
    int next_thread = 2, next_sema = 1, next_handler = 1, next_alarm = 1;
    int current = 1;
    bool in_interrupt = false;
    LPVOID main_fiber = nullptr;
    Clock::time_point t0;
    uint64_t vsyncs_delivered = 0;
    uint64_t hsync_now() const {
        return (uint64_t)(std::chrono::duration<double>(Clock::now() - t0).count() * kHsyncHz);
    }
    std::vector<LPVOID> dead_fibers;
    Clock::time_point sif_due{};   // when the queued IOP reply arrives (zero: none queued)
} K;

// IOP reply latency. A real RPC takes the IOP a while to answer; the calling
// thread blocks on its semaphore meanwhile and lower-priority threads run.
// The level loader decodes on such a thread and gets no time at all when
// replies arrive instantly.
double g_sif_latency_us = 500;

Thread& cur() { return K.threads[K.current]; }

const char* syscall_name(int n) {
    switch (n < 0 ? -n : n) {
    case 0x01: return "ResetEE";        case 0x02: return "SetGsCrt";      case 0x04: return "Exit";
    case 0x06: return "LoadExecPS2";    case 0x07: return "ExecPS2";
    case 0x10: return "AddIntcHandler"; case 0x11: return "RemoveIntcHandler";
    case 0x12: return "AddDmacHandler"; case 0x13: return "RemoveDmacHandler";
    case 0x14: return "EnableIntc";     case 0x15: return "DisableIntc";
    case 0x16: return "EnableDmac";     case 0x17: return "DisableDmac";
    case 0x20: return "CreateThread";   case 0x21: return "DeleteThread";  case 0x22: return "StartThread";
    case 0x23: return "ExitThread";     case 0x24: return "ExitDeleteThread"; case 0x25: return "TerminateThread";
    case 0x29: return "ChangeThreadPriority"; case 0x2B: return "RotateThreadReadyQueue";
    case 0x2D: return "ReleaseWaitThread"; case 0x2F: return "GetThreadId"; case 0x30: return "ReferThreadStatus";
    case 0x32: return "SleepThread";    case 0x33: return "WakeupThread";  case 0x35: return "CancelWakeupThread";
    case 0x37: return "SuspendThread";  case 0x39: return "ResumeThread";  case 0x3C: return "SetupThread";
    case 0x3D: return "SetupHeap";      case 0x3E: return "EndOfHeap";     case 0x40: return "CreateSema";
    case 0x41: return "DeleteSema";     case 0x42: return "SignalSema";    case 0x44: return "WaitSema";
    case 0x45: return "PollSema";       case 0x47: return "ReferSemaStatus"; case 0x64: return "FlushCache";
    case 0x70: return "GsGetIMR";       case 0x71: return "GsPutIMR";      case 0x73: return "SetVSyncFlag";
    case 0x74: return "SetSyscall";     case 0x6B: return "SifStopDma";     case 0x55: return "PutTLBEntry"; case 0x56: return "SetTLBEntry"; case 0x57: return "GetTLBEntry"; case 0x58: return "ProbeTLBEntry"; case 0x59: return "ExpandScratchPad";     case 0x76: return "SifDmaStat";    case 0x77: return "SifSetDma";
    case 0x78: return "SifSetDChain";   case 0x79: return "SifSetReg";     case 0x7A: return "SifGetReg";
    case 0x7C: return "Deci2Call";      case 0x7F: return "GetMemorySize"; case 0x81: return "SetAlarm";
    case 0x4A: return "SetOsdConfigParam"; case 0x4B: return "GetOsdConfigParam";
    case 0x6E: return "GetOsdConfigParam2"; case 0x6F: return "SetOsdConfigParam2"; case 0x7E: return "MachineType";
    case 0x82: return "ReleaseAlarm";
    default: return "?";
    }
}

uint32_t A(Context* c, int i) { return c->r[4 + i].u32[0]; }   // a0..a3
void ret(Context* c, int32_t v) { c->r[2].s64[0] = v; }          // v0

// ---- interrupt context ---------------------------------------------------------
// Runs a guest handler on the interrupt stack with a scratch register context.
void run_handler(Context* from, uint32_t func, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t a2) {
    static Context irq;   // handlers never nest
    std::memcpy(&irq, from, sizeof irq);
    irq.r[28].s64[0] = (int32_t)gp;
    irq.r[29].s64[0] = (int32_t)kIrqStackTop;
    irq.r[4].s64[0] = (int32_t)a0;
    irq.r[5].s64[0] = (int32_t)a1;
    irq.r[6].s64[0] = (int32_t)a2;
    irq.r[31].s64[0] = 0;
    const bool was = K.in_interrupt;
    K.in_interrupt = true;
    call_indirect(&irq, func);
    K.in_interrupt = was;
}

void reschedule(Context* ctx);

// Deliver vsync interrupts and alarms that are due. Returns true if any ran.
// `idle`: no thread can run, so a queued IOP reply is delivered without waiting.
bool deliver_interrupts(Context* ctx, bool idle = false) {
    if (K.in_interrupt) return false;
    hw_collect_gfx();   // transfers the GIF/VIF worker finished
    bool any = false;
    const double secs = std::chrono::duration<double>(Clock::now() - K.t0).count();
    const uint64_t vs = (uint64_t)(secs * kVsyncHz);
    if (vs > K.vsyncs_delivered) {
        K.vsyncs_delivered = vs;   // drop missed frames rather than flooding
        iop_vblank(ctx);
        gpu_vblank(ctx);
        for (uint32_t cause : {2u, 3u}) {   // INTC_VBLANK_S, INTC_VBLANK_E
            if (!(K.intc_mask & (1u << cause))) continue;
            for (auto& h : K.intc)
                if (h.second.cause == cause) { run_handler(ctx, h.second.func, h.second.gp, cause, h.second.arg, 0); any = true; }
        }
    }
    // EE timers 0-2 (INTC 9-11)
    // One handler run per event: the game's clock counts timer 0 overflows.
    for (uint32_t cause = 9; cause <= 11; ++cause)
        for (int n = 0; n < 1024 && timer_take_event((int)cause - 9, K.intc_mask); ++n)
            for (auto& h : K.intc)
                if (h.second.cause == cause) {
                    g_timer_in_handler = (int)cause - 9;
                    run_handler(ctx, h.second.func, h.second.gp, cause, h.second.arg, 0);
                    g_timer_in_handler = -1;
                    any = true;
                }
    // other INTC sources raised by the hardware model (IPU)
    if (const uint32_t ic = K.intc_pending & K.intc_mask) {
        K.intc_pending &= ~ic;
        for (uint32_t cause = 0; cause < 32; ++cause) {
            if (!(ic & (1u << cause))) continue;
            for (auto& h : K.intc)
                if (h.second.cause == cause) { run_handler(ctx, h.second.func, h.second.gp, cause, h.second.arg, 0); any = true; }
        }
    }
    const uint64_t hs = K.hsync_now();
    for (auto it = K.alarms.begin(); it != K.alarms.end();) {
        if (it->second.due_hsync <= hs) {
            const Alarm a = it->second;
            it = K.alarms.erase(it);
            run_handler(ctx, a.func, a.gp, (uint32_t)a.id, a.time, a.arg);
            any = true;
        } else {
            ++it;
        }
    }
    // DMA completion interrupts (handlers may start new transfers; bounded)
    for (int round = 0; round < 64 && (K.dmac_pending & K.dmac_mask); ++round) {
        const uint32_t p = K.dmac_pending & K.dmac_mask;
        K.dmac_pending &= ~p;
        for (int ch = 0; ch < 16; ++ch) {
            if (!(p & (1u << ch))) continue;
            for (auto& h : K.dmac)
                if (h.second.cause == (uint32_t)ch) run_handler(ctx, h.second.func, h.second.gp, (uint32_t)ch, h.second.arg, 0);
            any = true;
        }
    }
    // SIF0 (IOP -> EE): one packet per DMAC channel 5 interrupt, once the reply is due
    if ((K.dmac_mask & (1u << 5)) && iop_packet_pending()) {
        const auto now = Clock::now();
        if (K.sif_due == Clock::time_point{})
            K.sif_due = now + std::chrono::microseconds((int64_t)g_sif_latency_us);
        if (idle || now >= K.sif_due) {
            K.sif_due = {};
            while (iop_pop_packet(ctx)) {
            for (auto& h : K.dmac)
                if (h.second.cause == 5) run_handler(ctx, h.second.func, h.second.gp, 5, h.second.arg, 0);
                any = true;
            }
        }
    }
    return any;
}

int pick_next() {
    int best = -1, best_prio = 1 << 30;
    // round-robin: start scanning after the current thread
    for (int pass = 0; pass < 2; ++pass)
        for (auto& kv : K.threads) {
            if (pass == 0 && kv.first <= K.current) continue;
            if (pass == 1 && kv.first > K.current) break;
            const Thread& t = kv.second;
            if ((t.status == TS::Ready || t.status == TS::Running) && t.priority < best_prio) { best = kv.first; best_prio = t.priority; }
        }
    return best;
}

VOID CALLBACK fiber_main(LPVOID p) {
    Thread* t = static_cast<Thread*>(p);
    call_indirect(t->c, t->entry);
    // returned from its entry function: behaves like ExitThread
    t->status = TS::Dormant;
    t->fiber_finished = true;
    reschedule(t->c);
    fatal(t->c, t->entry, "exited thread was resumed");
}

void switch_to(int id) {
    if (id == K.current) return;
    Thread& t = K.threads[id];
    if (!t.fiber) {
        t.fiber = CreateFiber(256 * 1024 * 1024 / 64, fiber_main, &t);   // 4 MB host stack per guest thread
        if (!t.fiber) fatal(t.c, t.entry, "CreateFiber failed");
    }
    Thread& from = cur();
    if (from.status == TS::Running) from.status = TS::Ready;
    t.status = TS::Running;
    K.current = id;
    LPVOID target = t.fiber;
    if (id == 1) target = K.main_fiber;
    SwitchToFiber(target);
    // resumed: clean up fibers of threads that finished meanwhile
    for (LPVOID f : K.dead_fibers) DeleteFiber(f);
    K.dead_fibers.clear();
}

// Pick the next runnable thread; idle (delivering interrupts) if none.
void reschedule(Context* ctx) {
    for (;;) {
        const int next = pick_next();
        if (next > 0) {
            if (next != K.current) switch_to(next);
            else cur().status = TS::Running;
            return;
        }
        // nothing runnable: wait for the next timer event
        if (!deliver_interrupts(ctx, true)) std::this_thread::sleep_for(std::chrono::microseconds(500));
        if (K.intc_mask == 0 && K.alarms.empty()) fatal(ctx, 0, "deadlock: all threads waiting and no interrupt sources");
    }
}

void wait_current(Context* ctx, WaitKind kind, int id) {
    Thread& t = cur();
    t.status = TS::Waiting;
    t.wait = kind;
    t.wait_id = id;
    reschedule(ctx);
}

void make_ready(Thread& t) {
    t.wait = WaitKind::None;
    t.status = (t.status == TS::WaitSuspended) ? TS::Suspended : TS::Ready;
}

} // namespace

void kernel_init(Context* ctx) {
    K = Kernel{};
    K.t0 = Clock::now();
    K.main_fiber = ConvertThreadToFiber(nullptr);
    Thread& m = K.threads[1];
    m.id = 1;
    m.priority = 0;   // set by ChangeThreadPriority in crt0/_InitSys
    m.status = TS::Running;
    m.c = ctx;
}

void kernel_poll(Context* ctx);

void sif_latency_config(double usec) { g_sif_latency_us = usec < 0 ? 0 : usec; }

static uint64_t g_syscalls = 0;
uint64_t kernel_syscall_count() { return g_syscalls; }
void kernel_raise_dmac(int ch) { if (ch != 5) K.dmac_pending |= 1u << ch; }
void kernel_raise_intc(int cause) { K.intc_pending |= 1u << cause; }

void kernel_syscall(Context* ctx, uint32_t pc) {
    ++g_syscalls;
    if (!K.in_interrupt) kernel_poll(ctx);
    const int n = ctx->r[3].s32[0];   // v1
    int call = n < 0 ? -n : n;
    if (n < 0) {   // interrupt-context variants: most have their own number, one above the base call
        switch (call) {
        case 0x1A: case 0x1B: case 0x1C: case 0x1D: call -= 6; break;   // iEnable/DisableIntc/Dmac
        case 0x26: case 0x2A: case 0x2C: case 0x2E: case 0x31: case 0x34:
        case 0x36: case 0x38: case 0x3A: case 0x43: case 0x46: case 0x48: call -= 1; break;
        case 0x68: call = 0x64; break;   // iFlushCache
        default: break;                  // SIF and GS IMR variants share the number
        }
    }
    if (log_enabled(Log::Kernel))
        log(Log::Kernel, "[t%d] %s%s(0x%02x) a0=%08x a1=%08x a2=%08x a3=%08x @%08x ra=%08x", K.current, n < 0 ? "i" : "", syscall_name(call), call,
            A(ctx, 0), A(ctx, 1), A(ctx, 2), A(ctx, 3), pc, ctx->r[31].u32[0]);

    switch (call) {
    // ---- boot / memory -------------------------------------------------------------
    case 0x3C: {  // SetupThread(gp, stack, stack_size, args, root_func) -> sp
        const uint32_t stack = A(ctx, 1), size = A(ctx, 2), args = A(ctx, 3);
        const uint32_t top = (stack == 0xFFFFFFFFu ? kRamSize : stack + size);
        if (args) {   // argc/argv block: { argc, argv[0], NULL, "cdrom0:\SLUS_208.87;1" }
            static const char kArg0[] = "cdrom0:\\SLUS_208.87;1";
            uint8_t* p = guest_ptr(ctx, args, 12 + sizeof kArg0);
            const uint32_t argc = 1, argv0 = args + 12, null = 0;
            std::memcpy(p, &argc, 4);
            std::memcpy(p + 4, &argv0, 4);
            std::memcpy(p + 8, &null, 4);
            std::memcpy(p + 12, kArg0, sizeof kArg0);
        }
        cur().stack = stack;
        cur().stack_size = size;
        cur().gp = A(ctx, 0);
        ret(ctx, (int32_t)(top - 0x2A0));
        return;
    }
    case 0x3D:
        K.heap_start = A(ctx, 0);
        K.heap_end = A(ctx, 1) == 0xFFFFFFFFu ? kRamSize : A(ctx, 0) + A(ctx, 1);
        ret(ctx, (int32_t)K.heap_end);
        return;
    case 0x3E: ret(ctx, (int32_t)K.heap_end); return;
    case 0x64: ret(ctx, 0); return;                 // FlushCache
    case 0x7F: ret(ctx, (int32_t)kRamSize); return;  // GetMemorySize
    case 0x02: ret(ctx, 0); return;                 // SetGsCrt
    case 0x70: case 0x71: ret(ctx, 0); return;      // GsGetIMR / GsPutIMR
    case 0x74: ret(ctx, 0); return;                 // SetSyscall: kernel patches are not needed under HLE
    case 0x73: ret(ctx, 0); return;                 // SetVSyncFlag
    case 0x55: case 0x56: {   // PutTLBEntry / SetTLBEntry(index, PageMask, EntryHi, EntryLo0, EntryLo1)
        // The guest memory map is fixed in host_ptr; entries are only recorded for diagnostics.
        const uint32_t lo0 = ctx->r[8].u32[0], lo1 = ctx->r[9].u32[0];
        log(Log::Kernel, "  tlb[%u] mask=%08x hi=%08x lo0=%08x lo1=%08x", A(ctx, 0), A(ctx, 1), A(ctx, 2), A(ctx, 3), lo0 + 0 * lo1);
        ret(ctx, (int32_t)A(ctx, 0));
        return;
    }
    case 0x57: ret(ctx, (int32_t)A(ctx, 0)); return;   // GetTLBEntry
    case 0x58: ret(ctx, -1); return;                    // ProbeTLBEntry: not found
    case 0x59: ret(ctx, 0); return;                     // ExpandScratchPad
    case 0x6B: ret(ctx, 0); return;                 // SifStopDma: transfers complete instantly
    case 0x76: case 0x78: case 0x79: case 0x7A:
        sif_syscall(ctx, call);
        return;
    case 0x77:   // SifSetDma: the IOP answers at once; deliver its reply before returning
        sif_syscall(ctx, call);
        if (!K.in_interrupt) kernel_poll(ctx);
        return;

    // ---- interrupts --------------------------------------------------------------------
    case 0x10: case 0x12: {  // Add{Intc,Dmac}Handler(cause, handler, next, arg)
        const int id = K.next_handler++;
        auto& list = (call == 0x10) ? K.intc : K.dmac;
        Handler h{A(ctx, 0), A(ctx, 1), A(ctx, 2), A(ctx, 3), ctx->r[28].u32[0]};
        if ((int32_t)h.next < 0) list.insert(list.begin(), {id, h}); else list.push_back({id, h});
        log(Log::Kernel, "  %s cause=%u handler=%08x -> %d", call == 0x10 ? "intc" : "dmac", h.cause, h.func, id);
        ret(ctx, id);
        return;
    }
    case 0x11: case 0x13: {
        auto& list = (call == 0x11) ? K.intc : K.dmac;
        for (auto it = list.begin(); it != list.end(); ++it)
            if (it->first == (int)A(ctx, 1)) { list.erase(it); break; }
        ret(ctx, 0);
        return;
    }
    case 0x14: K.intc_mask |= 1u << A(ctx, 0); ret(ctx, 1); return;
    case 0x15: K.intc_mask &= ~(1u << A(ctx, 0)); ret(ctx, 1); return;
    case 0x16: K.dmac_mask |= 1u << A(ctx, 0); ret(ctx, 1); return;
    case 0x17: K.dmac_mask &= ~(1u << A(ctx, 0)); ret(ctx, 1); return;
    case 0x81: {  // SetAlarm(time_in_hsyncs, handler, arg)
        const int id = K.next_alarm++;
        K.alarms[id] = {id, K.hsync_now() + (A(ctx, 0) & 0xFFFF), A(ctx, 1), A(ctx, 2), ctx->r[28].u32[0], (uint16_t)A(ctx, 0)};
        ret(ctx, id);
        return;
    }
    case 0x82: ret(ctx, K.alarms.erase((int)A(ctx, 0)) ? (int32_t)A(ctx, 0) : -1); return;

    // ---- semaphores ------------------------------------------------------------------------
    case 0x40: {  // CreateSema({count, max_count, init_count, wait_threads, attr, option})
        int32_t v[6];
        std::memcpy(v, guest_ptr(ctx, A(ctx, 0), 24), sizeof v);
        Sema s{v[2], v[1], v[2], (uint32_t)v[4], (uint32_t)v[5], {}};
        const int id = K.next_sema++;
        K.semas[id] = s;
        ret(ctx, id);
        return;
    }
    case 0x41: {  // DeleteSema: release waiters with an error
        auto it = K.semas.find((int)A(ctx, 0));
        if (it == K.semas.end()) { ret(ctx, -1); return; }
        for (int w : it->second.waiters) { make_ready(K.threads[w]); K.threads[w].c->r[2].s64[0] = -1; }
        K.semas.erase(it);
        ret(ctx, (int32_t)A(ctx, 0));
        return;
    }
    case 0x42: {  // SignalSema / iSignalSema
        auto it = K.semas.find((int)A(ctx, 0));
        if (it == K.semas.end()) { ret(ctx, -1); return; }
        Sema& s = it->second;
        ret(ctx, (int32_t)A(ctx, 0));
        if (!s.waiters.empty()) {
            const int w = s.waiters.front();
            s.waiters.pop_front();
            Thread& t = K.threads[w];
            t.c->r[2].s64[0] = (int32_t)A(ctx, 0);
            make_ready(t);
            if (!K.in_interrupt && t.priority < cur().priority) reschedule(ctx);
        } else if (s.max <= 0 || s.count < s.max) {
            ++s.count;
        }
        return;
    }
    case 0x44: case 0x45: {  // WaitSema / PollSema
        auto it = K.semas.find((int)A(ctx, 0));
        if (it == K.semas.end()) { ret(ctx, -1); return; }
        Sema& s = it->second;
        if (s.count > 0) { --s.count; ret(ctx, (int32_t)A(ctx, 0)); return; }
        if (call == 0x45 || K.in_interrupt) { ret(ctx, -1); return; }
        s.waiters.push_back(K.current);
        wait_current(ctx, WaitKind::Sema, (int)A(ctx, 0));   // v0 set by the signaller
        return;
    }
    case 0x47: {
        auto it = K.semas.find((int)A(ctx, 0));
        if (it == K.semas.end()) { ret(ctx, -1); return; }
        const Sema& s = it->second;
        int32_t v[6] = {s.count, s.max, s.init, (int32_t)s.waiters.size(), (int32_t)s.attr, (int32_t)s.option};
        std::memcpy(guest_ptr(ctx, A(ctx, 1), 24), v, sizeof v);
        ret(ctx, (int32_t)A(ctx, 0));
        return;
    }

    // ---- threads ---------------------------------------------------------------------------
    case 0x20: {  // CreateThread({status, func, stack, stack_size, gp, initial_priority, current_priority, attr, option})
        uint32_t v[9];
        std::memcpy(v, guest_ptr(ctx, A(ctx, 0), 36), sizeof v);
        const int id = K.next_thread++;
        Thread& t = K.threads[id];
        t.id = id;
        t.entry = v[1]; t.stack = v[2]; t.stack_size = v[3]; t.gp = v[4];
        t.init_priority = v[5]; t.priority = (int)v[5]; t.attr = v[7]; t.option = v[8];
        t.ctx = std::make_unique<Context>();
        t.c = t.ctx.get();
        log(Log::Kernel, "  CreateThread -> %d entry=%08x stack=%08x+%x prio=%u", id, t.entry, t.stack, t.stack_size, v[5]);
        ret(ctx, id);
        return;
    }
    case 0x22: {  // StartThread(id, arg)
        auto it = K.threads.find((int)A(ctx, 0));
        if (it == K.threads.end() || it->second.status != TS::Dormant) { ret(ctx, -1); return; }
        Thread& t = it->second;
        std::memcpy(t.c, ctx, sizeof(Context));   // inherit COP/VU state; GPRs reset below
        std::memset(t.c->r, 0, sizeof t.c->r);
        t.c->r[28].s64[0] = (int32_t)t.gp;
        t.c->r[29].s64[0] = (int32_t)(t.stack + t.stack_size - 16);
        t.c->r[4].s64[0] = (int32_t)A(ctx, 1);
        t.arg = A(ctx, 1);
        t.priority = (int)t.init_priority;
        t.wakeup_count = 0;
        if (t.fiber && t.fiber_finished) { K.dead_fibers.push_back(t.fiber); t.fiber = nullptr; }
        t.fiber_finished = false;
        t.status = TS::Ready;
        ret(ctx, (int32_t)A(ctx, 0));
        if (!K.in_interrupt && t.priority < cur().priority) reschedule(ctx);
        return;
    }
    case 0x23: case 0x24:  // ExitThread / ExitDeleteThread
        cur().status = TS::Dormant;
        cur().fiber_finished = true;
        if (K.current == 1) fatal(ctx, pc, "main thread exited");
        reschedule(ctx);
        fatal(ctx, pc, "exited thread resumed");
    case 0x25: {  // TerminateThread(id)
        auto it = K.threads.find((int)A(ctx, 0));
        if (it == K.threads.end() || it->first == K.current) { ret(ctx, -1); return; }
        for (auto& s : K.semas) {
            auto& w = s.second.waiters;
            for (auto i = w.begin(); i != w.end();) i = (*i == it->first) ? w.erase(i) : i + 1;
        }
        it->second.status = TS::Dormant;
        if (it->second.fiber) { K.dead_fibers.push_back(it->second.fiber); it->second.fiber = nullptr; }
        ret(ctx, (int32_t)A(ctx, 0));
        return;
    }
    case 0x21: {  // DeleteThread
        auto it = K.threads.find((int)A(ctx, 0));
        if (it == K.threads.end() || it->second.status != TS::Dormant) { ret(ctx, -1); return; }
        if (it->second.fiber) K.dead_fibers.push_back(it->second.fiber);
        K.threads.erase(it);
        ret(ctx, (int32_t)A(ctx, 0));
        return;
    }
    case 0x29: {  // ChangeThreadPriority(id, prio)
        const int id = A(ctx, 0) ? (int)A(ctx, 0) : K.current;
        auto it = K.threads.find(id);
        if (it == K.threads.end()) { ret(ctx, -1); return; }
        const int old = it->second.priority;
        it->second.priority = (int)A(ctx, 1);
        ret(ctx, old);
        if (!K.in_interrupt) reschedule(ctx);
        return;
    }
    case 0x2B: {  // RotateThreadReadyQueue(prio): yield to an equal-priority thread
        ret(ctx, 0);
        if (!K.in_interrupt && (int)A(ctx, 0) == cur().priority) {
            cur().status = TS::Ready;
            reschedule(ctx);
        }
        return;
    }
    case 0x2F: ret(ctx, K.current); return;   // GetThreadId
    case 0x30: {  // ReferThreadStatus(id, info*)
        const int id = A(ctx, 0) ? (int)A(ctx, 0) : K.current;
        auto it = K.threads.find(id);
        if (it == K.threads.end()) { ret(ctx, -1); return; }
        if (A(ctx, 1)) {
            const Thread& t = it->second;
            static const uint32_t kStatus[] = {0x10, 0x02, 0x01, 0x04, 0x0C, 0x08};  // THS_DORMANT, READY, RUN, WAIT, WAITSUSPEND, SUSPEND
            uint32_t v[13] = {kStatus[(int)t.status], t.entry, t.stack, t.stack_size, t.gp, t.init_priority,
                              (uint32_t)t.priority, t.attr, t.option, (uint32_t)t.wait, (uint32_t)t.wait_id,
                              (uint32_t)t.wakeup_count, 0};
            std::memcpy(guest_ptr(ctx, A(ctx, 1), sizeof v), v, sizeof v);
        }
        ret(ctx, (int32_t)[&] { static const int32_t k[] = {0x10, 0x02, 0x01, 0x04, 0x0C, 0x08}; return k[(int)it->second.status]; }());
        return;
    }
    case 0x32:  // SleepThread
        if (cur().wakeup_count > 0) { --cur().wakeup_count; ret(ctx, K.current); return; }
        ret(ctx, K.current);
        wait_current(ctx, WaitKind::Sleep, 0);
        return;
    case 0x33: {  // WakeupThread / iWakeupThread
        auto it = K.threads.find((int)A(ctx, 0));
        if (it == K.threads.end()) { ret(ctx, -1); return; }
        Thread& t = it->second;
        ret(ctx, (int32_t)A(ctx, 0));
        if ((t.status == TS::Waiting || t.status == TS::WaitSuspended) && t.wait == WaitKind::Sleep) {
            make_ready(t);
            if (!K.in_interrupt && t.priority < cur().priority) reschedule(ctx);
        } else {
            ++t.wakeup_count;
        }
        return;
    }
    case 0x35: {  // CancelWakeupThread
        auto it = K.threads.find(A(ctx, 0) ? (int)A(ctx, 0) : K.current);
        if (it == K.threads.end()) { ret(ctx, -1); return; }
        ret(ctx, it->second.wakeup_count);
        it->second.wakeup_count = 0;
        return;
    }
    case 0x37: {  // SuspendThread
        auto it = K.threads.find((int)A(ctx, 0));
        if (it == K.threads.end()) { ret(ctx, -1); return; }
        Thread& t = it->second;
        if (t.status == TS::Waiting) t.status = TS::WaitSuspended;
        else if (t.status == TS::Ready) t.status = TS::Suspended;
        ret(ctx, (int32_t)A(ctx, 0));
        return;
    }
    case 0x39: {  // ResumeThread
        auto it = K.threads.find((int)A(ctx, 0));
        if (it == K.threads.end()) { ret(ctx, -1); return; }
        Thread& t = it->second;
        if (t.status == TS::WaitSuspended) t.status = TS::Waiting;
        else if (t.status == TS::Suspended) t.status = TS::Ready;
        ret(ctx, (int32_t)A(ctx, 0));
        if (!K.in_interrupt && t.status == TS::Ready && t.priority < cur().priority) reschedule(ctx);
        return;
    }

    // ---- system configuration --------------------------------------------------------------
    case 0x4A: ret(ctx, 0); return;   // SetOsdConfigParam
    case 0x4B: {                      // GetOsdConfigParam(ConfigParam*): bitfield word
        // spdif=0, screenType=0 (4:3), video=0 (RGB), japLanguage=1 (non-Japanese),
        // version=1, language=1 (English), timezoneOffset=0
        const uint32_t v = (1u << 4) | (1u << 13) | (1u << 16);
        std::memcpy(guest_ptr(ctx, A(ctx, 0), 4), &v, 4);
        return;
    }
    case 0x6E: {                      // GetOsdConfigParam2(buf, size, offset): extended config, all defaults
        if (A(ctx, 0) && A(ctx, 1)) std::memset(guest_ptr(ctx, A(ctx, 0), A(ctx, 1)), 0, A(ctx, 1));
        ret(ctx, 0);
        return;
    }
    case 0x6F: ret(ctx, 0); return;   // SetOsdConfigParam2
    case 0x7C: ret(ctx, -1); return;  // Deci2Call: no debugger attached
    case 0x7E: ret(ctx, 0); return;   // MachineType

    case 0x04:
        log(Log::Kernel, "guest called Exit(%d)", (int)A(ctx, 0));
        if (A(ctx, 0) != 0) fatal(ctx, pc, "guest called Exit with an error code");
        write_trace_file();
        std::exit(0);

    default: {
        char msg[96];
        std::snprintf(msg, sizeof msg, "unimplemented syscall %s (0x%02x%s)", syscall_name(call), call, n < 0 ? ", interrupt variant" : "");
        fatal(ctx, pc, msg);
    }
    }
}

// Called by the runtime at safe points (syscalls, idle loops) to keep time moving.
uint32_t g_poll_tick = 0;

// Interrupt delivery point inside guest code (function entries, MMIO polls).
void poll_point(Context* ctx) {
    g_poll_tick = 0;
    if (K.in_interrupt || !(ctx->cop0[12] & 0x10000u)) return;   // handler running, or guest did DI
    const uint64_t before = g_syscalls;
    kernel_poll(ctx);
    // A guest loop that only polls memory (waiting for a DMA or vsync interrupt)
    // reaches its poll point with exactly the registers it had last time. Instead
    // of burning a core the GS rasterizer needs, wait for the GIF/VIF worker to
    // finish (that is what such a loop waits for), or give the core away.
    static uint64_t last_gpr[32][2];
    static int same = 0;
    if (g_syscalls == before && std::memcmp(last_gpr, ctx->r, sizeof last_gpr) == 0) {
        if (++same >= 2) gfx_wait_progress(500);
    } else {
        same = 0;
        std::memcpy(last_gpr, ctx->r, sizeof last_gpr);
    }
}

void kernel_poll(Context* ctx) {
    // (the watchdog counts delivered interrupts as activity, like syscalls)
    if (deliver_interrupts(ctx)) {
        ++g_syscalls;
        const int next = pick_next();
        if (next > 0 && K.threads[next].priority < cur().priority) reschedule(ctx);
    }
}

} // namespace ps2
