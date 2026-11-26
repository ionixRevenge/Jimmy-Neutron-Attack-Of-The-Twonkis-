// Hardware GS renderer on Direct3D 11. See gs_hw.hpp for the memory model.
//
// Surfaces (render targets and depth buffers) are textures at `scale` times the
// GS resolution, keyed by base pointer. Colour surfaces are RGBA8 with alpha
// stored as a/128 (so 0x80 is 1.0, as the GS treats it; values above 0x80 clamp).
// Depth surfaces are D32_FLOAT holding z / zmax. Textures are decoded on the CPU
// into RGBA8 (TEXA and CLUT applied) and cached; a texture that is exactly a
// render target is sampled from the GPU surface instead, at full resolution.
//
// GS blending, Cv = (A - B) * C / 128 + D, maps onto D3D blend states with the
// pixel shader's second output carrying C (dual-source blending). The few
// combinations D3D cannot express are approximated. Alpha test failure modes
// other than KEEP take a second pass with the test inverted.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#undef DrawState   // a Win32 macro; ours is the GS drawing state

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "perf.hpp"
#include "texpack.hpp"
#include "gs_hw.hpp"
#include "gs_mem.hpp"
#include "ps2/runtime.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

namespace ps2 {
// renderer selection (host_main): "hw" or "sw", and the resolution scale
namespace { int g_want_hw = -1, g_want_scale = 0; }
void renderer_config(const char* name, int scale) {
    if (name) g_want_hw = std::strcmp(name, "hw") == 0 ? 1 : 0;
    if (scale > 0) g_want_scale = scale;
}
// the F4 menu: the scale lives in the video settings (0 = auto); the GS thread applies it
namespace { std::atomic<int> g_cur_scale{0}; }
void renderer_set_scale(int scale) {
    VideoSettings v = video_settings();
    v.scale = std::clamp(scale, 0, 8);
    video_settings_set(v);
}
int renderer_scale() { return g_cur_scale; }
} // namespace ps2

namespace ps2::gpu::hw {

using Microsoft::WRL::ComPtr;

namespace {

inline uint32_t bf(uint64_t v, int lo, int n) { return (uint32_t)((v >> lo) & ((1ull << n) - 1)); }

// ---- shaders ---------------------------------------------------------------------------------
const char* kShaders = R"HLSL(
cbuffer Draw : register(b0) {
    float2 ndc;          // 2 / surface size in GS pixels
    float zscale;        // 1 / zmax
    float tscale;        // texture pixels per GS texel (surface sources: the scale)
    float4 fogcol;
    uint tme, fst, tfx, tcc;
    uint tw, th, wms, wmt;
    uint minu, maxu, minv, maxv;
    uint bilinear, tsrc, aem, fge;     // tsrc: 0 decoded, 1 surface CT32, 2 surface CT24, 3 surface CT16
    float ta0, ta1, aref, fix;
    uint atst, amode, fba, cfac;       // amode: 0 off, 1 keep passing, 2 keep failing; cfac: 0 As, 1 Ad, 2 FIX
    uint pabe, outmode, sharp, pad1;   // outmode: 0 colour, 1 colour * (1 + C), 2 C as colour; sharp: text (glyph_mode)
};
Texture2D<float4> tex : register(t0);

struct VSIn { float2 pos : POSITION; uint z : ZVAL; float4 col : COLOR; float3 stq : STQ; float2 uv : UV; float fog : FOG; };
struct PSIn {
    float4 pos : SV_Position;
    noperspective float4 col : COLOR;
    noperspective float3 stq : STQ;
    noperspective float2 uv : UV;
    noperspective float fog : FOG;
};

PSIn vs_main(VSIn i) {
    PSIn o;
    float2 p = i.pos * ndc - 1.0;
    o.pos = float4(p.x, -p.y, saturate((float)i.z * zscale), 1.0);
    o.col = i.col; o.stq = i.stq; o.uv = i.uv; o.fog = i.fog;
    return o;
}

int wrapc(int c, uint mode, int size, int mn, int mx) {
    if (mode == 0) return c & (size - 1);
    if (mode == 1) return clamp(c, 0, size - 1);
    if (mode == 2) return clamp(c, mn, mx);
    return (c & mn) | mx;
}

float4 texel(int2 c) {   // c: in units of 1/tscale GS texel (replacement or surface pixels)
    int s = (int)tscale;
    int2 g = int2(floor(float2(c) / tscale));
    int2 sub = c - g * s;
    g.x = wrapc(g.x, wms, (int)tw, (int)minu, (int)maxu);
    g.y = wrapc(g.y, wmt, (int)th, (int)minv, (int)maxv);
    float4 t = tex.Load(int3(g * s + sub, 0));
    if (tsrc == 0) return round(t * 255.0);
    float4 r = float4(round(t.rgb * 255.0), round(t.a * 128.0));
    if (tsrc == 2) {
        r.a = (aem != 0 && all(r.rgb == 0)) ? 0 : ta0;
    } else if (tsrc == 3) {
        bool hi = r.a >= 128;
        r.rgb = floor(r.rgb / 8) * 8;
        r.a = hi ? ta1 : ((aem != 0 && all(r.rgb == 0)) ? 0 : ta0);
    }
    return r;
}

float4 sample_tex(float2 uv) {
    float2 p = uv * tscale;
    if (bilinear == 0) return texel(int2(floor(p)));
    p -= 0.5;
    float2 fl = floor(p);
    int2 i = int2(fl);
    float2 f = p - fl;
    float4 a = texel(i), b = texel(i + int2(1, 0)), c = texel(i + int2(0, 1)), d = texel(i + int2(1, 1));
    return floor(lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) + 0.5);
}

// Text: glyph sheets are white (or white with a black outline) with anti-aliased
// alpha, made for 640x448. Interpolated, their alpha (and the white/black boundary)
// behaves like a distance field: cut at one half with a ramp one output pixel wide,
// the letters stay sharp and smooth at any resolution. Colour is weighted by alpha so
// the clear texels around a letter do not darken its edge.
float4 sample_glyph(float2 uv) {
    float2 p = uv * tscale - 0.5;
    float2 fl = floor(p);
    int2 i = int2(fl);
    float2 f = p - fl;
    float4 t00 = texel(i), t10 = texel(i + int2(1, 0)), t01 = texel(i + int2(0, 1)), t11 = texel(i + int2(1, 1));
    float4 w = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y);
    float4 al = saturate(float4(t00.a, t10.a, t01.a, t11.a) / 128.0);
    float a = dot(w, al);
    float l = dot(w * al, float4(t00.r, t10.r, t01.r, t11.r) / 255.0) / max(a, 1e-4);
    float fa = fwidth(a), fl2 = fwidth(l);
    float as = saturate((a - 0.5) / max(fa, 1e-4) + 0.5);
    float ls = saturate((l - 0.5) / max(fl2, 1e-4) + 0.5);
    as = lerp(as, a, saturate(fa * 2 - 1));     // shrunk text (a texel under a pixel): leave it
    ls = lerp(ls, l, saturate(fl2 * 2 - 1));
    if (sharp != 2) ls = l;                     // alpha only: keep the colours
    return float4(floor(ls * 255 + 0.5).xxx, floor(as * 128 + 0.5));
}

bool atest(float a) {
    switch (atst) {
    case 0: return false;
    case 1: return true;
    case 2: return a < aref;
    case 3: return a <= aref;
    case 4: return a == aref;
    case 5: return a >= aref;
    case 6: return a > aref;
    default: return a != aref;
    }
}

struct PSOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };

PSOut ps_main(PSIn i) {
    float4 c = floor(i.col + 0.5);
    if (tme != 0) {
        float q = i.stq.z != 0 ? i.stq.z : 1.0;
        float2 uv = fst != 0 ? i.uv : i.stq.xy / q * float2(tw, th);
        float4 t = sharp != 0 ? sample_glyph(uv) : sample_tex(uv);
        if (tfx == 0) {
            c.rgb = min(255, floor(c.rgb * t.rgb / 128));
            if (tcc != 0) c.a = min(255, floor(c.a * t.a / 128));
        } else if (tfx == 1) {
            c.rgb = t.rgb;
            if (tcc != 0) c.a = t.a;
        } else {
            c.rgb = min(255, floor(c.rgb * t.rgb / 128) + c.a);
            if (tcc != 0) c.a = tfx == 2 ? min(255, c.a + t.a) : t.a;
        }
    }
    if (fge != 0) {
        float f = clamp(floor(i.fog), 0, 255);
        c.rgb = floor((c.rgb * f + fogcol.rgb * (255 - f)) / 256);
    }
    if (amode != 0 && (amode == 1) != atest(c.a)) discard;
    float a = c.a;
    if (fba != 0 && a < 128) a += 128;
    float fac = cfac == 0 ? c.a / 128.0 : fix / 128.0;
    if (pabe != 0 && c.a < 128) fac = 1.0;
    float3 rgb = c.rgb / 255.0;
    if (outmode == 1) rgb *= 1.0 + fac;
    else if (outmode == 2) rgb = fac;
    PSOut o;
    o.c0 = float4(rgb, a / 128.0);
    o.c1 = float4(0, 0, 0, fac);
    return o;
}

// ---- copies between surfaces, uploads, downloads and the screen -------------------------------
cbuffer Blit : register(b0) {
    float2 org;          // viewport origin
    float k;             // source pixels per destination pixel
    float pad2;
    int2 src_off;
    float2 pad3;
    float4 uvrect;       // present: source rectangle in UV
    float4 texsz;        // present: source texture width, height, 1/width, 1/height
    float4 prm;          // present: x, y source pixels per output pixel; z sharpening 0-1
};
Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);

struct QOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };
QOut vs_quad(uint id : SV_VertexID) {
    QOut o;
    float2 t = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);
    o.uv = lerp(uvrect.xy, uvrect.zw, t);
    o.pos = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);
    return o;
}
int2 src_px(float2 pos) { return int2(floor((pos - org) * k)) + src_off; }
float4 ps_copy(QOut i) : SV_Target { return src.Load(int3(src_px(i.pos.xy), 0)); }
float ps_depth_write(QOut i) : SV_Depth { return src.Load(int3(src_px(i.pos.xy), 0)).r; }
float4 ps_present(QOut i) : SV_Target { return float4(src.Sample(lin, i.uv).rgb, 1); }

// Shrinking (supersampling resolve): the average over the output pixel's footprint
// in the source, from up to 8x8 bilinear taps spread evenly over it.
float4 ps_down(QOut i) : SV_Target {
    const int2 n = clamp(int2(ceil(prm.xy - 0.01)), 1, 8);
    const float2 step = prm.xy / float2(n);
    const float2 o = i.uv * texsz.xy - prm.xy * 0.5;
    float3 acc = 0;
    for (int y = 0; y < n.y; ++y)
        for (int x = 0; x < n.x; ++x)
            acc += src.SampleLevel(lin, (o + step * (float2(x, y) + 0.5)) * texsz.zw, 0).rgb;
    return float4(acc / (n.x * n.y), 1);
}

// Enlarging: Lanczos-2 (4x4 taps), clamped to the four nearest source pixels so
// edges get no ringing halos.
float lanczos2(float x) {
    x = abs(x);
    if (x < 1e-4) return 1;
    if (x >= 2) return 0;
    const float px = 3.14159265 * x;
    return 2 * sin(px) * sin(px * 0.5) / (px * px);
}
float4 ps_lanczos(QOut i) : SV_Target {
    const float2 p = i.uv * texsz.xy - 0.5;
    const float2 f = floor(p), t = p - f;
    const int2 hi = int2(texsz.xy) - 1;
    float3 acc = 0, mn = 1e9, mx = -1e9;
    float wsum = 0;
    [unroll] for (int y = -1; y <= 2; ++y) {
        const float wy = lanczos2(y - t.y);
        [unroll] for (int x = -1; x <= 2; ++x) {
            const float w = lanczos2(x - t.x) * wy;
            const float3 c = src.Load(int3(clamp(int2(f) + int2(x, y), int2(0, 0), hi), 0)).rgb;
            if (x >= 0 && x <= 1 && y >= 0 && y <= 1) { mn = min(mn, c); mx = max(mx, c); }
            acc += c * w;
            wsum += w;
        }
    }
    return float4(clamp(acc / wsum, mn, mx), 1);
}

