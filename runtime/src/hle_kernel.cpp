// Native replacements for Sony libkernl routines that only make sense on real hardware.
#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace hle {

// 0x117948 (called from _InitSys): installs temporary syscalls, scans BIOS kernel
// memory at 0x80000000-0x80080000 and patches it. Under HLE there is no BIOS to patch.
void kernel_patch_noop(ps2::Context*) {
    ps2::log(ps2::Log::Hle, "skipped libkernl BIOS patch routine");
}

} // namespace hle

namespace hle {

// 0x118010 (libkernl): raises to kernel mode (Status.ERL), does "sw a1, 0(a0)" and
// returns through eret to ra. Used to poke timer registers via kseg1 (0xB000xxxx).
void kernel_store_word(ps2::Context* ctx) {
    ps2::mem_write<uint32_t>(ctx, ctx->r[4].u32[0], ctx->r[5].u32[0]);
}

} // namespace hle

namespace hle {

// 0x117070 (_InitSys): copies 0x330 bytes of kernel code from .data (0x39b0c8) to
// 0x80075000 with a temporary syscall 0x5A, then SetSyscall()s several kernel entries
// to point into it. Those syscalls are served natively by the HLE kernel.
void kernel_patch2_noop(ps2::Context*) {
    ps2::log(ps2::Log::Hle, "skipped libkernl kernel-code install (0x39b0c8 -> 0x80075000)");
}

} // namespace hle
