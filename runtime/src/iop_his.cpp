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
//   fno 513  voice status masks (polled every frame), see voice_status()
//   fno 768  voice command { u16 voice, u16 cmd, u32 addr, u32 size, u32 start }:
//            cmd 0x13 starts streaming SPU2 ADPCM from an IOP ring buffer, cmd 2 stops
//   fno 769  { u16 voice } -> current play address in that ring (the EE refills behind it)
//   fno 770  { voice, iop buffer, size, block } block-transfer (PCM) buffer setup
//   fno 771  { u16 cmd, u16 value }: 0x0201 = sample rate, 0x0f81/0x1081 = volumes, ...
// Until the SPU2 is emulated, streaming voices are virtual: their play position
// advances in real time at the stream's byte rate, which paces the game's stream
// pumps (movies, music). Too slow a rate starves the movie demuxer: its audio ring
// fills, so it hands the IPU a partial qword and the video decode hits garbage.
// Reads complete synchronously here, so a request is already finished when 257 returns.
#include <algorithm>
#include <chrono>
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

// ---- virtual stream voices ------------------------------------------------------------
struct Voice {
    bool active = false;
    uint32_t addr = 0, size = 0, start = 0;
    std::chrono::steady_clock::time_point t0;
};
Voice g_voice[48];
uint32_t g_sample_rate = 48000;

// fno 770 sets up a block-transfer buffer: those voices carry 16-bit stereo PCM
// (the movies' SShd audio), everything else SPU2 ADPCM.
bool g_pcm[48];
double bytes_per_second(uint32_t voice) {
    return g_pcm[voice] ? g_sample_rate * 4.0 : g_sample_rate / 28.0 * 16.0;
}

uint32_t voice_command(Call& c) {
    const uint32_t w0 = rd32(c.buff), voice = w0 & 0xFFFF, cmd = w0 >> 16;
    if (voice >= 48) return 0;
    Voice& v = g_voice[voice];
    switch (cmd) {
    case 0x13:
        v.active = true;
        v.addr = rd32(c.buff + 4);
        v.size = rd32(c.buff + 8);
        v.start = rd32(c.buff + 12) - v.addr;
        v.t0 = std::chrono::steady_clock::now();
        log(Log::Iop, "his voice %u: stream %08x size %x", voice, v.addr, v.size);
        break;
    case 2: v.active = false; break;
    default: break;
    }
    return 0;
}

uint32_t voice_position(Call& c) {
    const uint32_t voice = rd32(c.buff) & 0xFFFF;
    if (voice >= 48 || !g_voice[voice].active || !g_voice[voice].size) return voice < 48 ? g_voice[voice].addr : 0;
    const Voice& v = g_voice[voice];
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - v.t0).count();
    const uint64_t played = (uint64_t)(secs * bytes_per_second(voice)) & ~15ull;
    return v.addr + (uint32_t)((v.start + played) % v.size);
}

// fno 513: three 48-bit voice masks { playing, keyed on, ended (SPU2 ENDX) }, two words
// each. The game drops a sound only once its voice reports ENDX, and after stopping all
// sounds it waits for that (e.g. leaving the save screen), so without an SPU2 every
// one-shot voice is reported as started and already finished. Virtual stream voices play.
void voice_status(Call& c) {
    uint64_t playing = 0;
    for (int v = 0; v < 48; ++v)
        if (g_voice[v].active) playing |= 1ull << v;
    constexpr uint64_t kAll = (1ull << 48) - 1;
    const uint64_t masks[3] = {playing, kAll, kAll & ~playing};
    for (int i = 0; i < 3; ++i) {
        wr32(c.buff + 8 * i, (uint32_t)masks[i]);
        wr32(c.buff + 8 * i + 4, (uint32_t)(masks[i] >> 32));
    }
}

uint32_t his_rpc(Call& c) {
    uint32_t result = 0;
    switch (c.fno) {
    case 513:
        std::memset(ram(c.buff, std::max(c.rsize, 24u)), 0, std::max(c.rsize, 24u));
        voice_status(c);
        return c.buff;
    case 1: result = 0x00010802; break;   // version
    case 256: result = open_file(c); break;
    case 257: result = disc_read(c); break;
    case 265: result = file_size(rd32(c.buff)); break;
    case 258: result = kReadDone; break;
    case 260: case 263: case 270: break;
    case 768: result = voice_command(c); break;
    case 769: result = voice_position(c); break;
    case 770: case 771:
        if (c.fno == 770) {
            if ((rd32(c.buff) & 0xFFFF) < 48) g_pcm[rd32(c.buff) & 0xFFFF] = true;
            break;
        }
        if ((rd32(c.buff) & 0xFFFF) == 0x0201 && (rd32(c.buff) >> 16)) g_sample_rate = rd32(c.buff) >> 16;
        break;
    default: {
        if (g_seen.insert(c.fno).second)
            std::fprintf(stderr, "[iop] sound driver: function %u (send %u, recv %u) - no audio yet\n", c.fno, c.ssize, c.rsize);
        static int dumped[1024];
        if (log_enabled(Log::Iop) && c.fno < 1024 && dumped[c.fno]++ < 6)
            log(Log::Iop, "his fno %u payload %08x %08x %08x %08x", c.fno, rd32(c.buff), rd32(c.buff + 4), rd32(c.buff + 8), rd32(c.buff + 12));
        break;
    }
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
