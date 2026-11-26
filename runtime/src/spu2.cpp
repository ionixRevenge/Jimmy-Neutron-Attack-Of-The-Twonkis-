// SPU2 emulation at the libsd level, mixed to XAudio2. See spu2.hpp.
//
// Per voice: SPU2 ADPCM (16-byte blocks of 28 samples, loop flags), a 4.12 pitch
// counter with linear interpolation, the ADSR envelope (PS1/PS2 rules) and fixed
// volumes. Per core: dry voice mix (VMIXL/VMIXR), the block-transfer PCM input
// (BVOL) and master volume; core 0 feeds core 1, whose output is played.
// Not modelled yet: reverb, noise, pitch modulation, volume sweeps.
#include "spu2.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "iop.hpp"
#include "iop_irx.hpp"
#include "ps2/runtime.hpp"

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace ps2::spu2 {

namespace {

constexpr uint32_t kRamBytes = 2 * 1024 * 1024;
constexpr int kRate = 48000;

std::mutex g_m;   // every SPU2 state change and every mixed chunk
std::vector<uint8_t> g_ram(kRamBytes);
std::atomic<float> g_out_volume{1.0f};

enum Reg : uint32_t {
    VOLL = 0x00, VOLR = 0x01, PITCH = 0x02, ADSR1 = 0x03, ADSR2 = 0x04, ENVX = 0x05, VOLXL = 0x06, VOLXR = 0x07,
    MMIX = 0x08, MVOLL = 0x09, MVOLR = 0x0A, EVOLL = 0x0B, EVOLR = 0x0C, AVOLL = 0x0D, AVOLR = 0x0E,
    BVOLL = 0x0F, BVOLR = 0x10, MVOLXL = 0x11, MVOLXR = 0x12,
    PMON = 0x13, NON = 0x14, KON = 0x15, KOFF = 0x16, ENDX = 0x17, VMIXL = 0x18, VMIXEL = 0x19, VMIXR = 0x1A, VMIXER = 0x1B,
    ESA = 0x1C, EEA = 0x1D, TSA = 0x1E, IRQA = 0x1F, SSA = 0x20, LSAX = 0x21, NAX = 0x22,
};

enum class Phase { Off, Attack, Decay, Sustain, Release };

struct Voice {
    uint16_t voll = 0, volr = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
    uint32_t ssa = 0, lsa = 0, block = 0;   // byte addresses in sound RAM
    Phase phase = Phase::Off;
    int32_t env = 0, env_wait = 0;
    uint32_t counter = 0;                   // 4.12 position between prev and cur
    int16_t buf[28] = {};
    int idx = 0;
    int32_t s1 = 0, s2 = 0;                 // ADPCM history
    uint8_t flags = 0;                      // loop flags of the current block
    int32_t prev = 0, cur = 0;
};

struct Adma {   // block transfer: 512-byte blocks of 128 left then 128 right samples
    bool active = false, loop = false;
    uint32_t addr = 0, size = 0, samples = 0;
};

struct Core {
    Voice v[24];
    // sceSdInit leaves every voice on the dry mix: the game never writes VMIXL/VMIXR
    uint32_t endx = 0, vmixl = 0xFFFFFF, vmixr = 0xFFFFFF, vmixel = 0, vmixer = 0, pmon = 0, non = 0;
    uint16_t mvoll = 0, mvolr = 0, bvoll = 0, bvolr = 0, avoll = 0, avolr = 0, evoll = 0, evolr = 0, mmix = 0;
    uint32_t esa = 0, eea = 0, tsa = 0, irqa = 0;
    Adma adma;
    uint32_t handler = 0, handler_arg = 0;   // DMA channel completion (IRX function)
} g_core[2];

// Fixed-mode volume register: signed 15 bits. Sweep mode is not modelled (full volume).
inline int32_t vol(uint16_t r) { return (r & 0x8000) ? 0x7FFF : (int32_t)(int16_t)(r << 1); }

// ---- ADPCM ---------------------------------------------------------------------------------------
void decode(Voice& v) {
    const uint8_t* b = &g_ram[v.block & (kRamBytes - 16)];
    int shift = b[0] & 15;
    if (shift > 12) shift = 9;
    const int filter = std::min((b[0] >> 4) & 7, 4);
    static const int f0[5] = {0, 60, 115, 98, 122}, f1[5] = {0, 0, -52, -55, -60};
    v.flags = b[1];
    for (int i = 0; i < 28; ++i) {
        const int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int32_t s = ((int32_t)(int16_t)(nib << 12)) >> shift;
        s += (v.s1 * f0[filter] + v.s2 * f1[filter] + 32) >> 6;
        s = std::clamp(s, -32768, 32767);
        v.s2 = v.s1;
        v.s1 = s;
        v.buf[i] = (int16_t)s;
    }
    if (v.flags & 4) v.lsa = v.block;   // loop start
}

void next_sample(Core& c, int vi) {
    Voice& v = c.v[vi];
    if (++v.idx >= 28) {
        v.idx = 0;
        if (v.flags & 1) {   // loop end
            c.endx |= 1u << vi;
            v.block = v.lsa;
            if (!(v.flags & 2)) { v.phase = Phase::Off; v.env = 0; }   // no repeat: the voice stops
        } else {
            v.block = (v.block + 16) & (kRamBytes - 1);
        }
        decode(v);
    }
    v.prev = v.cur;
    v.cur = v.buf[v.idx];
}

// ---- ADSR ----------------------------------------------------------------------------------------
void env_step(Voice& v, bool exp, bool dec, int shift, int step) {
    if (v.env_wait > 0) { --v.env_wait; return; }
    int32_t cycles = 1 << std::max(0, shift - 11);
    int32_t s = step << std::max(0, 11 - shift);
    if (exp && !dec && v.env > 0x6000) cycles *= 4;
    if (exp && dec) s = (int32_t)((int64_t)s * v.env >> 15);
    v.env = std::clamp(v.env + s, 0, 0x7FFF);
    v.env_wait = cycles - 1;
}

void envelope(Voice& v) {
    switch (v.phase) {
    case Phase::Attack:
        env_step(v, v.adsr1 >> 15, false, (v.adsr1 >> 10) & 31, 7 - ((v.adsr1 >> 8) & 3));
        if (v.env >= 0x7FFF) { v.phase = Phase::Decay; v.env_wait = 0; }
        break;
    case Phase::Decay: {
        env_step(v, true, true, (v.adsr1 >> 4) & 15, -8);
        const int32_t level = ((v.adsr1 & 15) + 1) * 0x800;
        if (v.env <= level) { v.phase = Phase::Sustain; v.env_wait = 0; }
        break;
    }
    case Phase::Sustain: {
        const bool dec = (v.adsr2 >> 14) & 1;
        const int st = (v.adsr2 >> 6) & 3;
        env_step(v, v.adsr2 >> 15, dec, (v.adsr2 >> 8) & 31, dec ? -8 + st : 7 - st);
        break;
    }
    case Phase::Release:
        env_step(v, (v.adsr2 >> 5) & 1, true, v.adsr2 & 31, -8);
        if (v.env <= 0) v.phase = Phase::Off;
        break;
    default: break;
    }
}

void key_on(Core& c, int vi) {
    Voice& v = c.v[vi];
    v.block = v.ssa & (kRamBytes - 16);
    v.s1 = v.s2 = 0;
    decode(v);
    v.idx = 0;
    v.prev = 0;
    v.cur = v.buf[0];
    v.counter = 0;
    v.env = 0;
    v.env_wait = 0;
    v.phase = Phase::Attack;
    c.endx &= ~(1u << vi);
}

void key_off(Core& c, int vi) {
    Voice& v = c.v[vi];
    if (v.phase != Phase::Off) { v.phase = Phase::Release; v.env_wait = 0; }
}

// One voice sample at 48 kHz, after the envelope (unclamped 16-bit range).
inline int32_t voice_sample(Core& c, int vi) {
    Voice& v = c.v[vi];
    if (v.phase == Phase::Off) return 0;
    const int32_t s = v.prev + (((v.cur - v.prev) * (int32_t)(v.counter & 0xFFF)) >> 12);
    v.counter += std::min<uint32_t>(v.pitch, 0x3FFF);
    while (v.counter >= 0x1000) {
        v.counter -= 0x1000;
        next_sample(c, vi);
        if (v.phase == Phase::Off) break;
    }
    const int32_t out = (s * v.env) >> 15;
    envelope(v);
    return out;
}

// $TWONK_WAV=<file.wav>: record everything played (for checking the mix without listening).
void record(const int16_t* out, int frames) {
    static std::FILE* f = [] {
        char* path = nullptr; size_t n = 0;
        std::FILE* file = nullptr;
        if (_dupenv_s(&path, &n, "TWONK_WAV") == 0 && path && *path && fopen_s(&file, path, "wb") == 0)
            std::fprintf(stderr, "[spu2] recording to %s\n", path);
        std::free(path);
        return file;
    }();
    if (!f) return;
    static uint32_t bytes = 0;
    auto hdr = [&] {
        const uint32_t h[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, (uint32_t)kRate,
                                (uint32_t)kRate * 4, 0x00100004, 0x61746164, bytes};
        std::fseek(f, 0, SEEK_SET);
        std::fwrite(h, 4, 11, f);
        std::fseek(f, 0, SEEK_END);
    };
    if (!bytes) hdr();
    std::fwrite(out, 4, (size_t)frames, f);
    bytes += (uint32_t)frames * 4;
    if ((bytes & 0xFFFFF) < (uint32_t)frames * 4) { hdr(); std::fflush(f); }   // ~every 5 s: a playable file at any point
}

// ---- mixing --------------------------------------------------------------------------------------
void mix(int16_t* out, int frames) {
    std::lock_guard<std::mutex> lock(g_m);
    uint8_t* iop = iop::ram(0, iop::kRamSize);
    const float master = g_out_volume.load(std::memory_order_relaxed);
    for (int f = 0; f < frames; ++f) {
        int32_t core_l[2] = {}, core_r[2] = {};
        for (int ci = 0; ci < 2; ++ci) {
            Core& c = g_core[ci];
            int32_t l = 0, r = 0;
            for (int vi = 0; vi < 24; ++vi) {
                if (c.v[vi].phase == Phase::Off) continue;
                const int32_t s = voice_sample(c, vi);
                if (c.vmixl & (1u << vi)) l += (s * vol(c.v[vi].voll)) >> 15;
                if (c.vmixr & (1u << vi)) r += (s * vol(c.v[vi].volr)) >> 15;
            }
            Adma& a = c.adma;
            if (a.active && a.size >= 1024) {
                const uint32_t blk = (a.samples / 128) * 512, j = (a.samples % 128) * 2;
                const uint32_t at = a.addr + blk + j;
                if (at + 258 <= iop::kRamSize) {
                    int16_t sl, sr;
                    std::memcpy(&sl, iop + at, 2);
                    std::memcpy(&sr, iop + at + 256, 2);
                    l += (sl * (int32_t)(int16_t)c.bvoll) >> 15;
                    r += (sr * (int32_t)(int16_t)c.bvolr) >> 15;
                }
                const uint32_t before = a.samples * 4, half = a.size / 2;
                ++a.samples;
                const uint32_t after = a.samples * 4;
                if (before < half && after >= half && c.handler) iop::irx::raise(c.handler, (uint32_t)ci, c.handler_arg);
                if (after >= a.size) {
                    if (c.handler) iop::irx::raise(c.handler, (uint32_t)ci, c.handler_arg);
                    if (a.loop) a.samples = 0; else a.active = false;
                }
            }
            core_l[ci] = l;
            core_r[ci] = r;
        }
        // core 0's output is an input of core 1, whose master volume is the final stage
        const int32_t l = ((core_l[0] * vol(g_core[0].mvoll)) >> 15) + core_l[1];
        const int32_t r = ((core_r[0] * vol(g_core[0].mvolr)) >> 15) + core_r[1];
        const float fl = (float)((l * vol(g_core[1].mvoll)) >> 15) * master, fr = (float)((r * vol(g_core[1].mvolr)) >> 15) * master;
        out[2 * f] = (int16_t)std::clamp((int)fl, -32768, 32767);
        out[2 * f + 1] = (int16_t)std::clamp((int)fr, -32768, 32767);
    }
    record(out, frames);
    static int since = 0;
    if ((since += frames) >= kRate * 5 && log_enabled(Log::Iop)) {   // every 5 s: what is sounding
        since = 0;
        for (int ci = 0; ci < 2; ++ci) {
            const Core& c = g_core[ci];
            int on = 0;
            for (const Voice& v : c.v) on += v.phase != Phase::Off;
            const Voice& v0 = c.v[0];
            log(Log::Iop, "[spu2] core%d: %d voices on, mvol %04x/%04x bvol %04x/%04x vmix %06x/%06x mmix %04x; v0 phase %d env %04x vol %04x/%04x pitch %04x",
                ci, on, c.mvoll, c.mvolr, c.bvoll, c.bvolr, c.vmixl, c.vmixr, c.mmix, (int)v0.phase, v0.env, v0.voll, v0.volr, v0.pitch);
        }
    }
}

// ---- output --------------------------------------------------------------------------------------
struct Callback : IXAudio2VoiceCallback {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { SetEvent(ev); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

constexpr int kChunk = 256;    // frames per buffer (5.3 ms)
constexpr int kQueued = 4;     // buffers in flight (~21 ms)

void audio_thread() {
    profiler_name_thread("audio");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    static int16_t bufs[kQueued + 1][kChunk * 2];
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* src = nullptr;
    static Callback cb;
    bool ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
    ok = ok && SUCCEEDED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR));
    ok = ok && SUCCEEDED(xa->CreateMasteringVoice(&master, 2, kRate));
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = kRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = kRate * 4;
    ok = ok && SUCCEEDED(xa->CreateSourceVoice(&src, &wf, 0, 2.0f, &cb));
    if (ok) ok = SUCCEEDED(src->Start());
    if (!ok) {
        // No audio device: keep the SPU2 clock running so streams still drain in real time.
        std::fprintf(stderr, "[spu2] no audio output (XAudio2 unavailable); sound is muted\n");
        auto t = std::chrono::steady_clock::now();
        for (;;) {
            t += std::chrono::microseconds(1000000LL * kChunk / kRate);
            std::this_thread::sleep_until(t);
            mix(bufs[0], kChunk);
        }
    }
    std::fprintf(stderr, "[spu2] audio: XAudio2, %d Hz stereo, %d ms buffer\n", kRate, kChunk * kQueued * 1000 / kRate);
    int next = 0;
    for (;;) {
        XAUDIO2_VOICE_STATE st{};
        src->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        while (st.BuffersQueued < (UINT32)kQueued) {
            mix(bufs[next], kChunk);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kChunk * 4;
            xb.pAudioData = reinterpret_cast<const BYTE*>(bufs[next]);
            src->SubmitSourceBuffer(&xb);
            next = (next + 1) % (kQueued + 1);
            ++st.BuffersQueued;
        }
        WaitForSingleObject(cb.ev, 100);
    }
}

void log_first(const char* what, uint16_t entry, uint32_t value) {
    static bool seen[2][256];
    const int kind = what[0] == 'p' ? 0 : 1;
    if (!seen[kind][entry >> 8]) {
        seen[kind][entry >> 8] = true;
        log(Log::Iop, "[spu2] first %s %04x = %x", what, entry, value);
    }
}

} // namespace

void start() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(audio_thread).detach(); });
}

