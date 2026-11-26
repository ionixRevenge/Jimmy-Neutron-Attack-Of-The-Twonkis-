// Texture dumping and replacement (see texpack.hpp). PNG I/O goes through WIC on two
// worker threads, so the GS thread never waits for the disk: a replacement that is
// still loading is drawn with the original texture until it is ready.
#include "texpack.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace ps2::gpu::texpack {
namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

std::string env(const char* name) {
    char buf[1024];
    size_t n = 0;
    if (getenv_s(&n, buf, sizeof buf, name) || !n) return {};
    return buf;
}

IWICImagingFactory* wic() {   // per thread (COM is initialised on the calling thread)
    thread_local ComPtr<IWICImagingFactory> f;
    if (!f) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    }
    return f.Get();
}

bool save_png(const fs::path& path, const uint32_t* px, uint32_t w, uint32_t h) {
    IWICImagingFactory* f = wic();
    if (!f) return false;
    std::vector<uint32_t> rgba(px, px + (size_t)w * h);
    for (uint32_t& c : rgba) {   // GS alpha (0x80 = 1.0) -> PNG alpha; RGBA -> BGRA
        const uint32_t a = std::min<uint32_t>(255, (c >> 24) * 2);
        c = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF) | a << 24;
    }
    HRESULT hr = S_OK;
    auto fail = [&](const char* what) {
        static int n;
        if (n++ < 5) std::fprintf(stderr, "[texpack] cannot write %s: %s failed (%08lx)\n", path.string().c_str(), what, (unsigned long)hr);
        return false;
    };
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(hr = f->CreateStream(&st))) return fail("CreateStream");
    if (FAILED(hr = st->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return fail("open");
    if (FAILED(hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return fail("CreateEncoder");
    if (FAILED(hr = enc->Initialize(st.Get(), WICBitmapEncoderNoCache))) return fail("Initialize");
    if (FAILED(hr = enc->CreateNewFrame(&fr, nullptr))) return fail("CreateNewFrame");
    if (FAILED(hr = fr->Initialize(nullptr))) return fail("frame Initialize");
    if (FAILED(hr = fr->SetSize(w, h))) return fail("SetSize");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(hr = fr->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat32bppBGRA) return fail("SetPixelFormat");
    if (FAILED(hr = fr->WritePixels(h, w * 4, w * h * 4, (BYTE*)rgba.data()))) return fail("WritePixels");
    if (FAILED(hr = fr->Commit())) return fail("frame Commit");
    if (FAILED(hr = enc->Commit())) return fail("Commit");
    return true;
}

bool load_png(const fs::path& path, Image* out) {
    IWICImagingFactory* f = wic();
    if (!f) return false;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> fr;
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &fr)) || FAILED(f->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    cv->GetSize(&w, &h);
    if (!w || !h || w > 16384 || h > 16384) return false;
    out->w = w; out->h = h;
    out->px.resize((size_t)w * h);
    if (FAILED(cv->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)out->px.data()))) return false;
    for (uint32_t& c : out->px) c = (c & 0xFFFFFF) | (((c >> 24) + 1) / 2) << 24;   // PNG alpha -> GS alpha
    return true;
}

// ---- dumping ---------------------------------------------------------------------------------
struct DumpJob { uint64_t h; uint32_t w, hh; std::vector<uint32_t> px; std::string info; };
struct Dumper {
    fs::path dir;
    std::mutex m;
    std::condition_variable cv;
    std::deque<DumpJob> q;
    std::unordered_set<uint64_t> seen;
    FILE* index = nullptr;
};
Dumper* dumper() {
    static Dumper* inst = [] () -> Dumper* {
        const std::string dir = env("TWONK_TEX_DUMP");
        if (dir.empty()) return nullptr;
        auto* d = new Dumper;
        d->dir = dir;
        std::error_code ec;
        fs::create_directories(d->dir, ec);
        for (auto& e : fs::directory_iterator(d->dir, ec))   // keep what an earlier run dumped
            if (e.path().extension() == ".png") d->seen.insert(std::strtoull(e.path().stem().string().c_str(), nullptr, 16));
        _wfopen_s(&d->index, (d->dir / "index.txt").c_str(), L"a");
        std::fprintf(stderr, "[texpack] dumping textures to %s (%zu already there)\n", dir.c_str(), d->seen.size());
        std::thread([d] {
            for (;;) {
                DumpJob j;
                {
                    std::unique_lock<std::mutex> lock(d->m);
                    d->cv.wait(lock, [d] { return !d->q.empty(); });
                    j = std::move(d->q.front());
                    d->q.pop_front();
                }
                char name[32];
                std::snprintf(name, sizeof name, "%016llx.png", (unsigned long long)j.h);
                if (save_png(d->dir / name, j.px.data(), j.w, j.hh) && d->index) {
                    std::fprintf(d->index, "%016llx %ux%u %s\n", (unsigned long long)j.h, j.w, j.hh, j.info.c_str());
                    std::fflush(d->index);
                }
            }
        }).detach();
        return d;
    }();
    return inst;
}

