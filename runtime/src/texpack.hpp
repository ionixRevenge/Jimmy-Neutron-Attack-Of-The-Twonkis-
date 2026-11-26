// Texture dumping and replacement for the GPU renderer (texpack.cpp).
//
// Every texture the renderer decodes from GS memory is identified by a hash of its
// decoded RGBA pixels (after the CLUT). TWONK_TEX_DUMP=<dir> writes each new one to
// <dir>/<hash>.png. Replacements are PNGs in TWONK_TEX_DIR (default "textures",
// searched recursively) whose file name starts with that 16-digit hash. A replacement
// must be the original size times a whole number (2x, 4x, ...).
//
// PNG alpha is the GS alpha doubled (GS 0x80, fully opaque, is PNG 255).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace ps2::gpu::texpack {

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint32_t> px;   // RGBA8, R in the low byte, GS alpha (0x80 = 1.0)
};

bool dumping();
bool replacing();
uint64_t hash(const uint32_t* px, uint32_t w, uint32_t h);
// Queue a dump of a decoded texture (once per hash). `info` goes into <dir>/index.txt.
void dump(uint64_t h, const uint32_t* px, uint32_t w, uint32_t h_, const char* info);
// The replacement for `h`: nullptr if there is none. While it is still loading, returns
// nullptr and sets *pending; call again later. Loaded images are handed over once:
// the caller keeps them (on the GPU) and calls forget() when it drops them.
std::shared_ptr<const Image> find(uint64_t h, bool* pending);
void forget(uint64_t h);

} // namespace ps2::gpu::texpack
