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
wchar_t g_title[128] = L"";
bool g_show_fps = true;            // F1 toggles; --no-fps starts with it off
double g_fps = 0, g_speed = 100;   // game frames per second, % of real-time vblanks

bool g_hw = false;   // the GPU renderer owns the window's contents

// F4 menu: video settings (video.cpp keeps and saves them)
std::atomic<bool> g_menu{false};
int g_menu_row = 0;   // window thread only
enum Row { RowScale, RowSsaa, RowFilter, RowText, RowAspect, RowFullscreen, kNumRows };

std::wstring row_value(int row, const VideoSettings& v, int cur) {
    wchar_t t[96];
    switch (row) {
    case RowScale:
        if (!cur) return L"n/a (software renderer)";
        if (!v.scale) swprintf(t, 96, L"Auto (now %dx, %dx%d)", cur, 640 * cur, 448 * cur);
        else swprintf(t, 96, L"%dx  (%dx%d)", v.scale, 640 * v.scale, 448 * v.scale);
        return t;
    case RowSsaa:
        if (!cur) return L"n/a";
        return v.ssaa == 0 ? L"Off" : v.ssaa == 1 ? L"2x" : L"4x";
    case RowFilter:
        if (!cur) return L"n/a";
        return v.filter ? L"Sharp (Lanczos + CAS)" : L"Bilinear";
    case RowText:
        if (!cur) return L"n/a";
        return v.sharp_text ? L"On" : L"Off";
    case RowAspect: {
        const char* n = video_aspect_name(v.aspect);
        std::wstring w(n, n + std::strlen(n));
        if (v.aspect == kNumAspects - 1) w = L"Stretch to window";
        return w;
    }
    case RowFullscreen: return v.fullscreen ? L"On" : L"Off";
    }
    return L"";
}

