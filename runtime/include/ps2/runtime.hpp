// Host-side runtime API (not used by generated code directly).
#pragma once
#include <cstdarg>
#include <cstdint>
#include <string>

#include "ps2/context.hpp"

namespace ps2 {

// ---- logging -------------------------------------------------------------------
enum class Log { Kernel, Mem, Cpu, Hle, Boot, Iop };
void log(Log ch, const char* fmt, ...);
bool log_enabled(Log ch);

// ---- guest memory helpers for HLE code ----------------------------------------------
uint8_t* guest_ptr(Context* ctx, uint32_t addr, uint32_t size);   // fatal if not RAM/SPR
std::string guest_string(Context* ctx, uint32_t addr, size_t max = 1024);

// ---- kernel ----------------------------------------------------------------------
void kernel_init(Context* ctx);
void kernel_syscall(Context* ctx, uint32_t pc);
void kernel_poll(Context* ctx);   // deliver due interrupts at a safe point
uint64_t kernel_syscall_count();
uint32_t timer_poll_irqs();          // EE timer compare/overflow events -> INTC causes
void kernel_raise_dmac(int channel);   // a DMA channel finished: run its DMAC handlers
void dump_trace();               // recent guest function entries (services.cpp)
void write_trace_file();         // full trace ring to $TWONK_TRACE_FILE, if set

// ---- IOP (iop.cpp) ------------------------------------------------------------------
// The IOP is not emulated: SIF DMA, sifcmd and sifrpc are answered natively and
// each RPC server the game binds is a C++ service.
void iop_init(const std::string& disc_root);
bool disc_open(const std::string& iso_path, std::string* err);   // the user's ISO
void sif_syscall(Context* ctx, int call);   // SifDmaStat/SetDma/SetDChain/SetReg/GetReg
void iop_vblank(Context* ctx);              // per-frame IOP work (controller refresh)
bool iop_pop_packet(Context* ctx);          // write the next IOP->EE packet; true if one was written

} // namespace ps2