void set_output_volume(float v) { g_out_volume.store(std::clamp(v, 0.0f, 1.0f)); }

void sd_init(int) {
    start();
    std::lock_guard<std::mutex> lock(g_m);
    for (Core& c : g_core) {
        const uint32_t h = c.handler, ha = c.handler_arg;
        c = Core{};
        c.handler = h; c.handler_arg = ha;
    }
}

void set_param(uint16_t e, uint16_t value) {
    log_first("param", e, value);
    std::lock_guard<std::mutex> lock(g_m);
    Core& c = g_core[e & 1];
    Voice& v = c.v[std::min((e >> 1) & 31, 23)];
    switch (e >> 8) {
    case VOLL: v.voll = value; break;
    case VOLR: v.volr = value; break;
    case PITCH: v.pitch = value; break;
    case ADSR1: v.adsr1 = value; break;
    case ADSR2: v.adsr2 = value; break;
    case ENVX: v.env = value & 0x7FFF; break;
    case MMIX: c.mmix = value; break;
    case MVOLL: c.mvoll = value; break;
    case MVOLR: c.mvolr = value; break;
    case EVOLL: c.evoll = value; break;
    case EVOLR: c.evolr = value; break;
    case AVOLL: c.avoll = value; break;
    case AVOLR: c.avolr = value; break;
    case BVOLL: c.bvoll = value; break;
    case BVOLR: c.bvolr = value; break;
    default: break;
    }
}

