#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>

class VideoFrameProvider;

/**
 * One person in a call (including yourself). CallManager owns these and
 * keeps them in sync with the signaling/pipeline layer; the call surface
 * renders from them. Additive over the legacy 1:1 getters: a P2P call is
 * simply self + one remote.
 *
 * A single coarse changed() drives repaint; videoProvidersChanged() is
 * separate so the surface only rebinds frame signals when a pointer
 * actually swaps (provider rebinds are the only expensive reaction).
 */
class CallParticipant : public QObject
{
    Q_OBJECT

public:
    enum ConnState { Connecting, Connected, Reconnecting, Failed };
    Q_ENUM(ConnState)

    CallParticipant(QString sessionId, bool isSelf, QObject *parent = nullptr)
        : QObject(parent), m_sessionId(std::move(sessionId)), m_isSelf(isSelf) {}

    QString sessionId() const { return m_sessionId; }
    bool isSelf() const { return m_isSelf; }
    QString displayName() const { return m_displayName; }
    QString peerId() const { return m_peerId; }
    QString peerClient() const { return m_peerClient; }
    bool audioMuted() const { return m_audioMuted; }
    bool videoMuted() const { return m_videoMuted; }
    bool screenSharing() const { return m_screenSharing; }
    VideoFrameProvider *camera() const { return m_camera; }
    VideoFrameProvider *screen() const { return m_screen; }
    ConnState connState() const { return m_connState; }
    bool speaking() const { return m_speaking; }
    double audioLevel() const { return m_audioLevel; }

    void setDisplayName(const QString &v) { if (v.isEmpty() || m_displayName == v) return; m_displayName = v; emit changed(); }
    void setPeerId(const QString &v)      { if (m_peerId == v) return; m_peerId = v; emit changed(); }
    void setPeerClient(const QString &v)  { if (m_peerClient == v) return; m_peerClient = v; emit changed(); }
    void setAudioMuted(bool v)            { if (m_audioMuted == v) return; m_audioMuted = v; emit changed(); }
    void setVideoMuted(bool v)            { if (m_videoMuted == v) return; m_videoMuted = v; emit changed(); }
    void setScreenSharing(bool v)         { if (m_screenSharing == v) return; m_screenSharing = v; emit changed(); }
    void setConnState(ConnState v)        { if (m_connState == v) return; m_connState = v; emit changed(); }
    void setSpeaking(bool v)              { if (m_speaking == v) return; m_speaking = v; emit changed(); }

    void setAudioLevel(double v) {
        // Coarse: the surface only needs level for a ring; avoid a repaint
        // storm by ignoring sub-perceptual deltas.
        if (qAbs(m_audioLevel - v) < 0.03) return;
        m_audioLevel = v;
        emit changed();
    }

    // Active-speaker detection from the LIVE audio level, so the speaking frame
    // works from ACTUAL audio rather than only a peer's self-reported
    // speaking/stoppedSpeaking signal (sent unreliably by some clients). A short
    // hold keeps the frame steady between words instead of flickering on/off.
    void noteAudioLevelForVad(double level) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (level > kSpeakOnLevel) {                 // above the noise floor → possibly talking
            m_lastLoudMs = now;
            if (m_loudSinceMs < 0) m_loudSinceMs = now;
            // Require SUSTAINED loudness before flagging speech, so low-level mic
            // noise or a brief click can't strobe the speaking frame on non-stop.
            if (!m_speaking && now - m_loudSinceMs >= kSpeakOnsetMs) setSpeaking(true);
        } else {
            m_loudSinceMs = -1;                      // dropped below → restart the onset window
            if (m_speaking && now - m_lastLoudMs > kSpeakHoldMs) setSpeaking(false);  // quiet → stopped
        }
    }

    void setCamera(VideoFrameProvider *p) {
        if (m_camera == p) return;
        m_camera = p;
        emit videoProvidersChanged();
        emit changed();
    }
    void setScreen(VideoFrameProvider *p) {
        if (m_screen == p) return;
        m_screen = p;
        emit videoProvidersChanged();
        emit changed();
    }

signals:
    void changed();
    void videoProvidersChanged();

private:
    QString m_sessionId;
    bool m_isSelf = false;
    QString m_displayName;
    QString m_peerId;
    QString m_peerClient;
    bool m_audioMuted = true;
    bool m_videoMuted = true;
    bool m_screenSharing = false;
    VideoFrameProvider *m_camera = nullptr;
    VideoFrameProvider *m_screen = nullptr;
    ConnState m_connState = Connecting;
    bool m_speaking = false;
    double m_audioLevel = 0.0;
    qint64 m_lastLoudMs = 0;                  // VAD: last time level crossed kSpeakOnLevel
    qint64 m_loudSinceMs = -1;                // VAD: when level first crossed on (onset debounce)
    static constexpr double kSpeakOnLevel = 0.20;  // 0..1 — clearly above the mic noise floor (was 0.08: noise strobed the frame)
    static constexpr qint64 kSpeakOnsetMs = 150;   // must stay above kSpeakOnLevel this long to count as speech
    static constexpr qint64 kSpeakHoldMs  = 500;   // keep the frame this long after quiet
};
