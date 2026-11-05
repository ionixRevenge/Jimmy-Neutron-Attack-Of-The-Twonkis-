// The game disc: 2048-byte sectors of the user's ISO, plus ISO9660 lookups.
#pragma once
#include <cstdint>
#include <string>

namespace ps2::disc {

bool open(const std::string& iso_path, std::string* err);
bool is_open();
uint32_t sector_count();
// Read `count` sectors starting at `lba` into dst. Sectors past the end read as zeros.
bool read(uint32_t lba, uint32_t count, uint8_t* dst);
// Look up "\DIR\FILE.EXT;1" (or "/dir/file.ext"); case-insensitive, version optional.
bool find(const std::string& path, uint32_t* lba, uint32_t* size);

} // namespace ps2::disc
