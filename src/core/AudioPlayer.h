#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <gst/gst.h>

/**
 * Plays ONE attachment at a time.
 *
 * Until 0.69.7 a voice message was unreachable in TalQ. It is an ordinary
 * `audio/*` attachment, so it fell to the generic file path -- which was the
 * broken download route, and even once that worked the only thing a click
 * could do was hand the file to whatever the OS opens .mp3 with. Leaving the
 * app to hear four seconds of speech is not playback.
 *
 * GStreamer rather than QtMultimedia: the whole GStreamer stack (including
 * gst-libav for mp3/aac decode and the wasapi2 sink) is already built,
 * deployed and initialised by this app, and QtMultimedia is neither linked nor
 * shipped. `playbin` picks the decoder and the sink itself, so this class is
 * only a lifecycle and a clock.
 *
 * ONE at a time by design: starting a second message stops the first. Two
 * voices talking over each other is never what was wanted, and it also means
 * exactly one bubble is ever in the playing state, so the painter never has to
 * reconcile several.
 */
class AudioPlayer : public QObject
{
    Q_OBJECT
public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    // Start `localPath`, tagged with the attachment it belongs to. Calling
    // this for the fileId already loaded resumes rather than restarting, so a
    // click on a paused bubble picks up where it stopped.
    void play(int fileId, const QString &localPath);
    void togglePause();
    void stop();
    // Jump to a fraction (0..1) of the clip. Ignored before the duration is
    // known -- seeking a stream of unknown length has no defined target.
    void seekFraction(double fraction);

    int  currentFileId() const { return m_fileId; }
    bool isPlaying()     const { return m_playing; }
    qint64 positionMs()  const { return m_positionMs; }
    qint64 durationMs()  const { return m_durationMs; }
    // 0 when nothing is loaded or the duration is not known yet, so a caller
    // can tell "no progress" from "at the start".
    double progress() const {
        return m_durationMs > 0 ? double(m_positionMs) / double(m_durationMs) : 0.0;
    }

signals:
    // Position advanced, or play/pause/stop changed. One signal for both: every
    // consumer is a repaint, and splitting them only makes callers connect to
    // both anyway.
    void changed();
    // Playback could not start or failed mid-clip. Carries a human-readable
    // reason -- a voice message that silently does nothing when clicked is the
    // bug this whole class exists to remove, so a failure must say so.
    void failed(int fileId, const QString &reason);

private:
    void teardown();
    void tick();

    GstElement *m_pipeline = nullptr;
    QTimer      m_timer;
    int         m_fileId = 0;
    QString     m_path;
    bool        m_playing = false;
    qint64      m_positionMs = 0;
    qint64      m_durationMs = 0;
};
