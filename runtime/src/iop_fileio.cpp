// fileio (RPC server 80000001), protocol version 2.2 as shipped in IOPRP300.
//
// Every request begins { sema_id, result_ptr, result_size }. The IOP answers by
// writing a 32-byte reply { sema_id, command, result_ptr, result_size, result,
// 0, 0, 0 } to the EE buffer registered at init, then sending sifcmd 80000011;
// the EE library's handler copies the result and signals the semaphore.
// (Layouts as documented by the Play! emulator's FileIoHandler2200.)
//
// Devices: cdrom0:/cdrom: are files on the ISO. Writes are refused (saves go
// through the memory card).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "disc.hpp"
#include "iop.hpp"
#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2::iop {

namespace {

enum Cmd : uint32_t {
    kOpen = 0, kClose = 1, kRead = 2, kWrite = 3, kSeek = 4, kRemove = 6, kMkdir = 7, kRmdir = 8,
    kDopen = 9, kDclose = 10, kDread = 11, kGetstat = 12, kChstat = 13, kChdir = 18, kSync = 19,
    kSeek64 = 22, kDevctl = 23, kIoctl2 = 26,
};
constexpr int32_t kENOENT = -2, kEBADF = -9, kEACCES = -13, kENODEV = -19;
constexpr uint32_t kSector = 2048;

struct File { uint32_t lba = 0, size = 0, pos = 0; std::string path; };

struct State {
    uint32_t result_ptr[2] = {0, 0};
    std::map<int32_t, File> files;
    int32_t next_fd = 3;
} F;

std::string strip_device(const std::string& p, bool* is_disc) {
    const size_t colon = p.find(':');
    std::string dev = colon == std::string::npos ? "" : p.substr(0, colon);
    for (char& c : dev) c = (char)std::tolower((unsigned char)c);
    *is_disc = dev == "cdrom0" || dev == "cdrom";
    return colon == std::string::npos ? p : p.substr(colon + 1);
}

int32_t do_open(const std::string& path, uint32_t flags) {
    bool disc;
    const std::string rest = strip_device(path, &disc);
    if (!disc) return kENODEV;
    if (flags & 0x0002) return kEACCES;   // O_WRONLY / O_RDWR on a read-only device
    File f;
    if (!disc::find(rest, &f.lba, &f.size)) return kENOENT;
    f.path = path;
    const int32_t fd = F.next_fd++;
    F.files[fd] = f;
    return fd;
}

int32_t do_read(Context* ctx, int32_t fd, uint32_t ee, uint32_t n) {
    auto it = F.files.find(fd);
    if (it == F.files.end()) return kEBADF;
    File& f = it->second;
    n = std::min(n, f.size - std::min(f.pos, f.size));
    if (!n) return 0;
    uint8_t* dst = guest_ptr(ctx, ee & 0x1FFFFFFF, n);
    const uint32_t first = f.pos / kSector, last = (f.pos + n - 1) / kSector;
    std::vector<uint8_t> buf((size_t)(last - first + 1) * kSector);
    disc::read(f.lba + first, last - first + 1, buf.data());
    std::memcpy(dst, buf.data() + (f.pos - first * kSector), n);
    f.pos += n;
    return (int32_t)n;
}

int32_t do_seek(int32_t fd, int32_t off, uint32_t whence) {
    auto it = F.files.find(fd);
    if (it == F.files.end()) return kEBADF;
    File& f = it->second;
    int64_t base = whence == 1 ? f.pos : whence == 2 ? f.size : 0;
    int64_t p = base + off;
    if (p < 0) return -22;   // EINVAL
    f.pos = (uint32_t)p;
    return (int32_t)f.pos;
}

// io_stat_t: mode, attr, size, ctime[8], atime[8], mtime[8], hisize, private[6]
int32_t do_getstat(Context* ctx, const std::string& path, uint32_t ee_stat) {
    bool disc;
    const std::string rest = strip_device(path, &disc);
    if (!disc) return kENODEV;
    uint32_t lba, size;
    if (!disc::find(rest, &lba, &size)) return kENOENT;
    uint8_t st[64] = {};
    const uint32_t mode = 0x10 | 0x7;   // regular file, readable
    std::memcpy(st, &mode, 4);
    std::memcpy(st + 8, &size, 4);
    std::memcpy(st + 36, &lba, 4);      // private[0]: first sector
    std::memcpy(guest_ptr(ctx, ee_stat & 0x1FFFFFFF, sizeof st), st, sizeof st);
    return 0;
}

void reply(Context* ctx, uint32_t req, uint32_t cmd, int32_t result) {
    if (F.result_ptr[0]) {
        uint32_t r[8] = {rd32(req + 0), cmd, rd32(req + 4), rd32(req + 8), (uint32_t)result, 0, 0, 0};
        std::memcpy(guest_ptr(ctx, F.result_ptr[0], sizeof r), r, sizeof r);
    }
    send_cmd_to_ee(0x80000011, nullptr, 0);
}

std::string str(uint32_t a, uint32_t max = 256) {
    const char* p = reinterpret_cast<const char*>(ram(a, max));
    return std::string(p, strnlen(p, max));
}

uint32_t fileio_rpc(Call& c) {
    const uint32_t b = c.buff;   // request: header at +0 (12 bytes), arguments from +12
    int32_t r = 0;
    switch (c.fno) {
    case 255:   // init: { result_ptr[2] } -> 8-byte version
        F.result_ptr[0] = rd32(b) & 0x1FFFFFFF;
        F.result_ptr[1] = rd32(b + 4) & 0x1FFFFFFF;
        // Module version of IOPRP300's FILEIO; the EE library refuses to open files unless it
        // matches its own "3000" (protocol is still the 2.2 one).
        std::memcpy(ram(b, 8), "3000....", 8);
        log(Log::Iop, "fileio init: replies to %08x", F.result_ptr[0]);
        return b;
    case kOpen: {
        const std::string path = str(b + 20);
        r = do_open(path, rd32(b + 12));
        log(Log::Iop, "open \"%s\" flags=%x -> %d", path.c_str(), rd32(b + 12), r);
        if (r < 0) std::fprintf(stderr, "[iop] open %s failed (%d)\n", path.c_str(), r);
        break;
    }
    case kClose: r = F.files.erase((int32_t)rd32(b + 12)) ? 0 : kEBADF; break;
    case kRead:
        r = do_read(c.ctx, (int32_t)rd32(b + 12), rd32(b + 16), rd32(b + 20));
        log(Log::Iop, "read fd=%d %u bytes -> %d", (int32_t)rd32(b + 12), rd32(b + 20), r);
        break;
    case kWrite: {
        const int32_t fd = (int32_t)rd32(b + 12);
        if (fd == 1 || fd == 2) {   // stdout / stderr: show the game's console output
            const uint32_t n = std::min<uint32_t>(rd32(b + 20), 4096);
            const char* s = reinterpret_cast<const char*>(guest_ptr(c.ctx, rd32(b + 16) & 0x1FFFFFFF, n));
            std::fprintf(stderr, "[tty] %.*s", (int)n, s);
            r = (int32_t)n;
        } else {
            r = kEACCES;
        }
        break;
    }
    case kSeek: r = do_seek((int32_t)rd32(b + 12), (int32_t)rd32(b + 16), rd32(b + 20)); break;
    case kGetstat: r = do_getstat(c.ctx, str(b + 16), rd32(b + 12)); break;
    case kDopen: case kDclose: case kDread: case kRemove: case kMkdir: case kRmdir:
    case kChstat: case kChdir: case kSync: case kSeek64: case kDevctl: case kIoctl2:
        r = kENODEV;
        log(Log::Iop, "fileio command %u not supported", c.fno);
        break;
    default: {
        char msg[64];
        std::snprintf(msg, sizeof msg, "fileio: unknown command %u", c.fno);
        fatal(c.ctx, 0, msg);
    }
    }
    reply(c.ctx, b, c.fno, r);
    wr32(b, 1);   // RPC return word (the result itself travels in the reply)
    return b;
}

} // namespace

void register_fileio_service() {
    F = State{};
    register_server(0x80000001, "fileio", 0x1000, fileio_rpc);
}

} // namespace ps2::iop