uint16_t get_param(uint16_t e) {
    std::lock_guard<std::mutex> lock(g_m);
    Core& c = g_core[e & 1];
    const Voice& v = c.v[std::min((e >> 1) & 31, 23)];
    switch (e >> 8) {
    case VOLL: return v.voll;
    case VOLR: return v.volr;
    case PITCH: return v.pitch;
    case ADSR1: return v.adsr1;
    case ADSR2: return v.adsr2;
    case ENVX: return (uint16_t)v.env;
    case VOLXL: return (uint16_t)(vol(v.voll) >> 1);
    case VOLXR: return (uint16_t)(vol(v.volr) >> 1);
    case MMIX: return c.mmix;
    case MVOLL: return c.mvoll;
    case MVOLR: return c.mvolr;
    case MVOLXL: return (uint16_t)(vol(c.mvoll) >> 1);
    case MVOLXR: return (uint16_t)(vol(c.mvolr) >> 1);
    case BVOLL: return c.bvoll;
    case BVOLR: return c.bvolr;
    default: return 0;
    }
}

void set_switch(uint16_t e, uint32_t value) {
    log_first("switch", e, value);
    std::lock_guard<std::mutex> lock(g_m);
    Core& c = g_core[e & 1];
    value &= 0xFFFFFF;
    switch (e >> 8) {
    case KON: for (int i = 0; i < 24; ++i) if (value & (1u << i)) key_on(c, i); break;
    case KOFF: for (int i = 0; i < 24; ++i) if (value & (1u << i)) key_off(c, i); break;
    case ENDX: c.endx = value; break;
    case PMON: c.pmon = value; break;
    case NON: c.non = value; break;
    case VMIXL: c.vmixl = value; break;
    case VMIXEL: c.vmixel = value; break;
    case VMIXR: c.vmixr = value; break;
    case VMIXER: c.vmixer = value; break;
    default: break;
    }
}

