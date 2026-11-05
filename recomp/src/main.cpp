// twonkrecomp: static recompiler for SLUS_208.87.
//
//   twonkrecomp disasm   <elf> <addr_hex> [count]
//   twonkrecomp coverage <elf> <regions.txt>
//   twonkrecomp gen      <elf> <regions.txt> <functions.tsv> <out_dir> [hle.txt]
//   twonkrecomp fn       <elf> <regions.txt> <functions.tsv> <addr_hex>   (print one function)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include <algorithm>

#include "analysis.hpp"
#include "codegen.hpp"
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

// Functions that lie entirely inside EE code regions.
static std::vector<Function> ee_functions(const std::vector<Function>& all, const std::vector<Region>& regions) {
    std::vector<Function> out;
    for (const auto& fn : all)
        for (const auto& r : regions)
            if (r.kind == "ee" && fn.start >= r.start && fn.start < r.end) {
                Function g = fn;
                if (g.end > r.end) g.end = r.end;
                out.push_back(g);
                break;
            }
    return out;
}

// Code addresses stored in data regions: jump tables, vtables, callbacks.
static std::vector<uint32_t> data_code_pointers(const Image& img, const std::vector<Region>& regions) {
    uint32_t code_lo = 0xFFFFFFFF, code_hi = 0;
    for (const auto& r : regions)
        if (r.kind == "ee") { code_lo = std::min(code_lo, r.start); code_hi = std::max(code_hi, r.end); }
    std::vector<uint32_t> out;
    for (const auto& r : regions) {
        if (r.kind != "data") continue;
        for (uint32_t a = r.start & ~3u; a + 4 <= r.end; a += 4) {
            const uint32_t v = img.read32(a);
            if (v >= code_lo && v < code_hi && (v & 3) == 0) out.push_back(v);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// A code address right after a "jr ra; <delay slot>" pair starts a function,
// even without a stack-allocating prologue (leaf functions).
static bool follows_return(const Image& img, uint32_t a) {
    return img.contains(a - 8) && img.read32(a - 8) == 0x03E00008u;
}

// Weakest shape: right after any unconditional jump (j / jr) and its delay slot,
// optionally followed by nop padding. Used only for soft entries.
static bool follows_jump(const Image& img, uint32_t a) {
    for (uint32_t pad = 0; pad <= 4; ++pad) {
        if (pad && (!img.contains(a - 4 * pad) || img.read32(a - 4 * pad) != 0)) return false;
        const uint32_t j = a - 4 * (pad + 2);
        if (!img.contains(j)) return false;
        const uint32_t w = img.read32(j);
        if ((w & 0xFC1FFFFFu) == 0x00000008u || (w >> 26) == 2 || (w >> 16) == 0x1000) return true;   // jr, j, b
    }
    return false;
}

// Stricter: "jr ra; <delay slot>; nop..." with at least one alignment nop. The
// compiler pads between functions, never before a jump-table case label.
static bool follows_padded_return(const Image& img, uint32_t a, bool any_jr = false) {
    for (uint32_t pad = 1; pad <= 4; ++pad) {   // jr ra at a - 4*(pad+2), delay slot, then `pad` nops
        if (!img.contains(a - 4 * pad) || img.read32(a - 4 * pad) != 0) return false;
        const uint32_t jr = a - 4 * (pad + 2);
        if (!img.contains(jr)) return false;
        const uint32_t w = img.read32(jr);
        // jr ra; with any_jr also register tail calls. Never `j` (loop heads get
        // padded too), and data pointers never use any_jr (switch dispatch is a
        // `jr` followed by padded case labels).
        if (w == 0x03E00008u || (any_jr && (w & 0xFC1FFFFFu) == 0x00000008u)) return true;
    }
    return false;
}

// Code addresses built in registers with lui + addiu/ori (callbacks registered
// at run time, e.g. sifcmd handlers). Only addresses that begin a function
// shape are kept, so data/jump-table addresses are never mistaken for entries.
static std::vector<uint32_t> code_constructed_pointers(const Image& img, const std::vector<Region>& regions, bool loose = false) {
    auto in_code = [&](uint32_t v) {
        for (const auto& r : regions)
            if (r.kind == "ee" && v >= r.start && v < r.end) return true;
        return false;
    };
    std::vector<uint32_t> out;
    for (const auto& r : regions) {
        if (r.kind != "ee") continue;
        for (uint32_t pc = r.start; pc < r.end; pc += 4) {
            const auto hi = r5900::decode(pc, img.read32(pc));
            if (hi.op != r5900::Op::LUI || hi.rt() == 0) continue;
            // Scan forward from `from` for the low half that completes this lui.
            auto scan = [&](uint32_t from) {
                for (uint32_t k = 0; k < 24 && from + 4 * k < r.end; ++k) {
                    const auto lo = r5900::decode(from + 4 * k, img.read32(from + 4 * k));
                    if (lo.op == r5900::Op::JR) break;   // the pair never spans a return
                    if ((lo.op == r5900::Op::ADDIU || lo.op == r5900::Op::ORI) && lo.rs() == hi.rt()) {
                        const uint32_t v = (uint32_t)hi.imm() << 16;
                        const uint32_t a = lo.op == r5900::Op::ORI ? (v | lo.imm()) : (uint32_t)(v + lo.simm());
                        if ((a & 3) == 0 && in_code(a) && (loose ? follows_jump(img, a) : follows_return(img, a) || follows_padded_return(img, a, true) || ((img.read32(a) >> 16) == 0x27BD && (img.read32(a) & 0x8000)))) out.push_back(a);
                        break;
                    }
                    // register overwritten (approximate: an rt field that is a destination)
                    const bool writes_rt = lo.op != r5900::Op::SW && lo.op != r5900::Op::SD && lo.op != r5900::Op::SQ &&
                                           lo.op != r5900::Op::J && lo.op != r5900::Op::JAL && !r5900::is_branch(lo.op);
                    if (writes_rt && lo.rt() == hi.rt()) break;
                }
            };
            scan(pc + 4);
            // A lui in a branch delay slot also feeds the code at the branch target.
            if (pc > r.start) {
                const auto br = r5900::decode(pc - 4, img.read32(pc - 4));
                if (r5900::is_branch(br.op) && !r5900::is_call(br.op)) scan(br.branch_target());
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
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
    if ((cmd == "gen" && argc >= 6) || (cmd == "fn" && argc >= 6)) {
        std::vector<Region> regions;
        std::vector<Function> all;
        if (!load_regions(argv[3], &regions, &err) || !load_functions(argv[4], &all, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        // optional HLE override list: "<addr_hex> <native_name>" per line, '#' comments
        std::map<uint32_t, std::string> hle;
        if (argc >= 7) {
            std::ifstream hf(argv[6]);
            if (!hf) { std::fprintf(stderr, "error: cannot open %s\n", argv[6]); return 1; }
            std::string line;
            while (std::getline(hf, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream ss(line);
                std::string a, name;
                if (ss >> a >> name) hle[(uint32_t)std::stoul(a, nullptr, 16)] = name;
            }
        }
        std::set<uint32_t> forced;   // HLE replacements are always function entries
        for (const auto& kv : hle) forced.insert(kv.first);
        RefineStats rs;
        // Code pointers stored in data that land on a stack-allocating prologue
        // (ctor tables, vtables, callbacks) are function entries.
        const auto ptrs = data_code_pointers(img, regions);
        for (uint32_t p : ptrs)
            if (((img.read32(p) >> 16) == 0x27BD && (img.read32(p) & 0x8000)) || follows_padded_return(img, p)) forced.insert(p);
        const auto cptrs = code_constructed_pointers(img, regions);
        forced.insert(cptrs.begin(), cptrs.end());
        std::fprintf(stderr, "code pointers: %zu in data, %zu built in registers\n", ptrs.size(), cptrs.size());
        // explicit function-pointer tables (e.g. .ctors): every non-null word is an entry
        for (const auto& r : regions)
            if (r.kind == "ptrs")
                for (uint32_t a = r.start; a + 4 <= r.end; a += 4)
                    if (uint32_t v = img.read32(a)) forced.insert(v);
        // EE regions the candidate list knows nothing about get one candidate spanning them
        auto cands = ee_functions(all, regions);
        for (const auto& r : regions) {
            if (r.kind != "ee") continue;
            const bool covered = std::any_of(cands.begin(), cands.end(), [&](const Function& f) { return f.start == r.start; });
            if (covered || std::any_of(cands.begin(), cands.end(), [&](const Function& f) { return f.start < r.start && f.end > r.start; }))
                continue;
            uint32_t end = r.end;   // stop at the first known function inside the region
            for (const auto& f : cands)
                if (f.start > r.start && f.start < end) end = f.start;
            cands.push_back({r.start, end});
        }
        // Soft entries: code pointers (data or register-built) that follow a jump.
        std::set<uint32_t> soft;
        for (uint32_t p : ptrs)
            if (!forced.count(p) && follows_jump(img, p)) soft.insert(p);
        for (uint32_t p : code_constructed_pointers(img, regions, true))
            if (!forced.count(p)) soft.insert(p);
        auto funcs = refine_functions(img, cands, &rs, forced, soft);
        std::fprintf(stderr, "soft entries: %zu kept, %zu merged away\n", rs.soft_kept, rs.soft_lost);
        std::fprintf(stderr, "functions: %zu candidates, %zu strong entries, %zu merges -> %zu functions, %zu residual cross-branches\n",
                     rs.input, rs.strong, rs.merges, rs.output, rs.residual_cross);
        CodeGen gen(img, funcs, data_code_pointers(img, regions), hle);
        GenStats st;
        if (cmd == "fn") {
            const uint32_t a = (uint32_t)std::strtoul(argv[5], nullptr, 16);
            for (const auto& fn : funcs)
                if (fn.start == a) {
                    std::string out;
                    gen.emit_function(fn, out, st);
                    std::fputs(out.c_str(), stdout);
                    return 0;
                }
            std::fprintf(stderr, "no function starts at %08x\n", a);
            return 1;
        }
        GenOptions opt;
        opt.out_dir = argv[5];
        if (!gen.generate(opt, &st, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::printf("functions %llu, instructions %llu: inlined %llu (%.1f%%), runtime hooks %llu (%.1f%%), invalid %llu\n"
                    "external branches %llu, indirect jumps %llu, control-flow anomalies %llu\n",
                    (unsigned long long)st.functions, (unsigned long long)st.instructions,
                    (unsigned long long)st.inlined, 100.0 * st.inlined / st.instructions,
                    (unsigned long long)st.hooked, 100.0 * st.hooked / st.instructions,
                    (unsigned long long)st.invalid, (unsigned long long)st.external_branches,
                    (unsigned long long)st.indirect_jumps, (unsigned long long)st.branch_in_delay);
        return 0;
    }
    return usage();
}
