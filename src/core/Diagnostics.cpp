#include "core/Diagnostics.h"
#include "core/TalqLog.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QSysInfo>
#include <QTextStream>

#include <gst/gst.h>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dxgi.h>
#endif

namespace {

#ifdef Q_OS_WIN
// Enumerate the DXGI graphics adapters (GPUs). This is the single most useful
// line for screen-share failures: a laptop showing BOTH an Intel iGPU and an
// NVIDIA/AMD dGPU is "hybrid graphics", where DXGI Desktop Duplication
// (monitor capture) fails with DXGI_ERROR_UNSUPPORTED unless TalQ runs on the
// GPU that drives the display. Loaded at runtime so there's no dxgi link dep.
QString dxgiAdapters()
{
    HMODULE h = LoadLibraryW(L"dxgi.dll");
    if (!h) return QStringLiteral("  (dxgi.dll not available)\n");
    typedef HRESULT(WINAPI * PFN_Create)(REFIID, void **);
    auto create = reinterpret_cast<PFN_Create>(
        reinterpret_cast<void *>(GetProcAddress(h, "CreateDXGIFactory1")));
    if (!create) { FreeLibrary(h); return QStringLiteral("  (CreateDXGIFactory1 unavailable)\n"); }

    IDXGIFactory1 *factory = nullptr;
    if (FAILED(create(IID_IDXGIFactory1, reinterpret_cast<void **>(&factory))) || !factory) {
        FreeLibrary(h);
        return QStringLiteral("  (DXGI factory create failed)\n");
    }
    QString out;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *ad = nullptr;
        if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
        if (!ad) continue;
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(ad->GetDesc1(&d))) {
            const bool sw = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
            out += QStringLiteral("  #%1  %2  (VRAM %3 MB)%4\n")
                       .arg(i)
                       .arg(QString::fromWCharArray(d.Description))
                       .arg(static_cast<qulonglong>(d.DedicatedVideoMemory) / (1024 * 1024))
                       .arg(sw ? QStringLiteral(" [software]") : QString());
        }
        ad->Release();
    }
    factory->Release();
    FreeLibrary(h);
    return out.isEmpty() ? QStringLiteral("  (no adapters enumerated)\n") : out;
}

// Ask the WGC capture DLL whether Windows Graphics Capture is supported here
// (the gate behind single-window sharing, and the proposed monitor-share path).
QString wgcAvailability()
{
    HMODULE h = LoadLibraryW(L"talq_wgc.dll");
    if (!h) return QStringLiteral("talq_wgc.dll NOT FOUND (single-window share unavailable)");
    typedef int(*PFN_avail)(void);
    auto avail = reinterpret_cast<PFN_avail>(
        reinterpret_cast<void *>(GetProcAddress(h, "talq_wgc_available")));
    QString r;
    if (!avail) r = QStringLiteral("talq_wgc.dll loaded but entry point missing");
    else        r = avail() ? QStringLiteral("AVAILABLE (WGC supported by this OS)")
                            : QStringLiteral("NOT supported by this OS (Win10 <1803 / LTSC 2016?)");
    FreeLibrary(h);
    return r;
}

// Trim a DXGI adapter description to a compact display name:
// "NVIDIA GeForce RTX 3070 Laptop GPU" -> "NVIDIA RTX 3070",
// "Intel(R) UHD Graphics" -> "Intel UHD".
QString shortGpuName(QString n)
{
    n.remove(QStringLiteral("(R)"));
    n.remove(QStringLiteral("(TM)"));
    n.remove(QStringLiteral(" Corporation"));
    n.remove(QStringLiteral(" Laptop GPU"));
    n.remove(QStringLiteral("GeForce "));
    n.replace(QStringLiteral(" Graphics"), QString());
    return n.simplified();
}
#endif // Q_OS_WIN

void gstElement(QTextStream &s, const char *name)
{
    GstElementFactory *f = gst_element_factory_find(name);
    s << "  " << (f ? "OK " : "-- ") << name << "\n";
    if (f) gst_object_unref(f);
}

} // namespace

