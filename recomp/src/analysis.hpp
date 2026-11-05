// Function boundary refinement.
#pragma once
#include <cstdint>
#include <set>
#include <vector>

#include "image.hpp"

namespace recomp {

struct RefineStats {
    size_t input = 0, output = 0;
    size_t strong = 0;          // entry point + jal targets
    size_t merges = 0;
    size_t residual_cross = 0;  // branches still leaving a function to a non-entry
    size_t soft_kept = 0, soft_lost = 0;   // soft entries that survived / were merged away
};

// Starting from candidate functions, merge weak boundaries (starts that are not
// jal targets) whenever intra-procedural branches cross them. Iterates to a fixpoint.
// Branching out of a function to a non-entry address would otherwise need a tail
// call into the middle of code, which turns loops into unbounded recursion.
// `forced` addresses (e.g. HLE replacements) always become function entries, split out of
// whatever candidate contains them, and are never merged away.
// `soft` addresses are probable entries (guessed from code shape): they are split
// out like forced ones but stay weak, so a crossing branch merges them back.
std::vector<Function> refine_functions(const Image& img, const std::vector<Function>& in, RefineStats* st,
                                       const std::set<uint32_t>& forced = {}, const std::set<uint32_t>& soft = {});

} // namespace recomp
