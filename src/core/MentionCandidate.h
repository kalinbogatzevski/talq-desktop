#pragma once

#include <QString>
#include <QStringView>

struct MentionCandidate {
    enum class Source { Unknown, Users, Groups, Calls, Guests };

    QString id;          // e.g. "alice", "all"
    QString label;       // display name, e.g. "Alice Smith"
    Source  source = Source::Unknown;

    static Source parseSource(QStringView s) {
        if (s == u"users")  return Source::Users;
        if (s == u"groups") return Source::Groups;
        if (s == u"calls")  return Source::Calls;
        if (s == u"guests") return Source::Guests;
        return Source::Unknown;
    }
};
