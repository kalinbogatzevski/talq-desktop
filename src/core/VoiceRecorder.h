#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <gst/gst.h>

/**
 * Records the microphone to an mp3 on disk, for sending as a voice message.
 *
 * MP3 rather than the opus TalQ already carries in calls, and deliberately so:
 * a voice message is a FILE that other people's clients have to play, not a
 * stream this app controls both ends of. Nextcloud Talk names its own
 * recordings `voice-message.mp3`, and mp3 is the one audio format every
 * browser, phone and desktop in the fleet decodes without argument. The extra
 * bytes over opus are irrelevant at 64 kbit for speech.
 *
 * Mono at 48 kHz: speech, and half the size of stereo for no loss at all.
 *
 * The recorder owns only the pipeline and the clock. It does not upload, does
 * not know about rooms, and never deletes the file it wrote -- whoever asked
 * for the recording decides whether it gets sent or discarded.
 */
class VoiceRecorder : public QObject
{
    Q_OBJECT
public:
    explicit VoiceRecorder(QObject *parent = nullptr);
    ~VoiceRecorder() override;

    // Begin recording to `outPath`. Emits failed() and stays stopped if the
    // microphone or the encoder is unavailable.
    void start(const QString &outPath);
    // Stop and finalise. Emits finished(path, durationMs) once the file is
    // complete -- NOT when the pipeline is told to stop: an mp3 whose trailer
    // has not been written is a truncated file, and sending one of those is
    // worse than failing.
    void stop();
    // Stop and delete the file. Nothing is emitted except stateChanged.
    void cancel();

    bool isRecording() const { return m_recording; }
    qint64 elapsedMs()  const { return m_elapsedMs; }

signals:
    void stateChanged();
    void tick();                                   // elapsedMs advanced
    void finished(const QString &path, qint64 durationMs);
    void failed(const QString &reason);

private:
    void teardown(bool deleteFile);

    GstElement *m_pipeline = nullptr;
    QTimer      m_timer;
    QString     m_path;
    bool        m_recording = false;
    qint64      m_elapsedMs = 0;
};
