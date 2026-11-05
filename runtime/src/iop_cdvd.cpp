// cdvdfsv: the CD/DVD RPC servers behind Sony's EE libcdvd.
//
//   80000592  init                 80000593  S-commands (status, disc type, media mode)
//   80000595  N-commands (reads)   80000596  power-off callback
//   80000597  file search          8000059A/C disk ready
//
// Reads complete immediately: sectors are copied from the ISO straight into EE
// (or IOP) memory before the RPC reply is sent. Request layouts follow Sony's
// libcdvd as seen in SLUS_208.87 and the Play! emulator's documentation of them.
#include <cstdio>
#include <string>
#include <vector>

#include "disc.hpp"
#include "iop.hpp"
#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2::iop {

namespace {

constexpr uint32_t kSector = 2048;
constexpr uint32_t kDiscPs2Dvd = 0x14;   // SCECdPS2DVD
constexpr uint32_t kStatusPause = 0x0A;  // SCECdStatPause
constexpr uint32_t kReady = 2;           // SCECdComplete

struct Stream { bool on = false; uint32_t pos = 0, buf_sectors = 0; } g_stream;

uint32_t* words(Call& c, uint32_t n) { return reinterpret_cast<uint32_t*>(ram(c.buff, std::max(n, 1u) * 4)); }

// Reply of `n` words, zero-filled, in the server buffer.
uint32_t* reply(Call& c, uint32_t n) {
    uint32_t* r = words(c, std::max(n, c.rsize / 4));
    std::memset(r, 0, std::max(n * 4, c.rsize));
    return r;
}

void need_disc(Call& c) {
    if (!disc::is_open()) fatal(c.ctx, 0, "the game read from the disc but no ISO is loaded (pass --iso <path>)");
}

void read_to_ee(Call& c, uint32_t lba, uint32_t count, uint32_t ee) {
    need_disc(c);
    disc::read(lba, count, guest_ptr(c.ctx, ee, count * kSector));
}

uint32_t init_592(Call& c) {
    if (c.fno != 0) fatal(c.ctx, 0, "cdvdfsv 592: unknown function");
    uint32_t* r = reply(c, 4);
    r[3] = 0xFF;
    log(Log::Iop, "cdInit(mode=%u)", c.ssize >= 4 ? rd32(c.buff) : 0);
    return c.buff;
}

uint32_t scmd_593(Call& c) {
    const uint32_t arg0 = c.ssize >= 4 ? rd32(c.buff) : 0;
    uint32_t* r = reply(c, 4);
    switch (c.fno) {
    case 0x01: r[0] = 1; break;                        // ReadClock: ok, clock zeros
    case 0x03: r[0] = kDiscPs2Dvd; break;              // GetDiskType
    case 0x04: r[0] = 0; break;                        // GetError: none
    case 0x05: r[0] = 1; r[1] = 0; break;              // TrayReq: no tray change
    case 0x0C: r[0] = kStatusPause; break;             // Status
    case 0x16: r[0] = 1; break;                        // Break
    case 0x22: r[0] = 1; break;                        // SetMediaMode
    case 0x27: r[0] = 1; r[1] = 0; break;              // ReadDvdDualInfo: single layer
    default: {
        char msg[64];
        std::snprintf(msg, sizeof msg, "cdvdfsv 593: unknown S-command %u", c.fno);
        fatal(c.ctx, 0, msg);
    }
    }
    log(Log::Iop, "cd S-command %u(arg %u) -> %u", c.fno, arg0, r[0]);
    return c.buff;
}

uint32_t ncmd_595(Call& c) {
    const uint32_t a[5] = {rd32(c.buff), rd32(c.buff + 4), rd32(c.buff + 8), rd32(c.buff + 12), rd32(c.buff + 16)};
    switch (c.fno) {
    case 0x01:   // Read(lba, count, ee_dst, mode)
        log(Log::Iop, "cdRead lba=%u count=%u -> %08x", a[0], a[1], a[2]);
        read_to_ee(c, a[0], a[1], a[2]);
        reply(c, 1)[0] = 0;
        break;
    case 0x0D:   // ReadIopMem(lba, count, iop_dst, mode)
        need_disc(c);
        log(Log::Iop, "cdReadIOPm lba=%u count=%u -> iop %06x", a[0], a[1], a[2]);
        disc::read(a[0], a[1], ram(a[2], a[1] * kSector));
        reply(c, 1)[0] = 0;
        break;
    case 0x0F: {   // ReadChain: up to 64 (lba, count, dst) tuples, ~0 terminated
        need_disc(c);
        std::vector<uint32_t> t(64 * 3);
        std::memcpy(t.data(), ram(c.buff, (uint32_t)t.size() * 4), t.size() * 4);
        for (uint32_t i = 0; i < 64; ++i) {
            const uint32_t lba = t[i * 3], n = t[i * 3 + 1], dst = t[i * 3 + 2];
            if (lba == ~0u || n == ~0u || dst == ~0u) break;
            if (dst & 1) disc::read(lba, n, ram(dst & ~1u, n * kSector));   // bit 0: IOP destination
            else read_to_ee(c, lba, n, dst);
        }
        reply(c, 1)[0] = 0;
        break;
    }
    case 0x04: reply(c, 1)[0] = 1; break;   // GetToc
    case 0x05: reply(c, 1)[0] = 1; break;   // Seek
    case 0x08: reply(c, 1)[0] = 1; break;   // Pause
    case 0x0E: reply(c, 1)[0] = kReady; break;   // NDiskReady
    case 0x09: {   // Stream(lba, count, dst, cmd, mode)
        uint32_t* r = reply(c, 1);
        switch (a[3]) {
        case 1: g_stream.on = true; g_stream.pos = a[0]; r[0] = 1; break;      // start
        case 2:                                                                // read
            read_to_ee(c, g_stream.pos, a[1], a[2]);
            g_stream.pos += a[1];
            r[0] = a[1];
            break;
        case 3: g_stream.on = false; r[0] = 1; break;                          // stop
        case 4: case 9: g_stream.pos = a[0]; r[0] = 1; break;                  // seek
        case 5: g_stream.buf_sectors = a[0]; r[0] = 1; break;                  // init
        case 6: r[0] = g_stream.buf_sectors; break;                            // status
        default: fatal(c.ctx, 0, "cdvdfsv: unknown stream command");
        }
        log(Log::Iop, "cdStream cmd=%u lba=%u count=%u -> %u", a[3], a[0], a[1], r[0]);
        break;
    }
    default: {
        char msg[64];
        std::snprintf(msg, sizeof msg, "cdvdfsv 595: unknown N-command %u", c.fno);
        fatal(c.ctx, 0, msg);
    }
    }
    return c.buff;
}

uint32_t poweroff_596(Call& c) {
    reply(c, 1);
    return c.buff;
}

// SearchFile: request is { sceCdlFILE file (36 bytes); char path[256]; u32 ee_file; u32 layer }.
// The result file entry is written back to the EE copy of the request at ee_file.
uint32_t search_597(Call& c) {
    const uint32_t path_off = c.ssize == 0x124 ? 0x20 : 0x24;
    const uint32_t ee_off = c.ssize == 0x124 ? 0x120 : 0x124;
    const std::string path(reinterpret_cast<const char*>(ram(c.buff + path_off, 256)));
    need_disc(c);
    uint32_t lba = 0, size = 0;
    const bool found = disc::find(path, &lba, &size);
    log(Log::Iop, "cdSearchFile \"%s\" -> %s lba=%u size=%u", path.c_str(), found ? "found" : "missing", lba, size);
    if (!found) std::fprintf(stderr, "[iop] WARNING: cdSearchFile: %s is not on the disc\n", path.c_str());
    if (found) {
        uint8_t file[36] = {};
        std::memcpy(file, &lba, 4);
        std::memcpy(file + 4, &size, 4);
        const size_t slash = path.find_last_of("\\/");
        const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        std::memcpy(file + 8, name.c_str(), std::min<size_t>(name.size(), 15));
        const uint32_t ee = rd32(c.buff + ee_off);
        std::memcpy(guest_ptr(c.ctx, ee, 36), file, 36);
    }
    reply(c, 1)[0] = found ? 1 : 0;
    return c.buff;
}

uint32_t ready_59c(Call& c) {
    reply(c, 1)[0] = kReady;
    return c.buff;
}

} // namespace

void register_cdvd_services() {
    g_stream = Stream{};
    register_server(0x80000592, "cdvdfsv init", 0x40, init_592);
    register_server(0x80000593, "cdvdfsv scmd", 0x40, scmd_593);
    register_server(0x80000595, "cdvdfsv ncmd", 0x400, ncmd_595);
    register_server(0x80000596, "cdvdfsv poweroff", 0x40, poweroff_596);
    register_server(0x80000597, "cdvdfsv search", 0x140, search_597);
    register_server(0x8000059A, "cdvdfsv ready", 0x40, ready_59c);
    register_server(0x8000059C, "cdvdfsv ready", 0x40, ready_59c);
}

} // namespace ps2::iop
