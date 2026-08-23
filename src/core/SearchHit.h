#pragma once
#include <QString>

struct SearchHit {
    // Which conversation the hit is in. Empty for a search scoped to the open
    // room (the caller already knows); set by the cross-conversation search,
    // where the whole point is that the hit is somewhere ELSE.
    QString conversationToken;
    QString conversationName;
    int     messageId = 0;
    qint64  timestamp = 0;
    QString actorName;
    QString snippet;
};
