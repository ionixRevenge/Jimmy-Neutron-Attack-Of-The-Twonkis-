// IOP high-level emulation at the SIF protocol boundary.
//
// The EE side (Sony's sifcmd/sifrpc and every library built on them) runs as
// recompiled code, unmodified. It talks to the IOP only through the SIF
// syscalls: SifSetReg/SifGetReg for the mailbox registers and SifSetDma to
// copy data and command packets into IOP RAM. This file plays the IOP's part:
//
//   EE -> IOP  SifSetDma copies into a 2 MB IOP RAM image. A transfer whose
//              destination is the IOP command buffer is a sifcmd packet and is
//              handled immediately (init, reset, RPC bind/call/rdata).
//   IOP -> EE  Reply packets are queued. The kernel writes each one into the
//              EE receive buffer and runs the SIF0 DMAC handler (channel 5),
//              exactly what the EE sees when the real IOP DMAs a packet.
//
// RPC servers are native C++ (iop_services.cpp), registered by SID.
//
// Packet layouts (Sony sifcmd/sifrpc, verified against SLUS_208.87):
//   header   +0 psize:8|dsize:24  +4 dest  +8 cid  +12 opt
//   rpc      +16 rec_id  +20 pkt_addr (EE)  +24 rpc_id  +28 client (EE)
//   bind     +32 sid
//   call     +32 fno  +36 send_size  +40 receive (EE)  +44 recv_size  +48 rmode  +52 server
//   rdata    +32 src (IOP)  +36 dest (EE)  +40 size
//   end      +32 cid of the request  +36 server  +40 buff  +44 cbuff
#include "iop.hpp"

#include <fstream>

#include "disc.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

