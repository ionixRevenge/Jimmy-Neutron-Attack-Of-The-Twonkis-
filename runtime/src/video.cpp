// Video settings: resolution scale, supersampling, upscale filter, aspect ratio,
// fullscreen. Changed from the F4 menu (window thread), read by the GPU renderer
// (GS thread) and the widescreen camera hook (game thread); saved to
// twonk_settings.ini in the working directory whenever they change.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ps2/runtime.hpp"

namespace ps2 {

namespace {

std::mutex g_mutex;
VideoSettings g_vs;
bool g_loaded = false;
bool g_scale_from_cli = false;   // --scale given: this session uses it until the menu changes it
std::atomic<float> g_wide{1.0f};
std::atomic<bool> g_sharp_text{true};
constexpr const char* kFile = "twonk_settings.ini";

const char* const kAspectNames[] = {"4:3", "16:9", "16:10", "21:9", "stretch"};
constexpr float kAspectValues[] = {4.0f / 3.0f, 16.0f / 9.0f, 16.0f / 10.0f, 64.0f / 27.0f, 0.0f};

void update_wide_locked() {
    const float a = kAspectValues[g_vs.aspect];
    g_wide = a > 0 ? a / (4.0f / 3.0f) : 1.0f;
    g_sharp_text = g_vs.sharp_text;
}

void save_locked() {
    std::FILE* f = std::fopen(kFile, "w");
    if (!f) return;
    std::fprintf(f, "# Twonkies video settings (the F4 menu writes this file)\n");
    if (!g_scale_from_cli) std::fprintf(f, "scale=%d\n", g_vs.scale);
    std::fprintf(f, "ssaa=%d\nfilter=%d\naspect=%s\nfullscreen=%d\nsharp_text=%d\n", g_vs.ssaa, g_vs.filter,
                 kAspectNames[g_vs.aspect], g_vs.fullscreen ? 1 : 0, g_vs.sharp_text ? 1 : 0);
    std::fclose(f);
}

int aspect_index(const char* s) {
    for (int i = 0; i < kNumAspects; ++i)
        if (!std::strcmp(s, kAspectNames[i])) return i;
    return -1;
}

void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    std::FILE* f = std::fopen(kFile, "r");
    if (f) {
        char line[128];
        while (std::fgets(line, sizeof line, f)) {
            char* eq = std::strchr(line, '=');
            if (!eq || line[0] == '#') continue;
            *eq = 0;
            char* v = eq + 1;
            v[std::strcspn(v, "\r\n")] = 0;
            if (!std::strcmp(line, "scale") && !g_scale_from_cli) g_vs.scale = std::atoi(v);
            else if (!std::strcmp(line, "ssaa")) g_vs.ssaa = std::atoi(v);
            else if (!std::strcmp(line, "filter")) g_vs.filter = std::atoi(v);
            else if (!std::strcmp(line, "aspect")) { if (const int a = aspect_index(v); a >= 0) g_vs.aspect = a; }
            else if (!std::strcmp(line, "fullscreen")) g_vs.fullscreen = std::atoi(v) != 0;
            else if (!std::strcmp(line, "sharp_text")) g_vs.sharp_text = std::atoi(v) != 0;
        }
        std::fclose(f);
    }
    if (g_vs.scale < 0 || g_vs.scale > 8) g_vs.scale = 0;
    if (g_vs.ssaa < 0 || g_vs.ssaa >= kNumSsaa) g_vs.ssaa = 0;
    if (g_vs.filter < 0 || g_vs.filter > 1) g_vs.filter = 1;
    update_wide_locked();
}

} // namespace

const char* video_aspect_name(int i) { return i >= 0 && i < kNumAspects ? kAspectNames[i] : "?"; }

VideoSettings video_settings() {
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    return g_vs;
}

void video_settings_set(const VideoSettings& v) {
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    if (v.scale != g_vs.scale) g_scale_from_cli = false;
    g_vs = v;
    update_wide_locked();
    save_locked();
}

// Command line (before anything reads the settings): overrides for this session.
void video_cli(const char* scale, const char* aspect, bool fullscreen) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (scale) g_scale_from_cli = true;   // the saved scale is neither read nor overwritten
    load_locked();
    if (scale) {
        const int k = std::atoi(scale);
        g_vs.scale = k < 0 || k > 8 ? 0 : k;
    }
    if (aspect) {
        const int a = aspect_index(aspect);
        if (a >= 0) g_vs.aspect = a;
        else std::fprintf(stderr, "warning: --aspect %s: use 4:3, 16:9, 16:10, 21:9 or stretch\n", aspect);
    }
    if (fullscreen) g_vs.fullscreen = true;
    update_wide_locked();
}

bool video_sharp_text() { return g_sharp_text.load(std::memory_order_relaxed); }

float video_widescreen_factor() { return g_wide.load(std::memory_order_relaxed); }

float video_display_aspect(bool movie) {
    const VideoSettings v = video_settings();
    const float a = kAspectValues[v.aspect];
    if (a == 0) return 0;              // stretch to the window
    return movie ? 4.0f / 3.0f : a;    // movies are 4:3 pictures: pillarboxed
}

float video_ssaa_factor(int ssaa) {   // per-axis supersampling for "Off", "2x", "4x"
    static constexpr float k[] = {1.0f, 1.4142f, 2.0f};
    return ssaa >= 0 && ssaa < kNumSsaa ? k[ssaa] : 1.0f;
}

} // namespace ps2