uint32_t get_switch(uint16_t e) {
    std::lock_guard<std::mutex> lock(g_m);
    const Core& c = g_core[e & 1];
    switch (e >> 8) {
    case ENDX: return c.endx;
    case PMON: return c.pmon;
    case NON: return c.non;
    case VMIXL: return c.vmixl;
    case VMIXEL: return c.vmixel;
    case VMIXR: return c.vmixr;
    case VMIXER: return c.vmixer;
    default: return 0;
    }
}

void set_addr(uint16_t e, uint32_t value) {
    log_first("addr", e, value);
    std::lock_guard<std::mutex> lock(g_m);
    Core& c = g_core[e & 1];
    Voice& v = c.v[std::min((e >> 1) & 31, 23)];
    value &= kRamBytes - 1;
    switch (e >> 8) {
    case SSA: v.ssa = value; break;
    case LSAX: v.lsa = value & ~15u; break;
    case NAX: v.block = value & ~15u; break;
    case ESA: c.esa = value; break;
    case EEA: c.eea = value; break;
    case TSA: c.tsa = value; break;
    case IRQA: c.irqa = value; break;
    default: break;
    }
}

uint32_t get_addr(uint16_t e) {
    std::lock_guard<std::mutex> lock(g_m);
    const Core& c = g_core[e & 1];
    const Voice& v = c.v[std::min((e >> 1) & 31, 23)];
    switch (e >> 8) {
    case SSA: return v.ssa;
    case LSAX: return v.lsa;
    case NAX: return v.block + (uint32_t)(v.idx / 2 + 2) / 16 * 16;   // the block being played
    case ESA: return c.esa;
    case EEA: return c.eea;
    case TSA: return c.tsa;
    case IRQA: return c.irqa;
    default: return 0;
    }
}

