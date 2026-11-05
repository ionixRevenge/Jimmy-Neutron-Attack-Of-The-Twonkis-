#include "analysis.hpp"

#include <algorithm>
#include <set>

#include "r5900.hpp"

namespace recomp {

using r5900::Op;

std::vector<Function> refine_functions(const Image& img, const std::vector<Function>& in, RefineStats* st,
                                       const std::set<uint32_t>& forced, const std::set<uint32_t>& soft) {
    *st = RefineStats{};
    st->input = in.size();
    if (in.empty()) return {};

    std::vector<Function> fns = in;
    std::sort(fns.begin(), fns.end(), [](const Function& a, const Function& b) { return a.start < b.start; });
    // Tail calls: a `j` that leaves its function targets a real entry point.
    std::set<uint32_t> entries(forced.begin(), forced.end());
    for (const auto& fn : fns)
        for (uint32_t pc = fn.start; pc < fn.end; pc += 4) {
            const auto insn = r5900::decode(pc, img.read32(pc));
            if (insn.op != Op::J && insn.op != Op::JAL) continue;
            const uint32_t t = insn.jump_target();
            if (insn.op == Op::JAL || t < fn.start || t >= fn.end)   // calls always; jumps when they leave
                if (t >= fns.front().start && t < fns.back().end) entries.insert(t);
        }
    // split candidates at forced and soft entries
    std::set<uint32_t> splits(entries);
    for (uint32_t a : soft)
        if (a >= fns.front().start && a < fns.back().end) splits.insert(a);
    for (uint32_t a : splits) {
        for (size_t i = 0; i < fns.size(); ++i) {
            if (a > fns[i].start && a < fns[i].end) {
                Function tail{a, fns[i].end};
                fns[i].end = a;
                fns.insert(fns.begin() + i + 1, tail);
                break;
            }
        }
    }

    // A boundary can never fall in a delay slot: merge such a piece into its predecessor.
    for (size_t i = 1; i < fns.size();) {
        const uint32_t a = fns[i].start;
        if (fns[i - 1].end == a && !entries.count(a) && r5900::has_delay_slot(r5900::decode(a - 4, img.read32(a - 4)).op)) {
            fns[i - 1].end = fns[i].end;
            fns.erase(fns.begin() + i);
            entries.erase(a);
            ++st->merges;
        } else {
            ++i;
        }
    }

    // Strong entries: program entry and every jal target.
    std::set<uint32_t> strong{img.entry()};
    strong.insert(entries.begin(), entries.end());
    for (const auto& fn : fns)
        for (uint32_t pc = fn.start; pc < fn.end; pc += 4) {
            const auto insn = r5900::decode(pc, img.read32(pc));
            if (insn.op == Op::JAL) strong.insert(insn.jump_target());
        }
    st->strong = strong.size();

    // Work on contiguous runs: fns[i].end == fns[i+1].start inside a code region.
    auto find = [&](uint32_t a) -> long {
        auto it = std::upper_bound(fns.begin(), fns.end(), a, [](uint32_t v, const Function& f) { return v < f.start; });
        if (it == fns.begin()) return -1;
        --it;
        return (a < it->end) ? (long)(it - fns.begin()) : -1;
    };

    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < fns.size(); ++i) {
            for (uint32_t pc = fns[i].start; pc < fns[i].end; pc += 4) {
                const auto insn = r5900::decode(pc, img.read32(pc));
                uint32_t t;
                if (r5900::is_branch(insn.op) && !r5900::is_call(insn.op)) t = insn.branch_target();
                else if (insn.op == Op::J) t = insn.jump_target();
                else continue;
                if (t >= fns[i].start && t < fns[i].end) continue;   // internal
                const long j = find(t);
                if (j < 0) continue;
                if (fns[j].start == t && (strong.count(t) || insn.op == Op::J)) continue;  // tail call to an entry
                // merge the run [lo, hi] if every boundary inside it is weak and contiguous
                const size_t lo = std::min(i, (size_t)j), hi = std::max(i, (size_t)j);
                bool ok = true;
                for (size_t k = lo + 1; k <= hi && ok; ++k)
                    ok = !strong.count(fns[k].start) && fns[k - 1].end == fns[k].start;
                if (!ok) continue;
                fns[lo].end = fns[hi].end;
                fns.erase(fns.begin() + lo + 1, fns.begin() + hi + 1);
                st->merges += hi - lo;
                changed = true;
                i = lo;
                pc = fns[i].start - 4;   // rescan merged function
            }
        }
    }

    for (const auto& fn : fns)
        for (uint32_t pc = fn.start; pc < fn.end; pc += 4) {
            const auto insn = r5900::decode(pc, img.read32(pc));
            uint32_t t;
            if (r5900::is_branch(insn.op) && !r5900::is_call(insn.op)) t = insn.branch_target();
            else if (insn.op == Op::J) t = insn.jump_target();
            else continue;
            if (t >= fn.start && t < fn.end) continue;
            const long j = find(t);
            if (j >= 0 && fns[j].start == t) continue;
            ++st->residual_cross;
        }
    for (uint32_t a : soft) {
        if (strong.count(a)) continue;
        const long j = find(a);
        if (j < 0) continue;
        if (fns[j].start == a) ++st->soft_kept;
        else ++st->soft_lost;
    }
    st->output = fns.size();
    return fns;
}

} // namespace recomp