// Contrast-adaptive sharpening (after AMD's CAS) of an output-sized picture: each
// pixel is pushed away from its four neighbours, less where contrast is already high.
float4 ps_cas(QOut i) : SV_Target {
    const int2 c = src_px(i.pos.xy), hi = int2(texsz.xy) - 1;
    const float3 b = src.Load(int3(clamp(c + int2(0, -1), int2(0, 0), hi), 0)).rgb;
    const float3 d = src.Load(int3(clamp(c + int2(-1, 0), int2(0, 0), hi), 0)).rgb;
    const float3 e = src.Load(int3(c, 0)).rgb;
    const float3 f = src.Load(int3(clamp(c + int2(1, 0), int2(0, 0), hi), 0)).rgb;
    const float3 h = src.Load(int3(clamp(c + int2(0, 1), int2(0, 0), hi), 0)).rgb;
    const float3 mn = min(min(min(b, d), min(e, f)), h);
    const float3 mx = max(max(max(b, d), max(e, f)), h);
    const float3 amp = sqrt(saturate(min(mn, 1 - mx) / max(mx, 1e-5)));
    const float3 w = amp * (-1.0 / lerp(8.0, 5.0, prm.z));
    return float4(saturate((b * w + d * w + f * w + h * w + e) / (1 + 4 * w)), 1);
}
)HLSL";

struct DrawCB {
    float ndc[2], zscale, tscale;
    float fogcol[4];
    uint32_t tme, fst, tfx, tcc;
    uint32_t tw, th, wms, wmt;
    uint32_t minu, maxu, minv, maxv;
    uint32_t bilinear, tsrc, aem, fge;
    float ta0, ta1, aref, fix;
    uint32_t atst, amode, fba, cfac;
    uint32_t pabe, outmode, sharp, pad1;
};
struct BlitCB {
    float org[2], k, pad2;
    int32_t src_off[2];
    float pad3[2];
    float uvrect[4];
    float texsz[4];
    float prm[4];
};

struct HwVert {
    float x, y;
    uint32_t z;
    float r, g, b, a;
    float s, t, q;
    float u, v;
    float f;
};

// ---- device ----------------------------------------------------------------------------------
int g_state = 0;   // 0 not started, 1 running, -1 off (software renderer)
int g_scale = 2;
ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<ID3D11VertexShader> g_vs, g_vs_quad;
ComPtr<ID3D11PixelShader> g_ps, g_ps_copy, g_ps_depth_write, g_ps_present, g_ps_down, g_ps_lanczos, g_ps_cas;
ComPtr<ID3D11InputLayout> g_layout;
ComPtr<ID3D11Buffer> g_vb, g_cb_draw, g_cb_blit;
ComPtr<ID3D11RasterizerState> g_rs, g_rs_noscissor;
ComPtr<ID3D11SamplerState> g_lin;
ComPtr<ID3D11DepthStencilState> g_ds_off, g_ds_always_write;
constexpr UINT kVbBytes = 8 << 20;
UINT g_vb_pos = 0;

// staging for CPU <-> GPU transfers (GS pixels, 1x)
constexpr UINT kXfer = 2048;
ComPtr<ID3D11Texture2D> g_up_c, g_up_z, g_down_c, g_down_z, g_stage_c, g_stage_z;
ComPtr<ID3D11ShaderResourceView> g_up_c_srv, g_up_z_srv;
ComPtr<ID3D11RenderTargetView> g_down_c_rtv, g_down_z_rtv;

// swap chain
HWND g_hwnd = nullptr;
ComPtr<IDXGISwapChain> g_swap;
ComPtr<ID3D11RenderTargetView> g_back_rtv;
UINT g_back_w = 0, g_back_h = 0;
bool g_window = false;

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>* out) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(kShaders, std::strlen(kShaders), "gs_hw", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out->GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        std::fprintf(stderr, "[gs-hw] shader %s: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "failed");
        return false;
    }
    return true;
}

uint64_t g_tex_made = 0, g_tex_made_px = 0, g_fb_copies = 0, g_fb_copy_px = 0;   // perf counters
std::string g_make_site = "setup";   // what the next make_tex() is for (perf log)
std::mutex g_made_mutex;
std::map<std::string, uint32_t> g_made_count;
bool make_tex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, D3D11_USAGE usage, UINT cpu, ComPtr<ID3D11Texture2D>* out,
              const void* init = nullptr, UINT pitch = 0) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
    d.SampleDesc.Count = 1; d.Usage = usage; d.BindFlags = bind; d.CPUAccessFlags = cpu;
    D3D11_SUBRESOURCE_DATA sd{init, pitch, 0};
    ++g_tex_made;
    g_tex_made_px += (uint64_t)w * h;
    {
        std::lock_guard<std::mutex> lock(g_made_mutex);
        ++g_made_count[g_make_site + " " + std::to_string(w) + "x" + std::to_string(h)];
    }
    const HRESULT hr = g_dev->CreateTexture2D(&d, init ? &sd : nullptr, out->ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        static int logged;
        if (logged++ < 10)
            std::fprintf(stderr, "[gs-hw] cannot create a %ux%u texture (format %d, bind %x, usage %d): %08lx\n", w, h, (int)f, bind,
                         (int)usage, (unsigned long)hr);
        return false;
    }
    return true;
}

bool init_device() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 1, D3D11_SDK_VERSION,
                                 g_dev.GetAddressOf(), nullptr, g_ctx.GetAddressOf()))) {
        std::fprintf(stderr, "[gs-hw] no Direct3D 11 device\n");
        return false;
    }
    ComPtr<ID3DBlob> vs, vsq, ps, psc, psdw, psp, psdn, psl, pscas;
    if (!compile("vs_main", "vs_5_0", &vs) || !compile("ps_main", "ps_5_0", &ps) || !compile("vs_quad", "vs_5_0", &vsq) ||
        !compile("ps_copy", "ps_5_0", &psc) || !compile("ps_depth_write", "ps_5_0", &psdw) ||
        !compile("ps_present", "ps_5_0", &psp) || !compile("ps_down", "ps_5_0", &psdn) || !compile("ps_lanczos", "ps_5_0", &psl) ||
        !compile("ps_cas", "ps_5_0", &pscas))
        return false;
    g_dev->CreatePixelShader(psdn->GetBufferPointer(), psdn->GetBufferSize(), nullptr, &g_ps_down);
    g_dev->CreatePixelShader(psl->GetBufferPointer(), psl->GetBufferSize(), nullptr, &g_ps_lanczos);
    g_dev->CreatePixelShader(pscas->GetBufferPointer(), pscas->GetBufferSize(), nullptr, &g_ps_cas);
    g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreateVertexShader(vsq->GetBufferPointer(), vsq->GetBufferSize(), nullptr, &g_vs_quad);
    g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps);
    g_dev->CreatePixelShader(psc->GetBufferPointer(), psc->GetBufferSize(), nullptr, &g_ps_copy);
    g_dev->CreatePixelShader(psdw->GetBufferPointer(), psdw->GetBufferSize(), nullptr, &g_ps_depth_write);
    g_dev->CreatePixelShader(psp->GetBufferPointer(), psp->GetBufferSize(), nullptr, &g_ps_present);
    const D3D11_INPUT_ELEMENT_DESC ie[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"ZVAL", 0, DXGI_FORMAT_R32_UINT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"STQ", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"UV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"FOG", 0, DXGI_FORMAT_R32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(g_dev->CreateInputLayout(ie, 6, vs->GetBufferPointer(), vs->GetBufferSize(), &g_layout))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kVbBytes; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_vb))) return false;
    bd.ByteWidth = sizeof(DrawCB); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_cb_draw))) return false;
    bd.ByteWidth = sizeof(BlitCB);
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_cb_blit))) return false;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = FALSE; rd.ScissorEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_rs);
    rd.ScissorEnable = FALSE;
    g_dev->CreateRasterizerState(&rd, &g_rs_noscissor);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_lin);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    g_dev->CreateDepthStencilState(&dd, &g_ds_off);
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    g_dev->CreateDepthStencilState(&dd, &g_ds_always_write);
    // transfer textures
    const UINT rt_srv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bool ok = make_tex(kXfer, kXfer, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, &g_up_c) &&
              make_tex(kXfer, kXfer, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, &g_up_z) &&
              make_tex(kXfer, kXfer, DXGI_FORMAT_R8G8B8A8_UNORM, rt_srv, D3D11_USAGE_DEFAULT, 0, &g_down_c) &&
              make_tex(kXfer, kXfer, DXGI_FORMAT_R32_FLOAT, rt_srv, D3D11_USAGE_DEFAULT, 0, &g_down_z) &&
              make_tex(kXfer, kXfer, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &g_stage_c) &&
              make_tex(kXfer, kXfer, DXGI_FORMAT_R32_FLOAT, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &g_stage_z);
    if (!ok) return false;
    g_dev->CreateShaderResourceView(g_up_c.Get(), nullptr, &g_up_c_srv);
    g_dev->CreateShaderResourceView(g_up_z.Get(), nullptr, &g_up_z_srv);
    g_dev->CreateRenderTargetView(g_down_c.Get(), nullptr, &g_down_c_rtv);
    g_dev->CreateRenderTargetView(g_down_z.Get(), nullptr, &g_down_z_rtv);
    return true;
}

// ---- GS memory geometry ----------------------------------------------------------------------
bool is16(uint32_t psm) { return psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S; }
uint32_t page_h(uint32_t psm) {
    if (is16(psm) || psm == PSMT8) return 64;
    if (psm == PSMT4) return 128;
    return 32;   // 32-bit layouts, including T8H/T4HL/T4HH
}
uint32_t pages_per_row(uint32_t bw, uint32_t psm) {
    bw = std::max(bw, 1u);
    return (psm == PSMT8 || psm == PSMT4) ? (bw + 1) / 2 : bw;
}

constexpr uint32_t kPages = 512;   // 8 KB pages in 4 MB
uint32_t g_gen[kPages];

void bump(uint32_t lo, uint32_t hi) {
    if (hi <= lo) return;
    for (uint32_t p = lo / 32; p <= (hi - 1) / 32 && p < lo / 32 + kPages; ++p) ++g_gen[p % kPages];
}
uint64_t gen_sum(uint32_t lo, uint32_t hi) {
    uint64_t s = 0;
    if (hi <= lo) return 0;
    for (uint32_t p = lo / 32; p <= (hi - 1) / 32 && p < lo / 32 + kPages; ++p) s += (uint64_t)g_gen[p % kPages] * (p + 1);
    return s;
}

uint32_t zmax_of(uint32_t zpsm) { return zpsm == PSMZ32 ? 0xFFFFFFFFu : zpsm == PSMZ24 ? 0xFFFFFFu : 0xFFFFu; }

// $TWONK_HW_TRACE=<present number>: log every operation for two frames from there
uint64_t g_presents = 0, g_trace_at = ~0ull;
bool tracing() { return g_presents >= g_trace_at && g_presents < g_trace_at + 2; }

// ---- surfaces --------------------------------------------------------------------------------
struct Surface {
    uint32_t bp, bw, psm;
    bool depth;
    uint32_t w, h;          // allocated, GS pixels
    uint32_t rows = 0;      // rows in use (their memory belongs to this surface)
    bool dirty = false;     // GPU copy newer than GS memory
    bool alpha32 = false;   // drawn as CT32 at least once (alpha is ours)
    // Row ranges whose memory another surface took over (the same base pointer at another
    // width): written back when that happened, reloaded from GS memory before the next use.
    std::vector<std::pair<uint32_t, uint32_t>> stale;
    // GS memory pages (page numbers) another surface took over by a GPU copy: what is
    // here is old, the current content is in that surface (see take_pages()).
    std::vector<uint32_t> lent;
    uint64_t last_use = 0;  // present count when last drawn to, shown or sampled
    // A copy of what a draw that samples this surface while drawing into it reads.
    ComPtr<ID3D11Texture2D> fb_tex;
    ComPtr<ID3D11ShaderResourceView> fb_srv;
    uint32_t fb_made = 0;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    void range(uint32_t* lo, uint32_t* hi) const { rect_blocks(bp, bw, psm, 0, rows, lo, hi); }
};
std::vector<std::unique_ptr<Surface>> g_surf;
uint64_t g_downloads = 0, g_refreshes = 0, g_tex_decodes = 0, g_draws = 0;