// ---- replacement -----------------------------------------------------------------------------
struct Entry {
    fs::path path;
    int state = 0;   // 0 not loaded, 1 queued, 2 ready, 3 failed
    std::shared_ptr<const Image> img;
};
struct Loader {
    std::mutex m;
    std::condition_variable cv;
    std::deque<uint64_t> q;
    std::unordered_map<uint64_t, Entry> files;
};
Loader* loader() {
    static Loader* inst = [] () -> Loader* {
        std::string dir = env("TWONK_TEX_DIR");
        if (dir.empty()) dir = "textures";
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return nullptr;
        auto* l = new Loader;
        for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const fs::path& p = it->path();
            std::wstring ext = p.extension().wstring();
            for (auto& c : ext) c = (wchar_t)towlower(c);
            const std::string stem = p.stem().string();
            if (ext != L".png" || stem.size() < 16) continue;
            bool hex = true;
            for (int i = 0; i < 16; ++i) hex &= std::isxdigit((unsigned char)stem[i]) != 0;
            if (!hex) continue;
            l->files[std::strtoull(stem.substr(0, 16).c_str(), nullptr, 16)].path = p;
        }
        std::fprintf(stderr, "[texpack] %zu replacement textures in %s\n", l->files.size(), dir.c_str());
        if (l->files.empty()) { delete l; return nullptr; }
        std::thread([l] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;) {
                uint64_t h;
                fs::path path;
                {
                    std::unique_lock<std::mutex> lock(l->m);
                    l->cv.wait(lock, [l] { return !l->q.empty(); });
                    h = l->q.front();
                    l->q.pop_front();
                    path = l->files[h].path;
                }
                auto img = std::make_shared<Image>();
                const bool ok = load_png(path, img.get());
                if (!ok) std::fprintf(stderr, "[texpack] cannot read %s\n", path.string().c_str());
                std::lock_guard<std::mutex> lock(l->m);
                Entry& e = l->files[h];
                if (e.state != 1) continue;   // forgotten meanwhile
                e.state = ok ? 2 : 3;
                if (ok) e.img = std::move(img);
            }
        }).detach();
        return l;
    }();
    return inst;
}

} // namespace

bool dumping() { return dumper() != nullptr; }
bool replacing() { return loader() != nullptr; }

uint64_t hash(const uint32_t* px, uint32_t w, uint32_t h) {
    uint64_t x = 0x9E3779B97F4A7C15ull ^ ((uint64_t)w << 32 | h);
    const size_t n = (size_t)w * h;
    size_t i = 0;
    for (; i + 2 <= n; i += 2) {
        uint64_t v;
        std::memcpy(&v, px + i, 8);
        x = (x ^ v) * 0xFF51AFD7ED558CCDull;
        x ^= x >> 32;
    }
    if (i < n) x = ((x ^ px[i]) * 0xC4CEB9FE1A85EC53ull);
    x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
    return x;
}

void dump(uint64_t h, const uint32_t* px, uint32_t w, uint32_t hh, const char* info) {
    Dumper* d = dumper();
    if (!d) return;
    std::lock_guard<std::mutex> lock(d->m);
    if (!d->seen.insert(h).second) return;
    d->q.push_back({h, w, hh, std::vector<uint32_t>(px, px + (size_t)w * hh), info});
    d->cv.notify_one();
}

std::shared_ptr<const Image> find(uint64_t h, bool* pending) {
    *pending = false;
    Loader* l = loader();
    if (!l) return nullptr;
    std::lock_guard<std::mutex> lock(l->m);
    auto it = l->files.find(h);
    if (it == l->files.end()) return nullptr;
    Entry& e = it->second;
    if (e.state == 2) return e.img;
    if (e.state == 0) {
        e.state = 1;
        l->q.push_back(h);
        l->cv.notify_one();
    }
    *pending = e.state == 1;
    return nullptr;
}

void forget(uint64_t h) {
    Loader* l = loader();
    if (!l) return;
    std::lock_guard<std::mutex> lock(l->m);
    auto it = l->files.find(h);
    if (it != l->files.end() && it->second.state != 3) {
        it->second.state = 0;
        it->second.img.reset();
    }
}

} // namespace ps2::gpu::texpack
