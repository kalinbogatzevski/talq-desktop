/*
 * talq_wgc.cpp -- Windows Graphics Capture (WGC) single-window capture.
 * Built with MSVC (cl.exe) + the Win11 SDK (WinRT headers). See talq_wgc.h for
 * why this is a separate, C-ABI-only component loaded by the MinGW TalQ app.
 *
 * Threading: all WGC objects live on ONE dedicated MTA worker thread we spawn
 * (isolated from the host app's apartment, which Qt may have put in STA). The
 * frame pool is FreeThreaded, so FrameArrived fires on WinRT pool threads; a
 * mutex + a `stopping` flag + event-revoke make teardown race-free (no callback
 * runs after talq_wgc_stop returns, so the caller can free `user` safely).
 *
 * Frame path per arrival: TryGetNextFrame -> ID3D11Texture2D (from the
 * IDirect3DSurface) -> CopyResource into a CPU-readable STAGING texture -> Map
 * -> hand the BGRA rows to the callback -> Unmap. On a content-size change the
 * pool is Recreate()d at the new size (WGC requirement).
 */
#include "talq_wgc.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <inspectable.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <mutex>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")   /* WinRT */

using namespace winrt;
using winrt::Windows::Graphics::SizeInt32;
namespace WGC  = winrt::Windows::Graphics::Capture;
namespace WDX  = winrt::Windows::Graphics::DirectX;
namespace WD3D = winrt::Windows::Graphics::DirectX::Direct3D11;

/* IDirect3DSurface -> ID3D11Texture2D (via the DXGI-interface-access bridge). */
static com_ptr<ID3D11Texture2D> SurfaceToTexture(WD3D::IDirect3DSurface const &surface)
{
    auto access = surface.as<
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> tex;
    check_hresult(access->GetInterface(guid_of<ID3D11Texture2D>(), tex.put_void()));
    return tex;
}

struct talq_wgc_session
{
    /* config */
    talq_wgc_frame_cb cb{nullptr};
    void             *user{nullptr};
    int               show_border{0};
    HWND              hwnd{nullptr};   /* window capture target (mutually exclusive with hmon) */
    HMONITOR          hmon{nullptr};   /* monitor capture target (mutually exclusive with hwnd) */

    /* lifecycle */
    std::thread        worker;
    HANDLE             stopEvent{nullptr};
    std::atomic<bool>  stopping{false};
    std::mutex         frameMtx;   /* serialises OnFrame body vs teardown */

    /* GPU + WGC (owned by the worker thread) */
    com_ptr<ID3D11Device>          dev;
    com_ptr<ID3D11DeviceContext>   ctx;
    WD3D::IDirect3DDevice          rtDev{nullptr};
    WGC::GraphicsCaptureItem       item{nullptr};
    WGC::Direct3D11CaptureFramePool pool{nullptr};
    WGC::GraphicsCaptureSession    session{nullptr};
    event_token                    frameToken{};
    com_ptr<ID3D11Texture2D>       staging;
    UINT                           stagingW{0}, stagingH{0};
    SizeInt32                      lastSize{0, 0};

    void OnFrame(WGC::Direct3D11CaptureFramePool const &p,
                 winrt::Windows::Foundation::IInspectable const &)
    {
        std::lock_guard<std::mutex> lk(frameMtx);
        if (stopping.load()) return;
        auto frame = p.TryGetNextFrame();
        if (!frame) return;
        const SizeInt32 size = frame.ContentSize();

        com_ptr<ID3D11Texture2D> tex;
        try { tex = SurfaceToTexture(frame.Surface()); } catch (...) { return; }
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);

        if (!staging || stagingW != d.Width || stagingH != d.Height) {
            staging = nullptr;
            D3D11_TEXTURE2D_DESC sd = d;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            if (FAILED(dev->CreateTexture2D(&sd, nullptr, staging.put()))) return;
            stagingW = d.Width;
            stagingH = d.Height;
        }

        ctx->CopyResource(staging.get(), tex.get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
            if (cb && !stopping.load())
                cb(user, static_cast<const uint8_t *>(m.pData),
                   static_cast<int>(d.Width), static_cast<int>(d.Height),
                   static_cast<int>(m.RowPitch));
            ctx->Unmap(staging.get(), 0);
        }