bool alloc_surface(Surface& s, uint32_t h, const char* why = "new surface") {
    const UINT W = s.w * g_scale, H = h * g_scale;
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s bp %x bw %u psm %x%s", why, s.bp, s.bw, s.psm, s.depth ? " depth" : "");
        g_make_site = buf;
    }
    if (s.depth) {
        if (!make_tex(W, H, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, &s.tex))
            return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        g_dev->CreateDepthStencilView(s.tex.Get(), &dv, s.dsv.ReleaseAndGetAddressOf());
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        g_dev->CreateShaderResourceView(s.tex.Get(), &sv, s.srv.ReleaseAndGetAddressOf());
        g_ctx->ClearDepthStencilView(s.dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
    } else {
        if (!make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, &s.tex))
            return false;
        g_dev->CreateRenderTargetView(s.tex.Get(), nullptr, s.rtv.ReleaseAndGetAddressOf());
        g_dev->CreateShaderResourceView(s.tex.Get(), nullptr, s.srv.ReleaseAndGetAddressOf());
        const float zero[4] = {};
        g_ctx->ClearRenderTargetView(s.rtv.Get(), zero);
    }
    s.h = h;
    return true;
}

void set_blit(float ox, float oy, float k, int sx, int sy, const float* uv = nullptr, const float* texsz = nullptr,
              const float* prm = nullptr) {
    D3D11_MAPPED_SUBRESOURCE m;
    g_ctx->Map(g_cb_blit.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    BlitCB cb{};
    cb.org[0] = ox; cb.org[1] = oy; cb.k = k; cb.src_off[0] = sx; cb.src_off[1] = sy;
    if (uv) std::memcpy(cb.uvrect, uv, sizeof cb.uvrect);
    if (texsz) std::memcpy(cb.texsz, texsz, sizeof cb.texsz);
    if (prm) std::memcpy(cb.prm, prm, sizeof cb.prm);
    std::memcpy(m.pData, &cb, sizeof cb);
    g_ctx->Unmap(g_cb_blit.Get(), 0);
}

// Draw `srv` into the bound target(s) over viewport (x, y, w, h) with a full-rect quad.
void blit(ID3D11ShaderResourceView* srv, ID3D11PixelShader* ps, float x, float y, float w, float h) {
    D3D11_VIEWPORT vp{x, y, w, h, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rs_noscissor.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_vs_quad.Get(), nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, g_cb_blit.GetAddressOf());
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, g_cb_blit.GetAddressOf());
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->PSSetSamplers(0, 1, g_lin.GetAddressOf());
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->Draw(4, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &none);
}

uint32_t color_psm(uint32_t psm) {   // a Z format drawn as colour
    switch (psm) {
    case PSMZ32: return PSMCT32;
    case PSMZ24: return PSMCT24;
    case PSMZ16: return PSMCT16;
    case PSMZ16S: return PSMCT16S;
    default: return psm;
    }
}

// GS memory -> surface rows [r0, r1), columns [x0, x1).
void refresh(Surface& s, uint32_t r0, uint32_t r1, uint32_t x0 = 0, uint32_t x1 = ~0u) {
    r1 = std::min(r1, s.h);
    x1 = std::min(x1, s.w);
    if (r0 >= r1 || x0 >= x1) return;
    ++g_refreshes;
    if (tracing()) std::fprintf(stderr, "[hw-trace] refresh %x rows %u-%u x %u-%u\n", s.bp, r0, r1, x0, x1);
    const uint32_t n = r1 - r0, w = x1 - x0;
    const float ox = (float)(x0 * g_scale), oy = (float)(r0 * g_scale);
    if (s.depth) {
        std::vector<float> px((size_t)w * n);
        const double inv = 1.0 / zmax_of(s.psm);
        for (uint32_t y = 0; y < n; ++y)
            for (uint32_t x = 0; x < w; ++x) px[(size_t)y * w + x] = (float)(read_pixel(s.psm, s.bp, s.bw, x0 + x, r0 + y) * inv);
        D3D11_BOX box{0, 0, 0, w, n, 1};
        g_ctx->UpdateSubresource(g_up_z.Get(), 0, &box, px.data(), w * 4, 0);
        g_ctx->OMSetRenderTargets(0, nullptr, s.dsv.Get());
        g_ctx->OMSetDepthStencilState(g_ds_always_write.Get(), 0);
        set_blit(ox, oy, 1.0f / g_scale, 0, 0);
        blit(g_up_z_srv.Get(), g_ps_depth_write.Get(), ox, oy, (float)(w * g_scale), (float)(n * g_scale));
        return;
    }
    std::vector<uint32_t> px((size_t)w * n);
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t raw = read_pixel(s.psm, s.bp, s.bw, x0 + x, r0 + y);
            uint32_t c;
            if (is16(s.psm)) {
                c = ((raw & 0x1F) << 3) | (((raw >> 5) & 0x1F) << 11) | (((raw >> 10) & 0x1F) << 19) | ((raw & 0x8000) ? 0xFF000000u : 0);
            } else {
                const uint32_t a = s.psm == PSMCT24 ? 0x80 : std::min(raw >> 24, 0x80u);
                c = (raw & 0xFFFFFF) | ((a * 255 + 64) / 128) << 24;
            }
            px[(size_t)y * w + x] = c;
        }
    D3D11_BOX box{0, 0, 0, w, n, 1};
    g_ctx->UpdateSubresource(g_up_c.Get(), 0, &box, px.data(), w * 4, 0);
    g_ctx->OMSetRenderTargets(1, s.rtv.GetAddressOf(), nullptr);
    g_ctx->OMSetDepthStencilState(g_ds_off.Get(), 0);
    set_blit(ox, oy, 1.0f / g_scale, 0, 0);
    blit(g_up_c_srv.Get(), g_ps_copy.Get(), ox, oy, (float)(w * g_scale), (float)(n * g_scale));
}

// ---- page hand-over between surfaces ---------------------------------------------------------
// Within a page (8 KB) the layout of a 16/32-bit format does not depend on the buffer width.
// When a surface needs memory another surface of the same format holds (the same base
// pointer at another width, such as a small pass sharing the Z buffer's address), whole
// pages are copied GPU to GPU and lent: the giver keeps its texture but knows those pages
// are elsewhere, and takes them back the same way when it is next used. No read-back.
bool stale_in(const Surface& s, uint32_t r0, uint32_t r1);
bool compatible(uint32_t a, uint32_t b);
const bool g_no_page_copy = std::getenv("TWONK_HW_NOPAGECOPY") != nullptr;
uint64_t g_page_copies = 0;

bool pageable(const Surface& s) {
    const uint32_t p = s.psm;
    return s.bp % 32 == 0 && (p == PSMCT32 || p == PSMCT24 || p == PSMCT16 || p == PSMCT16S || p == PSMZ32 || p == PSMZ24 ||
                              p == PSMZ16 || p == PSMZ16S);
}
// Column and row (in pages) of page `pg` in `s`, if `s` has any of its rows.
bool page_pos(const Surface& s, uint32_t pg, uint32_t* px, uint32_t* py) {
    const uint32_t first = s.bp / 32, ppr = std::max(s.bw, 1u);
    if (!pageable(s) || pg < first) return false;
    *px = (pg - first) % ppr;
    *py = (pg - first) / ppr;
    return (*px + 1) * 64 <= s.w && *py * page_h(s.psm) < s.rows;
}
bool is_lent(const Surface& s, uint32_t pg) { return std::find(s.lent.begin(), s.lent.end(), pg) != s.lent.end(); }
bool lent_in(const Surface& s, uint32_t r0, uint32_t r1) {
    const uint32_t ph = page_h(s.psm);
    for (uint32_t pg : s.lent) {
        uint32_t px, py;
        if (page_pos(s, pg, &px, &py) && py * ph < r1 && r0 < py * ph + ph) return true;
    }
    return false;
}
// Whether `s` has the current content of the whole page.
bool page_live(const Surface& s, uint32_t pg) {
    uint32_t px, py;
    if (!page_pos(s, pg, &px, &py)) return false;
    const uint32_t ph = page_h(s.psm);
    return (py + 1) * ph <= s.rows && !is_lent(s, pg) && !stale_in(s, py * ph, py * ph + ph);
}

void copy_page(Surface& dst, uint32_t dx, uint32_t dy, Surface& src, uint32_t sx, uint32_t sy) {
    ++g_page_copies;
    const uint32_t ph = page_h(dst.psm), S = (uint32_t)g_scale;
    if (tracing()) std::fprintf(stderr, "[hw-trace] page %x (%u,%u) -> %x (%u,%u)\n", src.bp, sx, sy, dst.bp, dx, dy);
    if (!dst.depth) {
        const D3D11_BOX box{sx * 64 * S, sy * ph * S, 0, (sx + 1) * 64 * S, (sy + 1) * ph * S, 1};
        g_ctx->CopySubresourceRegion(dst.tex.Get(), 0, dx * 64 * S, dy * ph * S, 0, src.tex.Get(), 0, &box);
    } else {   // depth buffers can only be copied whole: draw the page as depth
        g_ctx->OMSetRenderTargets(0, nullptr, dst.dsv.Get());
        g_ctx->OMSetDepthStencilState(g_ds_always_write.Get(), 0);
        const float ox = (float)(dx * 64 * S), oy = (float)(dy * ph * S);
        set_blit(ox, oy, 1.0f, (int)(sx * 64 * S), (int)(sy * ph * S));
        blit(src.srv.Get(), g_ps_depth_write.Get(), ox, oy, (float)(64 * S), (float)(ph * S));
    }
    dst.dirty |= src.dirty;
    dst.alpha32 |= src.alpha32;
}

// Copy page `pg` (at column px, row py of `s`) from a surface that has it; that one lends it.
bool take_page(Surface& s, uint32_t pg, uint32_t px, uint32_t py) {
    if (g_no_page_copy || !pageable(s)) return false;
    for (auto& p : g_surf) {
        Surface& h = *p;
        if (&h == &s || h.depth != s.depth || !compatible(h.psm, s.psm) || !page_live(h, pg)) continue;
        uint32_t hx, hy;
        page_pos(h, pg, &hx, &hy);
        copy_page(s, px, py, h, hx, hy);
        h.lent.push_back(pg);
        return true;
    }
    return false;
}

// Rows [r0, r1) of `s` are about to be loaded: take the whole pages there that other
// surfaces have. Returns the pages taken.
std::vector<uint32_t> take_pages(Surface& s, uint32_t r0, uint32_t r1) {
    std::vector<uint32_t> taken;
    if (g_no_page_copy || !pageable(s)) return taken;
    const uint32_t ph = page_h(s.psm), ppr = std::max(s.bw, 1u);
    for (uint32_t py = (r0 + ph - 1) / ph; (py + 1) * ph <= std::min(r1, s.h); ++py)
        for (uint32_t px = 0; px < ppr && (px + 1) * 64 <= s.w; ++px) {
            const uint32_t pg = s.bp / 32 + py * ppr + px;
            if (take_page(s, pg, px, py)) taken.push_back(pg);
        }
    return taken;
}

// refresh() rows [r0, r1) except the pages in `skip`.
void refresh_except(Surface& s, uint32_t r0, uint32_t r1, const std::vector<uint32_t>& skip) {
    if (skip.empty()) { refresh(s, r0, r1); return; }
    const uint32_t ph = page_h(s.psm), ppr = std::max(s.bw, 1u), cols = (s.w + 63) / 64;
    for (uint32_t y = r0; y < r1;) {
        const uint32_t py = y / ph, yb = std::min(r1, (py + 1) * ph);
        for (uint32_t c = 0; c < cols;) {
            auto skipped = [&](uint32_t x) { return std::find(skip.begin(), skip.end(), s.bp / 32 + py * ppr + x) != skip.end(); };
            if (skipped(c)) { ++c; continue; }
            uint32_t e = c;
            while (e < cols && !skipped(e)) ++e;
            refresh(s, y, yb, c * 64, e * 64);
            c = e;
        }
        y = yb;
    }
}

// Surface -> GS memory (rows in use).
const char* g_why = "";
std::mutex g_why_mutex;
std::map<std::string, uint32_t> g_why_count;   // downloads by reason since the last perf line
bool stale_in(const Surface& s, uint32_t r0, uint32_t r1) {
    for (auto& [a, b] : s.stale)
        if (a < r1 && r0 < b) return true;
    return false;
}

