#include "core/MediaDeviceManager.h"
#include <QDebug>
#include <QPointer>
#include <QCoreApplication>
#include <algorithm>
#include <memory>
#include <thread>

CameraMode CameraMode::fromKey(const QString &k)
{
    CameraMode m;
    const QStringList p = k.split(QLatin1Char('|'));
    if (p.size() != 5) return m;
    m.mjpeg  = (p[0] == QStringLiteral("mjpeg"));
    bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
    m.width  = p[1].toInt(&ok1);
    m.height = p[2].toInt(&ok2);
    m.fpsNum = p[3].toInt(&ok3);
    m.fpsDen = p[4].toInt(&ok4);
    if (!(ok1 && ok2 && ok3 && ok4) || m.fpsDen <= 0) return CameraMode{};
    return m;
}

QString CameraMode::label() const
{
    QString res;
    switch (height) {
        case 2160: res = "4K";    break;
        case 1440: res = "1440p"; break;
        case 1080: res = "1080p"; break;
        case 720:  res = "720p";  break;
        case 480:  res = "480p";  break;
        case 360:  res = "360p";  break;
        default:   res = QStringLiteral("%1×%2").arg(width).arg(height);
    }
    QString s = QStringLiteral("%1 · %2fps").arg(res).arg(fps());
    if (mjpeg) s += QStringLiteral(" · MJPEG");
    return s;
}

namespace {

// Largest integer a width/height field can resolve to (fixed int or the
// max of an int-range — mfvideosrc occasionally exposes ranged dimensions).
int fieldMaxInt(const GValue *v)
{
    if (!v) return 0;
    if (G_VALUE_HOLDS_INT(v)) return g_value_get_int(v);
    if (GST_VALUE_HOLDS_INT_RANGE(v)) return gst_value_get_int_range_max(v);
    return 0;
}

// Highest framerate a framerate field offers (fraction / fraction-range /
// list of fractions). Returns num/den of that maximum.
void fieldMaxFraction(const GValue *v, int *num, int *den)
{
    *num = 0; *den = 1;
    if (!v) return;
    auto consider = [&](int n, int d) {
        if (d > 0 && qint64(n) * (*den) > qint64(*num) * d) { *num = n; *den = d; }
    };
    if (GST_VALUE_HOLDS_FRACTION(v)) {
        consider(gst_value_get_fraction_numerator(v),
                 gst_value_get_fraction_denominator(v));
    } else if (GST_VALUE_HOLDS_FRACTION_RANGE(v)) {
        const GValue *mx = gst_value_get_fraction_range_max(v);
        consider(gst_value_get_fraction_numerator(mx),
                 gst_value_get_fraction_denominator(mx));
    } else if (GST_VALUE_HOLDS_LIST(v)) {
        for (guint i = 0; i < gst_value_list_get_size(v); ++i) {
            const GValue *e = gst_value_list_get_value(v, i);
            if (GST_VALUE_HOLDS_FRACTION(e))
                consider(gst_value_get_fraction_numerator(e),
                         gst_value_get_fraction_denominator(e));
        }
    }
}

void sortModesBestFirst(QVector<CameraMode> &m)
{
    std::sort(m.begin(), m.end(), [](const CameraMode &a, const CameraMode &b) {
        if (a.pixels() != b.pixels()) return a.pixels() > b.pixels();
        if (a.fps()    != b.fps())    return a.fps()    > b.fps();
        return a.mjpeg && !b.mjpeg;   // MJPEG preferred on ties
    });
}

// Union `add` into `into` (one entry per format+resolution; existing kept
// since its fps is already the best-per-res), then re-sort best-first.
// Used to merge the caps of the SAME physical camera enumerated by
// multiple Windows device providers (MF + KS/DirectShow).
void mergeCameraModes(QVector<CameraMode> &into, const QVector<CameraMode> &add)
{
    for (const CameraMode &m : add) {
        bool dup = false;
        for (const CameraMode &e : into)
            if (e.mjpeg == m.mjpeg && e.width == m.width && e.height == m.height) {
                dup = true; break;
            }
        if (!dup) into.append(m);
    }
    sortModesBestFirst(into);
}

// Truncated raw caps, logged only when a camera yields zero parseable
// modes — turns "only Auto" field reports into authoritative evidence
// without another round trip.
QString capsSummary(GstDevice *dev)
{
    GstCaps *c = gst_device_get_caps(dev);
    if (!c) return QStringLiteral("(null caps)");
    gchar *s = gst_caps_to_string(c);
    QString out = s ? QString::fromUtf8(s).left(500) : QStringLiteral("(empty)");
    g_free(s);
    gst_caps_unref(c);
    return out;
}

// Enumerate a camera's advertised modes, then curate: keep only the best
// fps per (format, resolution), sort absolute-best first (most pixels →
// highest fps → MJPEG preferred). This is the "real capacity" list and
// the basis for the Auto pick.
QVector<CameraMode> parseCameraModes(GstDevice *dev)
{
    QVector<CameraMode> modes;
    GstCaps *caps = gst_device_get_caps(dev);
    if (!caps) return modes;

    for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
        GstStructure *s = gst_caps_get_structure(caps, i);
        const gchar *name = gst_structure_get_name(s);
        const bool mjpeg = name && g_str_equal(name, "image/jpeg");
        const bool raw   = name && g_str_equal(name, "video/x-raw");
        if (!mjpeg && !raw) continue;  // skip exotic compressed formats

        CameraMode m;
        m.mjpeg = mjpeg;
        m.width  = fieldMaxInt(gst_structure_get_value(s, "width"));
        m.height = fieldMaxInt(gst_structure_get_value(s, "height"));
        fieldMaxFraction(gst_structure_get_value(s, "framerate"),
                         &m.fpsNum, &m.fpsDen);
        if (!m.valid()) continue;

        // Dedup: collapse to the best fps for each format+resolution.
        bool merged = false;
        for (CameraMode &e : modes) {
            if (e.mjpeg == m.mjpeg && e.width == m.width && e.height == m.height) {
                if (qint64(m.fpsNum) * e.fpsDen > qint64(e.fpsNum) * m.fpsDen) {
                    e.fpsNum = m.fpsNum; e.fpsDen = m.fpsDen;
                }
                merged = true;
                break;
            }
        }
        if (!merged) modes.append(m);
    }
    gst_caps_unref(caps);

