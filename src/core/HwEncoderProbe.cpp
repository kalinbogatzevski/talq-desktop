// Feature #74 — cached out-of-process hardware-encoder probe. See HwEncoderProbe.h
// for the design and the fail-open contract.

#include "HwEncoderProbe.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QMutex>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <gst/gst.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dxgi.h>
#endif

namespace {

// Bump when the cache format or the probe methodology changes so an old cache is
// treated as stale (re-probed) rather than misread.
constexpr int kProbeSchemaVersion = 2;

// QSettings keys (flat, under the established "Video/" group that already holds
// avoidNvenc / forceSoftwareVideo / forceSoftwareDecode).
constexpr char kKeySchema[]    = "Video/hwProbeSchema";
constexpr char kKeySignature[] = "Video/hwProbeSignature";
constexpr char kKeyFailed[]    = "Video/hwProbeFailed";   // operative: excluded encoders
constexpr char kKeyWorking[]   = "Video/hwProbeWorking";  // diagnostics
constexpr char kKeyAbsent[]    = "Video/hwProbeAbsent";   // diagnostics
constexpr char kKeyEpoch[]     = "Video/hwProbeEpoch";    // diagnostics
constexpr char kKeySummary[]   = "Video/hwProbeSummary";  // diagnostics
constexpr char kKeyEnabled[]   = "Video/hwEncoderProbeEnabled";

// The only encoders the probe tests and the only ones the reader may exclude.
// x264enc / vp8enc (software) are deliberately absent — the software floor is
// never touched.
const char *const kCandidates[] = { "nvh264enc", "qsvh264enc", "mfh264enc" };

bool isProbeCandidate(const char *f)
{
    if (!f) return false;
    for (const char *c : kCandidates)
        if (!std::strcmp(f, c)) return true;
    return false;
}

// ── GPU signature ────────────────────────────────────────────────────────────
// A stable per-machine key: adapter name + PCI ids + LUID + user-mode driver
// version (which changes on a driver update, so a driver update re-probes). An
// empty return means "can't identify the GPU" → the whole feature fails open.
QString computeGpuSignatureRaw()
{
#ifdef Q_OS_WIN
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void **>(&factory)))
        || !factory)
        return QString();

    IDXGIAdapter1 *best = nullptr;
    DXGI_ADAPTER_DESC1 bestDesc{};
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *ad = nullptr;
        if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 d{};
        // Skip the Microsoft Basic Render Driver / WARP software adapter — we key
        // on the real GPU. Prefer the adapter with the most dedicated VRAM; an
        // Intel iGPU reports 0 dedicated and is still selected when it's the only
        // non-software adapter (strict '>' keeps the first such adapter).
        if (SUCCEEDED(ad->GetDesc1(&d))
            && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            && (!best || d.DedicatedVideoMemory > bestDesc.DedicatedVideoMemory)) {
            if (best) best->Release();
            best = ad;
            bestDesc = d;
            ad = nullptr;   // ownership transferred to `best`
        }
        if (ad) ad->Release();
    }

    QString sig;
    if (best) {
        // WDDM user-mode driver version (LARGE_INTEGER); best-effort — zero if the
        // query fails, which just makes the driver component of the key constant.
        LARGE_INTEGER umd{};
        best->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
        sig = QStringLiteral(
                  "%1|VEN_%2&DEV_%3&SUBSYS_%4&REV_%5|LUID_%6_%7|DRV_%8_%9")
                  .arg(QString::fromWCharArray(bestDesc.Description))
                  .arg(bestDesc.VendorId, 4, 16, QLatin1Char('0'))
                  .arg(bestDesc.DeviceId, 4, 16, QLatin1Char('0'))
                  .arg(bestDesc.SubSysId, 8, 16, QLatin1Char('0'))
                  .arg(bestDesc.Revision, 2, 16, QLatin1Char('0'))
                  .arg(static_cast<quint32>(bestDesc.AdapterLuid.LowPart), 8, 16, QLatin1Char('0'))
                  .arg(static_cast<qint32>(bestDesc.AdapterLuid.HighPart), 8, 16, QLatin1Char('0'))
                  .arg(static_cast<quint32>(umd.HighPart), 8, 16, QLatin1Char('0'))
                  .arg(static_cast<quint32>(umd.LowPart), 8, 16, QLatin1Char('0'));
        best->Release();
    }
    factory->Release();
    return sig;
#else
    return QString();
#endif
}

// Computed once per process (thread-safe local static). The child and the parent
// run the SAME code on the SAME machine, so they derive the same key.
QString gpuSignature()
{
    static const QString sig = computeGpuSignatureRaw();
    return sig;
}

// ── Reader state (parent, live path) ─────────────────────────────────────────
QMutex        g_readerMtx;
bool          g_readerValid = false;   // a FRESH cache for THIS GPU is loaded
QSet<QString> g_readerFailed;          // encoders that cache proved non-working

