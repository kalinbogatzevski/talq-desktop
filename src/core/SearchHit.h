#pragma once
#include <QString>

struct SearchHit {
    int     messageId = 0;
    qint64  timestamp = 0;
    QString actorName;
    QString snippet;
};