    sortModesBestFirst(modes);
    return modes;
}

}  // namespace

MediaDeviceManager::MediaDeviceManager(QObject *parent)
    : QObject(parent)
{
}

MediaDeviceManager::~MediaDeviceManager() = default;

// Pure enumeration into the three lists via a fresh one-shot GstDeviceMonitor.
// No member access; the free helpers (parseCameraModes/mergeCameraModes) and the
// local QSettings camera-caps cache are thread-safe (Qt serialises QSettings),
// so this ALSO runs on a worker thread (refreshAsync) to keep the Settings dialog
// from blocking on the slow camera-capability probing.
static void enumerateDevicesImpl(QVector<MediaDevice> &aIn,
                                 QVector<MediaDevice> &aOut,
                                 QVector<MediaDevice> &vid)
{
    GstDeviceMonitor *monitor = gst_device_monitor_new();

    gst_device_monitor_add_filter(monitor, "Audio/Source", nullptr);
    gst_device_monitor_add_filter(monitor, "Audio/Sink", nullptr);
    gst_device_monitor_add_filter(monitor, "Video/Source", nullptr);

    if (!gst_device_monitor_start(monitor)) {
        qWarning() << "MediaDeviceManager: failed to start device monitor";
        gst_object_unref(monitor);
        return;
    }

    GList *devices = gst_device_monitor_get_devices(monitor);
    for (GList *it = devices; it; it = it->next) {
        GstDevice *dev = GST_DEVICE(it->data);
        gchar *name = gst_device_get_display_name(dev);
        gchar *cls = gst_device_get_device_class(dev);

        MediaDevice md;
        md.name = QString::fromUtf8(name);
        gboolean isLoopback = FALSE;
        GstStructure *props = gst_device_get_properties(dev);
        if (props) {
            const gchar *strid = gst_structure_get_string(props, "device.strid");
            if (!strid)
                strid = gst_structure_get_string(props, "device.path");
            // wasapi2 (the Windows audio provider) exposes neither strid nor
            // path — its real, openable id is `device.id` ({GUID} form, exactly
            // what `wasapi2src device=...` expects). Falling back to the friendly
            // NAME here is what made wasapi2src fail to open the selected device
            // (mic test stayed flat; calls fell back to default) — read device.id
            // before giving up to the name.
            if (!strid)
                strid = gst_structure_get_string(props, "device.id");
            md.id = strid ? QString::fromUtf8(strid) : md.name;
            // wasapi2 also lists RENDER endpoints (speakers) as Audio/Source for
            // loopback capture; those are NOT microphones (loopback=true). Flag
            // them so they stay out of the mic picker.
            gst_structure_get_boolean(props, "wasapi2.device.loopback", &isLoopback);
            gst_structure_free(props);
        } else {
            md.id = md.name;
        }

        QString deviceClass = QString::fromUtf8(cls);
        if (deviceClass.contains("Source") && deviceClass.contains("Audio")) {
            if (isLoopback) {
                // Speaker exposed as a loopback source — skip; it's not a mic.
                qInfo() << "MediaDeviceManager: skipping loopback (speaker) source"
                        << md.name;
            } else {
                md.type = "audio-input";
                aIn.append(md);
            }
        } else if (deviceClass.contains("Sink") && deviceClass.contains("Audio")) {
            md.type = "audio-output";
            aOut.append(md);
        } else if (deviceClass.contains("Source") && deviceClass.contains("Video")) {
            md.type = "video-input";
            md.modes = parseCameraModes(dev);
            // Persistent per-device modes cache. GstDeviceMonitor on Windows
            // is non-deterministic across runs and especially after camera
            // use (MF/KS providers shift which caps they expose). Union
            // live modes with the cumulative cache so a once-seen MJPEG /
            // 30fps row stays in the picker forever instead of vanishing
            // between calls — and a persisted user pick stays visible.
            QString cacheKey = md.id;
            cacheKey.replace(QLatin1Char('/'),  QLatin1Char('_'));
            cacheKey.replace(QLatin1Char('\\'), QLatin1Char('_'));
            {
                QSettings cs("TalQ", "TalQ");
                cs.beginGroup("Video/CameraCaps");
                const QStringList cached = cs.value(cacheKey).toStringList();
                cs.endGroup();
                QVector<CameraMode> cachedModes;
                for (const QString &k : cached) {
                    CameraMode m = CameraMode::fromKey(k);
                    if (m.valid()) cachedModes.append(m);
                }
                mergeCameraModes(md.modes, cachedModes);

                QStringList toSave;
                toSave.reserve(md.modes.size());
                for (const CameraMode &m : md.modes) toSave << m.key();
                cs.beginGroup("Video/CameraCaps");
                cs.setValue(cacheKey, toSave);
                cs.endGroup();
            }
            // Windows enumerates the SAME physical camera once per backend
            // provider (Media Foundation + KS/DirectShow). Collapse by
            // display name and UNION the capability sets so the modes
            // survive even when the provider instance that would otherwise
            // be selected exposed none (the "only Auto + duplicated
            // camera" field report).
            int existing = -1;
            for (int i = 0; i < vid.size(); ++i)
                if (vid[i].name == md.name) { existing = i; break; }
            if (existing >= 0) {
                mergeCameraModes(vid[existing].modes, md.modes);
                MediaDevice &e = vid[existing];
                if ((e.id.isEmpty() || e.id == e.name)
                    && !md.id.isEmpty() && md.id != md.name)
                    e.id = md.id;   // prefer a real strid/path over the name
            } else {
                vid.append(md);
            }
            qInfo().nospace() << "MediaDeviceManager: video '" << md.name
                << "' (this provider) modes=" << md.modes.size()
                << (md.modes.isEmpty()
                      ? QStringLiteral(" RAWCAPS=") + capsSummary(dev)
                      : QString());
        }

        g_free(name);
        g_free(cls);
        gst_object_unref(dev);
    }
    g_list_free(devices);

    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);

    qDebug() << "MediaDeviceManager: enumerated"
             << aIn.size() << "mic(s),"
             << aOut.size() << "speaker(s),"
             << vid.size() << "camera(s)";
}