void draw_menu(HDC dc, int w, int h) {
    const int cur = renderer_scale();
    const VideoSettings v = video_settings();
    const int px = std::max(14, h / 30);
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Consolas");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    static const wchar_t* const kNames[kNumRows] = {L"Resolution", L"Supersampling", L"Upscale filter", L"Sharp text", L"Aspect ratio", L"Fullscreen"};
    std::vector<std::wstring> lines;
    std::vector<COLORREF> colors;
    lines.push_back(L"Video settings");
    colors.push_back(RGB(255, 255, 255));
    lines.push_back(L"");
    colors.push_back(0);
    for (int r = 0; r < kNumRows; ++r) {
        wchar_t t[160];
        swprintf(t, 160, L"%s %-15s %s", r == g_menu_row ? L">" : L" ", kNames[r], row_value(r, v, cur).c_str());
        lines.push_back(t);
        colors.push_back(r == g_menu_row ? RGB(255, 230, 90) : RGB(200, 200, 200));
    }
    lines.push_back(L"");
    colors.push_back(0);
    const wchar_t* hint = L"";
    switch (g_menu_row) {
    case RowScale: hint = L"Auto renders at the size of the picture on screen."; break;
    case RowSsaa: hint = v.scale ? L"Only with Auto resolution: renders larger, then shrinks." : L"Renders larger than the screen, then shrinks: smoother edges."; break;
    case RowFilter: hint = L"Used when the picture is shown larger or smaller than rendered."; break;
    case RowText: hint = L"Dialog, menu and HUD lettering redrawn crisp at 2x and up."; break;
    case RowAspect: hint = v.aspect == 0 ? L"The original picture." : L"The 3D view gets wider; menus and HUD are stretched. Movies stay 4:3."; break;
    case RowFullscreen: hint = L"Also Alt+Enter or F11, any time."; break;
    }
    lines.push_back(hint);
    colors.push_back(RGB(150, 190, 255));
    lines.push_back(L"Up/Down: choose  Left/Right: change  F4: close");
    colors.push_back(RGB(150, 150, 150));
    int tw = 0;
    SIZE sz{};
    for (auto& l : lines) {
        GetTextExtentPoint32W(dc, l.c_str(), (int)l.size(), &sz);
        tw = std::max(tw, (int)sz.cx);
    }
    const int lh = px * 5 / 4, pad = px;
    const int bw = tw + pad * 2, bh = lh * (int)lines.size() + pad * 2;
    const int x0 = (w - bw) / 2, y0 = (h - bh) / 2;
    RECT box{x0, y0, x0 + bw, y0 + bh};
    HBRUSH bg = CreateSolidBrush(RGB(20, 24, 40));
    FillRect(dc, &box, bg);
    DeleteObject(bg);
    HBRUSH edge = CreateSolidBrush(RGB(110, 140, 255));
    FrameRect(dc, &box, edge);
    DeleteObject(edge);
    for (size_t i = 0; i < lines.size(); ++i) {
        SetTextColor(dc, colors[i]);
        TextOutW(dc, x0 + pad, y0 + pad + lh * (int)i, lines[i].c_str(), (int)lines[i].size());
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
}

// ---- borderless fullscreen (window thread) ---------------------------------------
bool g_fullscreen = false;
WINDOWPLACEMENT g_placement{sizeof(WINDOWPLACEMENT)};

void apply_fullscreen(HWND hwnd, bool on) {
    if (on == g_fullscreen) return;
    g_fullscreen = on;
    if (on) {
        GetWindowPlacement(hwnd, &g_placement);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &g_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

void toggle_fullscreen(HWND hwnd) {
    VideoSettings v = video_settings();
    v.fullscreen = !v.fullscreen;
    video_settings_set(v);
    apply_fullscreen(hwnd, v.fullscreen);
}

// The FPS counter (top left) and the F4 menu.
void draw_overlay(HDC dc, int w, int h) {
    if (g_menu) draw_menu(dc, w, h);
    wchar_t fps_text[64] = L"";
    double speed;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_show_fps) swprintf(fps_text, 64, L"%.0f FPS  %.0f%%", g_fps, g_speed);
        speed = g_speed;
    }
    if (!fps_text[0]) return;
    const int px = std::max(14, h / 30);
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Consolas");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    const int n = (int)wcslen(fps_text), x = px / 2, y = px / 3;
    SetTextColor(dc, RGB(0, 0, 0));
    TextOutW(dc, x + 2, y + 2, fps_text, n);   // shadow
    SetTextColor(dc, speed >= 97 ? RGB(120, 255, 120) : speed >= 75 ? RGB(255, 230, 90) : RGB(255, 110, 90));
    TextOutW(dc, x, y, fps_text, n);
    SelectObject(dc, old_font);
    DeleteObject(font);
}

// Keys while the menu is open (true: handled).
bool menu_key(HWND hwnd, WPARAM wp) {
    if (wp == VK_F4) {
        g_menu = !g_menu;
    } else if (!g_menu) {
        return false;
    } else if (wp == VK_ESCAPE) {
        g_menu = false;
    } else if (wp == VK_UP) {
        g_menu_row = (g_menu_row + kNumRows - 1) % kNumRows;
    } else if (wp == VK_DOWN) {
        g_menu_row = (g_menu_row + 1) % kNumRows;
    } else if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_RETURN || wp == VK_SPACE) {
        const int d = wp == VK_LEFT ? -1 : 1;
        VideoSettings v = video_settings();
        const bool hw = renderer_scale() != 0;
        auto step = [d](int x, int n) { return (x + d + n) % n; };
        switch (g_menu_row) {
        case RowScale: if (hw) v.scale = step(v.scale, 9); break;   // auto, 1x-8x
        case RowSsaa: if (hw) v.ssaa = step(v.ssaa, kNumSsaa); break;
        case RowFilter: if (hw) v.filter = step(v.filter, 2); break;
        case RowText: if (hw) v.sharp_text = !v.sharp_text; break;
        case RowAspect: v.aspect = step(v.aspect, kNumAspects); break;
        case RowFullscreen: v.fullscreen = !v.fullscreen; break;
        }
        video_settings_set(v);
        apply_fullscreen(hwnd, v.fullscreen);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        if (g_hw) {   // presented by the GPU renderer
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        PAINTSTRUCT ps;
        HDC win = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        // drawn off screen, then copied: the FPS text never flickers over the picture
        HDC dc = CreateCompatibleDC(win);
        HBITMAP bmp = CreateCompatibleBitmap(win, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
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
                FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
                int dx = 0, dy = 0, dw = (int)rc.right, dh = (int)rc.bottom;
                if (const float a = video_display_aspect(movie_playing()); a > 0) {
                    if (dw > dh * a) { dw = (int)(dh * a + 0.5f); dx = ((int)rc.right - dw) / 2; }
                    else { dh = (int)(dw / a + 0.5f); dy = ((int)rc.bottom - dh) / 2; }
                }
                SetStretchBltMode(dc, HALFTONE);
                StretchDIBits(dc, dx, dy, dw, dh, 0, 0, (int)g_w, (int)g_h, g_frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            } else {
                FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
        }
        draw_overlay(dc, (int)rc.right, (int)rc.bottom);
        BitBlt(win, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old_bmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (menu_key(hwnd, wp)) return 0;
        if (wp == VK_F11) { toggle_fullscreen(hwnd); return 0; }
        if (wp == VK_F1) {   // F1: FPS counter on/off
            { std::lock_guard<std::mutex> lock(g_mutex); g_show_fps = !g_show_fps; }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { toggle_fullscreen(hwnd); return 0; }   // Alt+Enter
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;   // no beep for Alt+Enter
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_SETCURSOR:
        if (g_fullscreen && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_ERASEBKGND: return 1;
    case WM_APP: {
        wchar_t t[128];
        { std::lock_guard<std::mutex> lock(g_mutex); wcscpy_s(t, g_title); }
        SetWindowTextW(hwnd, t);
        return 0;
    }
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
    float a = video_display_aspect(false);
    if (a <= 0) a = 4.0f / 3.0f;
    RECT r{0, 0, (LONG)(720 * a + 0.5f), 720};   // the picture's shape
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"Jimmy Neutron: Attack of the Twonkies", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (video_settings().fullscreen) apply_fullscreen(g_hwnd, true);
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
// Live screenshot (display_request_shot): taken at the next presented frame.
std::mutex g_live_mutex;
std::string g_live_path;
std::atomic<uint64_t> g_live_frame{~0ull};

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

void display_stats(double game_fps, double vblank_hz) {
    if (log_enabled(Log::Gpu)) std::fprintf(stderr, "[gpu] %.1f fps (%.1f vblanks/s)\n", game_fps, vblank_hz);
    if (!g_hwnd) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        swprintf(g_title, 128, L"Jimmy Neutron: Attack of the Twonkies  |  %.1f fps  (%.0f%% speed)", game_fps, vblank_hz / 0.5994);
        g_fps = game_fps;
        g_speed = std::min(100.0, vblank_hz / 0.5994);
    }
    PostMessageW(g_hwnd, WM_APP, 0, 0);   // the window thread sets the title; SetWindowText here would block on it
}

void* display_window() {
    if (!g_window) return nullptr;
    if (!g_started.exchange(true)) std::thread(window_thread).detach();
    for (int i = 0; i < 500 && !g_hwnd; ++i) Sleep(10);
    return g_hwnd;
}

void display_set_hw(bool on) { g_hw = on; }

void display_overlay(void* hdc, int w, int h) { draw_overlay((HDC)hdc, w, h); }
bool display_menu_open() { return g_menu; }

void display_request_shot(const char* path) {
    std::lock_guard<std::mutex> lock(g_live_mutex);
    g_live_path = path;
}

bool display_shot_due(uint64_t frame) {
    {
        std::lock_guard<std::mutex> lock(g_live_mutex);
        if (!g_live_path.empty()) { g_live_frame = frame; return true; }
    }
    return !g_shot_path.empty() && std::find(g_shot_frames.begin(), g_shot_frames.end(), frame) != g_shot_frames.end();
}

void display_present(const uint32_t* px, uint32_t w, uint32_t h, uint64_t frame) {
    if (g_live_frame == frame || (!g_hw && !g_live_path.empty())) {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(g_live_mutex);
            path.swap(g_live_path);
        }
        g_live_frame = ~0ull;
        if (!path.empty() && write_png(path, px, w, h)) std::fprintf(stderr, "[live] screenshot (frame %llu) %s\n", (unsigned long long)frame, path.c_str());
    }
    if (!g_shot_path.empty() && std::find(g_shot_frames.begin(), g_shot_frames.end(), frame) != g_shot_frames.end()) {
        std::string path = g_shot_path;
        if (const size_t hash = path.find('#'); hash != std::string::npos) path.replace(hash, 1, std::to_string(frame));
        if (write_png(path, px, w, h)) std::fprintf(stderr, "[gpu] wrote %ux%u screenshot (frame %llu) to %s\n", w, h, (unsigned long long)frame, path.c_str());
        write_trace_file();   // with $TWONK_TRACE_FILE: what the game was doing at this frame (for hangs)
    }
    if (!g_window || g_hw) return;
    if (!g_started.exchange(true)) std::thread(window_thread).detach();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_frame.assign(px, px + (size_t)w * h);
        g_w = w; g_h = h;
    }
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

} // namespace ps2
