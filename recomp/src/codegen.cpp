#include "codegen.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>

#include "r5900.hpp"

namespace recomp {

using r5900::Insn;
using r5900::Op;

namespace {

const char* const kOpEnum[] = {
    "Invalid",
#define OP(name, mnem, fmt) #name,
#include "ps2/r5900_ops.inc"
#undef OP
};

std::string f(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

std::string hook(const Insn& in) {
    return f("ps2::exec<ps2::Op::%s>(ctx, 0x%08xu, 0x%08xu);", kOpEnum[(int)in.op], in.pc, in.raw);
}

// Signed immediate as a uint32 literal (keeps arithmetic in uint32 with no UB).
std::string simm32(const Insn& in) { return f("0x%08xu", (uint32_t)in.simm()); }

std::string addr(const Insn& in) { return f("ADDR(%u, %d)", in.rs(), in.simm()); }

} // namespace

std::string fn_name(uint32_t a) { return f("f_%08x", a); }

CodeGen::CodeGen(const Image& img, std::vector<Function> funcs, const std::vector<uint32_t>& ptrs)
    : img_(img), funcs_(std::move(funcs)), data_code_ptrs_(ptrs) {
    std::sort(funcs_.begin(), funcs_.end(), [](const Function& a, const Function& b) { return a.start < b.start; });
    for (const auto& fn : funcs_) starts_.insert(fn.start);
    std::sort(data_code_ptrs_.begin(), data_code_ptrs_.end());
}

std::string CodeGen::target_code(const Function& fn, uint32_t t, GenStats& st) const {
    if (t >= fn.start && t < fn.end) return f("goto L_%08x;", t);
    if (is_func_start(t)) return f("{ %s(ctx); return; }", fn_name(t).c_str());
    ++st.external_branches;
    return f("{ ps2::jump_indirect(ctx, 0x%08xu); return; }", t);
}

void CodeGen::emit_insn(const Function& fn, uint32_t pc, std::string& o, GenStats& st, bool in_delay) const {
    (void)fn;
    const Insn in = r5900::decode(pc, img_.read32(pc));
    ++st.instructions;
    if (in.raw == 0) { ++st.inlined; return; }  // nop
    if (!in.valid()) {
        ++st.invalid;
        o += f("    ps2::fatal(ctx, 0x%08xu, \"invalid instruction 0x%08x\");\n", pc, in.raw);
        return;
    }
    if (r5900::has_delay_slot(in.op)) {  // control flow is normally handled by emit_control
        ++st.branch_in_delay;
        o += f("    ps2::fatal(ctx, 0x%08xu, \"%s\");\n", pc,
               in_delay ? "branch in delay slot" : "branch at end of function");
        return;
    }

    const unsigned rs = in.rs(), rt = in.rt(), rd = in.rd(), sa = in.sa();
    const unsigned fs = in.fs(), ft = in.ft(), fd = in.fd();
    std::string s;          // statement
    bool inl = true;

    switch (in.op) {
    // --- shifts -------------------------------------------------------------
    case Op::SLL:    if (rd) s = f("S64(%u) = (int32_t)(U32(%u) << %u);", rd, rt, sa); break;
    case Op::SRL:    if (rd) s = f("S64(%u) = (int32_t)(U32(%u) >> %u);", rd, rt, sa); break;
    case Op::SRA:    if (rd) s = f("S64(%u) = S32(%u) >> %u;", rd, rt, sa); break;
    case Op::SLLV:   if (rd) s = f("S64(%u) = (int32_t)(U32(%u) << (U32(%u) & 31));", rd, rt, rs); break;
    case Op::SRLV:   if (rd) s = f("S64(%u) = (int32_t)(U32(%u) >> (U32(%u) & 31));", rd, rt, rs); break;
    case Op::SRAV:   if (rd) s = f("S64(%u) = S32(%u) >> (U32(%u) & 31);", rd, rt, rs); break;
    case Op::DSLLV:  if (rd) s = f("U64(%u) = U64(%u) << (U32(%u) & 63);", rd, rt, rs); break;
    case Op::DSRLV:  if (rd) s = f("U64(%u) = U64(%u) >> (U32(%u) & 63);", rd, rt, rs); break;
    case Op::DSRAV:  if (rd) s = f("S64(%u) = S64(%u) >> (U32(%u) & 63);", rd, rt, rs); break;
    case Op::DSLL:   if (rd) s = f("U64(%u) = U64(%u) << %u;", rd, rt, sa); break;
    case Op::DSRL:   if (rd) s = f("U64(%u) = U64(%u) >> %u;", rd, rt, sa); break;
    case Op::DSRA:   if (rd) s = f("S64(%u) = S64(%u) >> %u;", rd, rt, sa); break;
    case Op::DSLL32: if (rd) s = f("U64(%u) = U64(%u) << %u;", rd, rt, sa + 32); break;
    case Op::DSRL32: if (rd) s = f("U64(%u) = U64(%u) >> %u;", rd, rt, sa + 32); break;
    case Op::DSRA32: if (rd) s = f("S64(%u) = S64(%u) >> %u;", rd, rt, sa + 32); break;

    // --- moves / system -------------------------------------------------------
    case Op::MOVZ:   if (rd) s = f("if (U64(%u) == 0) U64(%u) = U64(%u);", rt, rd, rs); break;
    case Op::MOVN:   if (rd) s = f("if (U64(%u) != 0) U64(%u) = U64(%u);", rt, rd, rs); break;
    case Op::SYSCALL: s = f("ps2::syscall(ctx, 0x%08xu);", pc); break;
    case Op::BREAK:   s = f("ps2::breakpoint(ctx, 0x%08xu);", pc); break;
    case Op::SYNC: case Op::CACHE: case Op::PREF: break;
    case Op::MFHI:   if (rd) s = f("U64(%u) = ctx->hi.u64[0];", rd); break;
    case Op::MFLO:   if (rd) s = f("U64(%u) = ctx->lo.u64[0];", rd); break;
    case Op::MFHI1:  if (rd) s = f("U64(%u) = ctx->hi.u64[1];", rd); break;
    case Op::MFLO1:  if (rd) s = f("U64(%u) = ctx->lo.u64[1];", rd); break;
    case Op::MTHI:   s = f("ctx->hi.u64[0] = U64(%u);", rs); break;
    case Op::MTLO:   s = f("ctx->lo.u64[0] = U64(%u);", rs); break;
    case Op::MTHI1:  s = f("ctx->hi.u64[1] = U64(%u);", rs); break;
    case Op::MTLO1:  s = f("ctx->lo.u64[1] = U64(%u);", rs); break;
    case Op::MFSA:   if (rd) s = f("U64(%u) = ctx->sa;", rd); break;
    case Op::MTSA:   s = f("ctx->sa = U32(%u);", rs); break;

    // --- multiply / divide (R5900: MULT/MADD also write rd) -----------------------
    case Op::MULT: case Op::MULT1: case Op::MULTU: case Op::MULTU1: {
        const int pipe = (in.op == Op::MULT1 || in.op == Op::MULTU1);
        const bool u = (in.op == Op::MULTU || in.op == Op::MULTU1);
        s = u ? f("ps2::set_hilo64(ctx, %d, (uint64_t)U32(%u) * U32(%u));", pipe, rs, rt)
              : f("ps2::set_hilo64(ctx, %d, (uint64_t)((int64_t)S32(%u) * S32(%u)));", pipe, rs, rt);
        if (rd) s += f(" S64(%u) = ctx->lo.s64[%d];", rd, pipe);
        break;
    }
    case Op::MADD: case Op::MADD1: case Op::MADDU: case Op::MADDU1: {
        const int pipe = (in.op == Op::MADD1 || in.op == Op::MADDU1);
        const bool u = (in.op == Op::MADDU || in.op == Op::MADDU1);
        s = u ? f("ps2::set_hilo64(ctx, %d, ps2::get_hilo64(ctx, %d) + (uint64_t)U32(%u) * U32(%u));", pipe, pipe, rs, rt)
              : f("ps2::set_hilo64(ctx, %d, ps2::get_hilo64(ctx, %d) + (uint64_t)((int64_t)S32(%u) * S32(%u)));", pipe, pipe, rs, rt);
        if (rd) s += f(" S64(%u) = ctx->lo.s64[%d];", rd, pipe);
        break;
    }
    case Op::DIV:   s = f("ps2::div32(ctx, 0, S32(%u), S32(%u));", rs, rt); break;
    case Op::DIV1:  s = f("ps2::div32(ctx, 1, S32(%u), S32(%u));", rs, rt); break;
    case Op::DIVU:  s = f("ps2::divu32(ctx, 0, U32(%u), U32(%u));", rs, rt); break;
    case Op::DIVU1: s = f("ps2::divu32(ctx, 1, U32(%u), U32(%u));", rs, rt); break;

    // --- ALU (overflow traps of ADD/SUB/DADD/ADDI are not modelled) --------------
    case Op::ADD: case Op::ADDU:   if (rd) s = f("S64(%u) = (int32_t)(U32(%u) + U32(%u));", rd, rs, rt); break;
    case Op::SUB: case Op::SUBU:   if (rd) s = f("S64(%u) = (int32_t)(U32(%u) - U32(%u));", rd, rs, rt); break;
    case Op::DADD: case Op::DADDU: if (rd) s = f("U64(%u) = U64(%u) + U64(%u);", rd, rs, rt); break;
    case Op::DSUB: case Op::DSUBU: if (rd) s = f("U64(%u) = U64(%u) - U64(%u);", rd, rs, rt); break;
    case Op::AND:  if (rd) s = f("U64(%u) = U64(%u) & U64(%u);", rd, rs, rt); break;
    case Op::OR:   if (rd) s = f("U64(%u) = U64(%u) | U64(%u);", rd, rs, rt); break;
    case Op::XOR:  if (rd) s = f("U64(%u) = U64(%u) ^ U64(%u);", rd, rs, rt); break;
    case Op::NOR:  if (rd) s = f("U64(%u) = ~(U64(%u) | U64(%u));", rd, rs, rt); break;
    case Op::SLT:  if (rd) s = f("U64(%u) = S64(%u) < S64(%u) ? 1 : 0;", rd, rs, rt); break;
    case Op::SLTU: if (rd) s = f("U64(%u) = U64(%u) < U64(%u) ? 1 : 0;", rd, rs, rt); break;
    case Op::ADDI: case Op::ADDIU:   if (rt) s = f("S64(%u) = (int32_t)(U32(%u) + %s);", rt, rs, simm32(in).c_str()); break;
    case Op::DADDI: case Op::DADDIU: if (rt) s = f("U64(%u) = U64(%u) + (uint64_t)(int64_t)%d;", rt, rs, in.simm()); break;
    case Op::SLTI:  if (rt) s = f("U64(%u) = S64(%u) < (int64_t)%d ? 1 : 0;", rt, rs, in.simm()); break;
    case Op::SLTIU: if (rt) s = f("U64(%u) = U64(%u) < (uint64_t)(int64_t)%d ? 1 : 0;", rt, rs, in.simm()); break;
    case Op::ANDI:  if (rt) s = f("U64(%u) = U64(%u) & 0x%xu;", rt, rs, in.imm()); break;
    case Op::ORI:   if (rt) s = f("U64(%u) = U64(%u) | 0x%xu;", rt, rs, in.imm()); break;
    case Op::XORI:  if (rt) s = f("U64(%u) = U64(%u) ^ 0x%xu;", rt, rs, in.imm()); break;
    case Op::LUI:   if (rt) s = f("S64(%u) = (int32_t)0x%08xu;", rt, (uint32_t)in.imm() << 16); break;

    // --- loads / stores -----------------------------------------------------------
    case Op::LB:  s = rt ? f("S64(%u) = ps2::mem_read<int8_t>(ctx, %s);", rt, addr(in).c_str())   : f("(void)ps2::mem_read<int8_t>(ctx, %s);", addr(in).c_str()); break;
    case Op::LBU: s = rt ? f("U64(%u) = ps2::mem_read<uint8_t>(ctx, %s);", rt, addr(in).c_str())  : f("(void)ps2::mem_read<uint8_t>(ctx, %s);", addr(in).c_str()); break;
    case Op::LH:  s = rt ? f("S64(%u) = ps2::mem_read<int16_t>(ctx, %s);", rt, addr(in).c_str())  : f("(void)ps2::mem_read<int16_t>(ctx, %s);", addr(in).c_str()); break;
    case Op::LHU: s = rt ? f("U64(%u) = ps2::mem_read<uint16_t>(ctx, %s);", rt, addr(in).c_str()) : f("(void)ps2::mem_read<uint16_t>(ctx, %s);", addr(in).c_str()); break;
    case Op::LW:  s = rt ? f("S64(%u) = ps2::mem_read<int32_t>(ctx, %s);", rt, addr(in).c_str())  : f("(void)ps2::mem_read<int32_t>(ctx, %s);", addr(in).c_str()); break;
    case Op::LWU: s = rt ? f("U64(%u) = ps2::mem_read<uint32_t>(ctx, %s);", rt, addr(in).c_str()) : f("(void)ps2::mem_read<uint32_t>(ctx, %s);", addr(in).c_str()); break;
    case Op::LD:  s = rt ? f("U64(%u) = ps2::mem_read<uint64_t>(ctx, %s);", rt, addr(in).c_str()) : f("(void)ps2::mem_read<uint64_t>(ctx, %s);", addr(in).c_str()); break;
    case Op::LQ:  if (rt) s = f("ps2::mem_read128(ctx, %s, &GPR(%u));", addr(in).c_str(), rt); break;
    case Op::LWL: if (rt) s = f("ps2::lwl(ctx, %s, GPR(%u));", addr(in).c_str(), rt); break;
    case Op::LWR: if (rt) s = f("ps2::lwr(ctx, %s, GPR(%u));", addr(in).c_str(), rt); break;
    case Op::LDL: if (rt) s = f("ps2::ldl(ctx, %s, GPR(%u));", addr(in).c_str(), rt); break;
    case Op::LDR: if (rt) s = f("ps2::ldr(ctx, %s, GPR(%u));", addr(in).c_str(), rt); break;
    case Op::SB:  s = f("ps2::mem_write<uint8_t>(ctx, %s, (uint8_t)U32(%u));", addr(in).c_str(), rt); break;
    case Op::SH:  s = f("ps2::mem_write<uint16_t>(ctx, %s, (uint16_t)U32(%u));", addr(in).c_str(), rt); break;
    case Op::SW:  s = f("ps2::mem_write<uint32_t>(ctx, %s, U32(%u));", addr(in).c_str(), rt); break;
    case Op::SD:  s = f("ps2::mem_write<uint64_t>(ctx, %s, U64(%u));", addr(in).c_str(), rt); break;
    case Op::SQ:  s = f("ps2::mem_write128(ctx, %s, GPR(%u));", addr(in).c_str(), rt); break;
    case Op::SWL: s = f("ps2::swl(ctx, %s, GPR(%u));", addr(in).c_str(), rt); break;
    case Op::SWR: s = f("ps2::swr(ctx, %s, GPR(%u));", addr(in).c_str(), rt); break;
    case Op::SDL: s = f("ps2::sdl(ctx, %s, GPR(%u));", addr(in).c_str(), rt); break;
    case Op::SDR: s = f("ps2::sdr(ctx, %s, GPR(%u));", addr(in).c_str(), rt); break;
    case Op::LWC1: s = f("FPR(%u) = ps2::as_float(ps2::mem_read<uint32_t>(ctx, %s));", ft, addr(in).c_str()); break;
    case Op::SWC1: s = f("ps2::mem_write<uint32_t>(ctx, %s, ps2::as_u32(FPR(%u)));", addr(in).c_str(), ft); break;
    case Op::LQC2: if (rt) s = f("ps2::mem_read128(ctx, %s, &ctx->vf[%u]);", addr(in).c_str(), rt); break;
    case Op::SQC2: s = f("ps2::mem_write128(ctx, %s, ctx->vf[%u]);", addr(in).c_str(), rt); break;

    // --- COP1 -----------------------------------------------------------------------
    case Op::MFC1: if (rt) s = f("S64(%u) = (int32_t)ps2::as_u32(FPR(%u));", rt, fs); break;
    case Op::MTC1: s = f("FPR(%u) = ps2::as_float(U32(%u));", fs, rt); break;
    case Op::CFC1: if (rt) s = f("S64(%u) = (int32_t)%s;", rt, fs == 31 ? "ctx->fcr31" : "ctx->fcr0"); break;
    case Op::CTC1: if (fs == 31) s = f("ctx->fcr31 = U32(%u);", rt); break;
    case Op::ADD_S:   s = f("FPR(%u) = ps2::fadd(FPR(%u), FPR(%u));", fd, fs, ft); break;
    case Op::SUB_S:   s = f("FPR(%u) = ps2::fsub(FPR(%u), FPR(%u));", fd, fs, ft); break;
    case Op::MUL_S:   s = f("FPR(%u) = ps2::fmul(FPR(%u), FPR(%u));", fd, fs, ft); break;
    case Op::DIV_S:   s = f("FPR(%u) = ps2::fdiv(ctx, FPR(%u), FPR(%u));", fd, fs, ft); break;
    case Op::SQRT_S:  s = f("FPR(%u) = ps2::fsqrt(FPR(%u));", fd, ft); break;
    case Op::RSQRT_S: s = f("FPR(%u) = ps2::frsqrt(ctx, FPR(%u), FPR(%u));", fd, fs, ft); break;
    case Op::ABS_S:   s = f("FPR(%u) = std::fabs(FPR(%u));", fd, fs); break;
    case Op::MOV_S:   s = f("FPR(%u) = FPR(%u);", fd, fs); break;
    case Op::NEG_S:   s = f("FPR(%u) = -FPR(%u);", fd, fs); break;
    case Op::ADDA_S:  s = f("ctx->facc = ps2::fadd(FPR(%u), FPR(%u));", fs, ft); break;
    case Op::SUBA_S:  s = f("ctx->facc = ps2::fsub(FPR(%u), FPR(%u));", fs, ft); break;
    case Op::MULA_S:  s = f("ctx->facc = ps2::fmul(FPR(%u), FPR(%u));", fs, ft); break;
    case Op::MADD_S:  s = f("FPR(%u) = ps2::fadd(ctx->facc, ps2::fmul(FPR(%u), FPR(%u)));", fd, fs, ft); break;
    case Op::MSUB_S:  s = f("FPR(%u) = ps2::fsub(ctx->facc, ps2::fmul(FPR(%u), FPR(%u)));", fd, fs, ft); break;
    case Op::MADDA_S: s = f("ctx->facc = ps2::fadd(ctx->facc, ps2::fmul(FPR(%u), FPR(%u)));", fs, ft); break;
    case Op::MSUBA_S: s = f("ctx->facc = ps2::fsub(ctx->facc, ps2::fmul(FPR(%u), FPR(%u)));", fs, ft); break;
    case Op::CVT_W_S: s = f("FPR(%u) = ps2::as_float((uint32_t)ps2::cvt_w_s(FPR(%u)));", fd, fs); break;
    case Op::CVT_S_W: s = f("FPR(%u) = (float)(int32_t)ps2::as_u32(FPR(%u));", fd, fs); break;
    case Op::MAX_S:   s = f("FPR(%u) = std::fmax(FPR(%u), FPR(%u));", fd, fs, ft); break;
    case Op::MIN_S:   s = f("FPR(%u) = std::fmin(FPR(%u), FPR(%u));", fd, fs, ft); break;
    case Op::C_F_S:   s = "ctx->fcr31 &= ~ps2::kFcrC;"; break;
    case Op::C_EQ_S:  s = f("ctx->fcr31 = (FPR(%u) == FPR(%u)) ? (ctx->fcr31 | ps2::kFcrC) : (ctx->fcr31 & ~ps2::kFcrC);", fs, ft); break;
    case Op::C_LT_S:  s = f("ctx->fcr31 = (FPR(%u) < FPR(%u)) ? (ctx->fcr31 | ps2::kFcrC) : (ctx->fcr31 & ~ps2::kFcrC);", fs, ft); break;
    case Op::C_LE_S:  s = f("ctx->fcr31 = (FPR(%u) <= FPR(%u)) ? (ctx->fcr31 | ps2::kFcrC) : (ctx->fcr31 & ~ps2::kFcrC);", fs, ft); break;

    // --- COP2 transfers (vf0 is constant; control regs >= 16 go through the hook) --
    case Op::QMFC2: if (rt) s = f("GPR(%u) = ctx->vf[%u];", rt, rd); break;
    case Op::QMTC2: if (rd) s = f("ctx->vf[%u] = GPR(%u);", rd, rt); break;
    case Op::CFC2:  if (rt && rd < 16) s = f("S64(%u) = (int32_t)ctx->vi[%u];", rt, rd); else inl = false; break;
    case Op::CTC2:  if (rd && rd < 16) s = f("ctx->vi[%u] = U32(%u) & 0xFFFFu;", rd, rt); else if (rd) inl = false; break;

    // --- 128-bit MMI that are trivial -------------------------------------------------
    case Op::PCPYLD: if (rd) s = f("{ ps2::u128 t; t.u64[0] = GPR(%u).u64[0]; t.u64[1] = GPR(%u).u64[0]; GPR(%u) = t; }", rt, rs, rd); break;
    case Op::PCPYUD: if (rd) s = f("{ ps2::u128 t; t.u64[0] = GPR(%u).u64[1]; t.u64[1] = GPR(%u).u64[1]; GPR(%u) = t; }", rs, rt, rd); break;
    case Op::POR:  if (rd) s = f("GPR(%u).u64[0] = GPR(%u).u64[0] | GPR(%u).u64[0]; GPR(%u).u64[1] = GPR(%u).u64[1] | GPR(%u).u64[1];", rd, rs, rt, rd, rs, rt); break;
    case Op::PAND: if (rd) s = f("GPR(%u).u64[0] = GPR(%u).u64[0] & GPR(%u).u64[0]; GPR(%u).u64[1] = GPR(%u).u64[1] & GPR(%u).u64[1];", rd, rs, rt, rd, rs, rt); break;
    case Op::PXOR: if (rd) s = f("GPR(%u).u64[0] = GPR(%u).u64[0] ^ GPR(%u).u64[0]; GPR(%u).u64[1] = GPR(%u).u64[1] ^ GPR(%u).u64[1];", rd, rs, rt, rd, rs, rt); break;
    case Op::PNOR: if (rd) s = f("GPR(%u).u64[0] = ~(GPR(%u).u64[0] | GPR(%u).u64[0]); GPR(%u).u64[1] = ~(GPR(%u).u64[1] | GPR(%u).u64[1]);", rd, rs, rt, rd, rs, rt); break;

    default:
        inl = false;
        break;
    }

    if (inl) {
        ++st.inlined;
        if (!s.empty()) o += "    " + s + "\n";
    } else {
        ++st.hooked;
        o += "    " + hook(in) + "\n";
    }
}

void CodeGen::emit_control(const Function& fn, uint32_t pc, std::string& o, GenStats& st) const {
    const Insn in = r5900::decode(pc, img_.read32(pc));
    ++st.instructions;
    ++st.inlined;
    std::string delay;
    emit_insn(fn, pc + 4, delay, st, true);
    const unsigned rs = in.rs(), rt = in.rt();

    auto call = [&](uint32_t t) {
        return is_func_start(t) ? f("%s(ctx);", fn_name(t).c_str()) : f("ps2::call_indirect(ctx, 0x%08xu);", t);
    };

    std::string cond;
    switch (in.op) {
    case Op::BEQ: case Op::BEQL:
        cond = (rs == rt) ? "true" : f("U64(%u) == U64(%u)", rs, rt); break;
    case Op::BNE: case Op::BNEL:  cond = f("U64(%u) != U64(%u)", rs, rt); break;
    case Op::BLEZ: case Op::BLEZL: cond = f("S64(%u) <= 0", rs); break;
    case Op::BGTZ: case Op::BGTZL: cond = f("S64(%u) > 0", rs); break;
    case Op::BLTZ: case Op::BLTZL: case Op::BLTZAL: case Op::BLTZALL: cond = f("S64(%u) < 0", rs); break;
    case Op::BGEZ: case Op::BGEZL: case Op::BGEZAL: case Op::BGEZALL: cond = f("S64(%u) >= 0", rs); break;
    case Op::BC1F: case Op::BC1FL: cond = "(ctx->fcr31 & ps2::kFcrC) == 0"; break;
    case Op::BC1T: case Op::BC1TL: cond = "(ctx->fcr31 & ps2::kFcrC) != 0"; break;
    case Op::BC0F: case Op::BC0FL: cond = "!ps2::cop0_condition(ctx)"; break;
    case Op::BC0T: case Op::BC0TL: cond = "ps2::cop0_condition(ctx)"; break;
    case Op::BC2F: case Op::BC2FL: cond = "!ps2::vu0_busy(ctx)"; break;
    case Op::BC2T: case Op::BC2TL: cond = "ps2::vu0_busy(ctx)"; break;
    default: break;
    }

    if (r5900::is_branch(in.op)) {
        const uint32_t t = in.branch_target();
        const bool link = r5900::is_call(in.op);
        std::string taken = link ? call(t) : target_code(fn, t, st);
        std::string link_s = link ? f("S64(31) = (int32_t)0x%08xu; ", pc + 8) : "";
        if (cond == "true") {
            o += "    { " + link_s + "\n" + delay + "    " + taken + " }\n";
        } else if (r5900::is_likely(in.op)) {
            o += "    " + link_s + "if (" + cond + ") {\n" + delay + "    " + taken + " }\n";
        } else {
            o += "    { const bool c = " + cond + "; " + link_s + "\n" + delay + "    if (c) " + taken + " }\n";
        }
        return;
    }

    switch (in.op) {
    case Op::J:
        o += "    {\n" + delay + "    " + target_code(fn, in.jump_target(), st) + " }\n";
        break;
    case Op::JAL:
        o += f("    S64(31) = (int32_t)0x%08xu;\n", pc + 8) + delay + "    " + call(in.jump_target()) + "\n";
        break;
    case Op::JR:
        if (rs == 31) {
            o += "    {\n" + delay + "    return; }\n";
        } else {
            ++st.indirect_jumps;
            o += f("    { const uint32_t t = U32(%u);\n", rs) + delay;
            // jump-table candidates: code pointers stored in data that land inside this function
            auto lo = std::lower_bound(data_code_ptrs_.begin(), data_code_ptrs_.end(), fn.start);
            std::string cases;
            for (auto it = lo; it != data_code_ptrs_.end() && *it < fn.end; ++it)
                if (*it != fn.start) cases += f("        case 0x%08xu: goto L_%08x;\n", *it, *it);
            if (!cases.empty()) o += "    switch (t) {\n" + cases + "    default: break; }\n";
            o += "    ps2::jump_indirect(ctx, t); return; }\n";
        }
        break;
    case Op::JALR: {
        o += f("    { const uint32_t t = U32(%u);", rs);
        if (in.rd()) o += f(" S64(%u) = (int32_t)0x%08xu;", in.rd(), pc + 8);
        o += "\n" + delay + "    ps2::call_indirect(ctx, t); }\n";
        break;
    }
    default:
        break;
    }
}

void CodeGen::emit_function(const Function& fn, std::string& o, GenStats& st) const {
    ++st.functions;
    // Pass 1: labels
    std::set<uint32_t> labels;
    for (uint32_t pc = fn.start; pc < fn.end; pc += 4) {
        const Insn in = r5900::decode(pc, img_.read32(pc));
        uint32_t t = 0;
        if (r5900::is_branch(in.op) && !r5900::is_call(in.op)) t = in.branch_target();
        else if (in.op == Op::J) t = in.jump_target();
        if (t && t >= fn.start && t < fn.end) labels.insert(t);
    }
    auto lo = std::lower_bound(data_code_ptrs_.begin(), data_code_ptrs_.end(), fn.start);
    for (auto it = lo; it != data_code_ptrs_.end() && *it < fn.end; ++it)
        if (*it != fn.start) labels.insert(*it);
    // a labelled delay slot is also emitted standalone; the branch then skips over it
    for (uint32_t pc = fn.start; pc + 4 < fn.end; pc += 4)
        if (r5900::has_delay_slot(r5900::decode(pc, img_.read32(pc)).op) && labels.count(pc + 4))
            labels.insert(pc + 8);

    o += f("void %s(ps2::Context* ctx) {\n", fn_name(fn.start).c_str());
    for (uint32_t pc = fn.start; pc < fn.end; pc += 4) {
        if (labels.count(pc)) o += f("L_%08x:;\n", pc);
        const Insn in = r5900::decode(pc, img_.read32(pc));
        if (r5900::has_delay_slot(in.op) && pc + 4 < fn.end) {
            emit_control(fn, pc, o, st);
            if (labels.count(pc + 4)) o += f("    goto L_%08x;\n", pc + 8);
            else pc += 4;
        } else {
            emit_insn(fn, pc, o, st, false);
        }
    }
    // fall-through off the end of the function
    if (labels.count(fn.end)) o += f("L_%08x:;\n", fn.end);
    if (is_func_start(fn.end)) o += f("    %s(ctx); return;\n", fn_name(fn.end).c_str());
    else o += f("    ps2::jump_indirect(ctx, 0x%08xu); return;\n", fn.end);
    o += "}\n\n";
}

bool CodeGen::generate(const GenOptions& opt, GenStats* stats, std::string* err) {
    GenStats st;
    // declarations
    {
        std::ofstream h(opt.out_dir + "/funcs.hpp");
        if (!h) { *err = "cannot write to " + opt.out_dir; return false; }
        h << "// Generated by twonkrecomp. Do not edit.\n#pragma once\n#include \"ps2/recomp.hpp\"\n\n";
        for (const auto& fn : funcs_) h << "void " << fn_name(fn.start) << "(ps2::Context* ctx);\n";
    }
    // function table for indirect calls
    {
        std::ofstream t(opt.out_dir + "/func_table.cpp");
        t << "// Generated by twonkrecomp. Do not edit.\n#include \"funcs.hpp\"\n\n"
             "extern const ps2::FuncEntry g_func_table[] = {\n";
        for (const auto& fn : funcs_) t << f("    {0x%08xu, %s},\n", fn.start, fn_name(fn.start).c_str());
        t << "};\nextern const unsigned g_func_table_size = " << funcs_.size() << ";\n";
    }
    // bodies
    size_t file_index = 0;
    for (size_t i = 0; i < funcs_.size(); i += opt.functions_per_file, ++file_index) {
        std::string body = "// Generated by twonkrecomp. Do not edit.\n#include \"funcs.hpp\"\n\n"
                           "#if defined(_MSC_VER)\n#pragma warning(disable: 4102 4702)  // unreferenced label, unreachable code\n#endif\n\n";
        for (size_t j = i; j < std::min(funcs_.size(), i + opt.functions_per_file); ++j)
            emit_function(funcs_[j], body, st);
        std::ofstream c(opt.out_dir + f("/recomp_%03zu.cpp", file_index));
        c << body;
    }
    *stats = st;
    return true;
}

} // namespace recomp
