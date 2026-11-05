// mcserv (RPC server 80000400): a virtual memory card backed by a host folder.
//
// The game links the old libmc, whose RPC numbering is v1 (GetInfo = 0x01 ...).
// Requests arrive in one of two 48-byte (or larger) layouts:
//   descriptor  { port/fd, slot, flags, size, offset, origin, ee buffer, ee param, data[16] }
//   name        { port, slot, flags, maxent, ee table/curdir, name[1024] }
// mcserv writes results straight into EE memory (it would use SIF DMA). Reads leave
// the "unaligned head/tail" record at `param` empty, because the whole buffer is
// written directly.
//
// Card 1 (port 0) is <memcard dir>/slot1, an 8 MB formatted card. Port 1 is empty.
// Saves are ordinary host files, one directory per save, as on the console.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>

#include "iop.hpp"
#include "ps2/runtime.hpp"

namespace fs = std::filesystem;

namespace ps2 {
std::string g_memcard_dir = "memcard";
void memcard_config(const char* dir) { if (dir && *dir) g_memcard_dir = dir; }
}

namespace ps2::iop {

namespace {

// libmc result codes
constexpr int32_t kChangedCard = -1, kNoEntry = -4, kNotEmpty = -6, kTooManyHandles = -7, kNoCard = -10;
constexpr uint32_t kUserClusters = 8000;   // allocatable 1-KB clusters on a formatted 8 MB card

struct Handle { std::FILE* f = nullptr; fs::path path; };
std::map<int32_t, Handle> g_files;
int32_t g_next_fd = 1;
std::string g_cwd[2] = {"/", "/"};
bool g_reported[2];   // GetInfo has told the game about the card once

bool card_present(uint32_t port) { return port == 0; }
fs::path card_root(uint32_t port) { return fs::path(g_memcard_dir) / ("slot" + std::to_string(port + 1)); }

// Free clusters, counted the way the card's FAT would: each file takes whole 1-KB
// clusters and each directory one cluster per two 512-byte entries (with . and ..).
// Games compare free space before and after a save (this one sizes its padding file
// from the difference), so this must track what is really on the card.
uint32_t free_clusters(uint32_t port) {
    std::error_code ec;
    uint64_t used = 0;
    auto dir_clusters = [](uint64_t entries) { return (entries + 2 + 1) / 2; };
    uint64_t root_entries = 0;
    for (fs::recursive_directory_iterator it(card_root(port), ec), end; !ec && it != end; it.increment(ec)) {
        if (it.depth() == 0) ++root_entries;
        if (it->is_directory(ec)) {
            uint64_t n = 0;
            for (fs::directory_iterator d(it->path(), ec), e; !ec && d != e; d.increment(ec)) ++n;
            used += dir_clusters(n);
        } else {
            used += (it->file_size(ec) + 1023) / 1024;
        }
    }
    used += dir_clusters(root_entries);
    return used >= kUserClusters ? 0 : (uint32_t)(kUserClusters - used);
}

uint8_t* ee(Context* ctx, uint32_t addr, uint32_t size) { return guest_ptr(ctx, addr & 0x1FFFFFFF, size); }

// "/BASLUS-20887/data" or "data" relative to the card's current directory -> components
std::string normalize(uint32_t port, const std::string& name) {
    std::string p = (!name.empty() && name[0] == '/') ? name : g_cwd[port & 1] + "/" + name;
    std::string out;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string part = p.substr(i, j - i);
        if (part == "..") { const size_t k = out.rfind('/'); out = k == std::string::npos ? "" : out.substr(0, k); }
        else if (!part.empty() && part != ".") out += "/" + part;
        i = j + 1;
    }
    return out.empty() ? "/" : out;
}
fs::path host(uint32_t port, const std::string& card_path) {
    fs::path p = card_root(port);
    if (card_path != "/") p /= card_path.substr(1);
    return p;
}

bool wildcard(const char* pat, const char* s) {   // '*' and '?'
    if (!*pat) return !*s;
    if (*pat == '*') return wildcard(pat + 1, s) || (*s && wildcard(pat, s + 1));
    return *s && (*pat == '?' || *pat == *s) && wildcard(pat + 1, s + 1);
}

std::string name_of(uint32_t buff) {
    const char* n = reinterpret_cast<const char*>(ram(buff + 20, 1024));
    return std::string(n, strnlen(n, 1024));
}

// sceMcTblGetDir (64 bytes)
void fill_entry(uint8_t* e, const std::string& name, bool dir, uint32_t size, std::time_t t) {
    std::memset(e, 0, 64);
    std::tm tm{};
    localtime_s(&tm, &t);
    for (int k = 0; k < 2; ++k) {   // created, modified
        uint8_t* d = e + k * 8;
        d[1] = (uint8_t)tm.tm_sec; d[2] = (uint8_t)tm.tm_min; d[3] = (uint8_t)tm.tm_hour;
        d[4] = (uint8_t)tm.tm_mday; d[5] = (uint8_t)(tm.tm_mon + 1);
        const uint16_t y = (uint16_t)(tm.tm_year + 1900);
        std::memcpy(d + 6, &y, 2);
    }
    std::memcpy(e + 16, &size, 4);
    const uint16_t attr = dir ? 0x8427 : 0x8497;
    std::memcpy(e + 20, &attr, 2);
    std::memcpy(e + 32, name.c_str(), std::min<size_t>(name.size(), 31));
}

std::time_t mtime(const fs::path& p) {
    std::error_code ec;
    const auto ft = fs::last_write_time(p, ec);
    if (ec) return std::time(nullptr);
    const auto sys = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                                             ft - fs::file_time_type::clock::now());
    return std::chrono::system_clock::to_time_t(sys);
}

