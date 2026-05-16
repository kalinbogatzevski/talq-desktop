#pragma once

#include <QString>
#include <QDateTime>

// A scheduled reminder tied to a chat message. Originates from NC Talk today;
// future sources (ERP integration) can produce these same entries so the
// upcoming-reminders view can merge across origins.
struct Reminder {
    enum Source { NextcloudTalk, Erp };

    Source    source = NextcloudTalk;
    QDateTime when;          // local time the reminder fires
    QString   token;         // NC Talk conversation token (empty for non-NC sources)
    int       messageId = 0; // chat message the reminder is tied to
    QString   actorName;     // original message author (for preview)
    QString   messageText;   // original message text (for preview)
};
