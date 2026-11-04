#include "image.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

namespace recomp {

namespace {
template <typename T>
T rd(const std::vector<uint8_t>& b, size_t off) {
    T v;
    std::memcpy(&v, b.data() + off, sizeof v);
    return v;
}
} // namespace

bool Image::load_elf(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { *err = "cannot open " + path; return false; }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 0x34 || std::memcmp(b.data(), "\x7f" "ELF", 4) != 0 || b[4] != 1 || b[5] != 1) {
        *err = "not a 32-bit little-endian ELF";
        return false;
    }
    entry_ = rd<uint32_t>(b, 0x18);
    const uint32_t phoff = rd<uint32_t>(b, 0x1C);
    const uint16_t phnum = rd<uint16_t>(b, 0x2C);
    for (uint16_t i = 0; i < phnum; ++i) {
        const size_t p = phoff + i * 32u;
        if (p + 32 > b.size()) break;
        const uint32_t type = rd<uint32_t>(b, p), off = rd<uint32_t>(b, p + 4), vaddr = rd<uint32_t>(b, p + 8);
        const uint32_t filesz = rd<uint32_t>(b, p + 16), memsz = rd<uint32_t>(b, p + 20);
        if (type != 1 || memsz == 0) continue;
        if ((size_t)off + filesz > b.size()) { *err = "segment past end of file"; return false; }
        Segment s;
        s.vaddr = vaddr;
        s.data.assign(b.begin() + off, b.begin() + off + filesz);
        s.data.resize(memsz, 0);
        segs_.push_back(std::move(s));
    }
    if (segs_.empty()) { *err = "no loadable segments"; return false; }
    return true;
}

bool Image::contains(uint32_t addr, uint32_t size) const {
    for (const auto& s : segs_)
        if (addr >= s.vaddr && (uint64_t)addr + size <= (uint64_t)s.vaddr + s.data.size()) return true;
    return false;
}

uint32_t Image::read32(uint32_t addr) const {
    for (const auto& s : segs_)
        if (addr >= s.vaddr && (uint64_t)addr + 4 <= (uint64_t)s.vaddr + s.data.size())
            return rd<uint32_t>(s.data, addr - s.vaddr);
    return 0;
}

bool load_regions(const std::string& path, std::vector<Region>* out, std::string* err) {
    std::ifstream f(path);
    if (!f) { *err = "cannot open " + path; return false; }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string a, b;
        Region r;
        if (!(ss >> a >> b >> r.kind >> r.confidence)) continue;
        std::getline(ss >> std::ws, r.label);
        r.start = (uint32_t)std::stoul(a, nullptr, 16);
        r.end = (uint32_t)std::stoul(b, nullptr, 16);
        out->push_back(r);
    }
    return true;
}

bool load_functions(const std::string& path, std::vector<Function>* out, std::string* err) {
    std::ifstream f(path);
    if (!f) { *err = "cannot open " + path; return false; }
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string addr, size;
        if (!std::getline(ss, addr, '\t') || !std::getline(ss, size, '\t')) continue;
        Function fn;
        fn.start = (uint32_t)std::stoul(addr, nullptr, 16);
        fn.end = fn.start + (uint32_t)std::stoul(size);
        out->push_back(fn);
    }
    return true;
}

} // namespace recomp