namespace ps2 {

namespace iop {

namespace {

constexpr uint32_t kCmdBuf = 0x00002000;   // where the EE sends sifcmd packets (reported via SMCOM)

constexpr uint32_t kCmdChangeSaddr = 0x80000000, kCmdSetSreg = 0x80000001, kCmdInit = 0x80000002,
                   kCmdReset = 0x80000003, kCmdRpcEnd = 0x80000008, kCmdRpcBind = 0x80000009,
                   kCmdRpcCall = 0x8000000A, kCmdRpcRdata = 0x8000000C;

// SIF mailbox registers
constexpr uint32_t kRegMscom = 1, kRegSmcom = 2, kRegMsflag = 3, kRegSmflag = 4;
constexpr uint32_t kStatSifInit = 0x10000, kStatCmdInit = 0x20000, kStatBootEnd = 0x40000;

struct Server {
    uint32_t sid = 0;
    std::string name;
    uint32_t addr = 0, buff = 0, buff_size = 0;
    RpcFn fn = nullptr;
    std::set<uint32_t> logged_fno;
};

struct State {
    std::vector<uint8_t> ram = std::vector<uint8_t>(kRamSize);
    uint32_t brk = 0x00010000;
    std::string root;
    std::map<uint32_t, uint32_t> regs;
    uint32_t ee_buf = 0;                        // EE address of the sifcmd receive buffer
    std::deque<std::vector<uint8_t>> to_ee;     // queued IOP -> EE packets
    std::map<uint32_t, Server> by_sid;
    std::map<uint32_t, uint32_t> sid_by_addr;   // server struct address -> sid
    std::map<uint32_t, RpcFn> registered;       // native services, bound on first request
    std::map<uint32_t, std::pair<std::string, uint32_t>> reg_info;
    int32_t dma_id = 0;
    bool rebooting = false;   // reset requested: flags come back on the EE's next SMFLAG read
} S;

void ret(Context* c, int32_t v) { c->r[2].s64[0] = v; }

// Unknown servers: log the request and return zeros, so discovery can continue.
uint32_t stub_server(Call& c) {
    std::memset(ram(c.buff, c.rsize ? c.rsize : 1), 0, c.rsize);
    return c.buff;
}

Server& bind_server(uint32_t sid) {
    auto it = S.by_sid.find(sid);
    if (it != S.by_sid.end()) return it->second;
    Server s;
    s.sid = sid;
    auto r = S.registered.find(sid);
    if (r != S.registered.end()) {
        s.fn = r->second;
        s.name = S.reg_info[sid].first;
        s.buff_size = S.reg_info[sid].second;
    } else {
        s.fn = stub_server;
        s.name = "unimplemented";
        s.buff_size = 0x10000;
        std::fprintf(stderr, "[iop] WARNING: no native service for RPC server %08x; calls will return zeros\n", sid);
    }
    s.addr = alloc(0x80);
    s.buff = alloc(s.buff_size);
    S.sid_by_addr[s.addr] = sid;
    return S.by_sid[sid] = s;
}

// Queue a 64-byte RPC_END reply for an RPC request packet at `req`.
void send_end(uint32_t req, uint32_t req_cid, const Server* s) {
    std::vector<uint8_t> p(64, 0);
    auto put = [&](uint32_t off, uint32_t v) { std::memcpy(&p[off], &v, 4); };
    put(0, 64);
    put(8, kCmdRpcEnd);
    put(16, rd32(req + 16));   // rec_id
    put(20, rd32(req + 20));   // pkt_addr
    put(24, rd32(req + 24));   // rpc_id
    put(28, rd32(req + 28));   // client
    put(32, req_cid);
    put(36, s ? s->addr : 0);
    put(40, s ? s->buff : 0);
    S.to_ee.push_back(std::move(p));
}

void copy_to_ee(Context* ctx, uint32_t ee, uint32_t iop_src, uint32_t size) {
    if (!size) return;
    std::memcpy(guest_ptr(ctx, ee, size), ram(iop_src, size), size);
}

void hexdump(const char* what, uint32_t addr, uint32_t size) {
    size = std::min<uint32_t>(size, 64);
    std::fprintf(stderr, "[iop]   %s @%06x:", what, addr);
    for (uint32_t i = 0; i < size; ++i) std::fprintf(stderr, "%s%02x", (i % 16) ? " " : "\n[iop]     ", ram(addr + i, 1)[0]);
    std::fputc('\n', stderr);
}

void rpc_call(Context* ctx, uint32_t pkt) {
    const uint32_t fno = rd32(pkt + 32), ssize = rd32(pkt + 36), recv = rd32(pkt + 40), rsize = rd32(pkt + 44),
                   rmode = rd32(pkt + 48), saddr = rd32(pkt + 52);
    auto sit = S.sid_by_addr.find(saddr);
    if (sit == S.sid_by_addr.end()) {
        char msg[96];
        std::snprintf(msg, sizeof msg, "sceSifCallRpc to unknown server struct %08x", saddr);
        fatal(ctx, 0, msg);
    }
    Server& s = S.by_sid[sit->second];
    if (ssize > s.buff_size) fatal(ctx, 0, "RPC request larger than the server buffer");
    const bool first = s.logged_fno.insert(fno).second;
    if (log_enabled(Log::Iop) || (first && s.fn == stub_server)) {
        std::fprintf(stderr, "[iop] call %08x(%s) fno=%u send=%u recv=%08x/%u mode=%u\n", s.sid, s.name.c_str(), fno, ssize,
                     recv, rsize, rmode);
        if (s.fn == stub_server) hexdump("request", s.buff, ssize);
    }
    Call c{ctx, fno, s.buff, ssize, recv, rsize};
    const uint32_t reply = s.fn(c);
    if (recv && rsize) copy_to_ee(ctx, recv, reply, rsize);
    if (rmode) {
        send_end(pkt, kCmdRpcCall, &s);
    } else {
        // No-reply mode: the IOP overwrites the EE's request packet with rpc_id=0,
        // which is what sceSifCheckStatRpc polls for.
        const uint32_t ee_pkt = rd32(pkt + 20);
        uint8_t* p = guest_ptr(ctx, ee_pkt, 64);
        std::memset(p + 16, 0, 12);   // rec_id, pkt_addr, rpc_id
    }
}

void handle_cmd(Context* ctx, uint32_t pkt) {
    const uint32_t cid = rd32(pkt + 8), opt = rd32(pkt + 12);
    switch (cid) {
    case kCmdChangeSaddr:
        S.ee_buf = rd32(pkt + 16);
        log(Log::Iop, "sifcmd: EE receive buffer %08x", S.ee_buf);
        return;
    case kCmdSetSreg:
        return;
    case kCmdInit:
        if (opt == 0) {
            S.ee_buf = rd32(pkt + 16);
            log(Log::Iop, "sifcmd init: EE receive buffer %08x", S.ee_buf);
        } else {   // sifrpc init: the IOP answers by setting software register 0
            std::vector<uint8_t> p(24, 0);
            const uint32_t v[6] = {24, 0, kCmdSetSreg, 0, 0, 1};
            std::memcpy(p.data(), v, sizeof v);
            S.to_ee.push_back(std::move(p));
            log(Log::Iop, "sifrpc init");
        }
        return;
    case kCmdReset: {
        const char* arg = reinterpret_cast<const char*>(ram(pkt + 24, 80));
        std::fprintf(stderr, "[iop] IOP reset \"%.80s\" (modules are native; nothing to load)\n", arg);
        S.rebooting = true;
        S.ee_buf = 0;   // the EE re-sends it after the reboot
        S.to_ee.clear();
        return;
    }
    case kCmdRpcBind: {
        const uint32_t sid = rd32(pkt + 32);
        Server& s = bind_server(sid);
        log(Log::Iop, "bind %08x (%s) -> server %06x buff %06x", sid, s.name.c_str(), s.addr, s.buff);
        send_end(pkt, kCmdRpcBind, &s);
        return;
    }
    case kCmdRpcCall:
        rpc_call(ctx, pkt);
        return;
    case kCmdRpcRdata: {
        copy_to_ee(ctx, rd32(pkt + 36), rd32(pkt + 32), rd32(pkt + 40));
        send_end(pkt, kCmdRpcRdata, nullptr);
        return;
    }
    default: {
        char msg[64];
        std::snprintf(msg, sizeof msg, "unhandled sifcmd packet cid %08x", cid);
        fatal(ctx, 0, msg);
    }
    }
}

int32_t set_dma(Context* ctx, uint32_t list, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t* d = reinterpret_cast<const uint32_t*>(guest_ptr(ctx, list + i * 16, 16));
        const uint32_t src = d[0], dest = d[1] & (kRamSize - 1), size = d[2];
        if (size) std::memcpy(ram(dest, size), guest_ptr(ctx, src, size), size);
        if (dest == kCmdBuf) handle_cmd(ctx, kCmdBuf);
    }
    if (++S.dma_id <= 0) S.dma_id = 1;
    return S.dma_id;
}

} // namespace

uint8_t* ram(uint32_t addr, uint32_t size) {
    addr &= 0x1FFFFFFF;
    if ((uint64_t)addr + size > kRamSize) fatal(nullptr, addr, "IOP RAM access out of range");
    return S.ram.data() + addr;
}

uint32_t alloc(uint32_t size, uint32_t align) {
    S.brk = (S.brk + align - 1) & ~(align - 1);
    const uint32_t a = S.brk;
    S.brk += size;
    if (S.brk > kRamSize) fatal(nullptr, a, "IOP RAM exhausted");
    return a;
}

const std::string& disc_root() { return S.root; }

void send_cmd_to_ee(uint32_t cid, const void* payload, uint32_t size) {
    std::vector<uint8_t> p(16 + size, 0);
    const uint32_t hdr[4] = {16 + size, 0, cid, 0};
    std::memcpy(p.data(), hdr, 16);
    if (size) std::memcpy(p.data() + 16, payload, size);
    S.to_ee.push_back(std::move(p));
}

bool read_game_file(const std::string& p, std::vector<uint8_t>* out) {
    const size_t colon = p.find(':');
    if (disc::is_open() && colon != std::string::npos) {
        uint32_t lba, size;
        if (!disc::find(p.substr(colon + 1), &lba, &size)) return false;
        out->resize(((size_t)size + 2047) / 2048 * 2048);
        if (!disc::read(lba, (uint32_t)(out->size() / 2048), out->data())) return false;
        out->resize(size);
        return true;
    }
    const std::string host = host_path(p);
    if (host.empty()) return false;
    std::ifstream f(host, std::ios::binary | std::ios::ate);
    if (!f) return false;
    out->resize((size_t)f.tellg());
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out->data()), (std::streamsize)out->size());
    return true;
}