int32_t get_info(Call& c) {
    const uint32_t port = rd32(c.buff + 4);   // word 0 is not cleared by libmc and holds stale data
    const uint32_t param = rd32(c.buff + 28);
    if (!card_present(port & 1)) return kNoCard;
    std::error_code ec;
    fs::create_directories(card_root(port & 1), ec);
    if (param) {   // 0xC0-byte block read by libmc's completion callback (0x124e70)
        uint8_t* info = ee(c.ctx, param, 0xC0);
        const uint32_t type = 2, free = free_clusters(port & 1), formatted = 1;   // PS2 card, 1-KB clusters
        std::memcpy(info + 0, &type, 4);
        std::memcpy(info + 4, &free, 4);
        std::memcpy(info + 0x90, &formatted, 4);
    }
    if (!g_reported[port & 1]) { g_reported[port & 1] = true; return kChangedCard; }
    return 0;
}

int32_t open_file(Call& c) {
    const uint32_t port = rd32(c.buff) & 1, flags = rd32(c.buff + 8);
    const std::string cp = normalize(port, name_of(c.buff));
    const fs::path hp = host(port, cp);
    std::error_code ec;
    if (flags & 0x40) {   // mkdir
        if (fs::exists(hp, ec)) return kNoEntry;   // libmc reports an existing entry as -4 on mkdir
        fs::create_directories(hp, ec);
        return ec ? kNoEntry : 0;
    }
    if (g_files.size() >= 16) return kTooManyHandles;
    const bool create = flags & 0x200, write = (flags & 3) != 1;
    if (!fs::exists(hp, ec)) {
        if (!create) return kNoEntry;
        if (!fs::exists(hp.parent_path(), ec)) return kNoEntry;
        std::FILE* f = nullptr;
        if (!_wfopen_s(&f, hp.c_str(), L"wb") && f) std::fclose(f);
    } else if (fs::is_directory(hp, ec)) {
        return kNoEntry;
    }
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, hp.c_str(), write ? L"r+b" : L"rb") || !f) return kNoEntry;
    const int32_t fd = g_next_fd++;
    g_files[fd] = Handle{f, hp};
    return fd;
}

Handle* handle(uint32_t fd) {
    auto it = g_files.find((int32_t)fd);
    return it == g_files.end() ? nullptr : &it->second;
}

int32_t read_file(Call& c) {
    Handle* h = handle(rd32(c.buff));
    const uint32_t size = rd32(c.buff + 12), buf = rd32(c.buff + 24), param = rd32(c.buff + 28);
    if (param) std::memset(ee(c.ctx, param, 48), 0, 48);   // no unaligned head/tail to copy
    if (!h) return kNoEntry;
    return (int32_t)std::fread(ee(c.ctx, buf, size), 1, size, h->f);
}

int32_t write_file(Call& c) {
    Handle* h = handle(rd32(c.buff));
    if (!h) return kNoEntry;
    const uint32_t size = rd32(c.buff + 12), head = rd32(c.buff + 20), buf = rd32(c.buff + 24);
    size_t n = 0;
    if (head) n += std::fwrite(ram(c.buff + 32, head), 1, std::min(head, 16u), h->f);   // unaligned start, sent inline
    if (size) n += std::fwrite(ee(c.ctx, buf, size), 1, size, h->f);
    return (int32_t)n;
}

int32_t seek_file(Call& c) {
    Handle* h = handle(rd32(c.buff));
    if (!h) return kNoEntry;
    const int32_t offset = (int32_t)rd32(c.buff + 16), origin = (int32_t)rd32(c.buff + 20);
    std::fseek(h->f, offset, origin == 1 ? SEEK_CUR : origin == 2 ? SEEK_END : SEEK_SET);
    return (int32_t)std::ftell(h->f);
}

int32_t close_file(Call& c) {
    auto it = g_files.find((int32_t)rd32(c.buff));
    if (it == g_files.end()) return kNoEntry;
    std::fclose(it->second.f);
    g_files.erase(it);
    return 0;
}