void MediaDeviceManager::refresh()
{
    // Synchronous — used by the explicit "Refresh devices" button. Enumerate
    // into the live lists, then re-apply the saved selection + persist camera caps.
    m_audioInputs.clear(); m_audioOutputs.clear(); m_videoInputs.clear();
    enumerateDevicesImpl(m_audioInputs, m_audioOutputs, m_videoInputs);
    emit devicesChanged();
    restoreDevices();
    resolveAndPersistCameraSrcCaps();
}

void MediaDeviceManager::refreshAsync()
{
    // Non-blocking — for opening Settings. The slow GstDeviceMonitor enumeration
    // (camera capability probing especially) runs on a worker thread so the dialog
    // pops instantly with the cached list; the combos update via devicesChanged
    // when the scan lands. Debounced so a flurry of opens spawns one scan.
    if (m_refreshing) return;
    m_refreshing = true;
    QPointer<MediaDeviceManager> guard(this);
    std::thread([guard]() {
        auto *aIn  = new QVector<MediaDevice>();
        auto *aOut = new QVector<MediaDevice>();
        auto *vid  = new QVector<MediaDevice>();
        enumerateDevicesImpl(*aIn, *aOut, *vid);
        // Hop back to the main thread to swap in the results + run the UI-thread
        // tail (selection match + caps persist + signal). qApp is always alive;
        // the QPointer bails if the manager itself was destroyed mid-scan.
        QMetaObject::invokeMethod(qApp, [guard, aIn, aOut, vid]() {
            std::unique_ptr<QVector<MediaDevice>> a(aIn), b(aOut), v(vid);
            if (!guard) return;
            guard->m_audioInputs  = std::move(*a);
            guard->m_audioOutputs = std::move(*b);
            guard->m_videoInputs  = std::move(*v);
            guard->m_refreshing   = false;
            emit guard->devicesChanged();
            guard->restoreDevices();
            guard->resolveAndPersistCameraSrcCaps();
        }, Qt::QueuedConnection);
    }).detach();
}

