// Output window and screenshots.
//
// The window lives on its own thread (it owns the message loop); the game
// thread hands over a finished frame once per vblank. Screenshots are written
// as PNG (stored/uncompressed deflate) so they need no image library.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ps2/runtime.hpp"

namespace ps2 {

namespace {

std::mutex g_mutex;
std::vector<uint32_t> g_frame;   // 0x00RRGGBB
uint32_t g_w = 0, g_h = 0;
HWND g_hwnd = nullptr;
std::atomic<bool> g_started{false};

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_w && g_h) {
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof bi.bmiHeader;
                bi.bmiHeader.biWidth = (LONG)g_w;
                bi.bmiHeader.biHeight = -(LONG)g_h;   // top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(dc, HALFTONE);
                StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, (int)g_w, (int)g_h, g_frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            } else {
                FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: ExitProcess(0);
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void window_thread() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"TwonkiesWindow";
    RegisterClassW(&wc);
    RECT r{0, 0, 960, 720};   // 4:3
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"Jimmy Neutron: Attack of the Twonkies", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// ---- PNG ------------------------------------------------------------------------
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}

void be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    be32(out, crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
}

bool write_png(const std::string& path, const uint32_t* px, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t c = px[(size_t)y * w + x];
            raw.push_back((uint8_t)(c >> 16)); raw.push_back((uint8_t)(c >> 8)); raw.push_back((uint8_t)c);
        }
    }
    std::vector<uint8_t> z{0x78, 0x01};
    for (size_t off = 0; off < raw.size() || off == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);
    std::vector<uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    be32(ihdr, w); be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return true;
}

std::string g_shot_path;
std::vector<uint64_t> g_shot_frames;
bool g_window = true;

} // namespace

void display_config(bool window, const char* shot_path, const char* shot_frames) {
    g_window = window;
    if (shot_path) g_shot_path = shot_path;
    for (const char* p = shot_frames; p && *p;) {
        char* end;
        g_shot_frames.push_back(std::strtoull(p, &end, 10));
        p = *end == ',' ? end + 1 : "";
    }
}

void display_present(const uint32_t* px, uint32_t w, uint32_t h, uint64_t frame) {
    if (!g_shot_path.empty() && std::find(g_shot_frames.begin(), g_shot_frames.end(), frame) != g_shot_frames.end()) {
        std::string path = g_shot_path;
        if (const size_t hash = path.find('#'); hash != std::string::npos) path.replace(hash, 1, std::to_string(frame));
        if (write_png(path, px, w, h)) std::fprintf(stderr, "[gpu] wrote %ux%u screenshot (frame %llu) to %s\n", w, h, (unsigned long long)frame, path.c_str());
    }
    if (!g_window) return;
    if (!g_started.exchange(true)) std::thread(window_thread).detach();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_frame.assign(px, px + (size_t)w * h);
        g_w = w; g_h = h;
    }
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

} // namespace ps2
