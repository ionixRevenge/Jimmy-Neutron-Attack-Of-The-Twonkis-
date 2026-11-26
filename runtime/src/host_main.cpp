// twonk.exe: loads SLUS_208.87 into guest RAM and runs the recompiled code.
//
//   twonk <SLUS_208.87> [--log kernel,mem,cpu,hle,boot,iop|all]
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2::gpu { int vu1_bench(const char* snapshot, int runs); int vu1_aot_generate(const char* dir, const char* out); int vu1_fuzz(const char* dir, int seeds); int vif_fuzz(int rounds); }   // vu1.cpp

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace {
// A crash prints where it happened (symbols come from twonk.pdb) instead of vanishing.
LONG WINAPI crash_report(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    std::fprintf(stderr, "\n[crash] exception %08lx at %p", er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        std::fprintf(stderr, " (%s %p)", er->ExceptionInformation[0] ? "writing" : "reading", (void*)er->ExceptionInformation[1]);
    std::fprintf(stderr, " on thread %lu\n", GetCurrentThreadId());
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f{};
    f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) || !f.AddrPC.Offset)
            break;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof line;
        DWORD ldisp = 0;
        const bool have_sym = SymFromAddr(proc, f.AddrPC.Offset, &disp, sym) != FALSE;
        const bool have_line = SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &line) != FALSE;
        std::fprintf(stderr, "[crash]   %s+0x%llx", have_sym ? sym->Name : "?", (unsigned long long)disp);
        if (have_line) std::fprintf(stderr, "  %s:%lu", line.FileName, line.LineNumber);
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void f_00100008(ps2::Context* ctx);   // ELF entry point (generated)

namespace ps2 {

static unsigned g_log_mask = 1u << (int)Log::Boot;
static const char* const kLogNames[] = {"kernel", "mem", "cpu", "hle", "boot", "iop", "gpu"};
constexpr int kLogCount = 7;

bool log_enabled(Log ch) { return (g_log_mask >> (int)ch) & 1u; }

void log(Log ch, const char* fmt, ...) {
    if (!log_enabled(ch)) return;
    std::fprintf(stderr, "[%s] ", kLogNames[(int)ch]);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

uint8_t* guest_ptr(Context* ctx, uint32_t addr, uint32_t size) {
    uint8_t* p = host_ptr(ctx, addr);
    if (!p || !host_ptr(ctx, addr + size - 1)) fatal(ctx, addr, "guest_ptr: address is not RAM/scratchpad");
    return p;
}

std::string guest_string(Context* ctx, uint32_t addr, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        const uint8_t* p = host_ptr(ctx, addr + (uint32_t)i);
        if (!p || !*p) break;
        s += (char)*p;
    }
    return s;
}

} // namespace ps2