// Whether memory [lo, hi) is in rows of `s` that are not stale.
bool holds(const Surface& s, uint32_t lo, uint32_t hi) {
    uint32_t a, b;
    s.range(&a, &b);
    if (!(lo < b && a < hi)) return false;
    const uint32_t ph = page_h(s.psm), ppr = pages_per_row(s.bw, s.psm) * 32;
    const uint32_t r0 = std::max(lo, a) <= s.bp ? 0 : (std::max(lo, a) - s.bp) / ppr * ph;
    const uint32_t r1 = std::min(s.rows, (std::min(hi, b) - s.bp + ppr - 1) / ppr * ph);
    for (uint32_t r = r0; r < r1; r += ph)
        if (!stale_in(s, r, std::min(r + ph, r1))) return true;
    return false;
}

// Surface rows [r0, r1) -> GS memory, without the stale rows. Does not change `dirty`.
void download_rows(Surface& s, uint32_t r0, uint32_t r1) {
    r1 = std::min({r1, s.rows, s.h});
    if (r0 >= r1) return;
    for (auto& [a, b] : s.stale) {   // split around stale rows (there are only ever a few)
        if (!(a < r1 && r0 < b)) continue;
        if (r0 < a) download_rows(s, r0, a);
        if (b < r1) download_rows(s, b, r1);
        return;
    }
    // pages lent to other surfaces: their content is there, not here
    const uint32_t ph = page_h(s.psm), ppr = std::max(s.bw, 1u), cols = (s.w + 63) / 64;
    auto lent_at = [&](uint32_t px, uint32_t py) { return is_lent(s, s.bp / 32 + py * ppr + px); };
    if (!s.lent.empty()) {
        bool all = pageable(s);
        for (uint32_t py = r0 / ph; all && py * ph < r1; ++py)
            for (uint32_t px = 0; all && px < cols; ++px) all = lent_at(px, py);
        if (all) return;
    }
    ++g_downloads;
    if (perf_enabled()) {
        char key[160];
        std::snprintf(key, sizeof key, "%s [surface %x bw %u psm %x rows %u-%u]", g_why, s.bp, s.bw, s.psm, r0, r1);
        std::lock_guard<std::mutex> lock(g_why_mutex);
        ++g_why_count[key];
    }
    {
        static int logged;
        if (logged < 40 && log_enabled(Log::Gpu)) {
            ++logged;
            log(Log::Gpu, "[gs-hw] download surface bp %x bw %u psm %x%s rows %u-%u: %s", s.bp, s.bw, s.psm, s.depth ? " depth" : "", r0, r1, g_why);
        }
        if (tracing()) std::fprintf(stderr, "[hw-trace] download %x rows %u-%u (%s)\n", s.bp, r0, r1, g_why);
    }
    const uint32_t w = s.w, n = r1 - r0;
    const float k = (float)g_scale;
    ID3D11RenderTargetView* rtv = s.depth ? g_down_z_rtv.Get() : g_down_c_rtv.Get();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->OMSetDepthStencilState(g_ds_off.Get(), 0);
    set_blit(0, 0, k, 0, (int)(r0 * g_scale));
    blit(s.srv.Get(), g_ps_copy.Get(), 0, 0, (float)w, (float)n);
    ID3D11Texture2D* stage = s.depth ? g_stage_z.Get() : g_stage_c.Get();
    D3D11_BOX box{0, 0, 0, w, n, 1};
    g_ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, s.depth ? g_down_z.Get() : g_down_c.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) return;
    const uint8_t* base = (const uint8_t*)m.pData;
    const bool any_lent = !s.lent.empty() && pageable(s);
    if (s.depth) {
        const double zmax = zmax_of(s.psm);
        for (uint32_t y = 0; y < n; ++y) {
            const float* row = (const float*)(base + (size_t)y * m.RowPitch);
            for (uint32_t x = 0; x < w; ++x) {
                if (any_lent && lent_at(x / 64, (r0 + y) / ph)) continue;
                write_pixel(s.psm, s.bp, s.bw, x, r0 + y, (uint32_t)std::min(zmax, std::max(0.0, std::floor(row[x] * zmax + 0.5))));
            }
        }
    } else {
        const uint32_t psm = (s.psm == PSMCT32 || s.psm == PSMCT24) ? (s.alpha32 ? PSMCT32 : PSMCT24) : s.psm;
        for (uint32_t y = 0; y < n; ++y) {
            const uint32_t* row = (const uint32_t*)(base + (size_t)y * m.RowPitch);
            for (uint32_t x = 0; x < w; ++x) {
                if (any_lent && lent_at(x / 64, (r0 + y) / ph)) continue;
                const uint32_t c = row[x];
                uint32_t v;
                if (is16(psm)) {
                    v = ((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10) | ((c >> 31) << 15);
                } else {
                    const uint32_t a = ((c >> 24) * 128 + 127) / 255;
                    v = (c & 0xFFFFFF) | (a << 24);
                }
                write_pixel(psm, s.bp, s.bw, x, r0 + y, v);
            }
        }
    }
    g_ctx->Unmap(stage, 0);
    uint32_t lo, hi;
    rect_blocks(s.bp, s.bw, s.psm, r0, r1, &lo, &hi);
    bump(lo, hi);   // textures decoded from this memory are stale (the surface itself is current)
}

// Surface -> GS memory (rows in use).
void download(Surface& s) {
    if (s.dirty && s.rows) download_rows(s, 0, s.rows);
    s.dirty = false;
}

void drop(Surface* s) {
    for (size_t i = 0; i < g_surf.size(); ++i)
        if (g_surf[i].get() == s) { g_surf.erase(g_surf.begin() + (long long)i); return; }
}

// Memory [lo, hi) now belongs to `keep`. Other surfaces there are written back; a surface
// wholly inside is dropped, one that only partly overlaps keeps its other rows and marks
// these stale. (A small render target and a full-screen one often share a Z base pointer:
// handing over only the shared page row avoids moving the whole buffer every frame.)
void evict(uint32_t lo, uint32_t hi, const Surface* keep) {
    for (size_t i = 0; i < g_surf.size();) {
        Surface& s = *g_surf[i];
        uint32_t a, b;
        s.range(&a, &b);
        if (&s == keep || !s.rows || !(lo < b && a < hi)) { ++i; continue; }
        if (!s.lent.empty()) {   // nothing to hand over if every page there is lent already
            bool all = true;
            for (uint32_t pg = std::max(lo, a) / 32; all && pg <= (std::min(hi, b) - 1) / 32; ++pg) {
                uint32_t px, py;
                if (page_pos(s, pg, &px, &py) && !is_lent(s, pg)) all = false;
            }
            if (all && pageable(s)) { ++i; continue; }
        }
        static char why[96];
        std::snprintf(why, sizeof why, "evicted by %x bw %u psm %x%s", keep->bp, keep->bw, keep->psm, keep->depth ? " depth" : "");
        g_why = why;
        const uint32_t ph = page_h(s.psm), ppr = pages_per_row(s.bw, s.psm) * 32;
        const uint32_t r0 = std::max(lo, a) <= s.bp ? 0 : (std::max(lo, a) - s.bp) / ppr * ph;
        const uint32_t r1 = std::min(s.rows, (std::min(hi, b) - s.bp + ppr - 1) / ppr * ph);
        // A surface not used lately (such as the whole-memory clear at boot) is not kept in
        // pieces: it would only be refreshed by every upload into its memory.
        bool whole = (r0 == 0 && r1 >= s.rows) || g_presents - s.last_use > 2;
        if (!whole) {   // whole anyway if the rest is already stale
            whole = true;
            for (uint32_t r = 0; r < s.rows && whole; r += ph)
                if ((r < r0 || r >= r1) && !stale_in(s, r, std::min(r + ph, s.rows))) whole = false;
        }
        if (whole) {
            download(s);
            g_surf.erase(g_surf.begin() + (long long)i);
            continue;
        }
        if (s.dirty) download_rows(s, r0, r1);
        bool known = false;
        for (auto& [x0, x1] : s.stale) known |= x0 <= r0 && r1 <= x1;
        if (!known) s.stale.push_back({r0, r1});
        ++i;
    }
}

// Reload a surface's stale rows from GS memory, after whoever holds that memory writes it back.
void validate(Surface& s) {
    if (!s.lent.empty()) {   // take lent pages back: from the surface that has them, else from GS memory
        std::vector<uint32_t> pages;
        pages.swap(s.lent);
        const uint32_t ph = page_h(s.psm);
        for (uint32_t pg : pages) {
            uint32_t px, py;
            if (!page_pos(s, pg, &px, &py)) continue;
            if (take_page(s, pg, px, py)) continue;
            evict(pg * 32, pg * 32 + 32, &s);
            refresh(s, py * ph, std::min(py * ph + ph, s.rows), px * 64, px * 64 + 64);
        }
    }
    if (s.stale.empty()) return;
    std::vector<std::pair<uint32_t, uint32_t>> todo;
    todo.swap(s.stale);
    for (auto& [r0, r1] : todo) {
        uint32_t lo, hi;
        rect_blocks(s.bp, s.bw, s.psm, r0, r1, &lo, &hi);
        const std::vector<uint32_t> taken = take_pages(s, r0, r1);
        evict(lo, hi, &s);
        refresh_except(s, r0, r1, taken);
    }
}

// The surface takes rows [s.rows, rows): they come from GS memory.
bool grow(Surface& s, uint32_t rows) {
    rows = std::min(rows, 2048u);
    if (rows <= s.rows) return true;
    if (rows > s.h) {   // reallocate taller, keeping what is drawn
        const uint32_t nh = std::min(2048u, std::max(rows + 127 & ~127u, s.h * 3 / 2));
        ComPtr<ID3D11Texture2D> old = s.tex;
        const uint32_t oh = s.h;
        if (!alloc_surface(s, nh, "grow")) return false;
        if (old) {
            D3D11_BOX box{0, 0, 0, s.w * g_scale, oh * g_scale, 1};
            g_ctx->CopySubresourceRegion(s.tex.Get(), 0, 0, 0, 0, old.Get(), 0, &box);
        }
    }
    uint32_t lo, hi;
    rect_blocks(s.bp, s.bw, s.psm, s.rows, rows, &lo, &hi);
    const std::vector<uint32_t> taken = take_pages(s, s.rows, rows);
    evict(lo, hi, &s);
    refresh_except(s, s.rows, rows, taken);
    s.rows = rows;
    return true;
}

bool compatible(uint32_t a, uint32_t b) {
    auto cls = [](uint32_t p) { return (p == PSMCT32 || p == PSMCT24) ? 0x100u : p; };
    return cls(a) == cls(b);
}

Surface* get_surface(uint32_t bp, uint32_t bw, uint32_t psm, bool depth, uint32_t rows) {
    bw = std::max(bw, 1u);
    for (auto& p : g_surf) {
        Surface& s = *p;
        if (s.bp != bp || s.depth != depth || s.bw != bw || !compatible(s.psm, psm)) continue;
        if (!depth && psm != s.psm) s.psm = psm == PSMCT32 ? PSMCT32 : s.psm;
        validate(s);
        s.last_use = g_presents;
        if (!grow(s, rows)) return nullptr;
        return &s;
    }
    // The memory at bp is used another way now (another width or format): a new surface takes
    // the rows it needs from whatever holds them (grow -> evict).
    auto s = std::make_unique<Surface>();
    s->bp = bp; s->bw = bw; s->psm = psm; s->depth = depth; s->last_use = g_presents;
    s->w = std::min(2048u, bw * 64);
    if (!alloc_surface(*s, std::min(2048u, std::max(256u, rows + 127 & ~127u)))) return nullptr;
    Surface* r = s.get();
    g_surf.push_back(std::move(s));
    if (!grow(*r, rows)) return nullptr;
    return r;
}

// pending CPU writes: refresh surfaces before they are next used
struct Range { uint32_t lo, hi; };
std::vector<Range> g_pending;

void apply_pending() {
    if (g_pending.empty()) return;
    std::vector<Range> todo;
    todo.swap(g_pending);
    std::vector<std::pair<uint32_t, uint32_t>> rows;
    for (auto& p : g_surf) {   // per surface: the union of the rows the writes touched
        Surface& s = *p;
        uint32_t a, b;
        s.range(&a, &b);
        rows.clear();
        const uint32_t ph = page_h(s.psm), ppr = pages_per_row(s.bw, s.psm) * 32;
        for (const Range& r : todo) {
            if (!(r.lo < b && a < r.hi)) continue;
            const uint32_t r0 = r.lo <= s.bp ? 0 : (r.lo - s.bp) / ppr * ph;
            const uint32_t r1 = std::min(s.rows, (r.hi - s.bp + ppr - 1) / ppr * ph);
            if (r0 < r1) rows.push_back({r0, r1});
        }
        std::sort(rows.begin(), rows.end());
        for (size_t i = 0; i < rows.size();) {
            uint32_t r0 = rows[i].first, r1 = rows[i].second;
            for (++i; i < rows.size() && rows[i].first <= r1; ++i) r1 = std::max(r1, rows[i].second);
            refresh(s, r0, r1);
        }
    }
}

