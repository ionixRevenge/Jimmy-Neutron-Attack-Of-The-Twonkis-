// Function boundary refinement.
#pragma once
#include <cstdint>
#include <vector>

#include "image.hpp"

namespace recomp {

struct RefineStats {
    size_t input = 0, output = 0;
    size_t strong = 0;          // entry point + jal targets
    size_t merges = 0;
    size_t residual_cross = 0;  // branches still leaving a function to a non-entry
};

// Starting from candidate functions, merge weak boundaries (starts that are not
// jal targets) whenever intra-procedural branches cross them. Iterates to a fixpoint.
// Branching out of a function to a non-entry address would otherwise need a tail
// call into the middle of code, which turns loops into unbounded recursion.
std::vector<Function> refine_functions(const Image& img, const std::vector<Function>& in, RefineStats* st);

} // namespace recomp
