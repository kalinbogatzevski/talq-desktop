#pragma once

#include <QString>

struct NcUser {
    QString id;           // NC user id, group id, or circle id (what goes on the wire)
    QString displayName;  // human-readable label
    QString source;       // "users" | "groups" | "circles"
};