// ---- texture cache ---------------------------------------------------------------------------
struct TexKey {
    uint32_t bp, bw, psm, tw, th;
    uint64_t texa, clut;
    bool operator==(const TexKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
struct TexKeyHash {
    size_t operator()(const TexKey& k) const {
        uint64_t h = 1469598103934665603ull;
        const uint8_t* p = (const uint8_t*)&k;
        for (size_t i = 0; i < sizeof k; ++i) h = (h ^ p[i]) * 1099511628211ull;
        return (size_t)h;
    }
};
struct TexEntry {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t lo, hi;
    uint64_t gen, used;
    uint32_t scale = 1;        // texels per GS texel (a replacement texture)
    uint64_t pack_hash = 0;    // replacement still loading: its hash
    uint8_t glyphs = 0;        // glyph_mode
};
uint8_t g_tex_glyphs = 0;      // glyph_mode of the texture decoded_texture last returned

// Is this a sheet of text glyphs? Grey texels only, mostly clear, and crisp alpha
// edges (few partly clear texels per opaque one: soft glows and particles have
// many). 2: the opaque part is two-tone white/black (outlined letters), so the
// boundary inside is sharpened too; 1: alpha only.
uint8_t glyph_mode(const std::vector<uint32_t>& px) {
    size_t clear = 0, full = 0, mid = 0, two = 0;
    for (const uint32_t c : px) {
        const uint32_t a = c >> 24, r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
        if (a <= 4) { ++clear; continue; }
        if (std::max({r, g, b}) - std::min({r, g, b}) > 3) return 0;
        if (a >= 120) { ++full; two += r < 40 || r > 215; }
        else ++mid;
    }
    if (clear * 10 < px.size() * 3 || full < 64 || mid * 10 > full * 12) return 0;
    return two * 100 >= full * 85 ? 2 : 1;
}
std::unordered_map<TexKey, TexEntry, TexKeyHash> g_tex;
bool indexed(uint32_t psm) { return psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH; }

uint32_t rgba_of(uint32_t raw, bool sixteen, uint64_t texa) {   // to RGBA8, alpha raw (0x80 = 1.0)
    if (sixteen) {
        const uint32_t rgb = ((raw & 0x1F) << 3) | (((raw >> 5) & 0x1F) << 11) | (((raw >> 10) & 0x1F) << 19);
        const uint32_t a = (raw & 0x8000) ? bf(texa, 32, 8) : ((bf(texa, 15, 1) && !(raw & 0x7FFF)) ? 0 : bf(texa, 0, 8));
        return rgb | (a << 24);
    }
    return raw;
}

// ---- texture packs (texpack.cpp) -------------------------------------------------------------
struct RepTex { ComPtr<ID3D11ShaderResourceView> srv; uint32_t scale; size_t bytes; uint64_t used; };
std::unordered_map<uint64_t, RepTex> g_rep;   // replacements on the GPU, by hash
size_t g_rep_bytes = 0;
constexpr size_t kRepBudget = (size_t)1 << 30;
uint64_t g_tick = 0;

// The replacement for decoded pixels with hash h (tw x th), once it is on the GPU.
ID3D11ShaderResourceView* replacement(uint64_t h, uint32_t tw, uint32_t th, uint32_t* scale, bool* pending) {
    *pending = false;
    if (auto it = g_rep.find(h); it != g_rep.end()) {
        it->second.used = ++g_tick;
        *scale = it->second.scale;
        return it->second.srv.Get();
    }
    auto img = texpack::find(h, pending);
    if (!img) return nullptr;
    texpack::forget(h);   // the CPU copy is not needed once uploaded
    const uint32_t k = img->w / tw;
    if (k < 1 || img->w != tw * k || img->h != th * k) {
        static int warned;
        if (warned++ < 10) std::fprintf(stderr, "[texpack] %016llx: %ux%u is not a whole multiple of %ux%u\n", (unsigned long long)h, img->w, img->h, tw, th);
        return nullptr;
    }
    RepTex r;
    ComPtr<ID3D11Texture2D> tex;
    g_make_site = "replacement texture";
    if (!make_tex(img->w, img->h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_IMMUTABLE, 0, &tex, img->px.data(), img->w * 4))
        return nullptr;
    g_dev->CreateShaderResourceView(tex.Get(), nullptr, &r.srv);
    r.scale = k; r.bytes = (size_t)img->w * img->h * 4; r.used = ++g_tick;
    g_rep_bytes += r.bytes;
    while (g_rep_bytes > kRepBudget && !g_rep.empty()) {   // drop the least recently used
        auto old = g_rep.begin();
        for (auto i = g_rep.begin(); i != g_rep.end(); ++i) if (i->second.used < old->second.used) old = i;
        g_rep_bytes -= old->second.bytes;
        g_rep.erase(old);
    }
    *scale = k;
    return (g_rep[h] = std::move(r)).srv.Get();
}

// `pack`: the texture may be dumped or replaced (not when its memory holds a render target).
ID3D11ShaderResourceView* decoded_texture(const Texture& t, bool pack, uint32_t* scale) {
    *scale = 1;
    TexKey k{};
    k.bp = t.bp; k.bw = t.bw; k.psm = t.psm; k.tw = t.tw; k.th = t.th; k.texa = t.texa & 0xFF000080FFull;
    uint32_t clut_psm = 0;
    const uint32_t* clut = nullptr;
    if (indexed(t.psm)) {
        clut = gs_clut(t.clut, &clut_psm);
        const uint32_t n = (t.psm == PSMT8 || t.psm == PSMT8H) ? 256 : 16, mask = clut_psm == PSMCT32 ? 255 : 511;
        uint64_t h = 1469598103934665603ull ^ clut_psm;
        for (uint32_t i = 0; i < n; ++i) h = (h ^ clut[(t.clut_off + i) & mask]) * 1099511628211ull;
        k.clut = h;
    }
    uint32_t lo, hi;
    rect_blocks(t.bp, t.bw, t.psm, 0, t.th, &lo, &hi);
    const uint64_t gen = gen_sum(lo, hi);
    auto it = g_tex.find(k);
    if (it != g_tex.end() && it->second.gen == gen) {
        TexEntry& e = it->second;
        e.used = ++g_tick;
        if (e.pack_hash) {   // its replacement was loading: is it there now?
            bool pending;
            uint32_t rk;
            if (ID3D11ShaderResourceView* r = replacement(e.pack_hash, t.tw, t.th, &rk, &pending)) {
                e.srv = r; e.scale = rk; e.pack_hash = 0;
            } else if (!pending) {
                e.pack_hash = 0;
            }
        }
        *scale = e.scale;
        g_tex_glyphs = e.glyphs;
        return e.srv.Get();
    }
    ++g_tex_decodes;
    const uint32_t tw = t.tw, th = t.th;
    std::vector<uint32_t> px((size_t)tw * th);
    const bool sixteen = is16(t.psm);
    for (uint32_t y = 0; y < th; ++y)
        for (uint32_t x = 0; x < tw; ++x) {
            uint32_t raw = read_pixel(t.psm, t.bp, t.bw, x, y), c;
            if (clut) {
                const uint32_t e = clut[(t.clut_off + raw) & (clut_psm == PSMCT32 ? 255 : 511)];
                c = clut_psm == PSMCT32 ? e : rgba_of(e & 0xFFFF, true, t.texa);
            } else if (t.psm == PSMCT24 || t.psm == PSMZ24) {
                raw &= 0xFFFFFF;
                c = raw | ((bf(t.texa, 15, 1) && !raw) ? 0u : bf(t.texa, 0, 8) << 24);
            } else {
                c = rgba_of(raw, sixteen, t.texa);
            }
            px[(size_t)y * tw + x] = c;
        }
    TexEntry e;
    e.glyphs = glyph_mode(px);
    if (texpack::dumping()) {   // how many decodes could be dumped
        static uint64_t n_pack, n_rt;
        ++(pack ? n_pack : n_rt);
        if ((n_pack + n_rt) % 2000 == 0)
            std::fprintf(stderr, "[texpack] %llu decodes: %llu dumpable, %llu in render-target memory\n",
                         (unsigned long long)(n_pack + n_rt), (unsigned long long)n_pack, (unsigned long long)n_rt);
    }
    if (pack && (texpack::dumping() || texpack::replacing())) {
        const uint64_t h = texpack::hash(px.data(), tw, th);
        if (texpack::dumping()) {
            char info[96];
            std::snprintf(info, sizeof info, "psm %x bp %x bw %u present %llu", t.psm, t.bp, t.bw, (unsigned long long)g_presents);
            texpack::dump(h, px.data(), tw, th, info);
        }
        bool pending = false;
        if (ID3D11ShaderResourceView* r = replacement(h, tw, th, &e.scale, &pending)) e.srv = r;
        else if (pending) e.pack_hash = h;
    }
    if (!e.srv) {
        g_make_site = "decoded texture";
        if (!make_tex(tw, th, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_IMMUTABLE, 0, &e.tex, px.data(), tw * 4))
            return nullptr;
        g_dev->CreateShaderResourceView(e.tex.Get(), nullptr, &e.srv);
    }
    e.lo = lo; e.hi = hi; e.gen = gen; e.used = ++g_tick;
    if (g_tex.size() > 768) {   // drop the least recently used half
        std::vector<uint64_t> ages;
        for (auto& [kk, v] : g_tex) ages.push_back(v.used);
        std::nth_element(ages.begin(), ages.begin() + ages.size() / 2, ages.end());
        const uint64_t cut = ages[ages.size() / 2];
        for (auto i = g_tex.begin(); i != g_tex.end();) i = i->second.used < cut ? g_tex.erase(i) : std::next(i);
    }
    auto& slot = g_tex[k];
    slot = std::move(e);
    *scale = slot.scale;
    g_tex_glyphs = slot.glyphs;
    return slot.srv.Get();
}

// ---- state objects ---------------------------------------------------------------------------
std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> g_blend_states;
std::unordered_map<uint32_t, ComPtr<ID3D11DepthStencilState>> g_depth_states;

ID3D11BlendState* blend_state(bool enable, D3D11_BLEND_OP op, D3D11_BLEND sf, D3D11_BLEND df, UINT8 mask) {
    const uint64_t key = (uint64_t)enable | (uint64_t)op << 1 | (uint64_t)sf << 8 | (uint64_t)df << 16 | (uint64_t)mask << 24;
    auto& b = g_blend_states[key];
    if (!b) {
        D3D11_BLEND_DESC d{};
        auto& rt = d.RenderTarget[0];
        rt.BlendEnable = enable;
        rt.BlendOp = op; rt.SrcBlend = sf; rt.DestBlend = df;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD; rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.RenderTargetWriteMask = mask;
        g_dev->CreateBlendState(&d, &b);
    }
    return b.Get();
}

ID3D11DepthStencilState* depth_state(bool test, uint32_t ztst, bool write) {
    const uint32_t key = (uint32_t)test | ztst << 1 | (uint32_t)write << 3;
    auto& s = g_depth_states[key];
    if (!s) {
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = test;
        d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        static const D3D11_COMPARISON_FUNC f[4] = {D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS, D3D11_COMPARISON_GREATER_EQUAL,
                                                   D3D11_COMPARISON_GREATER};
        d.DepthFunc = f[ztst & 3];
        g_dev->CreateDepthStencilState(&d, &s);
    }
    return s.Get();
}

// GS blend (A - B) * C + D as a D3D blend: op, factors, and how the shader shapes its colour.
struct BlendCfg { D3D11_BLEND_OP op; D3D11_BLEND sf, df; uint32_t outmode; };
BlendCfg blend_cfg(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    enum { Cs = 0, Cd = 1, Z = 2 };
    a = std::min(a, 2u); b = std::min(b, 2u); d = std::min(d, 2u);
    const D3D11_BLEND C = c == 0 ? D3D11_BLEND_SRC1_ALPHA : c == 1 ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_BLEND_FACTOR;
    const D3D11_BLEND IC = c == 0 ? D3D11_BLEND_INV_SRC1_ALPHA : c == 1 ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_BLEND_FACTOR;
    const auto ADD = D3D11_BLEND_OP_ADD, SUB = D3D11_BLEND_OP_SUBTRACT, RSUB = D3D11_BLEND_OP_REV_SUBTRACT;
    const auto ONE = D3D11_BLEND_ONE, ZERO = D3D11_BLEND_ZERO;
    if (a == b) return {ADD, d == Cs ? ONE : ZERO, d == Cd ? ONE : ZERO, 0};
    if (a == Cs && b == Cd) {
        if (d == Cs) return {SUB, ONE, C, 1};
        if (d == Cd) return {ADD, C, IC, 0};
        return {SUB, C, C, 0};
    }
    if (a == Cd && b == Cs) {
        if (d == Cs) return {ADD, IC, C, 0};
        if (d == Cd) return {RSUB, C, ONE, 0};   // approximation: Cd * (1 + C) - Cs * C without the Cd * C term
        return {RSUB, C, C, 0};
    }
    if (a == Cs && b == Z) {
        if (d == Cs) return {ADD, ONE, ZERO, 1};
        if (d == Cd) return {ADD, C, ONE, 0};
        return {ADD, C, ZERO, 0};
    }
    if (a == Cd && b == Z) {
        if (d == Cs) return {ADD, ONE, C, 0};
        if (d == Cd) return c == 1 ? BlendCfg{ADD, ZERO, ONE, 0} : BlendCfg{ADD, D3D11_BLEND_DEST_COLOR, ONE, 2};   // Cd + Cd * C
        return {ADD, ZERO, C, 0};
    }
    if (a == Z && b == Cs) {
        if (d == Cs) return {ADD, IC, ZERO, 0};
        if (d == Cd) return {RSUB, C, ONE, 0};
        return {ADD, ZERO, ZERO, 0};
    }
    // a == Z && b == Cd
    if (d == Cs) return {SUB, ONE, C, 0};
    if (d == Cd) return {ADD, ZERO, IC, 0};
    return {ADD, ZERO, ZERO, 0};
}

// ---- batching --------------------------------------------------------------------------------
struct Batch {
    bool valid = false;
    DrawState st;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    std::vector<HwVert> v;
    float maxy = 0;
} B;

HwVert hv(const Vertex& p, const Vertex& col, float q_override = 0) {
    HwVert o;
    o.x = p.x; o.y = p.y; o.z = p.z;
    o.r = col.r; o.g = col.g; o.b = col.b; o.a = col.a;
    o.s = p.s; o.t = p.t; o.q = q_override != 0 ? q_override : p.q;
    o.u = p.u; o.v = p.v; o.f = p.f;
    return o;
}

void bind_texture(const DrawState& D, Surface* rt, DrawCB& cb, ComPtr<ID3D11Texture2D>* feedback,
                  ComPtr<ID3D11ShaderResourceView>* feedback_srv, ID3D11ShaderResourceView** out) {
    const Texture& t = D.tex;
    uint32_t lo, hi;
    rect_blocks(t.bp, t.bw, t.psm, 0, t.th, &lo, &hi);
    cb.tscale = 1;
    cb.tsrc = 0;
    // texels the draw can reach (region modes can reach past tw/th)
    auto reach = [](uint32_t mode, uint32_t size, uint32_t mn, uint32_t mx) {
        return mode == 2 ? std::max(size, mx + 1) : mode == 3 ? std::max(size, (mn | mx) + 1) : size;
    };
    const uint32_t fw = reach(t.wms, t.tw, t.minu, t.maxu), fh = reach(t.wmt, t.th, t.minv, t.maxv);
    // a texture that is exactly a colour surface: sample it on the GPU. Within one row of
    // pages the layout does not depend on the buffer width, so a texture that fits there
    // (such as a 1x1 texture at the frame buffer's base) matches whatever width the surface has.
    const bool tex_color = t.psm == PSMCT32 || t.psm == PSMCT24 || t.psm == PSMCT16 || t.psm == PSMCT16S;
    if (tex_color)
        for (auto& p : g_surf) {
            Surface& s = *p;
            if (s.depth || s.bp != t.bp || !s.rows || !compatible(s.psm, t.psm)) continue;
            const uint32_t tbw = std::max(t.bw, 1u);
            if (s.bw != tbw && !(fh <= page_h(s.psm) && fw <= 64 * std::min(s.bw, tbw))) continue;
            if (stale_in(s, 0, fh) || lent_in(s, 0, fh)) continue;   // that memory belongs to another surface now
            s.last_use = g_presents;
            cb.tsrc = t.psm == PSMCT32 ? 1 : t.psm == PSMCT24 ? 2 : 3;
            cb.tscale = (float)g_scale;
            if (&s == rt) {   // rendering into the texture being sampled: sample a copy of what it reads
                D3D11_TEXTURE2D_DESC d;
                s.tex->GetDesc(&d);
                D3D11_TEXTURE2D_DESC fd{};
                if (s.fb_tex) s.fb_tex->GetDesc(&fd);
                if (!s.fb_tex || fd.Width != d.Width || fd.Height != d.Height) {   // made once per surface size
                    if (++s.fb_made == 8)
                        std::fprintf(stderr, "[gs-hw] surface %x: feedback copy made 8 times (had %ux%u, need %ux%u, %s)\n", s.bp,
                                     fd.Width, fd.Height, d.Width, d.Height, s.fb_tex ? "present" : "missing");
                    g_make_site = "feedback copy";
                    if (!make_tex(d.Width, d.Height, d.Format, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, &s.fb_tex)) {
                        s.fb_srv.Reset();
                        *out = nullptr;
                        return;
                    }
                    g_dev->CreateShaderResourceView(s.fb_tex.Get(), nullptr, s.fb_srv.ReleaseAndGetAddressOf());
                }
                (void)feedback; (void)feedback_srv;
                // Only the texels this batch can read (glow effects sample the frame in many small
                // pieces): the box of its texture coordinates, plus 2 texels for filtering.
                uint32_t x0 = 0, y0 = 0, x1 = fw, y1 = fh;
                float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
                bool ok = !B.v.empty();
                for (const HwVert& hv : B.v) {
                    float u = hv.u, v = hv.v;
                    if (!D.fst) {
                        const float q = hv.q != 0 ? hv.q : 1.0f;
                        if (q < 0) { ok = false; break; }
                        u = hv.s / q * t.tw;
                        v = hv.t / q * t.th;
                    }
                    umin = std::min(umin, u); umax = std::max(umax, u);
                    vmin = std::min(vmin, v); vmax = std::max(vmax, v);
                }
                auto span = [&](uint32_t mode, uint32_t size, float lo_, float hi_, uint32_t* a, uint32_t* b) {
                    const float lo2 = std::floor(lo_) - 2, hi2 = std::ceil(hi_) + 2;
                    if (mode == 0 && (lo2 < 0 || hi2 > (float)size)) return;   // wraps around: all of it
                    if (mode >= 2 || !size) return;                             // region modes: all of it
                    // clamp mode reads the edge texel for coordinates past it
                    const float last = (float)(std::min(size, *b) - 1);
                    *a = (uint32_t)std::clamp(lo2, 0.0f, last);
                    *b = (uint32_t)std::clamp(hi2, (float)*a + 1, last + 1);
                };
                if (ok) {
                    span(t.wms, t.tw, umin, umax, &x0, &x1);
                    span(t.wmt, t.th, vmin, vmax, &y0, &y1);
                }
                const D3D11_BOX box{std::min(d.Width, x0 * g_scale), std::min(d.Height, y0 * g_scale), 0,
                                    std::min(d.Width, x1 * g_scale), std::min(d.Height, y1 * g_scale), 1};
                if (box.right > box.left && box.bottom > box.top)
                    g_ctx->CopySubresourceRegion(s.fb_tex.Get(), 0, box.left, box.top, 0, s.tex.Get(), 0, &box);
                ++g_fb_copies;
                g_fb_copy_px += (uint64_t)(box.right - box.left) * (box.bottom - box.top);
                *out = s.fb_srv.Get();
            } else {
                *out = s.srv.Get();
            }
            return;
        }
    // otherwise decode from GS memory, after writing back what the GPU drew there
    bool pack = true;   // memory no render target holds: an ordinary texture
    for (auto& p : g_surf) {
        Surface& s = *p;
        uint32_t a, b;
        s.range(&a, &b);
        if (lo < b && a < hi && holds(s, lo, hi)) pack = false;
        if (s.dirty && lo < b && a < hi) {
            static int logged;
            if (logged < 30 && log_enabled(Log::Gpu)) {
                ++logged;
                log(Log::Gpu, "[gs-hw] texture bp %x bw %u psm %x %ux%u reads surface bp %x bw %u psm %x%s rows %u (fb %x): download",
                    t.bp, t.bw, t.psm, t.tw, t.th, s.bp, s.bw, s.psm, s.depth ? " (depth)" : "", s.rows, D.fbp);
            }
            static char why[96];
            std::snprintf(why, sizeof why, "texture bp %x bw %u psm %x %ux%u", t.bp, t.bw, t.psm, t.tw, t.th);
            g_why = why;
            download(s);
        }
    }
    uint32_t k = 1;
    g_tex_glyphs = 0;
    *out = decoded_texture(t, pack, &k);
    cb.tscale = (float)k;
    cb.sharp = (g_scale > 1 && video_sharp_text()) ? g_tex_glyphs : 0;
}

ComPtr<ID3D11Texture2D> g_feedback;
ComPtr<ID3D11ShaderResourceView> g_feedback_srv;

void submit() {
    if (B.v.empty()) return;
    const DrawState& D = B.st;
    if (tracing())
        std::fprintf(stderr, "[hw-trace] draw %zu verts fb %x bw %u psm %x msk %x z %x(%d) tex %d bp %x bw %u psm %x %ux%u tfx %u abe %d %u%u%u%u scissor %d,%d-%d,%d\n",
                     B.v.size(), D.fbp, D.fbw, D.fpsm, D.fbmsk, D.zbp, (int)D.zte, (int)D.tme, D.tex.bp, D.tex.bw, D.tex.psm, D.tex.tw,
                     D.tex.th, D.tex.tfx, (int)D.abe, D.a_a, D.a_b, D.a_c, D.a_d, D.scax0, D.scay0, D.scax1, D.scay1);
    apply_pending();
    ++g_draws;
    const uint32_t fpsm = color_psm(D.fpsm);
    const bool fb16 = is16(fpsm);
    // colour write mask from FBMSK (partial channel masks are not supported)
    UINT8 mask = 0;
    const uint32_t m = D.fbmsk;
    if (fb16) {
        if (((m >> 3) & 0x1F) != 0x1F) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
        if (((m >> 11) & 0x1F) != 0x1F) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
        if (((m >> 19) & 0x1F) != 0x1F) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (!(m >> 31)) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    } else {
        if ((m & 0xFF) != 0xFF) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
        if (((m >> 8) & 0xFF) != 0xFF) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
        if (((m >> 16) & 0xFF) != 0xFF) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
        if ((m >> 24) != 0xFF && fpsm != PSMCT24) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    }
    const bool zwrite = D.zte && !D.zmsk;
    const uint32_t rows = (uint32_t)std::clamp((int)std::ceil(B.maxy), 1, std::min(2048, D.scay1 + 1));
    Surface* rt = get_surface(D.fbp, D.fbw, fpsm, false, rows);
    if (!rt) { B.v.clear(); return; }
    if (fpsm == PSMCT32 && (mask & D3D11_COLOR_WRITE_ENABLE_ALPHA)) rt->alpha32 = true;
    Surface* ds = nullptr;
    if (D.zte) {
        ds = get_surface(D.zbp, D.fbw, D.zpsm, true, rows);
        // taking the Z memory can hand over memory the frame buffer held
        if (std::none_of(g_surf.begin(), g_surf.end(), [&](auto& p) { return p.get() == rt; })) {
            rt = get_surface(D.fbp, D.fbw, fpsm, false, rows);
            if (!rt) { B.v.clear(); return; }
        }
        // the views must be the same size: the shorter one grows
        if (ds && ds->h != rt->h) {
            Surface& shorter = ds->h < rt->h ? *ds : *rt;
            const uint32_t want = std::max(ds->h, rt->h);
            ComPtr<ID3D11Texture2D> old = shorter.tex;
            const uint32_t oh = shorter.h;
            alloc_surface(shorter, want, "match Z height");
            D3D11_BOX box{0, 0, 0, shorter.w * g_scale, oh * g_scale, 1};
            g_ctx->CopySubresourceRegion(shorter.tex.Get(), 0, 0, 0, 0, old.Get(), 0, &box);
        }
        if (ds && ds->w != rt->w) ds = nullptr;   // a width the GS allows but D3D does not
    }
    DrawCB cb{};
    cb.ndc[0] = 2.0f / rt->w; cb.ndc[1] = 2.0f / rt->h;
    cb.zscale = (float)(1.0 / zmax_of(D.zpsm));
    cb.fogcol[0] = (float)D.fogcol.r; cb.fogcol[1] = (float)D.fogcol.g; cb.fogcol[2] = (float)D.fogcol.b;
    cb.tme = D.tme; cb.fst = D.fst; cb.tfx = D.tex.tfx; cb.tcc = D.tex.tcc;
    cb.tw = D.tex.tw; cb.th = D.tex.th; cb.wms = D.tex.wms; cb.wmt = D.tex.wmt;
    cb.minu = D.tex.minu; cb.maxu = D.tex.maxu; cb.minv = D.tex.minv; cb.maxv = D.tex.maxv;
    cb.bilinear = D.tex.bilinear; cb.aem = bf(D.tex.texa, 15, 1); cb.fge = D.fge;
    cb.ta0 = (float)bf(D.tex.texa, 0, 8); cb.ta1 = (float)bf(D.tex.texa, 32, 8);
    cb.aref = (float)D.aref; cb.fix = (float)D.a_fix; cb.atst = D.atst; cb.fba = D.fba;
    cb.cfac = std::min(D.a_c, 2u);
    ID3D11ShaderResourceView* srv = nullptr;
    if (D.tme) bind_texture(D, rt, cb, &g_feedback, &g_feedback_srv, &srv);
    // blending
    BlendCfg bc{D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, 0};
    if (D.abe) {
        bc = blend_cfg(D.a_a, D.a_b, D.a_c, D.a_d);
        cb.outmode = bc.outmode;
        cb.pabe = D.pabe && D.a_a == 0 && D.a_b == 1 && D.a_c == 0 && D.a_d == 1;   // the one form where C = 1 means "no blend"
    }
    const float bfac = D.a_fix / 128.0f;
    const float blend_factor[4] = {bfac, bfac, bfac, bfac};
    // vertices
    const UINT bytes = (UINT)(B.v.size() * sizeof(HwVert));
    if (bytes > kVbBytes) { B.v.clear(); return; }
    D3D11_MAPPED_SUBRESOURCE mv;
    const bool wrap = g_vb_pos + bytes > kVbBytes;
    if (wrap) g_vb_pos = 0;
    g_ctx->Map(g_vb.Get(), 0, wrap || g_vb_pos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mv);
    std::memcpy((uint8_t*)mv.pData + g_vb_pos, B.v.data(), bytes);
    g_ctx->Unmap(g_vb.Get(), 0);
    const UINT stride = sizeof(HwVert), first = g_vb_pos / sizeof(HwVert);
    const UINT zero = 0;
    g_vb_pos += bytes;
    g_vb_pos = (g_vb_pos + sizeof(HwVert) - 1) / sizeof(HwVert) * sizeof(HwVert);
    // pipeline
    g_ctx->IASetInputLayout(g_layout.Get());
    g_ctx->IASetVertexBuffers(0, 1, g_vb.GetAddressOf(), &stride, &zero);
    g_ctx->IASetPrimitiveTopology(B.topo);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, g_cb_draw.GetAddressOf());
    g_ctx->PSSetConstantBuffers(0, 1, g_cb_draw.GetAddressOf());
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->RSSetState(g_rs.Get());
    D3D11_VIEWPORT vp{0, 0, (float)(rt->w * g_scale), (float)(rt->h * g_scale), 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{D.scax0 * g_scale, D.scay0 * g_scale, (D.scax1 + 1) * g_scale, (D.scay1 + 1) * g_scale};
    g_ctx->RSSetScissorRects(1, &sc);
    g_ctx->OMSetRenderTargets(1, rt->rtv.GetAddressOf(), ds ? ds->dsv.Get() : nullptr);
    // alpha test: KEEP discards; the other failure modes draw the failing pixels in a second pass
    struct Pass { uint32_t amode; UINT8 mask; bool zw; };
    Pass passes[2];
    int np = 0;
    const bool test = D.ate && D.atst != 1;
    if (!test) passes[np++] = {0, mask, zwrite};
    else {
        passes[np++] = {1, mask, zwrite};
        switch (D.afail) {
        case 1: passes[np++] = {2, mask, false}; break;   // FB_ONLY
        case 2: passes[np++] = {2, 0, zwrite}; break;      // ZB_ONLY
        case 3: passes[np++] = {2, (UINT8)(mask & 7), false}; break;   // RGB_ONLY
        default: break;
        }
    }
    for (int i = 0; i < np; ++i) {
        const Pass& p = passes[i];
        if (!p.mask && !(p.zw && ds)) continue;
        cb.amode = p.amode;
        D3D11_MAPPED_SUBRESOURCE mc;
        g_ctx->Map(g_cb_draw.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mc);
        std::memcpy(mc.pData, &cb, sizeof cb);
        g_ctx->Unmap(g_cb_draw.Get(), 0);
        g_ctx->OMSetBlendState(blend_state(D.abe, bc.op, bc.sf, bc.df, p.mask), blend_factor, 0xFFFFFFFF);
        g_ctx->OMSetDepthStencilState(ds ? depth_state(true, D.ztst, p.zw) : g_ds_off.Get(), 0);
        g_ctx->Draw((UINT)B.v.size(), first);
        if (p.mask) rt->dirty = true;
        if (p.zw && ds) ds->dirty = true;
    }
    ID3D11ShaderResourceView* none = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &none);
    B.v.clear();
    B.maxy = 0;
}

// ---- presentation ----------------------------------------------------------------------------
bool ensure_swapchain() {
    if (!g_window) return false;
    if (!g_hwnd) {
        g_hwnd = (HWND)display_window();
        if (!g_hwnd) { g_window = false; return false; }
    }
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    const UINT w = (UINT)std::max(1L, rc.right), h = (UINT)std::max(1L, rc.bottom);
    if (!g_swap) {
        ComPtr<IDXGIDevice> xd;
        ComPtr<IDXGIAdapter> ad;
        ComPtr<IDXGIFactory> fac;
        g_dev.As(&xd);
        xd->GetAdapter(&ad);
        ad->GetParent(IID_PPV_ARGS(&fac));
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferDesc.Width = w; sd.BufferDesc.Height = h; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 1;
        sd.OutputWindow = g_hwnd; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
        if (FAILED(fac->CreateSwapChain(g_dev.Get(), &sd, &g_swap))) {
            std::fprintf(stderr, "[gs-hw] cannot create a swap chain\n");
            g_window = false;
            return false;
        }
        fac->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
        g_back_w = g_back_h = 0;
    }
    if (w != g_back_w || h != g_back_h) {
        g_back_rtv.Reset();
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (g_back_w) g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE);
        ComPtr<ID3D11Texture2D> back;
        g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
        g_dev->CreateRenderTargetView(back.Get(), nullptr, &g_back_rtv);
        g_back_w = w; g_back_h = h;
    }
    return true;
}

