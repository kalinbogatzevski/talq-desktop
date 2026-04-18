#pragma once

#include <QString>

struct MentionCandidate {
    QString id;          // e.g. "alice", "all"
    QString label;       // display name, e.g. "Alice Smith"
    QString source;      // "users" | "groups" | "calls" | "guests"
};