void loadReaderCacheLocked()
{
    g_readerValid = false;
    g_readerFailed.clear();
    const QString sig = gpuSignature();
    if (sig.isEmpty()) return;                                   // GPU unknown → fail open
    QSettings s(QStringLiteral("TalQ"), QStringLiteral("TalQ"));
    if (s.value(QLatin1String(kKeySchema)).toInt() != kProbeSchemaVersion)
        return;                                                  // no / old-format cache
    if (s.value(QLatin1String(kKeySignature)).toString() != sig)
        return;                                                  // different GPU → stale
    const QStringList failed = s.value(QLatin1String(kKeyFailed)).toStringList();
    for (const QString &f : failed) g_readerFailed.insert(f);
    g_readerValid = true;
}

// ── Probe accumulator (child) ────────────────────────────────────────────────
// Shared between the probe loop and the hard watchdog; both may call
// persistProbeResult(), which writes exactly once.
struct ProbeAccumulator {
    QMutex      mtx;
    QStringList failed, working, absent;
    QString     signature;
    bool        envSane   = true;
    bool        persisted = false;
};
ProbeAccumulator *g_acc = nullptr;

void persistProbeResult()
{
    if (!g_acc) return;
    QMutexLocker lk(&g_acc->mtx);
    if (g_acc->persisted) return;
    g_acc->persisted = true;
    // No GPU id → don't write a cache at all (the reader would ignore it anyway,
    // and this leaves the door open to a successful probe on a later launch).
    if (g_acc->signature.isEmpty()) return;

    QSettings s(QStringLiteral("TalQ"), QStringLiteral("TalQ"));
    s.setValue(QLatin1String(kKeySchema),    kProbeSchemaVersion);
    s.setValue(QLatin1String(kKeySignature), g_acc->signature);
    s.setValue(QLatin1String(kKeyFailed),    g_acc->failed);
    s.setValue(QLatin1String(kKeyWorking),   g_acc->working);
    s.setValue(QLatin1String(kKeyAbsent),    g_acc->absent);
    s.setValue(QLatin1String(kKeyEpoch),     static_cast<qint64>(QDateTime::currentSecsSinceEpoch()));
    s.setValue(QLatin1String(kKeySummary),
               QStringLiteral("envSane=%1 working=[%2] failed=[%3] absent=[%4]")
                   .arg(g_acc->envSane ? QStringLiteral("1") : QStringLiteral("0"),
                        g_acc->working.join(QLatin1Char(',')),
                        g_acc->failed.join(QLatin1Char(',')),
                        g_acc->absent.join(QLatin1Char(','))));
    s.sync();
}

// Mirror main.cpp's plugin-path setup so this child finds nvh264enc/qsvh264enc/
// mfh264enc. Without it the child would see every hardware encoder as ABSENT
// (still fail-open, but useless). Must run before gst_init.
void setGstPluginPathFromExe(const char *argv0)
{
    std::string exePath(argv0 ? argv0 : "");
    const auto slash = exePath.find_last_of("\\/");
    const std::string dir = (slash != std::string::npos) ? exePath.substr(0, slash) : ".";
    qputenv("GST_PLUGIN_PATH",    QByteArray::fromStdString(dir + "/gst-plugins"));
    qputenv("GST_PLUGIN_SCANNER", QByteArray::fromStdString(dir + "/gst-plugin-scanner.exe"));
}