int voice_trans(int ch, uint16_t mode, uint32_t iop_addr, uint32_t spu_addr, uint32_t size) {
    ch &= 1;
    spu_addr &= kRamBytes - 1;
    size = std::min(size, kRamBytes - spu_addr);
    if ((uint64_t)iop_addr + size > iop::kRamSize) return -1;
    {
        std::lock_guard<std::mutex> lock(g_m);
        uint8_t* iop = iop::ram(iop_addr, size);
        if ((mode & 3) == 1) std::memcpy(iop, &g_ram[spu_addr], size);   // read back
        else std::memcpy(&g_ram[spu_addr], iop, size);
        g_core[ch].tsa = spu_addr + size;
    }
    if (log_enabled(Log::Iop)) {
        uint32_t nz = 0;
        for (uint32_t i = 0; i < size; ++i) nz += g_ram[spu_addr + i] != 0;
        log(Log::Iop, "[spu2] voice trans ch%d %s iop %06x spu %06x size %x (%u nonzero)", ch, (mode & 3) == 1 ? "read" : "write", iop_addr, spu_addr, size, nz);
    }
    if (g_core[ch].handler) iop::irx::raise(g_core[ch].handler, (uint32_t)ch, g_core[ch].handler_arg);
    return (int)size;
}

int block_trans(int ch, uint16_t mode, uint32_t iop_addr, uint32_t size, uint32_t start_addr) {
    ch &= 1;
    std::lock_guard<std::mutex> lock(g_m);
    Adma& a = g_core[ch].adma;
    switch (mode & 3) {
    case 2: a.active = false; return 0;   // stop
    case 0: case 3:
        a.active = true;
        a.loop = (mode & 0x10) != 0;
        a.addr = iop_addr;
        a.size = size & ~1023u;
        a.samples = (mode & 3) == 3 && start_addr > iop_addr ? (start_addr - iop_addr) / 4 : 0;
        if (a.size && a.samples * 4 >= a.size) a.samples = 0;
        log(Log::Iop, "[spu2] block trans ch%d %s iop %06x size %x from %06x", ch, a.loop ? "loop" : "once", iop_addr, size, start_addr);
        return 0;
    default: return -1;   // reading the output back is not supported
    }
}

uint32_t block_trans_status(int ch, int) {
    std::lock_guard<std::mutex> lock(g_m);
    const Adma& a = g_core[ch & 1].adma;
    return a.addr + a.samples * 4;
}

void set_trans_handler(int ch, uint32_t func, uint32_t arg) {
    std::lock_guard<std::mutex> lock(g_m);
    g_core[ch & 1].handler = func;
    g_core[ch & 1].handler_arg = arg;
}

} // namespace ps2::spu2
