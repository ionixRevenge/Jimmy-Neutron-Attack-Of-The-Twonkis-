// padman (old libpad: RPC servers 80000100 / 80000101). mcserv is in iop_mc.cpp.
//
// padman: scePadPortOpen hands the IOP an EE buffer holding two pad frames.
// The real driver refreshes it over SIF DMA every vblank; here iop_vblank()
// writes the current host controller state into it. Frame layouts (64-byte
// std, 128-byte std, 128-byte "EX") are detected from the 0xFF markers libpad
// leaves in the buffer, as documented by the Play! emulator.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "iop.hpp"
#include "iop_irx.hpp"
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
    if (port == 0 && !display_menu_open()) {   // keyboard on pad 1 (not while the F4 menu has it)
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

// Scripted input (--input "frame:button[:frames],..."): a press is held for
// `frames` frames, 15 by default.
struct ScriptPress { uint64_t frame; uint32_t bit; uint64_t hold; };   // bits 16-19: left stick up/down/left/right
std::vector<ScriptPress> g_script;
uint64_t g_frame = 0;

uint32_t script_buttons() {
    uint32_t b = 0;
    for (const auto& s : g_script)
        if (g_frame >= s.frame && g_frame < s.frame + s.hold) b |= s.bit;
    return b;
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

} // namespace

void register_pad_mc_services() {
    g_port[0] = g_port[1] = Port{};
    g_pad_logged.clear();
    register_server(0x80000100, "padman", 0x100, pad_rpc);
    register_server(0x80000101, "padman ext", 0x100, pad_rpc);
}

} // namespace ps2::iop

namespace ps2 {

uint16_t g_pad0_buttons;   // controller 1 as last written to the game (bit 3 = START)

// Live control for testing (TWONK_LIVE=<dir>): every few frames <dir>/cmd.txt is
// read and deleted. Lines: "input <frame>:<button>[:<hold>],..." with frames
// relative to now (same syntax as --input), and "shot <name>" to save the next
// frame as <dir>/<name>.png. <dir>/frame.txt always holds the current frame.
void live_commands() {
    static const char* dir = std::getenv("TWONK_LIVE");
    if (!dir || iop::g_frame % 6) return;
    const std::string base = dir;
    if (FILE* f = std::fopen((base + "/frame.txt").c_str(), "w")) {
        std::fprintf(f, "%llu\n", (unsigned long long)iop::g_frame);
        std::fclose(f);
    }
    const std::string cmd = base + "/cmd.txt";
    FILE* f = std::fopen(cmd.c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    std::fclose(f);
    std::remove(cmd.c_str());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("input ", 0) == 0) {
            const size_t first = iop::g_script.size();
            pad_script(line.c_str() + 6);
            for (size_t i = first; i < iop::g_script.size(); ++i) iop::g_script[i].frame += iop::g_frame + 1;
        } else if (line.rfind("shot ", 0) == 0) {
            display_request_shot((base + "/" + line.substr(5) + ".png").c_str());
        }
    }
}

void iop_vblank(Context* ctx) {
    ++iop::g_frame;
    static const bool time_debug = std::getenv("TWONK_TIME_DEBUG") != nullptr;
    if (time_debug && iop::g_frame % 120 == 0) {   // the game's frame time vs real time
        static double t0 = 0; static uint32_t ms0 = 0;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const uint32_t gp = ctx->r[28].u32[0];
        uint32_t ms = 0, dtb = 0;
        std::memcpy(&ms, guest_ptr(ctx, gp - 29108, 4), 4);
        std::memcpy(&dtb, guest_ptr(ctx, gp - 29100, 4), 4);
        float dt; std::memcpy(&dt, &dtb, 4);
        uint64_t acc = 0;
        std::memcpy(&acc, guest_ptr(ctx, gp - 29000, 8), 8);
        static uint64_t acc0 = 0;
        if (t0 > 0) std::fprintf(stderr, "[time] frame %llu: game %u ms, overflow clock %.0f ms, in %.0f ms real, last dt %.4f s\n",
                                 (unsigned long long)iop::g_frame, ms - ms0, (double)(acc - acc0) / 9216.0, (now - t0) * 1000, dt);
        std::fprintf(stderr, "[time]   timer0 events delivered %llu, dropped while masked %llu\n",
                     (unsigned long long)g_timer_delivered[0], (unsigned long long)g_timer_dropped[0]);
        g_timer_delivered[0] = g_timer_dropped[0] = 0;
        t0 = now; ms0 = ms; acc0 = acc;
    }
    live_commands();
    iop::irx::tick(ctx);   // the sound driver's threads (no-op without HIS.IRX)
    for (int p = 0; p < 2; ++p) {
        if (!iop::g_port[p].ee) continue;
        iop::HostPad hp = iop::read_host_pad(p);
        if (p == 0) {
            const uint32_t sb = iop::script_buttons();
            hp.buttons |= (uint16_t)sb;
            if (sb & 0x10000) hp.ly = 0x00;
            if (sb & 0x20000) hp.ly = 0xFF;
            if (sb & 0x40000) hp.lx = 0x00;
            if (sb & 0x80000) hp.lx = 0xFF;
            g_pad0_buttons = hp.buttons;
        }
        iop::write_frames(ctx, iop::g_port[p], hp, false);
    }
}

void pad_script(const char* spec) {
    static const char* const names[20] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                          "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square",
                                          "lup", "ldown", "lleft", "lright"};   // the last four: left stick
    std::string s = spec;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(pos, end - pos);
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
            const uint64_t frame = std::strtoull(item.c_str(), nullptr, 10);
            std::string name = item.substr(colon + 1);
            uint64_t hold = 15;
            if (const size_t c2 = name.find(':'); c2 != std::string::npos) {
                hold = std::strtoull(name.c_str() + c2 + 1, nullptr, 10);
                name.erase(c2);
            }
            for (int i = 0; i < 20; ++i)
                if (name == names[i]) iop::g_script.push_back({frame, 1u << i, hold});
        }
        pos = end + 1;
    }
}

} // namespace ps2
