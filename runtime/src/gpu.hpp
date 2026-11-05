// Graphics pipeline: DMA -> VIF1 -> (VU1) -> GIF -> GS.
//
// Each stage is a stream consumer: DMA hands quadwords to VIF1 (channel 1) or
// the GIF (channel 2, PATH3); VIF1 forwards DIRECT data to the GIF (PATH2) and
// starts VU1 microprograms, whose XGKICK output is PATH1. The GIF decodes tags
// into GS register writes and image transfers.
#pragma once
#include <cstdint>
#include <vector>

#include "ps2/context.hpp"

namespace ps2::gpu {

// ---- VIF1 / VU1 ------------------------------------------------------------------
constexpr uint32_t kVu1MemSize = 16 * 1024;    // data memory (bytes)
constexpr uint32_t kVu1MicroSize = 16 * 1024;  // micro memory (bytes)

struct Vu1 {
    alignas(16) uint8_t mem[kVu1MemSize];
    alignas(16) uint8_t micro[kVu1MicroSize];
};
Vu1& vu1();

// VIF1 consumes `qwc` quadwords (the tag's upper half arrives first with TTE).
void vif1_write(Context* ctx, const uint8_t* data, uint32_t qwc);
// VIF1 codes written to the FIFO register directly; returns false if VIF1 is mid-command.
void vif1_fifo_words(Context* ctx, const uint32_t* words, int count);
bool vif1_mskpath3();   // PATH3 masked by MSKPATH3

// ---- GIF -----------------------------------------------------------------------
enum class Path { P1 = 1, P2 = 2, P3 = 3 };
void gif_write(Path p, const uint8_t* data, uint32_t qwc);

// ---- GS ------------------------------------------------------------------------
void gs_write_reg(uint32_t reg, uint64_t value);          // GIF A+D / PACKED / REGLIST
void gs_vertex_kick(bool draw);                            // XYZ2/XYZF2 (draw) or XYZ3/XYZF3
void gs_image_data(const uint8_t* data, uint32_t qwc);     // HWREG / IMAGE transfer
void gs_priv_write(uint32_t addr, uint64_t value);         // 0x12000000 block
uint64_t gs_priv_read(uint32_t addr);
bool gs_display(std::vector<uint32_t>* rgb, uint32_t* w, uint32_t* h);   // the picture on screen

// ---- statistics (printed by `--log gpu`) -----------------------------------------
struct Stats {
    uint64_t vif1_qw = 0, vif1_direct_qw = 0, vif1_unpack = 0, vif1_mscal = 0, vif1_mpg_words = 0;
    uint64_t gif_qw[4] = {}, gif_tags = 0, gs_prims = 0, gs_vertices = 0, gs_image_qw = 0;
    uint64_t prim_type[8] = {};
};
Stats& stats();
void print_stats();

// Inclusive wall time per subsystem (seconds), printed with the statistics.
struct Prof { double vif = 0, vu1 = 0, gif = 0, image = 0, display = 0, ipu = 0; };
Prof& prof();
struct ProfScope {
    double& slot;
    int64_t t0;
    explicit ProfScope(double& s);
    ~ProfScope();
};

} // namespace ps2::gpu