void MediaDeviceManager::setSelectedAudioInput(int idx)
{
    if (m_selectedInput != idx) {
        m_selectedInput = idx;
        emit selectedChanged();
        if (!m_restoring) saveDevices();
    }
}

void MediaDeviceManager::setSelectedAudioOutput(int idx)
{
    if (m_selectedOutput != idx) {
        m_selectedOutput = idx;
        emit selectedChanged();
        if (!m_restoring) saveDevices();
    }
}

void MediaDeviceManager::setSelectedVideoInput(int idx)
{
    if (m_selectedVideo != idx) {
        m_selectedVideo = idx;
        emit selectedChanged();
        if (!m_restoring) {
            saveDevices();
            resolveAndPersistCameraSrcCaps();
        }
    }
}

QVector<CameraMode> MediaDeviceManager::cameraModes(int videoIdx) const
{
    if (videoIdx >= 0 && videoIdx < m_videoInputs.size())
        return m_videoInputs[videoIdx].modes;
    return {};
}

CameraMode MediaDeviceManager::autoCameraMode(int videoIdx) const
{
    // modes are sorted absolute-best-first (pixels → fps → MJPEG).
    const QVector<CameraMode> m = cameraModes(videoIdx);
    return m.isEmpty() ? CameraMode{} : m.first();
}

QString MediaDeviceManager::cameraQualityChoice() const
{
    QSettings s("TalQ", "TalQ");
    s.beginGroup("Video");
    const QString v = s.value("cameraQuality", "auto").toString();
    s.endGroup();
    return v.isEmpty() ? QStringLiteral("auto") : v;
}

void MediaDeviceManager::setCameraQualityChoice(const QString &key)
{
    QSettings s("TalQ", "TalQ");
    s.beginGroup("Video");
    s.setValue("cameraQuality", key.isEmpty() ? QStringLiteral("auto") : key);
    s.endGroup();
    resolveAndPersistCameraSrcCaps();
}

void MediaDeviceManager::resolveAndPersistCameraSrcCaps()
{
    const int idx = m_selectedVideo >= 0 ? m_selectedVideo
                  : (m_videoInputs.isEmpty() ? -1 : 0);
    const QVector<CameraMode> modes = cameraModes(idx);

    CameraMode chosen;
    const QString choice = cameraQualityChoice();
    if (choice != "auto") {
        for (const CameraMode &m : modes)
            if (m.key() == choice) { chosen = m; break; }
    }
    // CRITICAL (0.31.0 Zenbook regression): a forced exact caps that the
    // actually-opened mfvideosrc instance cannot negotiate kills the
    // camera outright — no LED, no preview, no capture. Auto therefore
    // NEVER hard-forces: empty caps → PublishPipeline's permissive
    // negotiation, which always starts the camera (the long-stable
    // pre-0.30.12 behavior). Only an EXPLICIT user pick that exists on
    // this camera forces its exact mode — opt-in and reversible in
    // Settings (revert to Auto = guaranteed-working escape hatch).
    const QString caps = chosen.valid() ? chosen.srcCaps() : QString();

    QSettings s("TalQ", "TalQ");
    s.beginGroup("Video");
    s.setValue("cameraSrcCaps", caps);
    s.endGroup();
    qDebug() << "MediaDeviceManager: cameraSrcCaps ->"
             << (caps.isEmpty() ? QStringLiteral("(auto: permissive negotiate)")
                                 : caps)
             << "[choice:" << choice << "idx:" << idx << "]";
}