ComPtr<ID3D11Texture2D> g_shot, g_shot_stage;
ComPtr<ID3D11RenderTargetView> g_shot_rtv;

void print_stats() {
    static uint64_t last_d, last_r, last_t, last_draws, last_p;
    log(Log::Gpu, "[gs-hw] %u surfaces, %zu textures; per 10 s: %llu draws, %llu downloads, %llu refreshes, %llu page copies, %llu texture decodes",
        (unsigned)g_surf.size(), g_tex.size(), (unsigned long long)(g_draws - last_draws),
        (unsigned long long)(g_downloads - last_d), (unsigned long long)(g_refreshes - last_r),
        (unsigned long long)(g_page_copies - last_p), (unsigned long long)(g_tex_decodes - last_t));
    last_d = g_downloads; last_r = g_refreshes; last_t = g_tex_decodes; last_draws = g_draws; last_p = g_page_copies;
}

} // namespace

void rect_blocks(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t y0, uint32_t y1, uint32_t* lo, uint32_t* hi) {
    const uint32_t ph = page_h(psm), ppr = pages_per_row(bw, psm) * 32;
    *lo = bp + y0 / ph * ppr;
    *hi = bp + (y1 + ph - 1) / ph * ppr;
}

bool active() {
    if (g_state == 0) {
        int want = g_want_hw;
        if (want < 0) {
            char buf[16]; size_t n = 0;
            want = (!getenv_s(&n, buf, sizeof buf, "TWONK_RENDERER") && n && std::strcmp(buf, "sw") == 0) ? 0 : 1;
        }
        g_scale = g_want_scale;
        if (!g_scale) {
            char buf[16]; size_t n = 0;
            if (!getenv_s(&n, buf, sizeof buf, "TWONK_SCALE") && n) video_cli(buf, nullptr, false);
            g_scale = video_settings().scale;   // auto: 2 until the window size is known
            if (!g_scale) g_scale = 2;
        }
        g_scale = std::clamp(g_scale, 1, 8);
        g_state = -1;
        {
            char buf[32]; size_t n = 0;
            if (!getenv_s(&n, buf, sizeof buf, "TWONK_HW_TRACE") && n) g_trace_at = std::strtoull(buf, nullptr, 10);
        }
        if (want == 1) {
            if (init_device()) {
                g_state = 1;
                g_cur_scale = g_scale;
                g_window = display_window() != nullptr;
                display_set_hw(true);
                std::fprintf(stderr, "[gs-hw] Direct3D 11 renderer at %dx (%dx%d)\n", g_scale, 640 * g_scale, 448 * g_scale);
            } else {
                std::fprintf(stderr, "[gs-hw] falling back to the software renderer\n");
            }
        }
    }
    return g_state == 1;
}

