// Sampling profiler (`--profile <seconds>`): a background thread samples the
// game thread's instruction pointer every millisecond, then prints the
// hottest functions (resolved through the PDB with DbgHelp) and exits.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ps2/runtime.hpp"

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace ps2 {

namespace {

void report(const std::unordered_map<uint64_t, uint32_t>& hits, uint64_t total) {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    std::map<std::string, uint64_t> by_func;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    for (const auto& [addr, n] : hits) {
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        std::string name = SymFromAddr(proc, addr, &disp, sym) ? std::string(sym->Name) : "?";
        by_func[name] += n;
    }
    std::vector<std::pair<uint64_t, std::string>> sorted;
    for (auto& [name, n] : by_func) sorted.emplace_back(n, name);
    std::sort(sorted.rbegin(), sorted.rend());
    std::fprintf(stderr, "[profile] %llu samples\n", (unsigned long long)total);
    for (size_t i = 0; i < sorted.size() && i < 45; ++i)
        std::fprintf(stderr, "[profile] %5.1f%%  %s\n", 100.0 * (double)sorted[i].first / (double)total, sorted[i].second.c_str());
}

} // namespace

void profiler_start(double delay, double seconds) {
    HANDLE target = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::thread([target, delay, seconds] {
        Sleep((DWORD)(delay * 1000));
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
