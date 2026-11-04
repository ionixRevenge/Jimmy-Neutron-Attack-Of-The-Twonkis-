// twonkrecomp: static recompiler for SLUS_208.87.
//
//   twonkrecomp disasm   <elf> <addr_hex> [count]
//   twonkrecomp coverage <elf> <regions.txt>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "image.hpp"
#include "r5900.hpp"

using namespace recomp;

static int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  twonkrecomp disasm   <elf> <addr_hex> [count]\n"
                 "  twonkrecomp coverage <elf> <regions.txt>\n");
    return 2;
}

static int cmd_disasm(const Image& img, uint32_t addr, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i, addr += 4) {
        if (!img.contains(addr)) break;
        const auto in = r5900::decode(addr, img.read32(addr));
        std::printf("%08x  %08x  %s\n", addr, in.raw, r5900::disasm(in).c_str());
    }
    return 0;
}

// Decode every word of every EE region; report undecodable words and the mnemonic mix.
static int cmd_coverage(const Image& img, const std::vector<Region>& regions) {
    uint64_t total = 0, invalid = 0;
    std::map<std::string, uint64_t> mix;
    for (const auto& r : regions) {
        if (r.kind != "ee") continue;
        uint64_t n = 0, bad = 0;
        uint32_t first_bad = 0;
        for (uint32_t a = r.start; a < r.end; a += 4) {
            const uint32_t w = img.read32(a);
            ++n;
            if (w == 0) continue;  // nop / padding
            const auto in = r5900::decode(a, w);
            if (!in.valid()) {
                if (!bad) first_bad = a;
                ++bad;
                continue;
            }
            ++mix[r5900::mnemonic(in.op)];
        }
        total += n;
        invalid += bad;
        std::printf("%08x-%08x %8llu words %6llu undecodable (%.3f%%)%s  %s\n", r.start, r.end,
                    (unsigned long long)n, (unsigned long long)bad, n ? 100.0 * bad / n : 0.0,
                    bad ? (" first@" + std::to_string(first_bad)).c_str() : "", r.label.substr(0, 48).c_str());
    }
    std::printf("TOTAL %llu words, %llu undecodable (%.4f%%), %zu distinct mnemonics\n",
                (unsigned long long)total, (unsigned long long)invalid, total ? 100.0 * invalid / total : 0.0,
                mix.size());
    std::multimap<uint64_t, std::string, std::greater<uint64_t>> by_count;
    for (const auto& kv : mix) by_count.emplace(kv.second, kv.first);
    int i = 0;
    for (const auto& kv : by_count) {
        std::printf("%-10s %8llu%s", kv.second.c_str(), (unsigned long long)kv.first, (++i % 6) ? "  " : "\n");
    }
    std::printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    Image img;
    std::string err;
    if (!img.load_elf(argv[2], &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (cmd == "disasm" && argc >= 4) {
        return cmd_disasm(img, (uint32_t)std::strtoul(argv[3], nullptr, 16),
                          argc >= 5 ? (uint32_t)std::strtoul(argv[4], nullptr, 0) : 32);
    }
    if (cmd == "coverage" && argc >= 4) {
        std::vector<Region> regions;
        if (!load_regions(argv[3], &regions, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        return cmd_coverage(img, regions);
    }
    return usage();
}
