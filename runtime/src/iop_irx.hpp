// Running a real IOP module (IRX) in an R3000 interpreter.
//
// Used for the game's sound driver, HIS.IRX: its sound logic (voice allocation,
// streams, batched commands) runs as shipped, on a small IOP kernel (threads,
// event flags, semaphores) whose imports are answered natively: CD reads come
// from the ISO, SIF DMA writes EE memory, and libsd drives the SPU2 (spu2.cpp).
// Everything runs on the game thread: an RPC call runs the module's server
// function, and the module's own threads run on each call and each vblank.
#pragma once
#include <cstdint>
#include <vector>

#include "ps2/context.hpp"

namespace ps2::iop::irx {

// Loads and relocates an IRX image into IOP RAM. False if it is not a usable IRX.
bool load(const std::vector<uint8_t>& image, const char* name);
uint32_t symbol(const char* name);   // relocated address of a module symbol (0: none)

// Runs func(a0..a3) on the module's RPC thread, letting its other threads run
// while it waits. Returns v0.
uint32_t call(Context* ctx, uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);

// Per vblank: deliver interrupt callbacks, wake sleeping threads, let them run.
void tick(Context* ctx);

// Queue an interrupt-context call of func(a0, a1) (CD/DMA completion). Thread-safe.
void raise(uint32_t func, uint32_t a0, uint32_t a1);

} // namespace ps2::iop::irx
