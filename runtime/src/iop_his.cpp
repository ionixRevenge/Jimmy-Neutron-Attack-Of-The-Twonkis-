// HIS.IRX: the game's own IOP sound driver (RPC server 10000000).
//
// Placeholder until the driver is reverse-engineered: it answers the version
// handshake, serves the driver's disc reader, and accepts every other request
// silently, so the game runs without sound. Known so far (EE wrappers at
// 0x132ab0-0x133560):
//   fno 0    init(output mode: 0/1/2)      16-byte request
//   fno 1    version -> 0x00010802         the game aborts on any other value
//   fno 256  open file: 48-byte path ("cdrom0:\DATA\X.PAK;1") -> file id, -1 if missing
//   fno 257  disc read: { file id, sector in file, count, dest, dest_is_iop, prio, 0 } -> handle
//   fno 258  read status(handle) -> 5 when finished
//   fno 260  release(handle)
//   fno 263  service the read queue (the EE polls 258/263 until done)
//   fno 265  file size(file id)
//   fno 270  stream pause/resume around fileio access
// Reads complete synchronously here, so a request is already finished when 257 returns.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "disc.hpp"
#include "iop.hpp"
#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2::iop {

namespace {

std::set<uint32_t> g_seen;
uint32_t g_next_handle = 1;
constexpr uint32_t kReadDone = 5;
struct DiscFile { uint32_t lba, size; };
std::vector<DiscFile> g_files;

uint32_t open_file(Call& c) {
    const char* p = reinterpret_cast<const char*>(ram(c.buff, 48));
    std::string path(p, strnlen(p, 48));
    if (const size_t colon = path.find(':'); colon != std::string::npos) path.erase(0, colon + 1);
    DiscFile f;
    if (!disc::find(path, &f.lba, &f.size)) {
        std::fprintf(stderr, "[iop] his open %s: not found\n", path.c_str());
        return 0xFFFFFFFFu;
    }
    g_files.push_back(f);
    log(Log::Iop, "his open %s -> %zu (lba %u, %u bytes)", path.c_str(), g_files.size() - 1, f.lba, f.size);
    return (uint32_t)(g_files.size() - 1);
}

uint32_t file_size(uint32_t id) { return id < g_files.size() ? g_files[id].size : 0; }

// fno 257: read sectors from the disc into EE (or IOP) memory.
uint32_t disc_read(Call& c) {
    const uint32_t id = rd32(c.buff), count = rd32(c.buff + 8);
    if (id >= g_files.size()) {
        std::fprintf(stderr, "[iop] his read: unknown file id %u\n", id);
        return g_next_handle++;
    }
    const uint32_t lba = g_files[id].lba + rd32(c.buff + 4);
    const uint32_t dest = rd32(c.buff + 12), to_iop = rd32(c.buff + 16);
    if (count == 0 || count > 0x4000) {   // more than 32 MB: a request we misunderstand
        std::fprintf(stderr, "[iop] his read: bad request %08x %08x %08x %08x %08x %08x %08x\n", rd32(c.buff),
                     rd32(c.buff + 4), count, dest, to_iop, rd32(c.buff + 20), rd32(c.buff + 24));
        return g_next_handle++;
    }
    const uint32_t bytes = count * 2048;
    uint8_t* dst = to_iop ? ram(dest & (kRamSize - 1), bytes) : guest_ptr(c.ctx, dest & 0x1FFFFFFF, bytes);
    disc::read(lba, count, dst);
    log(Log::Iop, "his read lba %u x%u -> %s %08x", lba, count, to_iop ? "iop" : "ee", dest);
    return g_next_handle++;
}

uint32_t his_rpc(Call& c) {
    uint32_t result = 0;
    switch (c.fno) {
    case 1: result = 0x00010802; break;   // version
    case 256: result = open_file(c); break;
    case 257: result = disc_read(c); break;
    case 265: result = file_size(rd32(c.buff)); break;
    case 258: result = kReadDone; break;
    case 260: case 263: case 270: break;
    default:
        if (g_seen.insert(c.fno).second)
            std::fprintf(stderr, "[iop] sound driver: function %u (send %u, recv %u) - no audio yet\n", c.fno, c.ssize, c.rsize);
        break;
    }
    std::memset(ram(c.buff, std::max(c.rsize, 4u)), 0, c.rsize);
    wr32(c.buff, result);
    return c.buff;
}

} // namespace

void register_his_service() {
    g_seen.clear();
    g_next_handle = 1;
    g_files.clear();
    register_server(0x10000000, "HIS sound", 0x4000, his_rpc);
}

} // namespace ps2::iop