// Drive a pipeline to PLAYING with a bounded wait. Returns true only if it
// actually reaches PLAYING within `waitSec` (a broken/hanging hardware encoder
// never prerolls in time → false, which is exactly the signal we want).
bool pipelineReachesPlaying(const char *desc, int waitSec)
{
    GError *err = nullptr;
    GstElement *pipe = gst_parse_launch(desc, &err);
    if (err) g_error_free(err);
    if (!pipe) return false;

    bool ok = false;
    if (gst_element_set_state(pipe, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE) {
        GstState st = GST_STATE_NULL;
        if (gst_element_get_state(pipe, &st, nullptr,
                                  static_cast<GstClockTime>(waitSec) * GST_SECOND)
                == GST_STATE_CHANGE_SUCCESS
            && st == GST_STATE_PLAYING)
            ok = true;
    }
    // Teardown of a wedged hardware encoder can itself block — the detached
    // watchdog is the backstop that guarantees the process still dies.
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(pipe);
    return ok;
}

// Environment sanity gate: a plain videotestsrc→videoconvert→fakesink pipeline
// with NO encoder. If even this can't reach PLAYING the probe ENVIRONMENT is
// broken (e.g. videotestsrc not deployed) — not the encoders — so we must record
// NOTHING as failed and leave the full ladder in place.
bool controlPipelineSane()
{
    return pipelineReachesPlaying(
        "videotestsrc is-live=false num-buffers=10 ! "
        "video/x-raw,format=I420,width=1280,height=720,framerate=30/1 ! "
        "videoconvert ! fakesink sync=false",
        5);
}

// Probe one candidate and record the verdict BEFORE teardown (teardown of a
// wedged encoder can block, and the watchdog must be able to persist this verdict
// regardless). Called only when the environment is known sane, so a candidate
// that fails is genuinely the encoder's fault.
void probeAndRecord(const char *factory, ProbeAccumulator &acc)
{
    // Not registered on this box at all → ABSENT, not a failure. The live ladder
    // already skips an unregistered factory naturally, so this never excludes.
    GstElementFactory *f = gst_element_factory_find(factory);
    if (!f) {
        QMutexLocker lk(&acc.mtx);
        acc.absent << QString::fromUtf8(factory);
        return;
    }
    gst_object_unref(f);

    // Real 720p path: testsrc → I420 720p → convert → <encoder> → fakesink.
    // is-live=false forces a PREROLL, which pushes a real frame through the
    // encoder and opens the hardware session — exactly the init that hangs on a
    // broken GPU.
    gchar *desc = g_strdup_printf(
        "videotestsrc is-live=false num-buffers=30 ! "
        "video/x-raw,format=I420,width=1280,height=720,framerate=30/1 ! "
        "videoconvert ! %s ! fakesink sync=false",
        factory);
    // 7s ≫ a healthy encoder preroll; a hardware encoder that would stall the
    // live call for 30-60s never reaches PLAYING within it, so the timeout marks
    // it FAILED — which is precisely the encoder we want the live path to avoid.
    const bool working = pipelineReachesPlaying(desc, 7);
    g_free(desc);

    QMutexLocker lk(&acc.mtx);
    (working ? acc.working : acc.failed) << QString::fromUtf8(factory);
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

bool talqHwEncoderProbeEnabled()
{
    if (qEnvironmentVariableIsSet("TALQ_DISABLE_HWPROBE"))
        return false;
    QSettings s(QStringLiteral("TalQ"), QStringLiteral("TalQ"));
    return s.value(QLatin1String(kKeyEnabled), true).toBool();
}

bool talqHwEncoderProbeExcludes(const char *factory)
{
    // Software floor (x264enc / vp8enc) and anything the probe never tests is
    // NEVER excluded — the ladder's ultimate fallback is untouchable.
    if (!isProbeCandidate(factory)) return false;
    if (!talqHwEncoderProbeEnabled()) return false;

    QMutexLocker lk(&g_readerMtx);
    // Retry the cheap QSettings read until a fresh cache appears, so the async
    // probe completing mid-session becomes visible; once valid, keep it.
    if (!g_readerValid) loadReaderCacheLocked();
    if (!g_readerValid) return false;                 // no valid cache → attempt as before
    return g_readerFailed.contains(QString::fromUtf8(factory));
}

void talqMaybeLaunchHwEncoderProbe()
{
    if (!talqHwEncoderProbeEnabled()) {
        qInfo() << "HwEncoderProbe: disabled (settings/env) — full encoder ladder";
        return;
    }
    const QString sig = gpuSignature();
    if (sig.isEmpty()) {
        qInfo() << "HwEncoderProbe: GPU signature unavailable — full ladder (no probe)";
        return;
    }
    QSettings s(QStringLiteral("TalQ"), QStringLiteral("TalQ"));
    if (s.value(QLatin1String(kKeySchema)).toInt() == kProbeSchemaVersion
        && s.value(QLatin1String(kKeySignature)).toString() == sig) {
        qInfo().noquote() << "HwEncoderProbe: fresh cache present ["
                          << s.value(QLatin1String(kKeySummary)).toString()
                          << "] — no re-probe";
        return;
    }
    const QString exe = QCoreApplication::applicationFilePath();
    const bool ok = QProcess::startDetached(
        exe, QStringList{ QStringLiteral("--probe-hwcodec") });
    qInfo().noquote() << "HwEncoderProbe: launched background probe"
                      << (ok ? "OK" : "FAILED") << "for GPU" << sig;
}

int talqRunHwEncoderProbe(int argc, char **argv)
{
    setGstPluginPathFromExe(argc > 0 ? argv[0] : nullptr);
    gst_init(&argc, &argv);

    static ProbeAccumulator acc;      // process lifetime — the watchdog references it
    g_acc = &acc;
    acc.signature = gpuSignature();

    // Hard watchdog (detached): guarantees this probe process can NEVER linger,
    // even if tearing down a wedged hardware encoder blocks the main thread. It
    // persists whatever was definitively classified before the wedge (untested
    // candidates stay out of `failed` → fail-open) then hard-exits.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(25));
        persistProbeResult();
#ifdef Q_OS_WIN
        ::TerminateProcess(::GetCurrentProcess(), 0);
#else
        std::_Exit(0);
#endif
    }).detach();

    if (controlPipelineSane()) {
        for (const char *cand : kCandidates)
            probeAndRecord(cand, acc);
    } else {
        QMutexLocker lk(&acc.mtx);
        acc.envSane = false;   // record NOTHING as failed → live path keeps full ladder
    }

    persistProbeResult();
    return 0;
}
