// Whether a new chat message may raise a desktop popup (and its sound).
//
// Talk lets the user set, per conversation and per topic (thread), how much a
// conversation may disturb them: 0 default, 1 all messages, 2 mentions only,
// 3 never. TalQ has always WRITTEN that setting (right-click a chat, or a topic
// chip, -> Notifications) and shown "Muted" in the list for level 3, but the two
// places that raise a chat popup never read it, so a muted chat still popped up.
//
// The rules are Talk's own (lib/Chat/Notifier.php, shouldMentionedUserBeNotified):
//   * a topic's own level wins, unless it is "default", which follows the chat;
//   * never = nothing, not even a mention;
//   * mentions only = a message that mentions you (by name, @all, or a group).
//
// "Default" and "All messages" are NOT changed here: they keep showing a popup
// for every message, as TalQ always did. (Talk's server decides what a default
// group chat does; TalQ has never mirrored that, and doing it silently now would
// hide messages people are used to seeing.)
//
// Pure: no Qt widgets, no network, so the whole table is unit-tested.

#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>

namespace talq::toastgate {

enum Level { Default = 0, Always = 1, MentionsOnly = 2, Never = 3 };

// The level that applies to a message. `threadLevel` is the topic's own level
// (0 when the message is in no topic, or when the topic has none set).
inline int effectiveLevel(int roomLevel, int threadLevel)
{
    return threadLevel != Default ? threadLevel : roomLevel;
}

// May a message raise a popup at this level? `mentionsMe` only matters for
// "mentions only".
inline bool allowsPopup(int level, bool mentionsMe)
{
    switch (level) {
    case Never:        return false;
    case MentionsOnly: return mentionsMe;
    default:           return true;   // default, all messages, or a level we do not know
    }
}

// Does a message's `messageParameters` mention `userId`?
//
// A parameter is a mention when its key starts with "mention-". `type`:
//   user        -> a mention of that person: ours only if the id is ours AND the
//                  parameter names no `server` (a federated user from another
//                  server who happens to share our id is not us; Talk's own
//                  FederationChatNotifier::isMentioned checks the same)
//   call        -> @all
//   user-group / group / team / circle -> a group; TalQ does not know the
//                  user's groups, so it counts (a missed mention is worse than
//                  one extra popup in a room the user chose "mentions only" for)
//   guest / email / anything else -> not us
inline bool mentionsUser(const QJsonObject &messageParameters, const QString &userId)
{
    for (auto it = messageParameters.begin(); it != messageParameters.end(); ++it) {
        if (!it.key().startsWith(QLatin1String("mention-")))
            continue;
        const QJsonObject p = it.value().toObject();
        const QString type = p.value(QLatin1String("type")).toString();
        if (type == QLatin1String("call"))
            return true;
        if (type == QLatin1String("user")
            && !userId.isEmpty()
            && p.value(QLatin1String("server")).toString().isEmpty()
            && p.value(QLatin1String("id")).toString() == userId)
            return true;
        if (type == QLatin1String("user-group") || type == QLatin1String("group")
            || type == QLatin1String("team") || type == QLatin1String("circle"))
            return true;
    }
    return false;
}

// Is a message a reply to one of OUR messages? Talk treats that like a mention.
//
// The trap: a plain post into a topic is stored with the topic's ROOT message as
// its `parent`, with no explicit quote. When the topic is ours, that would count
// every post in it as "a reply to me" and defeat "mentions only" in exactly the
// topics we started. So a parent that IS the topic root does not count. The API
// cannot tell an explicit quote of the root from an implicit post, so that one
// rare case is under-notified (the unread-mention badge still shows it).
inline bool isReplyToUser(const QString &replyToActorId, int replyToId, int threadId,
                          const QString &userId)
{
    if (userId.isEmpty() || replyToActorId != userId)
        return false;
    return !(threadId > 0 && replyToId == threadId);
}

// One new message, reduced to what the gate needs.
struct Candidate {
    int threadId = 0;        // the topic it is in, 0 = none
    bool mentionsMe = false;
};

// The first message of a batch (newest first) that may raise a popup, or -1.
//
// A poll can bring several messages at once (two within the same second, or a
// backlog after a reconnect), and a user who chose "mentions only" must still
// hear the mention that is not the last of them: judging only the newest message
// dropped it. `threadLevels` holds each topic's own level; a topic that is not in
// it (lookup failed, deleted) has none, so the chat's level decides.
inline int firstAllowed(int roomLevel, const QVector<Candidate> &candidates,
                        const QHash<int, int> &threadLevels)
{
    for (int i = 0; i < candidates.size(); ++i) {
        const Candidate &c = candidates.at(i);
        const int threadLevel = c.threadId > 0 ? threadLevels.value(c.threadId, 0) : 0;
        if (allowsPopup(effectiveLevel(roomLevel, threadLevel), c.mentionsMe))
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Catching up on a chat the user is not looking at
//
// The chat list carries only the LAST message of a room. That is enough to decide
// a popup unless one of these is true, and then the real messages are read:
//   * a sensitive/classified room: the server sends no last message at all, so the
//     topic and the mention markup are unknown;
//   * "mentions only", where the room's unread-mention flag is the only hint about
//     an earlier message, and it is sticky (it stays on after later plain messages,
//     so a second mention while an older one is unread, or a reply to us, hides);
//   * several new messages whose last one is in a topic: the topic may be muted
//     while an earlier message is not.
//
// Not covered, on purpose: a chat on Never with ONE topic switched back on, when
// that topic's message is not the last of a burst. Reading every muted chat on
// every message would cost a request per message in exactly the noisy rooms people
// mute; the topic's message is honoured when it IS the last one.
// ---------------------------------------------------------------------------
inline bool needsCatchUp(int roomLevel, bool sensitive, bool cheapMentionsMe,
                         bool unreadMentionAlreadyOn, int newUnread, int lastMessageThreadId)
{
    if (roomLevel == Never)
        return false;
    if (sensitive)
        return true;
    if (newUnread <= 0)
        return false;
    if (roomLevel == MentionsOnly) {
        if (!cheapMentionsMe)
            return newUnread > 1 || unreadMentionAlreadyOn;
        // The unread-mention flag just turned on, but it belongs to SOME new message.
        // If the last one is in a topic (possibly a muted one) it may not be that one.
        return lastMessageThreadId > 0 && newUnread > 1;
    }
    return lastMessageThreadId > 0 && newUnread > 1;
}

// A new message as read from the chat itself.
struct CatchUpMessage {
    int id = 0;
    Candidate gate;
    QString text;        // plain text, mentions written as @name
};

// `message` with its {placeholders} replaced by what they stand for: a mention
// as "@name", anything else (a file) as its name.
inline QString plainTextWithParameters(QString message, const QJsonObject &params)
{
    for (auto it = params.begin(); it != params.end(); ++it) {
        const QJsonObject p = it.value().toObject();
        QString name = p.value(QLatin1String("name")).toString();
        if (name.isEmpty())
            name = p.value(QLatin1String("id")).toString();
        if (name.isEmpty())
            continue;
        if (it.key().startsWith(QLatin1String("mention-")))
            name.prepend(QLatin1Char('@'));
        message.replace(QLatin1Char('{') + it.key() + QLatin1Char('}'), name);
    }
    return message;
}

struct CatchUpResult {
    QVector<CatchUpMessage> messages;   // newest first; may be empty (all silent, own...)
    int inspected = 0;                  // how many unread-counting rows were looked at
    int highestId = 0;                  // the newest of them (the next look starts above it)
    // False when the page may have missed new messages (it was full and reached
    // neither the floor nor the wanted count): the caller must not treat "nothing
    // found" as "nothing there", and falls back to what the room list said.
    bool complete = true;
};

// Judge the new messages of a chat from a page of GET chat/{token}.
//
// Talk's unread count is the number of `comment` / `object_shared` rows after the
// read marker (ChatManager::getUnreadCount): system rows, deleted ones, commands and
// our own messages are not in it. So the new messages are exactly the newest
// `countLimit` such rows from other people (`countLimit` = how many unread messages
// arrived since the last look) -- which also bounds the page when no floor is known
// (sensitive rooms have no last message to anchor on) and keeps an older, already
// seen message out of the judgement.
//   floor   ids at or below it were seen before (0 = unknown)
//   upTo    ids above it are newer than the room list we are judging (0 = no bound):
//           they belong to the next look, not this one
//   limit   how many rows the page was asked for, to tell a short page (the whole
//           chat) from a full one
// Silent messages and TalQ's busy marker count as unread but are never a popup.
inline CatchUpResult catchUpMessages(const QJsonArray &chatMessages, const QString &me,
                                     int floor, int upTo, int countLimit, int limit)
{
    QVector<QJsonObject> rows;
    rows.reserve(chatMessages.size());
    for (const QJsonValue &v : chatMessages)
        rows.append(v.toObject());
    std::sort(rows.begin(), rows.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QLatin1String("id")).toInt() > b.value(QLatin1String("id")).toInt();
    });

    CatchUpResult r;
    bool reachedFloor = false;
    for (const QJsonObject &m : rows) {
        const int id = m.value(QLatin1String("id")).toInt();
        if (id <= 0)
            continue;
        if (upTo > 0 && id > upTo)
            continue;
        if (floor > 0 && id <= floor) {
            reachedFloor = true;
            break;
        }
        const QString type = m.value(QLatin1String("messageType")).toString();
        const bool counts = (type == QLatin1String("comment") || type == QLatin1String("voice-message"))
            && m.value(QLatin1String("systemMessage")).toString().isEmpty();
        if (!counts)
            continue;
        if (m.value(QLatin1String("actorType")).toString() == QLatin1String("users")
            && !me.isEmpty() && m.value(QLatin1String("actorId")).toString() == me)
            continue;                        // ours: never unread to us
        if (r.inspected >= countLimit)
            break;
        ++r.inspected;
        r.highestId = std::max(r.highestId, id);
        if (m.value(QLatin1String("silent")).toBool()
            || m.value(QLatin1String("referenceId")).toString() == QLatin1String("talq/busy"))
            continue;

        CatchUpMessage c;
        c.id = id;
        // `threadId` is sent on EVERY message; only `isThread` says it is a topic.
        c.gate.threadId = m.value(QLatin1String("isThread")).toBool()
                              ? m.value(QLatin1String("threadId")).toInt() : 0;
        const QJsonObject params = m.value(QLatin1String("messageParameters")).toObject();
        const QJsonObject parent = m.value(QLatin1String("parent")).toObject();
        c.gate.mentionsMe = mentionsUser(params, me)
            || isReplyToUser(parent.value(QLatin1String("actorId")).toString(),
                             parent.value(QLatin1String("id")).toInt(), c.gate.threadId, me);
        c.text = plainTextWithParameters(m.value(QLatin1String("message")).toString(), params);
        r.messages.append(c);
    }
    r.complete = r.inspected >= countLimit || reachedFloor || chatMessages.size() < limit;
    return r;
}

// ---------------------------------------------------------------------------
// Unread NUMBERS (the badge on a chat, a folder's total, the tray and taskbar
// count). A chat set to Never counts for nothing unless the user asked for muted
// chats to be counted ("Count unread messages from muted chats", off by default):
// they muted it to stop being told about it. Popups are decided elsewhere and use
// the raw count.
// ---------------------------------------------------------------------------
//
// `silencedTopicUnread` is how many of the room's unread messages sit in topics the user silenced
// (silencedUnreadCount): they are left out of the chat's number as well, unless muted things are
// counted. The server counts a room's unread without knowing the topics.
inline int countedUnread(int roomLevel, int unread, bool countMuted, int silencedTopicUnread = 0)
{
    if (countMuted)
        return unread;
    if (roomLevel == Never)
        return 0;
    return std::max(0, unread - std::max(0, silencedTopicUnread));
}

// How many of a room's UNREAD messages are in the topics `silencedTopics`, read off a page of
// GET chat/{token}. Talk's unread count is the number of `comment` rows after the read marker
// (ChatManager::getUnreadCount), so the unread messages are the newest `unread` such rows from other
// people -- the same rows catchUpMessages judges -- and of those this counts the ones whose
// `threadId` (with `isThread`: the id is sent on every message) is a silenced topic. Only the
// newest `unread` are looked at, so an older, already-read message in a silenced topic is never
// counted; a page that does not reach back that far under-counts, which leaves the chat's number
// a little high rather than hiding a message that is not silenced.
// `upTo`: rows with a higher id are newer than the room list the `unread` came from (a message that
// arrived while the page was being fetched) and are not looked at (0 = no bound), as in catchUpMessages.
inline int silencedUnreadCount(const QJsonArray &chatMessages, const QString &me, int unread,
                               const QSet<int> &silencedTopics, int upTo = 0)
{
    if (unread <= 0 || silencedTopics.isEmpty())
        return 0;
    QVector<QJsonObject> rows;
    rows.reserve(chatMessages.size());
    for (const QJsonValue &v : chatMessages)
        rows.append(v.toObject());
    std::sort(rows.begin(), rows.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QLatin1String("id")).toInt() > b.value(QLatin1String("id")).toInt();
    });
    int inspected = 0, silenced = 0;
    for (const QJsonObject &m : rows) {
        const int id = m.value(QLatin1String("id")).toInt();
        if (id <= 0)
            continue;
        if (upTo > 0 && id > upTo)
            continue;                        // newer than the list this was asked for
        const QString type = m.value(QLatin1String("messageType")).toString();
        const bool counts = (type == QLatin1String("comment") || type == QLatin1String("voice-message"))
            && m.value(QLatin1String("systemMessage")).toString().isEmpty();
        if (!counts)
            continue;
        if (m.value(QLatin1String("actorType")).toString() == QLatin1String("users")
            && !me.isEmpty() && m.value(QLatin1String("actorId")).toString() == me)
            continue;                        // ours: never unread to us
        if (inspected >= unread)
            break;
        ++inspected;
        if (m.value(QLatin1String("isThread")).toBool()
            && silencedTopics.contains(m.value(QLatin1String("threadId")).toInt()))
            ++silenced;
    }
    return silenced;
}

// Does the popup decision need a topic's level? Any message that is in a topic:
// a topic can mute a chat that is on "all messages", or un-mute one that is
// "never", so the chat's level alone is never enough.
inline bool needsThreadLevel(int threadId)
{
    return threadId > 0;
}

} // namespace talq::toastgate
