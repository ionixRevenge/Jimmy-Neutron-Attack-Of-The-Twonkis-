#include "disc.hpp"
#include "perf.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace ps2::disc {

namespace {

constexpr uint32_t kSector = 2048;

struct State {
    std::FILE* f = nullptr;
    uint32_t sectors = 0;
    uint32_t root_lba = 0, root_size = 0;
    std::mutex m;
} S;

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// Find `name` (no version) in a directory extent. Returns false if absent.
bool find_in_dir(uint32_t lba, uint32_t size, const std::string& name, uint32_t* out_lba, uint32_t* out_size, bool* is_dir) {
    std::vector<uint8_t> buf((size + kSector - 1) / kSector * kSector);
    if (!read(lba, (uint32_t)(buf.size() / kSector), buf.data())) return false;
    for (uint32_t off = 0; off < size;) {
        const uint8_t len = buf[off];
        if (len == 0) {   // records never span sectors: skip to the next one
            off = (off / kSector + 1) * kSector;
            continue;
        }
        const uint8_t* r = &buf[off];
        const uint8_t nlen = r[32];
        std::string id(reinterpret_cast<const char*>(r + 33), nlen);
        if (const size_t semi = id.find(';'); semi != std::string::npos) id.resize(semi);
        if (!id.empty() && id.back() == '.') id.pop_back();
        if (nlen > 1 || (r[33] != 0 && r[33] != 1)) {
            if (upper(id) == name) {
                *out_lba = le32(r + 2);
                *out_size = le32(r + 10);
                *is_dir = (r[25] & 2) != 0;
                return true;
            }
        }
        off += len;
    }
    return false;
}

} // namespace

bool open(const std::string& iso_path, std::string* err) {
    if (S.f) std::fclose(S.f);
    S.f = std::fopen(iso_path.c_str(), "rb");
    if (!S.f) { *err = "cannot open " + iso_path; return false; }
    _fseeki64(S.f, 0, SEEK_END);
    S.sectors = (uint32_t)(_ftelli64(S.f) / kSector);
    uint8_t pvd[kSector];
    if (!read(16, 1, pvd) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) {
        *err = iso_path + " is not an ISO9660 image";
        std::fclose(S.f);
        S.f = nullptr;
        return false;
    }
    S.root_lba = le32(pvd + 156 + 2);
    S.root_size = le32(pvd + 156 + 10);
    return true;
}

bool is_open() { return S.f != nullptr; }
uint32_t sector_count() { return S.sectors; }

bool read(uint32_t lba, uint32_t count, uint8_t* dst) {
    if (!S.f) return false;
    std::lock_guard<std::mutex> lk(S.m);
    uint32_t avail = lba < S.sectors ? std::min(count, S.sectors - lba) : 0;
    PerfScope timed(kPerfDiscNs);
    perf_add(kPerfDiscBytes, (uint64_t)count * kSector);
    if (avail) {
        _fseeki64(S.f, (int64_t)lba * kSector, SEEK_SET);
        if (std::fread(dst, kSector, avail, S.f) != avail) return false;
    }
    std::memset(dst + (size_t)avail * kSector, 0, (size_t)(count - avail) * kSector);
    return true;
}

bool find(const std::string& path, uint32_t* lba, uint32_t* size) {
    if (!S.f) return false;
    uint32_t cur = S.root_lba, cur_size = S.root_size;
    bool dir = true;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && (path[i] == '\\' || path[i] == '/')) ++i;
        size_t j = i;
        while (j < path.size() && path[j] != '\\' && path[j] != '/') ++j;
        if (j == i) break;
        std::string part = upper(path.substr(i, j - i));
        if (const size_t semi = part.find(';'); semi != std::string::npos) part.resize(semi);
        if (!dir || !find_in_dir(cur, cur_size, part, &cur, &cur_size, &dir)) return false;
        i = j;
    }
    *lba = cur;
    *size = cur_size;
    return true;
}

} // namespace ps2::disc

namespace ps2 {
bool disc_open(const std::string& iso, std::string* err) { return disc::open(iso, err); }
}
