// C++ code generation from decoded R5900 functions.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "image.hpp"

namespace recomp {

struct GenStats {
    uint64_t functions = 0, instructions = 0;
    uint64_t inlined = 0;          // emitted as plain C++
    uint64_t hooked = 0;           // emitted as ps2::exec<Op::X>
    uint64_t invalid = 0;
    uint64_t external_branches = 0;   // branch/j to a non-function-start outside the function
    uint64_t indirect_jumps = 0;      // jr other than ra
    uint64_t branch_in_delay = 0;
};

struct GenOptions {
    std::string out_dir;
    size_t functions_per_file = 400;
};

class CodeGen {
public:
    CodeGen(const Image& img, std::vector<Function> funcs, const std::vector<uint32_t>& data_code_ptrs);

    bool generate(const GenOptions& opt, GenStats* stats, std::string* err);

    // Emit one function's C++ body into out (exposed for tests / single-function dumps).
    void emit_function(const Function& fn, std::string& out, GenStats& st) const;

private:
    void emit_insn(const Function& fn, uint32_t pc, std::string& out, GenStats& st, bool in_delay) const;
    void emit_control(const Function& fn, uint32_t pc, std::string& out, GenStats& st) const;
    std::string target_code(const Function& fn, uint32_t target, GenStats& st) const;
    bool is_func_start(uint32_t a) const { return starts_.count(a) != 0; }

    const Image& img_;
    std::vector<Function> funcs_;
    std::set<uint32_t> starts_;
    std::vector<uint32_t> data_code_ptrs_;   // sorted; code addresses stored in data (jump tables, vtables)
};

std::string fn_name(uint32_t addr);

} // namespace recomp
