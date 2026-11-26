// Host-side runtime API (not used by generated code directly).
#pragma once
#include <cstdarg>
#include <cstdint>
#include <string>

#include "ps2/context.hpp"

namespace ps2 {

// ---- logging -------------------------------------------------------------------
enum class Log { Kernel, Mem, Cpu, Hle, Boot, Iop, Gpu };
void log(Log ch, const char* fmt, ...);
bool log_enabled(Log ch);

// ---- guest memory helpers for HLE code ----------------------------------------------
uint8_t* guest_ptr(Context* ctx, uint32_t addr, uint32_t size);   // fatal if not RAM/SPR
std::string guest_string(Context* ctx, uint32_t addr, size_t max = 1024);

// ---- kernel ----------------------------------------------------------------------
void kernel_init(Context* ctx);
void kernel_syscall(Context* ctx, uint32_t pc);
void kernel_poll(Context* ctx);   // deliver due interrupts at a safe point
uint64_t kernel_syscall_count();
void gpu_vblank(Context* ctx);
void memcard_config(const char* dir);   // --memcard <folder> (default ./memcard)
void pad_script(const char* spec);   // --input "frame:button,..."
void profiler_start(double delay, double seconds);   // --profile: sample the game thread, print hot functions, exit
void profiler_name_thread(const char* name);   // the calling thread can be profiled with TWONK_PROFILE_THREAD=<name>
bool profiler_snapshot(const char* thread, double seconds, const char* out_path);   // sample in the background, append report
void display_config(bool window, const char* shot_path, const char* shot_frames);
void display_stats(double game_fps, double vblank_hz);   // once a second: window title, --log gpu
void display_request_shot(const char* path);   // write the next presented frame to a PNG
void display_present(const uint32_t* rgb, uint32_t w, uint32_t h, uint64_t frame);       // once per frame: present, statistics
void* display_window();                          // the output window (HWND), created on demand; null with --no-window
void display_set_hw(bool on);                    // the GPU renderer presents to the window itself
void display_overlay(void* hdc, int w, int h);   // draw the FPS counter
bool display_shot_due(uint64_t frame);           // --shot wants this frame
void renderer_config(const char* name, int scale);   // --renderer hw|sw, --scale N
void renderer_set_scale(int scale);   // change the GPU renderer's scale (applied at the next frame)
int renderer_scale();                 // current GPU renderer scale; 0 = software renderer
bool display_menu_open();             // the F4 menu has the keyboard (no pad input)
bool movie_playing();                 // the IPU decoded a picture in the last moments (an FMV is on screen)

// ---- video settings (video.cpp; the F4 menu, saved to twonk_settings.ini) ----------
constexpr int kNumAspects = 5;   // 4:3, 16:9, 16:10, 21:9, stretch
constexpr int kNumSsaa = 3;      // off, 2x, 4x
struct VideoSettings {
    int scale = 0;        // GPU renderer resolution scale 1-8; 0 = auto (match the window)
    int ssaa = 0;         // supersampling: 0 off, 1 2x, 2 4x (auto scale only renders this much larger)
    int filter = 1;       // enlarging the picture: 0 bilinear, 1 sharp (Lanczos + contrast-adaptive sharpening)
    int aspect = 1;       // index into 4:3, 16:9, 16:10, 21:9, stretch
    bool fullscreen = false;
    bool sharp_text = true;   // re-sharpen text glyphs at high resolution scales
};
VideoSettings video_settings();
void video_settings_set(const VideoSettings& v);   // applies and saves
void video_cli(const char* scale, const char* aspect, bool fullscreen);   // --scale, --aspect, --fullscreen
const char* video_aspect_name(int i);
float video_widescreen_factor();       // horizontal view widening for 3D cameras (1 at 4:3)
float video_display_aspect(bool movie);   // output picture aspect; 0 = stretch to the window
float video_ssaa_factor(int ssaa);
bool video_sharp_text();               // cheap (atomic): read per draw
bool timer_take_event(int timer, uint32_t intc_mask);   // one due EE timer 0-2 event (INTC 9+timer)
extern uint64_t g_timer_delivered[3], g_timer_dropped[3];
extern int g_timer_in_handler;   // timer (0-2) whose INTC handler the kernel is running, else -1
extern uint16_t g_pad0_buttons;   // controller 1 buttons last given to the game (bit 3 = START)
void kernel_raise_dmac(int channel);   // a DMA channel finished: run its DMAC handlers
void kernel_raise_intc(int cause);     // an INTC source (IPU, ...) fired: run its INTC handlers
void hw_collect_gfx();          // game thread: interrupts for transfers the GIF/VIF worker finished (hw.cpp)
void gfx_present(uint64_t frame);
void gfx_wait_progress(int usec);   // spinning guest: wait for the GIF/VIF worker, or yield   // vblank: queue the present behind the GIF/VIF worker's work
void dump_hw_state();           // DMA/PATH3 state (hw.cpp)
void game_thread_register();     // call on the game thread (profiler.cpp)
void dump_game_stack();          // host stack of the game thread, for the watchdog
void dump_trace();               // recent guest function entries (services.cpp)
void write_trace_file();         // full trace ring to $TWONK_TRACE_FILE, if set

// ---- IOP (iop.cpp) ------------------------------------------------------------------
// The IOP is not emulated: SIF DMA, sifcmd and sifrpc are answered natively and
// each RPC server the game binds is a C++ service.
void iop_init(const std::string& disc_root);
bool disc_open(const std::string& iso_path, std::string* err);   // the user's ISO
void sif_syscall(Context* ctx, int call);   // SifDmaStat/SetDma/SetDChain/SetReg/GetReg
void iop_vblank(Context* ctx);              // per-frame IOP work (controller refresh)
bool iop_pop_packet(Context* ctx);          // write the next IOP->EE packet; true if one was written
bool iop_packet_pending();                  // an IOP->EE packet is waiting
void sif_latency_config(double usec);       // IOP reply latency (--sif-latency), kernel.cpp

} // namespace ps2
