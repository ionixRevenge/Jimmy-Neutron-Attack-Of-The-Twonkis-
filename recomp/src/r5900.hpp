// R5900 (PS2 Emotion Engine) instruction decoder.
#pragma once
#include <cstdint>
#include <string>

namespace r5900 {

enum class Op : uint16_t {
    Invalid = 0,
#define OP(name, mnem, fmt) name,
#include "ps2/r5900_ops.inc"
#undef OP
    Count
};

// Operand layout classes; used for disassembly and generic operand handling.
enum class Fmt : uint8_t {
    None, Code, R3, RsRt, RdRs, RdRt, Rd, Rs, Shift, ShiftV, ImmS, ImmU, Lui, RsImm,
    Mem, MemOp, MemF, MemV, Br1, Br2, BrC, Jump, PmfhlFmt,
    Cop0Move, FMove, FCtrl, F3, FdFs, FdFt, FsFt, V2Move, V2Ctrl,
    VBc, VQ, V3, V2, VABc, VA2, VA3, VI3, VIImm, VCall, VClip, VLoadStore, VDivFmt,
    VMtir, VMfir, VIlwr, VR, VRIn,
};

struct Insn {
    uint32_t pc = 0;
    uint32_t raw = 0;
    Op op = Op::Invalid;

    // raw fields (always filled)
    uint8_t rs() const { return (raw >> 21) & 31; }
    uint8_t rt() const { return (raw >> 16) & 31; }
    uint8_t rd() const { return (raw >> 11) & 31; }
    uint8_t sa() const { return (raw >> 6) & 31; }
    uint8_t funct() const { return raw & 63; }
    uint16_t imm() const { return raw & 0xFFFF; }
    int32_t simm() const { return (int16_t)(raw & 0xFFFF); }
    // FPU naming: fs = rd field, ft = rt field, fd = sa field
    uint8_t ft() const { return rt(); }
    uint8_t fs() const { return rd(); }
    uint8_t fd() const { return sa(); }
    // VU macro: dest mask xyzw = bits 24..21, bc = bits 1..0
    uint8_t vdest() const { return (raw >> 21) & 15; }
    uint8_t vbc() const { return raw & 3; }

    uint32_t branch_target() const { return pc + 4 + (simm() << 2); }
    uint32_t jump_target() const { return ((pc + 4) & 0xF0000000u) | ((raw & 0x03FFFFFFu) << 2); }

    bool valid() const { return op != Op::Invalid; }
};

Insn decode(uint32_t pc, uint32_t raw);

const char* mnemonic(Op op);
Fmt format(Op op);

// Control-flow properties
bool is_branch(Op op);         // conditional PC-relative branch (has delay slot)
bool is_likely(Op op);         // branch-likely: delay slot nullified when not taken
bool is_jump(Op op);           // J/JAL/JR/JALR
bool has_delay_slot(Op op);
bool is_call(Op op);           // JAL/JALR/xxxAL branches (write return address)

std::string disasm(const Insn& in);

} // namespace r5900
