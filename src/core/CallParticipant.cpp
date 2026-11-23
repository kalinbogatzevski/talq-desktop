// CallParticipant is mostly header-only (inline); this TU generates the
// meta-object (AUTOMOC) and holds the setters that log.
#include "core/CallParticipant.h"

// Mute changes are logged (one line per CHANGE, not per call): until 2026-11-02 a
// tile could sit on "Camera off" for a whole call and nothing in the log said
// when or why it was muted.
void CallParticipant::setAudioMuted(bool v)
{
    if (m_audioMuted == v) return;
    m_audioMuted = v;
    qInfo().noquote() << "CallParticipant"
                      << (m_isSelf ? QStringLiteral("self") : m_sessionId.left(8))
                      << "audio muted ->" << v;
    emit changed();
}

void CallParticipant::setVideoMuted(bool v)
{
    const bool wasUnknown = m_videoUnknown;
    m_videoUnknown = false;
    if (m_videoMuted == v) {
        if (wasUnknown) {
            qInfo().noquote() << "CallParticipant" << m_sessionId.left(8)
                              << "video muted ->" << v << "(was unknown)";
            emit changed();
        }
        return;
    }
    m_videoMuted = v;
    {   // the line is written when `line` goes out of scope, before changed()
        QDebug line = qInfo().noquote();
        line << "CallParticipant" << (m_isSelf ? QStringLiteral("self") : m_sessionId.left(8))
             << "video muted ->" << v;
        if (wasUnknown) line << "(was unknown)";
    }
    emit changed();
}

void CallParticipant::markVideoUnknown()
{
    if (m_videoUnknown && m_videoMuted) return;
    m_videoMuted = true;   // what every watchdog reads: an unknown peer counts as camera-off
    m_videoUnknown = true;
    qInfo().noquote() << "CallParticipant"
                      << (m_isSelf ? QStringLiteral("self") : m_sessionId.left(8))
                      << "video unknown (no report yet)";
    emit changed();
}
