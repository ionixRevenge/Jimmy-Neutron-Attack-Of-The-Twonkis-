// Host-side runtime API (not used by generated code directly).
#pragma once
#include <cstdarg>
#include <cstdint>
#include <string>

#include "ps2/context.hpp"

namespace ps2 {

// ---- logging -------------------------------------------------------------------
enum class Log { Kernel, Mem, Cpu, Hle, Boot };
void log(Log ch, const char* fmt, ...);
bool log_enabled(Log ch);

// ---- guest memory helpers for HLE code ----------------------------------------------
uint8_t* guest_ptr(Context* ctx, uint32_t addr, uint32_t size);   // fatal if not RAM/SPR
std::string guest_string(Context* ctx, uint32_t addr, size_t max = 1024);

// ---- kernel ----------------------------------------------------------------------
void kernel_init(Context* ctx);
void kernel_syscall(Context* ctx, uint32_t pc);
void kernel_poll(Context* ctx);   // deliver due interrupts at a safe point

} // namespace ps2