int32_t get_dir(Call& c) {
    const uint32_t port = rd32(c.buff) & 1, maxent = rd32(c.buff + 12), table = rd32(c.buff + 16);
    const std::string cp = normalize(port, name_of(c.buff));
    const size_t slash = cp.rfind('/');
    const std::string dir = slash == 0 ? "/" : cp.substr(0, slash), pat = cp.substr(slash + 1);
    const fs::path hd = host(port, dir);
    std::error_code ec;
    if (!fs::is_directory(hd, ec)) return kNoEntry;
    int32_t n = 0;
    auto emit = [&](const std::string& name, bool is_dir, uint32_t size, std::time_t t) {
        if ((uint32_t)n >= maxent || !wildcard(pat.c_str(), name.c_str())) return;
        if (table) fill_entry(ee(c.ctx, table + n * 64, 64), name, is_dir, size, t);
        ++n;
    };
    if (dir != "/") { emit(".", true, 0, mtime(hd)); emit("..", true, 0, mtime(hd)); }
    for (const auto& e : fs::directory_iterator(hd, ec)) {
        const bool d = e.is_directory(ec);
        emit(e.path().filename().string(), d, d ? 0 : (uint32_t)e.file_size(ec), mtime(e.path()));
    }
    return n;
}

int32_t change_dir(Call& c) {
    const uint32_t port = rd32(c.buff) & 1, curdir = rd32(c.buff + 16);
    const std::string name = name_of(c.buff);
    if (curdir) {   // the previous directory goes back to the EE
        char* out = reinterpret_cast<char*>(ee(c.ctx, curdir, 1024));
        std::snprintf(out, 1024, "%s", g_cwd[port].c_str());
    }
    if (name.empty()) return 0;
    const std::string cp = normalize(port, name);
    std::error_code ec;
    if (!fs::is_directory(host(port, cp), ec)) return kNoEntry;
    g_cwd[port] = cp;
    return 0;
}

int32_t delete_entry(Call& c) {
    const uint32_t port = rd32(c.buff) & 1;
    const fs::path hp = host(port, normalize(port, name_of(c.buff)));
    std::error_code ec;
    if (!fs::exists(hp, ec)) return kNoEntry;
    if (fs::is_directory(hp, ec) && !fs::is_empty(hp, ec)) return kNotEmpty;
    fs::remove(hp, ec);
    return ec ? kNoEntry : 0;
}

uint32_t mc_rpc(Call& c) {
    uint32_t* r = reinterpret_cast<uint32_t*>(ram(c.buff, std::max(c.rsize, 12u)));
    const uint32_t fno = c.fno & 0xFF;
    if (fno == 0xFE || fno == 0x70) {   // Init: result, mcserv and mcman versions
        r[0] = 0;
        r[1] = 0x20A;
        r[2] = 0x20E;
        return c.buff;
    }
    int32_t res;
    const bool by_port = fno == 0x02 || fno == 0x0C || fno == 0x0D || fno == 0x0F || fno == 0x10 || fno == 0x11 || fno == 0x12;
    if (by_port && !card_present(rd32(c.buff) & 1)) {
        std::memset(r, 0, c.rsize);
        r[0] = (uint32_t)kNoCard;
        return c.buff;
    }
    switch (fno) {
    case 0x01: res = get_info(c); break;
    case 0x02: res = open_file(c); break;
    case 0x03: res = close_file(c); break;
    case 0x04: res = seek_file(c); break;
    case 0x05: res = read_file(c); break;
    case 0x06: res = write_file(c); break;
    case 0x0A: { Handle* h = handle(rd32(c.buff)); if (h) std::fflush(h->f); res = h ? 0 : kNoEntry; break; }   // flush
    case 0x0C: res = change_dir(c); break;
    case 0x0D: res = get_dir(c); break;
    case 0x0E: res = 0; break;    // SetFileInfo: attributes/times are not kept
    case 0x0F: res = delete_entry(c); break;
    case 0x10: case 0x11: res = 0; break;   // format / unformat: the folder is always a formatted card
    case 0x12: res = (int32_t)free_clusters(rd32(c.buff) & 1); break;   // GetEntSpace
    default: {
        const uint32_t* q = reinterpret_cast<const uint32_t*>(ram(c.buff, 32));
        log(Log::Iop, "memory card function %02x not handled: %08x %08x %08x %08x %08x %08x %08x %08x", fno,
            q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
        res = 0;
        break;
    }
    }
    if (fno != 0x01 || log_enabled(Log::Iop))
        log(Log::Iop, "mc %02x %s [%x %x %x] -> %d", fno, (fno == 0x02 || fno == 0x0C || fno == 0x0D || fno == 0x0F) ? name_of(c.buff).c_str() : "",
            rd32(c.buff + 8), rd32(c.buff + 16), rd32(c.buff + 20), res);
    std::memset(r, 0, c.rsize);
    r[0] = (uint32_t)res;
    return c.buff;
}

} // namespace

void register_mc_service() {
    for (auto& [fd, h] : g_files) std::fclose(h.f);
    g_files.clear();
    g_next_fd = 1;
    g_cwd[0] = g_cwd[1] = "/";
    g_reported[0] = g_reported[1] = false;
    register_server(0x80000400, "mcserv", 0x500, mc_rpc);
}

} // namespace ps2::iop
