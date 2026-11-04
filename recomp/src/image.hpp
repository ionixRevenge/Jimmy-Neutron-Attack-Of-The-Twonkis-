// Guest executable image + analysis inputs (region map, function list).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace recomp {

struct Segment {
    uint32_t vaddr = 0;
    std::vector<uint8_t> data;
};

struct Region {
    uint32_t start, end;
    std::string kind;        // ee | vu | data
    std::string confidence;  // high | med | low
    std::string label;
};

struct Function {
    uint32_t start, end;
};

class Image {
public:
    bool load_elf(const std::string& path, std::string* err);

    uint32_t entry() const { return entry_; }
    bool contains(uint32_t addr, uint32_t size = 4) const;
    uint32_t read32(uint32_t addr) const;
    const std::vector<Segment>& segments() const { return segs_; }

private:
    uint32_t entry_ = 0;
    std::vector<Segment> segs_;
};

bool load_regions(const std::string& path, std::vector<Region>* out, std::string* err);
// analysis/functions.tsv produced by tools/analysis/elfmap.py
bool load_functions(const std::string& path, std::vector<Function>* out, std::string* err);

} // namespace recomp
