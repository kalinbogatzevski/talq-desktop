#pragma once

#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QVector>
#include <QTimer>
#include <gst/gst.h>

// One discrete capture mode a camera advertises (one gst_device_get_caps
// structure, deduped to the best fps per format+resolution). `mjpeg` =
// image/jpeg (compressed, fits USB bandwidth at HD); otherwise video/x-raw.
struct CameraMode {
    bool mjpeg = false;
    int width = 0;
    int height = 0;
    int fpsNum = 0;   // exact advertised framerate (e.g. 30000/1001)
    int fpsDen = 1;

    int fps() const { return fpsDen ? (fpsNum + fpsDen / 2) / fpsDen : 0; }
    qint64 pixels() const { return qint64(width) * height; }

    // Stable token for QSettings persistence of the user's explicit pick.
    QString key() const {
        return QStringLiteral("%1|%2|%3|%4|%5")
            .arg(mjpeg ? "mjpeg" : "raw").arg(width).arg(height)
            .arg(fpsNum).arg(fpsDen);
    }
    // Exact single-structure caps for mfvideosrc. A single fixed structure
    // is what actually forces the mode — a permissive multi-structure menu
    // lets mfvideosrc fall back to its native (often raw, low-fps) format.
    QString srcCaps() const {
        return QStringLiteral("%1,width=(int)%2,height=(int)%3,"
                              "framerate=(fraction)%4/%5")
            .arg(mjpeg ? "image/jpeg" : "video/x-raw")
            .arg(width).arg(height).arg(fpsNum).arg(fpsDen);
    }
    // "1080p · 60fps · MJPEG" style label for the Settings combo.
    QString label() const;
    bool valid() const { return width > 0 && height > 0 && fpsNum > 0; }

    // Inverse of key(); empty/invalid input → an invalid CameraMode.
    // Used to rehydrate the persistent per-device modes cache that
    // stabilizes the picker across Windows' non-deterministic
    // GstDeviceMonitor enumeration.
    static CameraMode fromKey(const QString &k);
};

struct MediaDevice {
    QString id;
    QString name;
    QString type;  // "audio-input", "audio-output", "video-input"
    QVector<CameraMode> modes;  // video-input only; curated + sorted best-first
};

class MediaDeviceManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList audioInputNames READ audioInputNames NOTIFY devicesChanged)
    Q_PROPERTY(QStringList audioOutputNames READ audioOutputNames NOTIFY devicesChanged)
    Q_PROPERTY(QStringList videoInputNames READ videoInputNames NOTIFY devicesChanged)

public:
    explicit MediaDeviceManager(QObject *parent = nullptr);
    ~MediaDeviceManager() override;

    Q_INVOKABLE void refresh();
    // Non-blocking refresh: enumerates devices on a worker thread and emits
    // devicesChanged on completion (debounced). Used when opening Settings so the
    // dialog isn't blocked on the slow camera-capability probing.
    Q_INVOKABLE void refreshAsync();
    void saveDevices();
    void restoreDevices();

    Q_PROPERTY(int selectedAudioInput READ selectedAudioInput WRITE setSelectedAudioInput NOTIFY selectedChanged)
    Q_PROPERTY(int selectedAudioOutput READ selectedAudioOutput WRITE setSelectedAudioOutput NOTIFY selectedChanged)
    Q_PROPERTY(int selectedVideoInput READ selectedVideoInput WRITE setSelectedVideoInput NOTIFY selectedChanged)

    int selectedAudioInput() const { return m_selectedInput; }
    int selectedAudioOutput() const { return m_selectedOutput; }
    int selectedVideoInput() const { return m_selectedVideo; }
    void setSelectedAudioInput(int idx);
    void setSelectedAudioOutput(int idx);
    void setSelectedVideoInput(int idx);

    // Get selected device name for pipeline configuration
    QString selectedInputName() const;
    QString selectedOutputName() const;

    // Get selected device ID (strid/path) for pipeline configuration
    QString selectedInputDeviceId() const;
    QString selectedOutputDeviceId() const;

    QStringList audioInputNames() const;
    QStringList audioOutputNames() const;
    QStringList videoInputNames() const;

    const QVector<MediaDevice> &audioInputs() const { return m_audioInputs; }
    const QVector<MediaDevice> &audioOutputs() const { return m_audioOutputs; }
    const QVector<MediaDevice> &videoInputs() const { return m_videoInputs; }
    // True once a full device scan has completed. The Settings dialog uses this
    // to AVOID re-running the (expensive, camera-opening) scan on every open —
    // the live hotplug monitor already keeps the lists current, so a re-scan per
    // open only re-loads the MediaFoundation Frame-Server/camera DLLs cold and
    // freezes the UI on the loader lock.
    bool hasEnumerated() const { return m_hasEnumerated; }

    // --- Camera capability / quality selection ---
    // Curated, deduped, best-first mode list for a video device.
    QVector<CameraMode> cameraModes(int videoIdx) const;
    // Auto pick: absolute max the device supports — most pixels, then
    // highest fps, MJPEG preferred on ties.
    CameraMode autoCameraMode(int videoIdx) const;
    // Persisted user choice for the *selected* camera: empty/"auto" = Auto.
    QString cameraQualityChoice() const;
    void setCameraQualityChoice(const QString &key);  // "auto" or CameraMode::key()
    // Resolve the selected camera + choice to one exact mfvideosrc caps
    // string and persist it to Video/cameraSrcCaps. Called on device
    // refresh, camera switch, and quality change so the pipeline always
    // reads a current value even if Settings was never opened.
    void resolveAndPersistCameraSrcCaps();

signals:
    void devicesChanged();
    void selectedChanged();

private:
    int m_selectedInput = -1;   // -1 = system default
    int m_selectedOutput = -1;
    int m_selectedVideo = -1;
    QVector<MediaDevice> m_audioInputs;
    QVector<MediaDevice> m_audioOutputs;
    QVector<MediaDevice> m_videoInputs;
    QSettings m_settings;
    bool m_restoring = false;  // suppress saveDevices() during restoreDevices()
    bool m_refreshing = false; // a worker-thread refreshAsync() scan is in flight
    bool m_hasEnumerated = false; // a full device scan has completed at least once

    // D7 (0.53.x) — LIVE device hotplug. A persistent GstDeviceMonitor whose bus
    // sync-handler (fires on a GStreamer thread, no GLib main loop needed — works
    // on Windows) marshals DEVICE_ADDED/REMOVED to the Qt thread and arms a
    // debounce; on settle we refreshAsync() (-> devicesChanged). So plugging /
    // unplugging a camera or mic mid-call is noticed, the picker stays current,
    // and CallManager can auto-resume a camera that comes back. NOT auto-switching
    // the active device — just making the change available (Zoom/Teams behaviour).
    void startHotplugMonitor();
    static GstBusSyncReply onMonitorBusSync(GstBus *, GstMessage *, gpointer);
    GstDeviceMonitor *m_liveMonitor = nullptr;
    QTimer m_hotplugDebounce;
};
