// Internal interface between the SIF/RPC core (iop.cpp) and the native IOP
// services (iop_*.cpp). Nothing here is visible to generated code.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ps2/context.hpp"

namespace ps2::iop {

constexpr uint32_t kRamSize = 2 * 1024 * 1024;

uint8_t* ram(uint32_t addr, uint32_t size);   // IOP RAM; fatal when out of range
inline uint32_t rd32(uint32_t a) { uint32_t v; std::memcpy(&v, ram(a, 4), 4); return v; }
inline void wr32(uint32_t a, uint32_t v) { std::memcpy(ram(a, 4), &v, 4); }
uint32_t alloc(uint32_t size, uint32_t align = 64);   // permanent IOP RAM (server structs, buffers, heap)
const std::string& disc_root();

// One sceSifCallRpc as the server sees it. The request is in IOP RAM at
// `buff` (ssize bytes). The handler returns the IOP address of its reply;
// rsize bytes from there are copied to the EE receive buffer.
struct Call {
    Context* ctx;
    uint32_t fno, buff, ssize, ee_recv, rsize;
};
using RpcFn = uint32_t (*)(Call&);

void register_server(uint32_t sid, const char* name, uint32_t buff_size, RpcFn fn);
void register_services();        // iop_services.cpp
void register_cdvd_services();   // iop_cdvd.cpp
void register_his_service();     // iop_his.cpp
void register_pad_mc_services(); // iop_pad.cpp
void register_fileio_service();  // iop_fileio.cpp

// Queue a sifcmd packet for the EE: 16-byte header (cid filled in) + payload.
void send_cmd_to_ee(uint32_t cid, const void* payload, uint32_t size);

} // namespace ps2::iop

namespace ps2::iop {

// "cdrom0:\DATA\FILE.PAK;1" -> "<disc root>/DATA/FILE.PAK". Empty if the device is not the disc.
std::string host_path(const std::string& ps2_path);
// Whole file from the disc (cdrom0:...): the ISO when loaded, else the extracted tree.
bool read_game_file(const std::string& ps2_path, std::vector<uint8_t>* out);

} // namespace ps2::iop
