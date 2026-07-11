#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QFuture>
#include <gst/gst.h>

#include <atomic>

/**
 * MicTester — a tiny, self-contained capture pipeline used by the Settings
 * dialog to let the user CONFIRM their selected microphone actually works:
 * speak, and the level meter moves. It captures the SAME GStreamer device the
 * call would use (wasapi2src with the device id from MediaDeviceManager), so
 * "the meter moves" really means "this device will work in a call".
 *
 * Pipeline: wasapi2src [device=id] -> audioconvert -> audioresample ->
 *           level -> fakesink. The `level` element posts peak-dB messages on
 *           the bus; a QTimer polls the bus and emits level() in 0..1.
 *
 * Lifecycle is owned by the caller: start(deviceId) on dialog-open / device
 * change, stop() on dialog-close. Safe to start()/stop() repeatedly.
 */
class MicTester : public QObject
{
    Q_OBJECT
public:
    explicit MicTester(QObject *parent = nullptr);
    ~MicTester() override;

    // Begin capturing `deviceId` (empty = system default). Restarts cleanly if
    // already running. NON-BLOCKING: the wasapi2src device open runs on a
    // worker thread (cold WASAPI init + first-time plugin-DLL load blocks
    // set_state ~seconds and must never freeze the UI); the meter starts
    // moving a moment later once the device is open. Always returns true (the
    // async open reports failure by simply leaving the meter at rest).
    bool start(const QString &deviceId);
    void stop();
    bool isRunning() const { return m_pipeline != nullptr; }

signals:
    // Smoothed capture level, 0.0 (silence) .. 1.0 (loud).
    void level(double value);

private slots:
    void pollBus();

private:
    // Worker-thread helper: build the capture chain and open the device
    // (the slow, blocking part). Tries `deviceId`, falling back to the
    // system default. Returns a PLAYING pipeline, or nullptr on failure.
    // Touches no member state — safe to run detached from any thread.
    static GstElement *buildAndOpen(const QString &deviceId);

    // Fully tear the pipeline down (device change / destruction). stop() keeps
    // the built pipeline resident and only closes the device, so a re-open
    // avoids the multi-second gst_element_factory_make("wasapi2src").
    void teardownPipeline();

    GstElement *m_pipeline = nullptr;
    QString     m_pipelineDevice;      // device the resident pipeline was built for
    bool        m_pipelineReady = false;  // m_pipeline exists and reached PLAYING once
    QTimer      m_busTimer;

    // Generation token: bumped by every cleanup()/start()/stop() so an
    // async open still in flight knows it has been superseded and must
    // discard the pipeline it built instead of adopting it. Guards against
    // start-then-close and rapid device-change races.
    std::atomic<quint64> m_startGen{0};
    QFuture<void>        m_startFuture;   // last async open (detached; not joined)
};