QString MediaDeviceManager::selectedInputName() const
{
    if (m_selectedInput >= 0 && m_selectedInput < m_audioInputs.size())
        return m_audioInputs[m_selectedInput].name;
    return {};
}

QString MediaDeviceManager::selectedOutputName() const
{
    if (m_selectedOutput >= 0 && m_selectedOutput < m_audioOutputs.size())
        return m_audioOutputs[m_selectedOutput].name;
    return {};
}

static QStringList deviceNames(const QVector<MediaDevice> &devices)
{
    QStringList names;
    names.reserve(devices.size());
    for (const auto &d : devices)
        names << d.name;
    return names;
}

QStringList MediaDeviceManager::audioInputNames() const  { return deviceNames(m_audioInputs); }
QStringList MediaDeviceManager::audioOutputNames() const { return deviceNames(m_audioOutputs); }
QStringList MediaDeviceManager::videoInputNames() const  { return deviceNames(m_videoInputs); }

QString MediaDeviceManager::selectedInputDeviceId() const
{
    if (m_selectedInput >= 0 && m_selectedInput < m_audioInputs.size())
        return m_audioInputs[m_selectedInput].id;
    return {};
}

QString MediaDeviceManager::selectedOutputDeviceId() const
{
    if (m_selectedOutput >= 0 && m_selectedOutput < m_audioOutputs.size())
        return m_audioOutputs[m_selectedOutput].id;
    return {};
}

void MediaDeviceManager::saveDevices()
{
    m_settings.beginGroup("Devices");
    if (m_selectedInput >= 0 && m_selectedInput < m_audioInputs.size()) {
        m_settings.setValue("audioInputName", m_audioInputs[m_selectedInput].name);
        m_settings.setValue("audioInputId", m_audioInputs[m_selectedInput].id);
    }
    if (m_selectedOutput >= 0 && m_selectedOutput < m_audioOutputs.size()) {
        m_settings.setValue("audioOutputName", m_audioOutputs[m_selectedOutput].name);
        m_settings.setValue("audioOutputId", m_audioOutputs[m_selectedOutput].id);
    }
    if (m_selectedVideo >= 0 && m_selectedVideo < m_videoInputs.size()) {
        m_settings.setValue("videoInputName", m_videoInputs[m_selectedVideo].name);
        m_settings.setValue("videoInputId", m_videoInputs[m_selectedVideo].id);
    }
    m_settings.endGroup();
}

void MediaDeviceManager::restoreDevices()
{
    m_restoring = true;
    m_settings.beginGroup("Devices");
    auto matchDevice = [](const QVector<MediaDevice> &list, const QString &name, const QString &id) -> int {
        QVector<int> nameMatches;
        for (int i = 0; i < list.size(); ++i) {
            if (list[i].name == name)
                nameMatches.append(i);
        }
        if (nameMatches.size() == 1)
            return nameMatches.first();
        if (nameMatches.size() > 1) {
            for (int idx : nameMatches) {
                if (list[idx].id == id)
                    return idx;
            }
            return nameMatches.first();
        }
        return -1;
    };

    QString inName = m_settings.value("audioInputName").toString();
    QString inId = m_settings.value("audioInputId").toString();
    if (!inName.isEmpty()) {
        int idx = matchDevice(m_audioInputs, inName, inId);
        if (idx >= 0) setSelectedAudioInput(idx);
    }

    QString outName = m_settings.value("audioOutputName").toString();
    QString outId = m_settings.value("audioOutputId").toString();
    if (!outName.isEmpty()) {
        int idx = matchDevice(m_audioOutputs, outName, outId);
        if (idx >= 0) setSelectedAudioOutput(idx);
    }

    QString vidName = m_settings.value("videoInputName").toString();
    QString vidId = m_settings.value("videoInputId").toString();
    if (!vidName.isEmpty()) {
        int idx = matchDevice(m_videoInputs, vidName, vidId);
        if (idx >= 0) setSelectedVideoInput(idx);
    }

    m_settings.endGroup();
    m_restoring = false;
    qDebug() << "MediaDeviceManager: restored devices — mic:" << m_selectedInput
             << "speaker:" << m_selectedOutput << "camera:" << m_selectedVideo;
}