        /* WGC: recreate the pool when the captured window resizes. */
        if (size.Width != lastSize.Width || size.Height != lastSize.Height) {
            lastSize = size;
            try {
                pool.Recreate(rtDev, WDX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                              2, size);
            } catch (...) {}
        }
    }

    bool setup()
    {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       flags, nullptr, 0, D3D11_SDK_VERSION,
                                       dev.put(), nullptr, ctx.put());
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION,
                                   dev.put(), nullptr, ctx.put());
            if (FAILED(hr)) return false;
        }

        auto dxgi = dev.as<IDXGIDevice>();
        com_ptr<::IInspectable> insp;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put())))
            return false;
        rtDev = insp.as<WD3D::IDirect3DDevice>();

        auto interop = get_activation_factory<WGC::GraphicsCaptureItem,
                                              ::IGraphicsCaptureItemInterop>();
        // Monitor vs window target — same frame-pool/callback path afterwards.
        if (hmon)
            hr = interop->CreateForMonitor(hmon, guid_of<WGC::GraphicsCaptureItem>(),
                                           reinterpret_cast<void **>(put_abi(item)));
        else
            hr = interop->CreateForWindow(hwnd, guid_of<WGC::GraphicsCaptureItem>(),
                                          reinterpret_cast<void **>(put_abi(item)));
        if (FAILED(hr) || !item) return false;

        lastSize = item.Size();
        if (lastSize.Width <= 0 || lastSize.Height <= 0) return false;

        pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
            rtDev, WDX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, lastSize);
        frameToken = pool.FrameArrived({this, &talq_wgc_session::OnFrame});
        session = pool.CreateCaptureSession(item);

        /* Hide the Win11 yellow capture border when asked + supported. */
        try {
            using winrt::Windows::Foundation::Metadata::ApiInformation;
            if (ApiInformation::IsPropertyPresent(
                    L"Windows.Graphics.Capture.GraphicsCaptureSession",
                    L"IsBorderRequired"))
                session.IsBorderRequired(show_border != 0);
        } catch (...) {}

        session.StartCapture();
        return true;
    }

    void teardown()
    {
        stopping.store(true);
        try { if (frameToken.value) pool.FrameArrived(frameToken); } catch (...) {}
        { std::lock_guard<std::mutex> lk(frameMtx); }   /* drain in-flight OnFrame */
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
        session = nullptr; pool = nullptr; item = nullptr; rtDev = nullptr;
        staging = nullptr; ctx = nullptr; dev = nullptr;
    }
};

extern "C" {

int talq_wgc_available(void)
{
    try { return WGC::GraphicsCaptureSession::IsSupported() ? 1 : 0; }
    catch (...) { return 0; }
}

/* Shared worker-thread launch for window + monitor capture: spin the MTA
 * thread, run setup() (which uses s->hwnd or s->hmon), and wait for the
 * synchronous ok/fail result. Returns s on success, frees + NULL on failure. */
static talq_wgc_session *start_common(talq_wgc_session *s)
{
    s->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> result{0};   /* 0 pending, 1 ok, -1 fail */

    s->worker = std::thread([s, &result, ready]() {
        init_apartment(apartment_type::multi_threaded);
        bool ok = false;
        try { ok = s->setup(); } catch (...) { ok = false; }
        result.store(ok ? 1 : -1);
        SetEvent(ready);
        if (ok) WaitForSingleObject(s->stopEvent, INFINITE);
        try { s->teardown(); } catch (...) {}
        uninit_apartment();
    });

    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);

    if (result.load() != 1) {
        SetEvent(s->stopEvent);
        if (s->worker.joinable()) s->worker.join();
        CloseHandle(s->stopEvent);
        delete s;
        return nullptr;
    }
    return s;
}

talq_wgc_session *talq_wgc_start_window(HWND hwnd, talq_wgc_frame_cb cb,
                                        void *user, int show_border)
{
    if (!hwnd || !cb || !IsWindow(hwnd)) return nullptr;
    auto *s = new talq_wgc_session();
    s->cb = cb; s->user = user; s->show_border = show_border; s->hwnd = hwnd;
    return start_common(s);
}

talq_wgc_session *talq_wgc_start_monitor(HMONITOR mon, talq_wgc_frame_cb cb,
                                         void *user, int show_border)
{
    if (!mon || !cb) return nullptr;
    auto *s = new talq_wgc_session();
    s->cb = cb; s->user = user; s->show_border = show_border; s->hmon = mon;
    return start_common(s);
}

void talq_wgc_stop(talq_wgc_session *s)
{
    if (!s) return;
    SetEvent(s->stopEvent);
    if (s->worker.joinable()) s->worker.join();
    CloseHandle(s->stopEvent);
    delete s;
}

}  /* extern "C" */