void draw(const DrawState& s, int kind, const Vertex& a, const Vertex& b, const Vertex& c) {
    const bool point = kind == 0 && a.x == b.x && a.y == b.y;
    const D3D11_PRIMITIVE_TOPOLOGY topo = (kind == 0 && !point) ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (!B.valid || topo != B.topo || std::memcmp(&B.st, &s, sizeof s) != 0) {
        submit();
        B.valid = true;
        B.st = s;
        B.topo = topo;
    }
    float ymax;
    if (kind == 1) {
        const Vertex& col = s.iip ? a : c;
        B.v.push_back(hv(a, s.iip ? a : col));
        B.v.push_back(hv(b, s.iip ? b : col));
        B.v.push_back(hv(c, c));
        ymax = std::max({a.y, b.y, c.y});
    } else if (kind == 2 || point) {
        Vertex p0 = a, p1 = point ? a : b;
        if (point) { p1.x += 1; p1.y += 1; }
        const float q = p1.q;
        Vertex c00 = p1, c10 = p1, c01 = p1, c11 = p1;   // attributes of the second vertex
        c00.x = p0.x; c00.y = p0.y; c00.u = p0.u; c00.v = p0.v; c00.s = p0.s; c00.t = p0.t;
        c10.y = p0.y; c10.v = p0.v; c10.t = p0.t;
        c01.x = p0.x; c01.u = p0.u; c01.s = p0.s;
        B.v.push_back(hv(c00, p1, q)); B.v.push_back(hv(c10, p1, q)); B.v.push_back(hv(c01, p1, q));
        B.v.push_back(hv(c10, p1, q)); B.v.push_back(hv(c11, p1, q)); B.v.push_back(hv(c01, p1, q));
        ymax = std::max(p0.y, p1.y);
    } else {
        B.v.push_back(hv(a, s.iip ? a : b));
        B.v.push_back(hv(b, b));
        ymax = std::max(a.y, b.y) + 1;
    }
    B.maxy = std::max(B.maxy, std::min(ymax, (float)s.scay1 + 1));
    if (B.v.size() >= 60000) submit();
}

void flush() {
    if (g_state == 1) submit();
}