static bool load_elf_into_ram(const char* path, ps2::Context* ctx) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 0x34 || std::memcmp(b.data(), "\x7f" "ELF", 4) != 0) return false;
    uint32_t phoff;
    uint16_t phnum;
    std::memcpy(&phoff, &b[0x1C], 4);
    std::memcpy(&phnum, &b[0x2C], 2);
    for (uint16_t i = 0; i < phnum; ++i) {
        uint32_t ph[8];
        std::memcpy(ph, &b[phoff + i * 32u], sizeof ph);
        const uint32_t type = ph[0], off = ph[1], vaddr = ph[2], filesz = ph[4], memsz = ph[5];
        if (type != 1 || !memsz) continue;
        if ((uint64_t)vaddr + memsz > ps2::kRamSize || (uint64_t)off + filesz > b.size()) return false;
        std::memcpy(ctx->ram + vaddr, &b[off], filesz);
        std::memset(ctx->ram + vaddr + filesz, 0, memsz - filesz);
        ps2::log(ps2::Log::Boot, "loaded segment %08x-%08x", vaddr, vaddr + memsz);
    }
    return true;
}

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(crash_report);
    if (argc >= 4 && !std::strcmp(argv[1], "--vu-aot"))   // --vu-aot <TWONK_VU_DUMP dir> <out.inc>
        return ps2::gpu::vu1_aot_generate(argv[2], argv[3]);
    if (argc >= 2 && !std::strcmp(argv[1], "--vif-fuzz")) return ps2::gpu::vif_fuzz(argc >= 3 ? std::atoi(argv[2]) : 20000);
    if (argc >= 3 && !std::strcmp(argv[1], "--vu-fuzz"))   // --vu-fuzz <TWONK_VU_DUMP dir> [seeds]
        return ps2::gpu::vu1_fuzz(argv[2], argc >= 4 ? std::atoi(argv[3]) : 4);
    if (argc >= 3 && !std::strcmp(argv[1], "--vu-bench")) {   // --vu-bench <snapshot> [runs [profile-seconds]]
        if (argc >= 5) {
            ps2::game_thread_register();
            ps2::profiler_start(0.0, std::atof(argv[4]));
        }
        const int rc = ps2::gpu::vu1_bench(argv[2], argc >= 4 ? std::atoi(argv[3]) : 2000);
        if (argc >= 5) std::this_thread::sleep_for(std::chrono::duration<double>(std::atof(argv[4]) + 2.0));
        return rc;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: twonk <SLUS_208.87> [--log kernel,mem,cpu,hle,boot,iop|all]\n");
        return 2;
    }
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--log")) {
            const std::string s = argv[i + 1];
            for (int c = 0; c < ps2::kLogCount; ++c)
                if (s.find(ps2::kLogNames[c]) != std::string::npos || s == "all") ps2::g_log_mask |= 1u << c;
        }

    {   // --no-window, --shot <file.png>:<frame>[,<frame>...]  ('#' in the name becomes the frame number)
        bool window = true;
        const char* shot = nullptr;
        const char* shot_frames = nullptr;
        static std::string shot_path;
        for (int i = 2; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--no-window")) window = false;
            if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) {   // --profile [<delay>:]<seconds>
                const char* spec = argv[i + 1];
                const char* colon = std::strchr(spec, ':');
                ps2::profiler_start(colon ? std::atof(spec) : 0.0, std::atof(colon ? colon + 1 : spec));
            }
            if (!std::strcmp(argv[i], "--input") && i + 1 < argc) ps2::pad_script(argv[i + 1]);
            if (!std::strcmp(argv[i], "--renderer") && i + 1 < argc) ps2::renderer_config(argv[i + 1], 0);   // hw | sw
            if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) ps2::video_cli(argv[i + 1], nullptr, false);   // 1-8, 0 = auto
            if (!std::strcmp(argv[i], "--aspect") && i + 1 < argc) ps2::video_cli(nullptr, argv[i + 1], false);  // 4:3 16:9 16:10 21:9 stretch
            if (!std::strcmp(argv[i], "--fullscreen")) ps2::video_cli(nullptr, nullptr, true);
            if (!std::strcmp(argv[i], "--memcard") && i + 1 < argc) ps2::memcard_config(argv[i + 1]);
            if (!std::strcmp(argv[i], "--sif-latency") && i + 1 < argc) ps2::sif_latency_config(std::atof(argv[i + 1]));
            if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) {
                shot_path = argv[i + 1];
                const size_t colon = shot_path.rfind(':');
                if (colon != std::string::npos && colon > 1) {
                    shot_path[colon] = 0;
                    shot_frames = shot_path.c_str() + colon + 1;
                }
                shot = shot_path.c_str();
            }
        }
        ps2::display_config(window, shot, shot_frames);
    }

    static ps2::Context ctx;   // large; keep off the stack
    std::memset(&ctx, 0, sizeof ctx);
    ctx.ram = static_cast<uint8_t*>(std::calloc(ps2::kRamSize, 1));
    ctx.spr = static_cast<uint8_t*>(std::calloc(ps2::kSprSize, 1));
    ctx.vf[0].f[3] = 1.0f;   // vf0 = (0, 0, 0, 1)
    ctx.cop0[12] = 0x70010001u;   // Status as ExecPS2 leaves it: COP0/1/2 usable, EIE and IE set (interrupts on)

    if (!load_elf_into_ram(argv[1], &ctx)) {
        std::fprintf(stderr, "error: cannot load %s\n", argv[1]);
        return 1;
    }
    {
        std::string root = argv[1];   // the disc root is the directory holding the ELF
        const size_t slash = root.find_last_of("/\\");
        root = slash == std::string::npos ? std::string(".") : root.substr(0, slash);
        ps2::iop_init(root);
    }
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--iso")) {
            std::string err;
            if (!ps2::disc_open(argv[i + 1], &err)) {
                std::fprintf(stderr, "error: %s\n", err.c_str());
                return 1;
            }
            ps2::log(ps2::Log::Boot, "disc: %s", argv[i + 1]);
    }
    ps2::kernel_init(&ctx);
    ps2::game_thread_register();
    // Hang watchdog: no syscall for 5 s means the guest is spinning on memory.
    std::thread([] {
        uint64_t last = ~0ull;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const uint64_t n = ps2::kernel_syscall_count();
            if (n == last) {
                std::fprintf(stderr, "[ps2] watchdog: no syscall for 5 s, guest appears stuck\n");
                ps2::dump_trace();
                ps2::dump_hw_state();
                ps2::dump_game_stack();
                std::_Exit(3);
            }
            last = n;
        }
    }).detach();
    ps2::log(ps2::Log::Boot, "entering guest entry point 00100008");
    f_00100008(&ctx);
    ps2::log(ps2::Log::Boot, "guest entry returned (v0=%08x)", ctx.r[2].u32[0]);
    return 0;
}
