// IPU: the EE's MPEG-2 macroblock decoder (registers 0x10002000-0x1000203F,
// FIFOs at 0x10007000 (out) / 0x10007010 (in), DMA channels 3 fromIPU / 4 toIPU).
#pragma once
#include <cstdint>

#include "ps2/context.hpp"

namespace ps2 {

// hw.cpp services used by the IPU
uint32_t& hw_reg(uint32_t addr);            // DMA channel registers etc.
void dma_complete(Context* ctx, int ch);   // STR off, D_STAT, DMAC interrupt

namespace ipu {

bool owns(uint32_t addr);                               // register or FIFO address
uint64_t mmio_read(Context* ctx, uint32_t addr);
void mmio_write(Context* ctx, uint32_t addr, uint64_t value);
void dma_start(Context* ctx, int ch);                   // STR written on channel 3 or 4

} // namespace ipu
} // namespace ps2