void sync(uint32_t lo, uint32_t hi) {
    if (g_state != 1) return;
    submit();
    if (tracing()) std::fprintf(stderr, "[hw-trace] sync %x-%x\n", lo, hi);
    bool pending_applied = false;
    for (auto& p : g_surf) {
        Surface& s = *p;
        uint32_t a, b;
        s.range(&a, &b);
        if (!s.dirty || !(lo < b && a < hi)) continue;
        if (!pending_applied) { apply_pending(); pending_applied = true; }   // the download must not undo CPU writes
        static char why[64];
        std::snprintf(why, sizeof why, "CPU access %x-%x", lo, hi);
        g_why = why;
        download(s);
    }
}

void written(uint32_t lo, uint32_t hi) {
    if (g_state != 1 || hi <= lo) return;
    if (tracing()) std::fprintf(stderr, "[hw-trace] written %x-%x\n", lo, hi);
    bump(lo, hi);
    if (!g_pending.empty() && g_pending.back().lo <= hi && lo <= g_pending.back().hi) {
        g_pending.back().lo = std::min(g_pending.back().lo, lo);
        g_pending.back().hi = std::max(g_pending.back().hi, hi);
    } else {
        g_pending.push_back({lo, hi});
    }
}

// New resolution scale: write every GPU-drawn surface back to GS memory and drop
// them all; they are recreated at the new scale from GS memory as they are used.
void rescale(int scale) {
    apply_pending();
    for (auto& p : g_surf) {
        g_why = "scale change";
        download(*p);
    }
    g_surf.clear();
    g_scale = scale;
    g_cur_scale = scale;
    std::fprintf(stderr, "[gs-hw] resolution scale %dx (%dx%d)\n", g_scale, 640 * g_scale, 448 * g_scale);
}

int g_next_scale = 0;   // GS thread: a new resolution scale for the next present

void present(uint64_t frame, const uint64_t* priv) {
    if (g_state != 1) return;
    submit();
    if (g_next_scale && g_next_scale != g_scale) rescale(g_next_scale);
    g_next_scale = 0;
    ++g_presents;
    if (tracing()) std::fprintf(stderr, "[hw-trace] present %llu: dispfb %llx\n", (unsigned long long)g_presents, (unsigned long long)priv[7]);
    apply_pending();
    // Surfaces unused for a second (such as the boot-time clear of all of Z memory) go back
    // to GS memory: otherwise every texture upload there refreshes them, and textures there
    // look like render targets.
    for (size_t i = 0; i < g_surf.size();) {
        Surface& o = *g_surf[i];
        if (g_presents - o.last_use <= 60) { ++i; continue; }
        g_why = "unused";
        download(o);
        g_surf.erase(g_surf.begin() + (long long)i);
    }
    {
        static uint64_t n;
        if (++n % 600 == 0 && log_enabled(Log::Gpu)) print_stats();
    }
    const uint64_t pmode = priv[0];
    const int circuit = (pmode & 1) ? 0 : (pmode & 2) ? 1 : -1;
    Surface* s = nullptr;
    uint32_t dbx = 0, dby = 0, w = 0, h = 0;
    if (circuit >= 0) {
        const uint64_t dispfb = priv[7 + circuit * 2], display = priv[8 + circuit * 2];
        const uint32_t fbp = bf(dispfb, 0, 9) * 32, fbw = bf(dispfb, 9, 6), psm = bf(dispfb, 15, 5);
        dbx = bf(dispfb, 32, 11); dby = bf(dispfb, 43, 11);
        const uint32_t magh = bf(display, 23, 4) + 1, magv = bf(display, 27, 2) + 1;
        w = (bf(display, 32, 12) + 1) / magh;
        h = (bf(display, 44, 11) + 1) / magv;
        if (w && h && w <= 2048 && h <= 2048) {
            for (auto& p : g_surf)   // the surface already holding the picture, whatever format it was drawn in
                if (!p->depth && p->bp == fbp && p->bw == std::max(fbw, 1u)) { s = p.get(); break; }
            if (s) { validate(*s); grow(*s, dby + h); s->last_use = g_presents; }
            else s = get_surface(fbp, fbw, psm, false, dby + h);
            w = std::min(w, s ? s->w - std::min(s->w, dbx) : 0);
        }
    }
    const float S = (float)g_scale;
    float uv[4] = {0, 0, 1, 1};
    if (s) {
        uv[0] = dbx * S / (s->w * S); uv[1] = dby * S / (s->h * S);
        uv[2] = (dbx + w) * S / (s->w * S); uv[3] = (dby + h) * S / (s->h * S);
    }
    // screenshots (and --no-window): read the picture back at full resolution
    if (s && display_shot_due(frame)) {
        const UINT W = w * g_scale, H = h * g_scale;
        D3D11_TEXTURE2D_DESC d{};
        if (g_shot) g_shot->GetDesc(&d);
        if (!g_shot || d.Width != W || d.Height != H) {
            g_make_site = "screenshot";
            make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0, &g_shot);
            make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &g_shot_stage);
            g_dev->CreateRenderTargetView(g_shot.Get(), nullptr, g_shot_rtv.ReleaseAndGetAddressOf());
        }
        g_ctx->OMSetRenderTargets(1, g_shot_rtv.GetAddressOf(), nullptr);
        g_ctx->OMSetDepthStencilState(g_ds_off.Get(), 0);
        set_blit(0, 0, 1, (int)(dbx * g_scale), (int)(dby * g_scale));
        blit(s->srv.Get(), g_ps_copy.Get(), 0, 0, (float)W, (float)H);
        g_ctx->CopyResource(g_shot_stage.Get(), g_shot.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(g_shot_stage.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            std::vector<uint32_t> px((size_t)W * H);
            for (UINT y = 0; y < H; ++y) {
                const uint32_t* row = (const uint32_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
                for (UINT x = 0; x < W; ++x) {
                    const uint32_t c = row[x];
                    px[(size_t)y * W + x] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
                }
            }
            g_ctx->Unmap(g_shot_stage.Get(), 0);
            display_present(px.data(), W, H, frame);
        }
    }
    if (!ensure_swapchain()) return;
    const float black[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(g_back_rtv.Get(), black);
    const VideoSettings vs = video_settings();
    // the picture's place in the window: the chosen aspect, centred (movies stay 4:3)
    float dx = 0, dy = 0, dw = (float)g_back_w, dh = (float)g_back_h;
    if (const float a = video_display_aspect(movie_playing()); a > 0) {
        if (dw > dh * a) { dw = std::floor(dh * a + 0.5f); dx = std::floor(((float)g_back_w - dw) / 2); }
        else { dh = std::floor(dw / a + 0.5f); dy = std::floor(((float)g_back_h - dh) / 2); }
    }
    // resolution scale: the setting, or (auto) enough to fill the picture, times supersampling
    if (w && h) {
        int want = vs.scale;
        if (!want) want = (int)std::ceil(std::max(dw / (float)w, dh / (float)h) * video_ssaa_factor(vs.ssaa) - 0.05f);
        want = std::clamp(want, 1, 8);
        static int pending = 0, stable = 0;   // wait for it to settle (window resizes come in bursts)
        if (want == g_scale) stable = 0;
        else if (want != pending) { pending = want; stable = 0; }
        else if (++stable >= 15) { g_next_scale = want; stable = 0; }   // applied before the next frame is drawn
    }
    if (s) {
        const float tw = (float)(s->w * g_scale), th = (float)(s->h * g_scale);
        const float texsz[4] = {tw, th, 1.0f / tw, 1.0f / th};
        const float sw = (float)w * S, sh = (float)h * S;   // the shown part, in surface pixels
        const float prm[4] = {sw / dw, sh / dh, 0.5f, 0};
        const bool shrink = sw > dw * 1.01f || sh > dh * 1.01f, grow = sw < dw * 0.99f || sh < dh * 0.99f;
        ID3D11PixelShader* resample = shrink ? g_ps_down.Get() : (grow && vs.filter == 1) ? g_ps_lanczos.Get() : g_ps_present.Get();
        g_ctx->OMSetDepthStencilState(g_ds_off.Get(), 0);
        if (vs.filter == 1 && (shrink || grow)) {   // resample to an output-sized picture, then sharpen it into the window
            static ComPtr<ID3D11Texture2D> mid;
            static ComPtr<ID3D11RenderTargetView> mid_rtv;
            static ComPtr<ID3D11ShaderResourceView> mid_srv;
            const UINT mw = (UINT)dw, mh = (UINT)dh;
            D3D11_TEXTURE2D_DESC d{};
            if (mid) mid->GetDesc(&d);
            if (!mid || d.Width != mw || d.Height != mh) {
                mid_rtv.Reset(); mid_srv.Reset();
                g_make_site = "present";
                make_tex(mw, mh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, &mid);
                g_dev->CreateRenderTargetView(mid.Get(), nullptr, &mid_rtv);
                g_dev->CreateShaderResourceView(mid.Get(), nullptr, &mid_srv);
            }
            g_ctx->OMSetRenderTargets(1, mid_rtv.GetAddressOf(), nullptr);
            set_blit(0, 0, 1, 0, 0, uv, texsz, prm);
            blit(s->srv.Get(), resample, 0, 0, dw, dh);
            g_ctx->OMSetRenderTargets(1, g_back_rtv.GetAddressOf(), nullptr);
            const float msz[4] = {dw, dh, 1.0f / dw, 1.0f / dh};
            set_blit(dx, dy, 1, 0, 0, nullptr, msz, prm);
            blit(mid_srv.Get(), g_ps_cas.Get(), dx, dy, dw, dh);
        } else {
            g_ctx->OMSetRenderTargets(1, g_back_rtv.GetAddressOf(), nullptr);
            set_blit(0, 0, 1, 0, 0, uv, texsz, prm);
            blit(s->srv.Get(), resample, dx, dy, dw, dh);
        }
    }
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    {   // the FPS counter, drawn with GDI onto the back buffer
        ComPtr<ID3D11Texture2D> back;
        ComPtr<IDXGISurface1> surf;
        g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
        if (back && SUCCEEDED(back.As(&surf))) {
            HDC dc;
            if (SUCCEEDED(surf->GetDC(FALSE, &dc))) {
                display_overlay(dc, (int)g_back_w, (int)g_back_h);
                surf->ReleaseDC(nullptr);
            }
        }
    }
    g_swap->Present(0, 0);
}

} // namespace ps2::gpu::hw

namespace ps2::gpu::hw {
// For the perf log: downloads by reason since the last call, most frequent first.
std::string take_download_reasons() {
    std::map<std::string, uint32_t> m;
    {
        std::lock_guard<std::mutex> lock(g_why_mutex);
        m.swap(g_why_count);
    }
    std::vector<std::pair<uint32_t, std::string>> v;
    for (auto& [k, n] : m) v.emplace_back(n, k);
    std::sort(v.rbegin(), v.rend());
    std::string out;
    for (size_t i = 0; i < v.size() && i < 4; ++i) out += "    " + std::to_string(v[i].first) + "x " + v[i].second + "\n";
    static uint64_t l_draws, l_made, l_made_px, l_fb, l_fb_px, l_pages;   // GPU work this second (read across threads: approximate)
    char line[256];
    std::snprintf(line, sizeof line, "    gpu: %llu draws, %llu feedback copies (%.1f Mpx), %llu textures created (%.1f Mpx), %llu page copies\n",
                  (unsigned long long)(g_draws - l_draws), (unsigned long long)(g_fb_copies - l_fb), (double)(g_fb_copy_px - l_fb_px) / 1e6,
                  (unsigned long long)(g_tex_made - l_made), (double)(g_tex_made_px - l_made_px) / 1e6,
                  (unsigned long long)(g_page_copies - l_pages));
    l_draws = g_draws; l_made = g_tex_made; l_made_px = g_tex_made_px; l_fb = g_fb_copies; l_fb_px = g_fb_copy_px; l_pages = g_page_copies;
    out += line;
    std::map<std::string, uint32_t> made;
    {
        std::lock_guard<std::mutex> lock(g_made_mutex);
        made.swap(g_made_count);
    }
    std::vector<std::pair<uint32_t, std::string>> mv;
    for (auto& [k, n] : made) mv.emplace_back(n, k);
    std::sort(mv.rbegin(), mv.rend());
    for (size_t i = 0; i < mv.size() && i < 4; ++i) out += "      created " + std::to_string(mv[i].first) + "x " + mv[i].second + "\n";
    return out;
}
} // namespace ps2::gpu::hw
