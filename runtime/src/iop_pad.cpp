// padman (old libpad: RPC servers 80000100 / 80000101) and mcserv (80000400).
//
// padman: scePadPortOpen hands the IOP an EE buffer holding two pad frames.
// The real driver refreshes it over SIF DMA every vblank; here iop_vblank()
// writes the current host controller state into it. Frame layouts (64-byte
// std, 128-byte std, 128-byte "EX") are detected from the 0xFF markers libpad
// leaves in the buffer, as documented by the Play! emulator.
//
// mcserv: no memory card inserted yet (saves come with the virtual card).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <set>

#include "iop.hpp"
#include "ps2/recomp.hpp"
#include "ps2/runtime.hpp"

#pragma comment(lib, "xinput9_1_0.lib")

namespace ps2::iop {

namespace {

enum class PadType { Std64, Std128, Ex };

struct Port {
    uint32_t ee = 0;   // EE pad area, 0 when closed
    PadType type = PadType::Std64;
} g_port[2];

// Offsets inside one frame for each layout.
struct Layout { uint32_t size, frame, state, req, ok, data, length; };
Layout layout(PadType t) {
    switch (t) {
    case PadType::Std64: return {0x40, 0x00, 0x04, 0x05, 0x06, 0x08, 0x28};
    case PadType::Std128: return {0x80, 0x00, 0x04, 0x05, 0x06, 0x08, 0x28};
    default: return {0x80, 0x58, 0x70, 0x71, 0x72, 0x00, 0x60};
    }
}

constexpr uint8_t kModeDualShock = 7;

// Buttons, active low: data[2] = SELECT L3 R3 START UP RIGHT DOWN LEFT (bit 0..7),
// data[3] = L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE.
struct HostPad { uint16_t buttons = 0; uint8_t rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80; };

HostPad read_host_pad(int port) {
    HostPad p;
    XINPUT_STATE xs{};
    if (XInputGetState((DWORD)port, &xs) == ERROR_SUCCESS) {
        const WORD b = xs.Gamepad.wButtons;
        auto set = [&](bool on, uint16_t bit) { if (on) p.buttons |= bit; };
        set(b & XINPUT_GAMEPAD_BACK, 1u << 0);
        set(b & XINPUT_GAMEPAD_LEFT_THUMB, 1u << 1);
        set(b & XINPUT_GAMEPAD_RIGHT_THUMB, 1u << 2);
        set(b & XINPUT_GAMEPAD_START, 1u << 3);
        set(b & XINPUT_GAMEPAD_DPAD_UP, 1u << 4);
        set(b & XINPUT_GAMEPAD_DPAD_RIGHT, 1u << 5);
        set(b & XINPUT_GAMEPAD_DPAD_DOWN, 1u << 6);
        set(b & XINPUT_GAMEPAD_DPAD_LEFT, 1u << 7);
        set(xs.Gamepad.bLeftTrigger > 64, 1u << 8);
        set(xs.Gamepad.bRightTrigger > 64, 1u << 9);
        set(b & XINPUT_GAMEPAD_LEFT_SHOULDER, 1u << 10);
        set(b & XINPUT_GAMEPAD_RIGHT_SHOULDER, 1u << 11);
        set(b & XINPUT_GAMEPAD_Y, 1u << 12);
        set(b & XINPUT_GAMEPAD_B, 1u << 13);
        set(b & XINPUT_GAMEPAD_A, 1u << 14);
        set(b & XINPUT_GAMEPAD_X, 1u << 15);
        auto axis = [](SHORT v, bool invert) {
            int x = (v + 32768) >> 8;   // 0..255
            if (invert) x = 255 - x;
            return (uint8_t)x;
        };
        p.lx = axis(xs.Gamepad.sThumbLX, false);
        p.ly = axis(xs.Gamepad.sThumbLY, true);
        p.rx = axis(xs.Gamepad.sThumbRX, false);
        p.ry = axis(xs.Gamepad.sThumbRY, true);
    }
    if (port == 0) {   // keyboard on pad 1
        auto key = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        auto set = [&](bool on, uint16_t bit) { if (on) p.buttons |= bit; };
        set(key(VK_BACK), 1u << 0);
        set(key(VK_RETURN), 1u << 3);
        set(key(VK_UP), 1u << 4);
        set(key(VK_RIGHT), 1u << 5);
        set(key(VK_DOWN), 1u << 6);
        set(key(VK_LEFT), 1u << 7);
        set(key('1'), 1u << 8);
        set(key('3'), 1u << 9);
        set(key('Q'), 1u << 10);
        set(key('E'), 1u << 11);
        set(key('W'), 1u << 12);   // triangle
        set(key('D'), 1u << 13);   // circle
        set(key('S'), 1u << 14);   // cross
        set(key('A'), 1u << 15);   // square
    }
    return p;
}

void write_frames(Context* ctx, Port& pt, const HostPad& hp, bool init) {
    const Layout L = layout(pt.type);
    uint8_t* base = guest_ptr(ctx, pt.ee, L.size * 2);
    for (int f = 0; f < 2; ++f) {
        uint8_t* fr = base + L.size * f;
        if (init) {
            const uint32_t frame = (uint32_t)f, len = 32;
            std::memcpy(fr + L.frame, &frame, 4);
            std::memcpy(fr + L.length, &len, 4);
            fr[L.state] = 6;   // stable
            fr[L.ok] = 1;
            fr[L.data + 0] = 0;
            fr[L.data + 1] = kModeDualShock << 4;
            if (pt.type == PadType::Ex) {
                fr[0x65] = kModeDualShock << 4;   // current mode id
                fr[0x69] = 0;                     // current mode offset
                const uint16_t mode = kModeDualShock;
                std::memcpy(fr + 0x50, &mode, 2);
                fr[0x68] = 4;                     // number of modes
            }
        }
        const uint16_t s = (uint16_t)~hp.buttons;
        fr[L.req] = 0;
        fr[L.data + 2] = (uint8_t)s;
        fr[L.data + 3] = (uint8_t)(s >> 8);
        fr[L.data + 4] = hp.rx;
        fr[L.data + 5] = hp.ry;
        fr[L.data + 6] = hp.lx;
        fr[L.data + 7] = hp.ly;
    }
}

std::set<uint32_t> g_pad_logged;

// Every padman request: word 0 is the function, the reply is the same 128-byte block.
uint32_t pad_rpc(Call& c) {
    uint32_t* a = reinterpret_cast<uint32_t*>(ram(c.buff, 128));
    const uint32_t fn = a[0];
    const uint32_t port = a[1] & 1;
    switch (fn) {
    case 0x00000001:
    case 0x80000100: {   // PortOpen(port, slot, ?, ee_area)
        Port& pt = g_port[port];
        pt.ee = a[4] & 0x1FFFFFFF;
        const uint8_t* d = guest_ptr(c.ctx, pt.ee, 0x100);
        pt.type = PadType::Std64;
        if (d[0x08] == 0xFF && d[0x88] == 0xFF) pt.type = PadType::Std128;
        if (d[0x00] == 0xFF && d[0x80] == 0xFF) pt.type = PadType::Ex;
        write_frames(c.ctx, pt, read_host_pad((int)port), true);
        log(Log::Iop, "pad open port %u area %08x type %d", port, pt.ee, (int)pt.type);
        a[3] = 1;
        break;
    }
    case 0x8000010D: g_port[port].ee = 0; a[3] = 1; break;   // PortClose
    case 0x80000105: a[3] = 1; break;                        // SetMainMode (always DualShock)
    case 0x00000008: a[5] = 1; break;                        // SetActuatorAlign
    case 0x00000010: a[3] = 1; break;                        // Init
    case 0x00000012: a[3] = 0x400; break;                    // GetModuleVersion
    default:
        if (g_pad_logged.insert(fn).second)
            std::fprintf(stderr, "[iop] padman: unhandled function %08x (answered with success)\n", fn);
        a[3] = 1;
        break;
    }
    return c.buff;
}

uint32_t mc_rpc(Call& c) {
    uint32_t* r = reinterpret_cast<uint32_t*>(ram(c.buff, std::max(c.rsize, 12u)));
    const uint32_t fno = c.fno & 0xFF;
    if (fno == 0xFE || fno == 0x70) {   // Init: result, mcserv and mcman versions
        r[0] = 0;
        r[1] = 0x20A;
        r[2] = 0x20E;
    } else {
        std::memset(r, 0, c.rsize);
        r[0] = (uint32_t)-10;   // sceMcResNoEntry-class result: no card in the slot
        log(Log::Iop, "memory card function %02x: no card", fno);
    }
    return c.buff;
}

} // namespace

void register_pad_mc_services() {
    g_port[0] = g_port[1] = Port{};
    g_pad_logged.clear();
    register_server(0x80000100, "padman", 0x100, pad_rpc);
    register_server(0x80000101, "padman ext", 0x100, pad_rpc);
    register_server(0x80000400, "mcserv", 0x500, mc_rpc);
}

} // namespace ps2::iop

namespace ps2 {

void iop_vblank(Context* ctx) {
    for (int p = 0; p < 2; ++p)
        if (iop::g_port[p].ee) iop::write_frames(ctx, iop::g_port[p], iop::read_host_pad(p), false);
}

} // namespace ps2