std::string host_path(const std::string& p) {
    const size_t colon = p.find(':');
    if (colon == std::string::npos) return {};
    std::string dev = p.substr(0, colon);
    for (char& c : dev) c = (char)std::tolower((unsigned char)c);
    if (dev != "cdrom0" && dev != "cdrom") return {};
    std::string rest = p.substr(colon + 1);
    if (const size_t semi = rest.rfind(';'); semi != std::string::npos) rest.resize(semi);   // ISO9660 version
    for (char& c : rest) if (c == '\\') c = '/';
    while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
    return S.root + "/" + rest;
}

void register_server(uint32_t sid, const char* name, uint32_t buff_size, RpcFn fn) {
    S.registered[sid] = fn;
    S.reg_info[sid] = {name, buff_size};
}

} // namespace iop

void iop_init(const std::string& disc_root) {
    using namespace iop;
    S = State{};
    S.root = disc_root;
    // The IOP is already up when the game starts (the BIOS booted it).
    S.regs[kRegSmcom] = kCmdBuf;
    S.regs[kRegSmflag] = kStatSifInit | kStatCmdInit | kStatBootEnd;
    register_services();
}

void sif_syscall(Context* ctx, int call) {
    using namespace iop;
    const uint32_t a0 = ctx->r[4].u32[0], a1 = ctx->r[5].u32[0];
    switch (call) {
    case 0x76: ret(ctx, -1); return;                      // SifDmaStat: every transfer has completed
    case 0x77: ret(ctx, set_dma(ctx, a0, a1)); return;    // SifSetDma(list, count)
    case 0x78: ret(ctx, 0); return;                       // SifSetDChain: re-arms SIF0; replies are pushed directly
    case 0x79: {                                          // SifSetReg(reg, value) -> old value
        uint32_t& r = S.regs[a0];
        const uint32_t old = r;
        if (a0 == kRegSmflag) r &= ~a1;        // EE writes clear IOP->EE flags
        else if (a0 == kRegMsflag) r |= a1;    // EE->IOP flags are set
        else r = a1;
        ret(ctx, (int32_t)old);
        return;
    }
    case 0x7A:   // SifGetReg(reg)
        if (a0 == kRegSmflag && S.rebooting) {   // the rebooted IOP is up again
            S.regs[kRegSmflag] |= kStatSifInit | kStatCmdInit | kStatBootEnd;
            S.rebooting = false;
        }
        ret(ctx, (int32_t)S.regs[a0]);
        return;
    default: fatal(ctx, 0, "sif_syscall: not a SIF syscall");
    }
    (void)kRegMscom;
}

bool iop_pop_packet(Context* ctx) {
    using namespace iop;
    if (S.to_ee.empty() || !S.ee_buf) return false;
    const auto& p = S.to_ee.front();
    std::memcpy(guest_ptr(ctx, S.ee_buf, (uint32_t)p.size()), p.data(), p.size());
    S.to_ee.pop_front();
    return true;
}

} // namespace ps2
