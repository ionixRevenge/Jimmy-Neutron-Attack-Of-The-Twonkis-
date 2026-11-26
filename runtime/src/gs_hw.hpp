// Hardware GS renderer (Direct3D 11), drawn at N times the PS2 resolution.
//
// gs.cpp still decodes registers, assembles primitives and owns GS local memory
// (g_gsmem), which stays authoritative for everything the CPU writes. Render
// targets and depth buffers live on the GPU at the chosen scale; the two views
// are kept coherent in both directions:
//   - before the CPU reads or partly overwrites GS memory, GPU-written surfaces
//     in that range are downloaded (sync);
//   - after the CPU writes GS memory, surfaces covering it are refreshed from it
//     and cached textures over it are decoded again (written).
// Every call runs on the GS thread.
#pragma once
#include <cstdint>

#include "gs_state.hpp"

namespace ps2::gpu::hw {

bool active();   // the hardware renderer is selected and its device works

// One primitive: kind 0 line/point (a, b), 1 triangle, 2 sprite (a, b).
void draw(const DrawState& s, int kind, const Vertex& a, const Vertex& b, const Vertex& c);
void flush();   // submit queued primitives

// GS memory in 256-byte blocks [lo, hi).
void sync(uint32_t lo, uint32_t hi);      // the CPU is about to read or partly overwrite it
void written(uint32_t lo, uint32_t hi);   // the CPU wrote it

// Blocks covered by rows [y0, y1) of a buffer (bp, bw, psm).
void rect_blocks(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t y0, uint32_t y1, uint32_t* lo, uint32_t* hi);

// Vblank: show the PCRTC picture (priv = the 0x12000000 register block).
void present(uint64_t frame, const uint64_t* priv);

} // namespace ps2::gpu::hw
