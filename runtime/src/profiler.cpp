// Sampling profiler (`--profile <seconds>`): a background thread samples the
// game thread's instruction pointer every millisecond, then prints the
// hottest functions (resolved through the PDB with DbgHelp) and exits.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ps2/runtime.hpp"

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace ps2 {

namespace {

void report(const std::unordered_map<uint64_t, uint32_t>& hits, uint64_t total, FILE* out = stderr) {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    static bool sym_ready = false;
    if (!sym_ready) { SymInitialize(proc, nullptr, TRUE); sym_ready = true; }
    std::map<std::string, uint64_t> by_func;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    if (std::getenv("TWONK_PROFILE_ADDRS")) {   // hottest instruction addresses, as offsets into the exe
        std::vector<std::pair<uint32_t, uint64_t>> v;
        const uint64_t base = (uint64_t)GetModuleHandleW(nullptr);
        for (const auto& [addr, n] : hits) v.emplace_back(n, addr);
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 60; ++i)
            std::fprintf(stderr, "[profile-addr] %5.1f%%  rva %llx\n", 100.0 * v[i].first / (double)total,
                         (unsigned long long)(v[i].second - base));
    }
    for (const auto& [addr, n] : hits) {
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        std::string name = SymFromAddr(proc, addr, &disp, sym) ? std::string(sym->Name) : "?";
        static const bool lines = std::getenv("TWONK_PROFILE_LINES") != nullptr;   // per source line
        if (lines) {
            IMAGEHLP_LINE64 ln{};
            ln.SizeOfStruct = sizeof ln;
            DWORD ldisp = 0;
            if (SymGetLineFromAddr64(proc, addr, &ldisp, &ln)) {
                const char* file = std::strrchr(ln.FileName, '\\');
                name += std::string("  ") + (file ? file + 1 : ln.FileName) + ":" + std::to_string(ln.LineNumber);
            }
        }
        by_func[name] += n;
    }
    std::vector<std::pair<uint64_t, std::string>> sorted;
    for (auto& [name, n] : by_func) sorted.emplace_back(n, name);
    std::sort(sorted.rbegin(), sorted.rend());
    std::fprintf(out, "[profile] %llu samples\n", (unsigned long long)total);
    for (size_t i = 0; i < sorted.size() && i < 45; ++i)
        std::fprintf(out, "[profile] %5.1f%%  %s\n", 100.0 * (double)sorted[i].first / (double)total, sorted[i].second.c_str());
}

HANDLE g_game_thread = nullptr;

// Threads the profiler can sample instead of the game thread (TWONK_PROFILE_THREAD=<name>).
std::mutex g_named_mutex;
std::map<std::string, HANDLE> g_named;

} // namespace

// The calling thread is the game thread; the watchdog later walks its stack.
void game_thread_register() {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_game_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
}

// For the hang watchdog: the host call stack of the (suspended) game thread.
void dump_game_stack() {
    if (!g_game_thread || SuspendThread(g_game_thread) == (DWORD)-1) return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(g_game_thread, &c)) return;
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    STACKFRAME64 f{};
    f.AddrPC.Offset = c.Rip; f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = c.Rbp; f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = c.Rsp; f.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    std::fprintf(stderr, "[ps2] host stack of the game thread:\n");
    for (int i = 0; i < 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, g_game_thread, &f, &c, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) || !f.AddrPC.Offset)
            break;
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof line;
        DWORD ldisp = 0;
        const bool has_line = SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &line);
        std::fprintf(stderr, "   %s +%llx  %s:%lu\n", SymFromAddr(proc, f.AddrPC.Offset, &disp, sym) ? sym->Name : "?",
                     (unsigned long long)disp, has_line ? line.FileName : "", has_line ? line.LineNumber : 0ul);
    }
}

void profiler_name_thread(const char* name) {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::lock_guard<std::mutex> lock(g_named_mutex);
    g_named[name] = h;
}

// Samples the named thread for `seconds` in the background and appends the hot
// functions to `out_path`; the game keeps running (the TWONK_PERF_LOG monitor
// uses it when the frame rate drops). Returns false while one is still running.
bool profiler_snapshot(const char* thread, double seconds, const char* out_path) {
    static std::atomic<bool> busy{false};
    HANDLE target = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_named_mutex);
        if (auto it = g_named.find(thread); it != g_named.end()) target = it->second;
    }
    if (!target && !std::strcmp(thread, "game")) target = g_game_thread;
    if (!target || busy.exchange(true)) return false;
    std::string name = thread, path = out_path;
    std::thread([target, seconds, name, path] {
        timeBeginPeriod(1);
        std::unordered_map<uint64_t, uint32_t> hits;
        uint64_t total = 0;
        const ULONGLONG end = GetTickCount64() + (ULONGLONG)(seconds * 1000);
        while (GetTickCount64() < end) {
            Sleep(1);
            if (SuspendThread(target) == (DWORD)-1) break;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(target, &c)) { ++hits[c.Rip]; ++total; }
            ResumeThread(target);
        }
        timeEndPeriod(1);
        if (FILE* f = std::fopen(path.c_str(), "a")) {
            std::fprintf(f, "=== thread %s, %.1f s\n", name.c_str(), seconds);
            report(hits, total, f);
            std::fclose(f);
        }
        busy = false;
    }).detach();
    return true;
}

void profiler_start(double delay, double seconds) {
    HANDLE game = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::thread([game, delay, seconds] {
        Sleep((DWORD)(delay * 1000));
        HANDLE target = game;
        char want[32]; size_t wn = 0;
        if (!getenv_s(&wn, want, sizeof want, "TWONK_PROFILE_THREAD") && wn) {
            std::lock_guard<std::mutex> lock(g_named_mutex);
            if (auto it = g_named.find(want); it != g_named.end()) target = it->second;
            std::fprintf(stderr, "[profile] sampling thread '%s'%s\n", want, target == game ? " (not found: game thread)" : "");
        }
        timeBeginPeriod(1);
        std::unordered_map<uint64_t, uint32_t> hits;
        uint64_t total = 0;
        const ULONGLONG end = GetTickCount64() + (ULONGLONG)(seconds * 1000);
        while (GetTickCount64() < end) {
            Sleep(1);
            if (SuspendThread(target) == (DWORD)-1) break;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(target, &c)) { ++hits[c.Rip]; ++total; }
            ResumeThread(target);
        }
        SuspendThread(target);   // keep the game still while symbols load
        report(hits, total);
        std::fflush(stderr);
        ExitProcess(0);
    }).detach();
}

} // namespace ps2
