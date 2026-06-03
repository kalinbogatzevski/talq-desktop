#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <gst/gst.h>

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
    // already running. Returns false if the capture pipeline couldn't start
    // (which is itself a useful signal: the meter will stay at zero).
    bool start(const QString &deviceId);
    void stop();
    bool isRunning() const { return m_pipeline != nullptr; }

signals:
    // Smoothed capture level, 0.0 (silence) .. 1.0 (loud).
    void level(double value);

private slots:
    void pollBus();

private:
    void cleanup();

    GstElement *m_pipeline = nullptr;
    QTimer      m_busTimer;
};
