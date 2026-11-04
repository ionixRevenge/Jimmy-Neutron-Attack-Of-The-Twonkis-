#include "r5900.hpp"

#include <cstdio>

namespace r5900 {

namespace {

struct OpInfo { const char* mnem; Fmt fmt; };

const OpInfo kInfo[] = {
    {"(invalid)", Fmt::None},
#define OP(name, mnem, fmt) {mnem, Fmt::fmt},
#include "ps2/r5900_ops.inc"
#undef OP
};

using O = Op;
constexpr O X = O::Invalid;

const O kPrimary[64] = {
    X, X, O::J, O::JAL, O::BEQ, O::BNE, O::BLEZ, O::BGTZ,
    O::ADDI, O::ADDIU, O::SLTI, O::SLTIU, O::ANDI, O::ORI, O::XORI, O::LUI,
    X, X, X, X, O::BEQL, O::BNEL, O::BLEZL, O::BGTZL,
    O::DADDI, O::DADDIU, O::LDL, O::LDR, X, X, O::LQ, O::SQ,
    O::LB, O::LH, O::LWL, O::LW, O::LBU, O::LHU, O::LWR, O::LWU,
    O::SB, O::SH, O::SWL, O::SW, O::SDL, O::SDR, O::SWR, O::CACHE,
    X, O::LWC1, X, O::PREF, X, X, O::LQC2, O::LD,
    X, O::SWC1, X, X, X, X, O::SQC2, O::SD,
};

const O kSpecial[64] = {
    O::SLL, X, O::SRL, O::SRA, O::SLLV, X, O::SRLV, O::SRAV,
    O::JR, O::JALR, O::MOVZ, O::MOVN, O::SYSCALL, O::BREAK, X, O::SYNC,
    O::MFHI, O::MTHI, O::MFLO, O::MTLO, O::DSLLV, X, O::DSRLV, O::DSRAV,
    O::MULT, O::MULTU, O::DIV, O::DIVU, X, X, X, X,
    O::ADD, O::ADDU, O::SUB, O::SUBU, O::AND, O::OR, O::XOR, O::NOR,
    O::MFSA, O::MTSA, O::SLT, O::SLTU, O::DADD, O::DADDU, O::DSUB, O::DSUBU,
    O::TGE, O::TGEU, O::TLT, O::TLTU, O::TEQ, X, O::TNE, X,
    O::DSLL, X, O::DSRL, O::DSRA, O::DSLL32, X, O::DSRL32, O::DSRA32,
};

const O kRegimm[32] = {
    O::BLTZ, O::BGEZ, O::BLTZL, O::BGEZL, X, X, X, X,
    O::TGEI, O::TGEIU, O::TLTI, O::TLTIU, O::TEQI, X, O::TNEI, X,
    O::BLTZAL, O::BGEZAL, O::BLTZALL, O::BGEZALL, X, X, X, X,
    O::MTSAB, O::MTSAH, X, X, X, X, X, X,
};

const O kMmi[64] = {
    O::MADD, O::MADDU, X, X, O::PLZCW, X, X, X,
    X /*MMI0*/, X /*MMI2*/, X, X, X, X, X, X,
    O::MFHI1, O::MTHI1, O::MFLO1, O::MTLO1, X, X, X, X,
    O::MULT1, O::MULTU1, O::DIV1, O::DIVU1, X, X, X, X,
    O::MADD1, O::MADDU1, X, X, X, X, X, X,
    X /*MMI1*/, X /*MMI3*/, X, X, X, X, X, X,
    O::PMFHL, O::PMTHL, X, X, O::PSLLH, X, O::PSRLH, O::PSRAH,
    X, X, X, X, O::PSLLW, X, O::PSRLW, O::PSRAW,
};

const O kMmi0[32] = {
    O::PADDW, O::PSUBW, O::PCGTW, O::PMAXW, O::PADDH, O::PSUBH, O::PCGTH, O::PMAXH,
    O::PADDB, O::PSUBB, O::PCGTB, X, X, X, X, X,
    O::PADDSW, O::PSUBSW, O::PEXTLW, O::PPACW, O::PADDSH, O::PSUBSH, O::PEXTLH, O::PPACH,
    O::PADDSB, O::PSUBSB, O::PEXTLB, O::PPACB, X, X, O::PEXT5, O::PPAC5,
};
const O kMmi1[32] = {
    X, O::PABSW, O::PCEQW, O::PMINW, O::PADSBH, O::PABSH, O::PCEQH, O::PMINH,
    X, X, O::PCEQB, X, X, X, X, X,
    O::PADDUW, O::PSUBUW, O::PEXTUW, X, O::PADDUH, O::PSUBUH, O::PEXTUH, X,
    O::PADDUB, O::PSUBUB, O::PEXTUB, O::QFSRV, X, X, X, X,
};
const O kMmi2[32] = {
    O::PMADDW, X, O::PSLLVW, O::PSRLVW, O::PMSUBW, X, X, X,
    O::PMFHI, O::PMFLO, O::PINTH, X, O::PMULTW, O::PDIVW, O::PCPYLD, X,
    O::PMADDH, O::PHMADH, O::PAND, O::PXOR, O::PMSUBH, O::PHMSBH, X, X,
    X, X, O::PEXEH, O::PREVH, O::PMULTH, O::PDIVBW, O::PEXEW, O::PROT3W,
};
const O kMmi3[32] = {
    O::PMADDUW, X, X, O::PSRAVW, X, X, X, X,
    O::PMTHI, O::PMTLO, O::PINTEH, X, O::PMULTUW, O::PDIVUW, O::PCPYUD, X,
    X, X, O::POR, O::PNOR, X, X, X, X,
    X, X, O::PEXCH, O::PCPYH, X, X, O::PEXCW, X,
};

const O kCop1S[64] = {
    O::ADD_S, O::SUB_S, O::MUL_S, O::DIV_S, O::SQRT_S, O::ABS_S, O::MOV_S, O::NEG_S,
    X, X, X, X, X, X, X, X,
    X, X, X, X, X, X, O::RSQRT_S, X,
    O::ADDA_S, O::SUBA_S, O::MULA_S, X, O::MADD_S, O::MSUB_S, O::MADDA_S, O::MSUBA_S,
    X, X, X, X, O::CVT_W_S, X, X, X,
    O::MAX_S, O::MIN_S, X, X, X, X, X, X,
    O::C_F_S, X, O::C_EQ_S, X, O::C_LT_S, X, O::C_LE_S, X,
    X, X, X, X, X, X, X, X,
};

const O kVuSpecial1[64] = {
    O::VADDbc, O::VADDbc, O::VADDbc, O::VADDbc, O::VSUBbc, O::VSUBbc, O::VSUBbc, O::VSUBbc,
    O::VMADDbc, O::VMADDbc, O::VMADDbc, O::VMADDbc, O::VMSUBbc, O::VMSUBbc, O::VMSUBbc, O::VMSUBbc,
    O::VMAXbc, O::VMAXbc, O::VMAXbc, O::VMAXbc, O::VMINIbc, O::VMINIbc, O::VMINIbc, O::VMINIbc,
    O::VMULbc, O::VMULbc, O::VMULbc, O::VMULbc, O::VMULq, O::VMAXi, O::VMULi, O::VMINIi,
    O::VADDq, O::VMADDq, O::VADDi, O::VMADDi, O::VSUBq, O::VMSUBq, O::VSUBi, O::VMSUBi,
    O::VADD, O::VMADD, O::VMUL, O::VMAX, O::VSUB, O::VMSUB, O::VOPMSUB, O::VMINI,
    O::VIADD, O::VISUB, O::VIADDI, X, O::VIAND, O::VIOR, X, X,
    O::VCALLMS, O::VCALLMSR, X, X, X, X, X, X,   // 0x3C-0x3F: Special2
};

const O kVuSpecial2[128] = {
    O::VADDAbc, O::VADDAbc, O::VADDAbc, O::VADDAbc, O::VSUBAbc, O::VSUBAbc, O::VSUBAbc, O::VSUBAbc,
    O::VMADDAbc, O::VMADDAbc, O::VMADDAbc, O::VMADDAbc, O::VMSUBAbc, O::VMSUBAbc, O::VMSUBAbc, O::VMSUBAbc,
    O::VITOF0, O::VITOF4, O::VITOF12, O::VITOF15, O::VFTOI0, O::VFTOI4, O::VFTOI12, O::VFTOI15,
    O::VMULAbc, O::VMULAbc, O::VMULAbc, O::VMULAbc, O::VMULAq, O::VABS, O::VMULAi, O::VCLIPw,
    O::VADDAq, O::VMADDAq, O::VADDAi, O::VMADDAi, O::VSUBAq, O::VMSUBAq, O::VSUBAi, O::VMSUBAi,
    O::VADDA, O::VMADDA, O::VMULA, X, O::VSUBA, O::VMSUBA, O::VOPMULA, O::VNOP,
    O::VMOVE, O::VMR32, X, X, O::VLQI, O::VSQI, O::VLQD, O::VSQD,
    O::VDIV, O::VSQRT, O::VRSQRT, O::VWAITQ, O::VMTIR, O::VMFIR, O::VILWR, O::VISWR,
    O::VRNEXT, O::VRGET, O::VRINIT, O::VRXOR, X, X, X, X,
    // 0x48-0x7F invalid
};

O decode_op(uint32_t w) {
    const uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, fn = w & 63, sa = (w >> 6) & 31;
    switch (op) {
    case 0x00: return kSpecial[fn];
    case 0x01: return kRegimm[rt];
    case 0x1C:
        switch (fn) {
        case 0x08: return kMmi0[sa];
        case 0x09: return kMmi2[sa];
        case 0x28: return kMmi1[sa];
        case 0x29: return kMmi3[sa];
        default: return kMmi[fn];
        }
    case 0x10:  // COP0
        switch (rs) {
        case 0x00: return O::MFC0;
        case 0x04: return O::MTC0;
        case 0x08: { const O b[4] = {O::BC0F, O::BC0T, O::BC0FL, O::BC0TL}; return rt < 4 ? b[rt] : X; }
        case 0x10:
            switch (fn) {
            case 0x01: return O::TLBR;  case 0x02: return O::TLBWI; case 0x06: return O::TLBWR;
            case 0x08: return O::TLBP;  case 0x18: return O::ERET;  case 0x38: return O::EI;
            case 0x39: return O::DI;    default: return X;
            }
        default: return X;
        }
    case 0x11:  // COP1
        switch (rs) {
        case 0x00: return O::MFC1;
        case 0x02: return O::CFC1;
        case 0x04: return O::MTC1;
        case 0x06: return O::CTC1;
        case 0x08: { const O b[4] = {O::BC1F, O::BC1T, O::BC1FL, O::BC1TL}; return rt < 4 ? b[rt] : X; }
        case 0x10: return kCop1S[fn];
        case 0x14: return fn == 0x20 ? O::CVT_S_W : X;
        default: return X;
        }
    case 0x12:  // COP2
        if (w & (1u << 25)) {
            if (fn < 0x3C) return kVuSpecial1[fn];
            return kVuSpecial2[(w & 3) | ((w >> 4) & 0x7C)];
        }
        switch (rs) {
        case 0x01: return O::QMFC2;
        case 0x02: return O::CFC2;
        case 0x05: return O::QMTC2;
        case 0x06: return O::CTC2;
        case 0x08: { const O b[4] = {O::BC2F, O::BC2T, O::BC2FL, O::BC2TL}; return rt < 4 ? b[rt] : X; }
        default: return X;
        }
    default:
        return kPrimary[op];
    }
}

const char* const kGpr[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

std::string dest_suffix(uint8_t d) {
    std::string s = ".";
    if (d & 8) s += 'x';
    if (d & 4) s += 'y';
    if (d & 2) s += 'z';
    if (d & 1) s += 'w';
    return s;
}

} // namespace

const char* mnemonic(Op op) { return kInfo[(int)op].mnem; }
Fmt format(Op op) { return kInfo[(int)op].fmt; }

Insn decode(uint32_t pc, uint32_t raw) {
    Insn in;
    in.pc = pc;
    in.raw = raw;
    in.op = decode_op(raw);
    return in;
}

bool is_branch(Op op) {
    switch (op) {
    case O::BEQ: case O::BNE: case O::BLEZ: case O::BGTZ:
    case O::BEQL: case O::BNEL: case O::BLEZL: case O::BGTZL:
    case O::BLTZ: case O::BGEZ: case O::BLTZL: case O::BGEZL:
    case O::BLTZAL: case O::BGEZAL: case O::BLTZALL: case O::BGEZALL:
    case O::BC0F: case O::BC0T: case O::BC0FL: case O::BC0TL:
    case O::BC1F: case O::BC1T: case O::BC1FL: case O::BC1TL:
    case O::BC2F: case O::BC2T: case O::BC2FL: case O::BC2TL:
        return true;
    default:
        return false;
    }
}

bool is_likely(Op op) {
    switch (op) {
    case O::BEQL: case O::BNEL: case O::BLEZL: case O::BGTZL: case O::BLTZL: case O::BGEZL:
    case O::BLTZALL: case O::BGEZALL: case O::BC0FL: case O::BC0TL: case O::BC1FL: case O::BC1TL:
    case O::BC2FL: case O::BC2TL:
        return true;
    default:
        return false;
    }
}

bool is_jump(Op op) { return op == O::J || op == O::JAL || op == O::JR || op == O::JALR; }
bool has_delay_slot(Op op) { return is_branch(op) || is_jump(op); }
bool is_call(Op op) {
    return op == O::JAL || op == O::JALR || op == O::BLTZAL || op == O::BGEZAL ||
           op == O::BLTZALL || op == O::BGEZALL;
}

std::string disasm(const Insn& in) {
    char b[128];
    const char* m = mnemonic(in.op);
    const char *rs = kGpr[in.rs()], *rt = kGpr[in.rt()], *rd = kGpr[in.rd()];
    const unsigned sa = in.sa();
    const std::string d = dest_suffix(in.vdest());
    const char bc = "xyzw"[in.vbc()];
    const char fsf = "xyzw"[(in.raw >> 21) & 3], ftf = "xyzw"[(in.raw >> 23) & 3];

    // Common idioms first
    if (in.raw == 0) return "nop";
    if (in.op == O::ADDU && in.rt() == 0) { std::snprintf(b, sizeof b, "move    %s, %s", rd, rs); return b; }
    if (in.op == O::DADDU && in.rt() == 0) { std::snprintf(b, sizeof b, "move    %s, %s", rd, rs); return b; }
    if (in.op == O::BEQ && in.rs() == 0 && in.rt() == 0) { std::snprintf(b, sizeof b, "b       0x%08x", in.branch_target()); return b; }

    switch (format(in.op)) {
    case Fmt::None:     std::snprintf(b, sizeof b, "%s", m); break;
    case Fmt::Code:     std::snprintf(b, sizeof b, "%-7s 0x%x", m, (in.raw >> 6) & 0xFFFFF); break;
    case Fmt::R3:       std::snprintf(b, sizeof b, "%-7s %s, %s, %s", m, rd, rs, rt); break;
    case Fmt::RsRt:     std::snprintf(b, sizeof b, "%-7s %s, %s", m, rs, rt); break;
    case Fmt::RdRs:     std::snprintf(b, sizeof b, "%-7s %s, %s", m, rd, rs); break;
    case Fmt::RdRt:     std::snprintf(b, sizeof b, "%-7s %s, %s", m, rd, rt); break;
    case Fmt::Rd:       std::snprintf(b, sizeof b, "%-7s %s", m, rd); break;
    case Fmt::Rs:       std::snprintf(b, sizeof b, "%-7s %s", m, rs); break;
    case Fmt::Shift:    std::snprintf(b, sizeof b, "%-7s %s, %s, %u", m, rd, rt, sa); break;
    case Fmt::ShiftV:   std::snprintf(b, sizeof b, "%-7s %s, %s, %s", m, rd, rt, rs); break;
    case Fmt::ImmS:     std::snprintf(b, sizeof b, "%-7s %s, %s, %d", m, rt, rs, in.simm()); break;
    case Fmt::ImmU:     std::snprintf(b, sizeof b, "%-7s %s, %s, 0x%x", m, rt, rs, in.imm()); break;
    case Fmt::Lui:      std::snprintf(b, sizeof b, "%-7s %s, 0x%x", m, rt, in.imm()); break;
    case Fmt::RsImm:    std::snprintf(b, sizeof b, "%-7s %s, %d", m, rs, in.simm()); break;
    case Fmt::Mem:      std::snprintf(b, sizeof b, "%-7s %s, %d(%s)", m, rt, in.simm(), rs); break;
    case Fmt::MemOp:    std::snprintf(b, sizeof b, "%-7s 0x%x, %d(%s)", m, in.rt(), in.simm(), rs); break;
    case Fmt::MemF:     std::snprintf(b, sizeof b, "%-7s $f%u, %d(%s)", m, in.rt(), in.simm(), rs); break;
    case Fmt::MemV:     std::snprintf(b, sizeof b, "%-7s vf%u, %d(%s)", m, in.rt(), in.simm(), rs); break;
    case Fmt::Br1:      std::snprintf(b, sizeof b, "%-7s %s, 0x%08x", m, rs, in.branch_target()); break;
    case Fmt::Br2:      std::snprintf(b, sizeof b, "%-7s %s, %s, 0x%08x", m, rs, rt, in.branch_target()); break;
    case Fmt::BrC:      std::snprintf(b, sizeof b, "%-7s 0x%08x", m, in.branch_target()); break;
    case Fmt::Jump:     std::snprintf(b, sizeof b, "%-7s 0x%08x", m, in.jump_target()); break;
    case Fmt::PmfhlFmt: {
        static const char* const f[] = {".lw", ".uw", ".slw", ".lh", ".sh"};
        std::snprintf(b, sizeof b, "%s%-3s %s", m, sa < 5 ? f[sa] : ".?", in.op == O::PMFHL ? rd : rs);
        break;
    }
    case Fmt::Cop0Move: std::snprintf(b, sizeof b, "%-7s %s, $%u", m, rt, in.rd()); break;
    case Fmt::FMove:    std::snprintf(b, sizeof b, "%-7s %s, $f%u", m, rt, in.fs()); break;
    case Fmt::FCtrl:    std::snprintf(b, sizeof b, "%-7s %s, $fcr%u", m, rt, in.fs()); break;
    case Fmt::F3:       std::snprintf(b, sizeof b, "%-7s $f%u, $f%u, $f%u", m, in.fd(), in.fs(), in.ft()); break;
    case Fmt::FdFs:     std::snprintf(b, sizeof b, "%-7s $f%u, $f%u", m, in.fd(), in.fs()); break;
    case Fmt::FdFt:     std::snprintf(b, sizeof b, "%-7s $f%u, $f%u", m, in.fd(), in.ft()); break;
    case Fmt::FsFt:     std::snprintf(b, sizeof b, "%-7s $f%u, $f%u", m, in.fs(), in.ft()); break;
    case Fmt::V2Move:   std::snprintf(b, sizeof b, "%-7s %s, vf%u", m, rt, in.rd()); break;
    case Fmt::V2Ctrl:   std::snprintf(b, sizeof b, "%-7s %s, vi%u", m, rt, in.rd()); break;
    case Fmt::VBc:      std::snprintf(b, sizeof b, "%s%c%-5s vf%u, vf%u, vf%u%c", m, bc, d.c_str(), in.sa(), in.rd(), in.rt(), bc); break;
    case Fmt::VQ:       std::snprintf(b, sizeof b, "%s%-5s vf%u, vf%u", m, d.c_str(), in.sa(), in.rd()); break;
    case Fmt::V3:       std::snprintf(b, sizeof b, "%s%-5s vf%u, vf%u, vf%u", m, d.c_str(), in.sa(), in.rd(), in.rt()); break;
    case Fmt::V2:       std::snprintf(b, sizeof b, "%s%-5s vf%u, vf%u", m, d.c_str(), in.rt(), in.rd()); break;
    case Fmt::VABc:     std::snprintf(b, sizeof b, "%s%c%-5s ACC, vf%u, vf%u%c", m, bc, d.c_str(), in.rd(), in.rt(), bc); break;
    case Fmt::VA2:      std::snprintf(b, sizeof b, "%s%-5s ACC, vf%u", m, d.c_str(), in.rd()); break;
    case Fmt::VA3:      std::snprintf(b, sizeof b, "%s%-5s ACC, vf%u, vf%u", m, d.c_str(), in.rd(), in.rt()); break;
    case Fmt::VI3:      std::snprintf(b, sizeof b, "%-7s vi%u, vi%u, vi%u", m, in.sa(), in.rd(), in.rt()); break;
    case Fmt::VIImm:    std::snprintf(b, sizeof b, "%-7s vi%u, vi%u, %d", m, in.rt(), in.rd(), (int)((int32_t)(sa << 27) >> 27)); break;
    case Fmt::VCall:    std::snprintf(b, sizeof b, "%-7s 0x%x", m, ((in.raw >> 6) & 0x7FFF) * 8); break;
    case Fmt::VClip:    std::snprintf(b, sizeof b, "%-7s vf%u, vf%u", "vclipw.xyz", in.rd(), in.rt()); break;
    case Fmt::VLoadStore:
        if (in.op == O::VLQI || in.op == O::VLQD)
            std::snprintf(b, sizeof b, "%s%-5s vf%u, (vi%u%s)", m, d.c_str(), in.rt(), in.rd(), in.op == O::VLQI ? "++" : "--");
        else
            std::snprintf(b, sizeof b, "%s%-5s vf%u, (vi%u%s)", m, d.c_str(), in.rd(), in.rt(), in.op == O::VSQI ? "++" : "--");
        break;
    case Fmt::VDivFmt:  std::snprintf(b, sizeof b, "%-7s Q, vf%u%c, vf%u%c", m, in.rd(), fsf, in.rt(), ftf); break;
    case Fmt::VMtir:    std::snprintf(b, sizeof b, "%-7s vi%u, vf%u%c", m, in.rt(), in.rd(), fsf); break;
    case Fmt::VMfir:    std::snprintf(b, sizeof b, "%s%-5s vf%u, vi%u", m, d.c_str(), in.rt(), in.rd()); break;
    case Fmt::VIlwr:    std::snprintf(b, sizeof b, "%s%-5s vi%u, (vi%u)", m, d.c_str(), in.rt(), in.rd()); break;
    case Fmt::VR:       std::snprintf(b, sizeof b, "%s%-5s vf%u, R", m, d.c_str(), in.rt()); break;
    case Fmt::VRIn:     std::snprintf(b, sizeof b, "%-7s R, vf%u%c", m, in.rd(), fsf); break;
    }
    return b;
}

} // namespace r5900
