// Native IOP RPC services, registered by server ID.
//
//   80000003  IOP heap (sceSifAllocIopHeap / FreeIopHeap / LoadIopHeap)
//   80000006  module loader (sceSifLoadModule): modules are native, loads only succeed
#include "iop.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <vector>

#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2::iop {

namespace {

// ---- IOP heap -----------------------------------------------------------------------
// First-fit allocator over a fixed arena of IOP RAM.
struct Heap {
    uint32_t base = 0, size = 0;
    std::map<uint32_t, uint32_t> used;   // addr -> size
    uint32_t allocate(uint32_t n) {
        n = (n + 63) & ~63u;
        uint32_t at = base;
        for (const auto& u : used) {
            if (u.first - at >= n) break;
            at = u.first + u.second;
        }
        if (at + n > base + size) return 0;
        used[at] = n;
        return at;
    }
    uint32_t largest_free() const {
        uint32_t at = base, best = 0;
        for (const auto& u : used) { best = std::max(best, u.first - at); at = u.first + u.second; }
        return std::max(best, base + size - at);
    }
    uint32_t total_free() const {
        uint32_t t = size;
        for (const auto& u : used) t -= u.second;
        return t;
    }
} g_heap;

uint32_t heap_rpc(Call& c) {
    const uint32_t b = c.buff;
    switch (c.fno) {
    case 1: {   // alloc(size) -> addr
        const uint32_t size = rd32(b), a = g_heap.allocate(size);
        log(Log::Iop, "heap alloc %u -> %06x", size, a);
        if (!a) std::fprintf(stderr, "[iop] WARNING: IOP heap exhausted (request %u bytes)\n", size);
        wr32(b, a);
        return b;
    }
    case 2: {   // free(addr) -> 0
        const uint32_t a = rd32(b);
        const int32_t ok = g_heap.used.erase(a) ? 0 : -1;
        log(Log::Iop, "heap free %06x", a);
        wr32(b, (uint32_t)ok);
        return b;
    }
    case 3: {   // load(addr, path[252]) -> 0 / error
        const uint32_t a = rd32(b);
        const std::string path(reinterpret_cast<const char*>(ram(b + 4, 252)));
        int32_t r = -1;
        std::vector<uint8_t> data;
        if (read_game_file(path, &data)) {
            std::memcpy(ram(a, (uint32_t)data.size()), data.data(), data.size());
            r = 0;
        }
        log(Log::Iop, "heap load \"%s\" -> %06x: %d", path.c_str(), a, r);
        if (r) std::fprintf(stderr, "[iop] WARNING: LoadIopHeap could not open %s\n", path.c_str());
        wr32(b, (uint32_t)r);
        return b;
    }
    case 4: wr32(b, g_heap.largest_free()); return b;   // query max free block
    case 5: wr32(b, g_heap.total_free()); return b;     // query total free
    default: {
        char msg[64];
        std::snprintf(msg, sizeof msg, "IOP heap: unknown RPC function %u", c.fno);
        fatal(c.ctx, 0, msg);
    }
    }
}

// ---- module loader ------------------------------------------------------------------
// Request: { int arg_len; int modres; char path[252]; char args[252]; }
// Reply:   { int result (module id or error); int modres; }
int32_t g_next_module = 1;

uint32_t loadfile_rpc(Call& c) {
    const uint32_t b = c.buff;
    switch (c.fno) {
    case 0:     // load module from a file
    case 6: {   // load module from an IOP buffer
        const std::string path = c.fno == 0 ? std::string(reinterpret_cast<const char*>(ram(b + 8, 252))) : "<buffer>";
        const int32_t id = g_next_module++;
        std::fprintf(stderr, "[iop] load module %s -> id %d (native)\n", path.c_str(), id);
        wr32(b, (uint32_t)id);
        wr32(b + 4, 0);   // module's start() result: resident
        return b;
    }
    case 255:   // version as 4 ASCII digits; the game requires "3000" (IOPRP300)
        std::memcpy(ram(b, 4), "3000", 4);
        return b;
    default: {
        char msg[64];
        std::snprintf(msg, sizeof msg, "loadfile: unknown RPC function %u", c.fno);
        fatal(c.ctx, 0, msg);
    }
    }
}

} // namespace

void register_services() {
    g_heap = Heap{};
    g_heap.size = 0x100000;              // 1 MB, like a retail IOP with drivers resident
    g_heap.base = alloc(g_heap.size);
    g_next_module = 1;
    register_server(0x80000003, "iopheap", 0x200, heap_rpc);
    register_server(0x80000006, "loadfile", 0x400, loadfile_rpc);
    register_cdvd_services();
    register_his_service();
    register_pad_mc_services();
    register_mc_service();
    register_fileio_service();
}

} // namespace ps2::iop
