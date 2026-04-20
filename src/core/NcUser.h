#pragma once

#include <QString>

struct NcUser {
    QString id;           // NC user id (what goes on the wire)
    QString displayName;  // human-readable label
};