namespace talq {

QString collectSystemDiagnostics()
{
    QString out;
    QTextStream s(&out);

    s << "[System]\n";
    s << "OS:          " << QSysInfo::prettyProductName() << "\n";
    s << "Kernel:      " << QSysInfo::kernelType() << " " << QSysInfo::kernelVersion()
      << "   (the last number is the Windows build)\n";
    s << "Arch:        " << QSysInfo::currentCpuArchitecture() << "\n";
    s << "\n";

    s << "[Displays]\n";
    const auto screens = QGuiApplication::screens();
    s << "Count: " << screens.size() << "\n";
    for (int i = 0; i < screens.size(); ++i) {
        QScreen *sc = screens[i];
        const QRect g = sc->geometry();
        s << "  #" << i << "  " << sc->name() << "  "
          << g.width() << "x" << g.height()
          << " @ (" << g.x() << "," << g.y() << ")  dpr=" << sc->devicePixelRatio()
          << (sc == QGuiApplication::primaryScreen() ? "  [primary]" : "") << "\n";
    }
    s << "\n";

    s << "[Graphics adapters (GPUs)]\n";
#ifdef Q_OS_WIN
    s << dxgiAdapters();
    s << "  NOTE: if both an integrated (Intel) and a discrete (NVIDIA/AMD) GPU\n"
         "  are listed, full-display sharing can fail (DXGI_ERROR_UNSUPPORTED)\n"
         "  unless TalQ runs on the GPU driving the display.\n";
#else
    s << "  (not enumerated on this platform)\n";
#endif
    s << "\n";

    s << "[Screen capture]\n";
#ifdef Q_OS_WIN
    s << "  Single-window (WGC): " << wgcAvailability() << "\n";
#endif
    s << "  Monitor capture element (DXGI): ";
    gstElement(s, "d3d11screencapturesrc");
    s << "\n";

    s << "[GStreamer]\n";
    s << "Version: " << gst_version_string() << "\n";
    const char *els[] = {
        "webrtcdsp", "webrtcechoprobe", "audiomixer", "audiotestsrc",
        "wasapi2src", "wasapi2sink", "webrtcsrc", "webrtcbin",
        "opusenc", "rtph264pay", "nvh264enc", "qsvh264enc", "x264enc", "vp8enc",
        "appsrc", "appsink",
    };
    for (const char *e : els) gstElement(s, e);
    s << "\n";

    s << "[Call settings]\n";
    {
        QSettings q("TalQ", "TalQ");
        auto onoff = [](bool b) { return b ? "ON" : "off"; };
        q.beginGroup("Audio");
        s << "  Echo cancellation:  " << onoff(q.value("echoCancellation", true).toBool()) << "\n";
        s << "  Noise suppression:  " << onoff(q.value("noiseSuppression", true).toBool()) << "\n";
        s << "  Auto gain control:  " << onoff(q.value("audioAutoGainControl", true).toBool()) << "\n";
        q.endGroup();
        q.beginGroup("Video");
        s << "  Screen-share quality level: " << q.value("screenShareQuality", 2).toInt()
          << "  (0=720p 1=1080p 2=1440p 3=High/4K)\n";
        q.endGroup();
        s << "  Beta-channel opt-in: " << onoff(q.value("updates/betaChannel", false).toBool()) << "\n";
        s << "  Automatic updates:   " << onoff(q.value("updates/autoCheck", true).toBool()) << "\n";
    }
    s << "\n";

    return out;
}

QString tailDebugLog(qint64 maxBytes)
{
    const QString path = TalqLog::g_logPath;
    if (path.isEmpty() || !QFile::exists(path))
        return QStringLiteral("(no talq_debug.log available)\n");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral("(could not open talq_debug.log)\n");
    const qint64 sz = f.size();
    QString header;
    if (sz > maxBytes) {
        f.seek(sz - maxBytes);
        f.readLine();   // drop the partial first line after the seek
        header = QStringLiteral("...(truncated to last %1 KB of %2 KB)...\n")
                     .arg(maxBytes / 1024).arg(sz / 1024);
    }
    return header + QString::fromUtf8(f.readAll());
}

QStringList gpuAdapterNames()
{
    QStringList out;
#ifdef Q_OS_WIN
    HMODULE h = LoadLibraryW(L"dxgi.dll");
    if (!h) return out;
    typedef HRESULT(WINAPI * PFN_Create)(REFIID, void **);
    auto create = reinterpret_cast<PFN_Create>(
        reinterpret_cast<void *>(GetProcAddress(h, "CreateDXGIFactory1")));
    if (!create) { FreeLibrary(h); return out; }
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(create(IID_IDXGIFactory1, reinterpret_cast<void **>(&factory))) || !factory) {
        FreeLibrary(h);
        return out;
    }
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *ad = nullptr;
        if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
        if (!ad) continue;
        DXGI_ADAPTER_DESC1 d{};
        // Skip the software "Microsoft Basic Render Driver" — show real GPUs only.
        if (SUCCEEDED(ad->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            const QString name = shortGpuName(QString::fromWCharArray(d.Description));
            if (!name.isEmpty()) out << name;
        }
        ad->Release();
    }
    factory->Release();
    FreeLibrary(h);
#endif
    return out;
}

} // namespace talq
