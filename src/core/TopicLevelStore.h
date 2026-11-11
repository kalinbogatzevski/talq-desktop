// A topic's own notification level, remembered (src/models/ThreadListModel.cpp).
//
// The server stores it per user and per topic (0 default / 1 all / 2 mentions only / 3 never) and
// hands it out with the list of the user's RECENT topics -- at most the 50 most recently active,
// and only the level, nothing about reading. TalQ used to throw it away: the topic list is rebuilt
// from the server on every inbound batch, on every switch of chat and on every start, and the
// rebuild never copied the level over. It lived in memory from the moment the user chose it until
// the next rebuild, so the menu showed "Default" again after a switch of chat, a silenced topic
// lost its mark, and the user silenced it again and again.
//
// So the model keeps a per-room map of the levels it knows, saved with QSettings like the hidden
// topics. The rule for combining it with the server's answer is here, pure, so it is tested:
//   * a topic the server LISTED takes the server's level (level 0 = default = forget it): the user
//     may have changed it on the phone or in the browser;
//   * a topic the server did NOT list keeps what we knew: the list is only the recent topics, and a
//     topic that has been quiet for a while must not lose its mark for that reason;
//   * only levels 1..3 are ever stored (default is the absence of an entry).

#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace talq::topiclevels {

using Levels = QHash<int, int>;      // topic (thread) id -> 1, 2 or 3

inline bool isStorable(int topicId, int level)
{
    return topicId > 0 && level >= 1 && level <= 3;
}

// "id:level" pairs, ordered by id so the stored value is stable.
inline QStringList encode(const Levels &levels)
{
    QList<int> ids;
    for (auto it = levels.cbegin(); it != levels.cend(); ++it)
        if (isStorable(it.key(), it.value()))
            ids.append(it.key());
    std::sort(ids.begin(), ids.end());
    QStringList out;
    out.reserve(ids.size());
    for (int id : ids)
        out.append(QString::number(id) + QLatin1Char(':') + QString::number(levels.value(id)));
    return out;
}

// What encode() wrote; anything malformed or out of range is ignored.
inline Levels decode(const QStringList &pairs)
{
    Levels out;
    for (const QString &pair : pairs) {
        const int colon = pair.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        bool okId = false, okLevel = false;
        const int id = pair.left(colon).toInt(&okId);
        const int level = pair.mid(colon + 1).toInt(&okLevel);
        if (okId && okLevel && isStorable(id, level))
            out.insert(id, level);
    }
    return out;
}

// `known` combined with the server's answer for the topics it listed (`listed`: id -> level, 0 for
// default). `changed` says whether the result differs from `known`.
inline Levels applyServer(const Levels &known, const Levels &listed, bool *changed = nullptr)
{
    Levels out = known;
    for (auto it = listed.cbegin(); it != listed.cend(); ++it) {
        if (isStorable(it.key(), it.value()))
            out.insert(it.key(), it.value());
        else
            out.remove(it.key());
    }
    if (changed)
        *changed = (out != known);
    return out;
}

// Where a room's levels are kept: per ACCOUNT as well as per room (two accounts on one machine in the
// same room have their own levels -- and a level now decides whether an unread number is shown). The
// user name is hex-encoded so no character of it can act as a registry path separator.
inline QString levelsKey(const QString &user, const QString &token)
{
    return QStringLiteral("Topics/levels/") + QString::fromLatin1(user.toUtf8().toHex())
           + QLatin1Char('-') + token;
}

inline Levels loadLevels(const QString &key)
{
    QSettings s(QStringLiteral("TalQ"), QStringLiteral("TalQ"));
    return decode(s.value(key).toStringList());
}

inline void saveLevels(const QString &key, const Levels &levels)
{
    QSettings s(QStringLiteral("TalQ"), QStringLiteral("TalQ"));
    const QStringList encoded = encode(levels);
    if (encoded.isEmpty()) s.remove(key);
    else                   s.setValue(key, encoded);
}

// The level of every topic in a threads/recent answer (id -> level, 0 for default), as applyServer wants it.
inline Levels listedFromThreadsRecent(const QJsonArray &list)
{
    Levels out;
    for (const QJsonValue &v : list) {
        const QJsonObject o = v.toObject();
        const int id = o.value(QLatin1String("thread")).toObject().value(QLatin1String("id")).toInt();
        if (id <= 0)
            continue;
        out.insert(id, o.value(QLatin1String("attendee")).toObject()
                           .value(QLatin1String("notificationLevel")).toInt(0));
    }
    return out;
}

// The topics the user has SILENCED (Never): what the chat's unread number leaves out.
inline QSet<int> silencedIds(const Levels &levels)
{
    QSet<int> out;
    for (auto it = levels.cbegin(); it != levels.cend(); ++it)
        if (it.value() == 3 && it.key() > 0)
            out.insert(it.key());
    return out;
}

// A stable text for a set of topic ids, to tell "the same set as when this was worked out" from a
// changed one (the user silenced another topic, or un-silenced one).
inline QString topicsKey(const QSet<int> &ids)
{
    QList<int> v = ids.values();
    std::sort(v.begin(), v.end());
    QStringList parts;
    for (int id : v)
        parts.append(QString::number(id));
    return parts.join(QLatin1Char(','));
}

} // namespace talq::topiclevels
