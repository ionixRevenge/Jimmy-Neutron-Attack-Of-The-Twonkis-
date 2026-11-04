// twonk.exe: loads SLUS_208.87 into guest RAM and runs the recompiled code.
//
//   twonk <SLUS_208.87> [--log kernel,mem,cpu,hle,boot]
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

void f_00100008(ps2::Context* ctx);   // ELF entry point (generated)

namespace ps2 {

static unsigned g_log_mask = 1u << (int)Log::Boot;

bool log_enabled(Log ch) { return (g_log_mask >> (int)ch) & 1u; }

void log(Log ch, const char* fmt, ...) {
    if (!log_enabled(ch)) return;
    static const char* const kNames[] = {"kernel", "mem", "cpu", "hle", "boot"};
    std::fprintf(stderr, "[%s] ", kNames[(int)ch]);
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
    if (argc < 2) {
        std::fprintf(stderr, "usage: twonk <SLUS_208.87> [--log kernel,mem,cpu,hle,boot]\n");
        return 2;
    }
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--log")) {
            const std::string s = argv[i + 1];
            static const char* const kNames[] = {"kernel", "mem", "cpu", "hle", "boot"};
            for (int c = 0; c < 5; ++c)
                if (s.find(kNames[c]) != std::string::npos || s == "all") ps2::g_log_mask |= 1u << c;
        }

    static ps2::Context ctx;   // large; keep off the stack
    std::memset(&ctx, 0, sizeof ctx);
    ctx.ram = static_cast<uint8_t*>(std::calloc(ps2::kRamSize, 1));
    ctx.spr = static_cast<uint8_t*>(std::calloc(ps2::kSprSize, 1));
    ctx.vf[0].f[3] = 1.0f;   // vf0 = (0, 0, 0, 1)
    ctx.cop0[12] = 0x70400004u;   // Status after BIOS: COP0/1/2 usable, kernel mode

    if (!load_elf_into_ram(argv[1], &ctx)) {
        std::fprintf(stderr, "error: cannot load %s\n", argv[1]);
        return 1;
    }
    ps2::kernel_init(&ctx);
    ps2::log(ps2::Log::Boot, "entering guest entry point 00100008");
    f_00100008(&ctx);
    ps2::log(ps2::Log::Boot, "guest entry returned (v0=%08x)", ctx.r[2].u32[0]);
    return 0;
}
