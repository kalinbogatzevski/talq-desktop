#pragma once
// Pure, Qt-free conversation-tag ordering and bucketing (Talk 24
// `conversation-tags`). Extracted for the same reason as ChatSyncLogic.h: the
// ordering rules are fiddly, wrong order is immediately visible to the user,
// and none of it needs a network or a painter to test.
//
// SERVER MODEL, verified against Talk 24.0.4 on 2026-08-20:
//   GET  /ocs/v2.php/apps/spreed/api/v4/tags
//        -> [{ id: numeric-string, name, sortOrder: int, collapsed: bool,
//              type: "custom"|"favorites"|"other" }]
//        (lib/Controller/ConversationTagController.php:192-200,
//         lib/ResponseDefinitions.php:15-26)
//   POST /ocs/v2.php/apps/spreed/api/v4/room/{token}/tags  { tagIds: [string] }
//        REPLACES the conversation's whole tag set; [] unassigns all.
//        (lib/Controller/RoomController.php:1983-1991)
//   Each conversation carries `tagIds: list<string>`
//        (lib/Service/RoomFormatter.php:254, ResponseDefinitions.php:584)
//
// Note the API versions differ and it is an easy mistake: tags are v4, but the
// preset endpoint is v1 (PresetController.php:52). Getting this wrong yields a
// 404 that looks exactly like "the server is too old".
#include <algorithm>
#include <string>
#include <vector>

namespace talq {

// Mirrors one entry of GET /tags. `type` is kept as the raw server string
// rather than an enum so an unrecognised future type round-trips intact
// instead of being coerced into the wrong bucket.
struct ConversationTag {
    std::string id;
    std::string name;
    int sortOrder = 0;
    bool collapsed = false;
    std::string type = "custom";

    bool isFavorites() const { return type == "favorites"; }
    bool isOther() const { return type == "other"; }
    bool isCustom() const { return !isFavorites() && !isOther(); }
};

// Rank for the fixed-position special tags. The server states that "favorites
// and other are special tags and have a fixed sorting position"
// (ResponseDefinitions.php:24) but does NOT guarantee their sortOrder values
// place them correctly — so position is decided here, not by trusting
// sortOrder. Favourites pins to the top, "Other" sinks to the bottom, and
// everything the user created sits between them in server sortOrder.
inline int tagBucketRank(const ConversationTag &t)
{
    if (t.isFavorites())
        return 0;
    if (t.isOther())
        return 2;
    return 1;
}

// Strict-weak ordering for the tag list. Ties break on name and then id so the
// order is TOTAL and therefore stable across refreshes: two custom tags sharing
// a sortOrder (the server permits it — reorder assigns positions, but a rename
// or a race can duplicate one) would otherwise swap places on every poll and
// make the sidebar visibly jitter.
inline bool tagSortsBefore(const ConversationTag &a, const ConversationTag &b)
{
    const int ra = tagBucketRank(a);
    const int rb = tagBucketRank(b);
    if (ra != rb)
        return ra < rb;
    if (a.sortOrder != b.sortOrder)
        return a.sortOrder < b.sortOrder;
    if (a.name != b.name)
        return a.name < b.name;
    return a.id < b.id;
}

inline void sortTags(std::vector<ConversationTag> &tags)
{
    std::sort(tags.begin(), tags.end(), tagSortsBefore);
}

inline bool hasTag(const std::vector<std::string> &tagIds, const std::string &tagId)
{
    return std::find(tagIds.begin(), tagIds.end(), tagId) != tagIds.end();
}

// Does a conversation belong under `tag`?
//
// The two special buckets are CLIENT-side presentation, because the server
// sends neither of them in a conversation's `tagIds`:
//   favorites -> the conversation's existing isFavorite flag (which TalQ has
//                parsed since long before tags existed).
//   other     -> the conversation carries no tags at all. This is the "Others"
//                bucket upstream added in 24.0.0-rc.2 ("Move created tags
//                before Others when Others is the last tag").
// A favourited conversation that ALSO has custom tags appears under both, which
// matches how favourites have always behaved in this client (pinned to the top
// AND still present in the list).
inline bool conversationInTag(const ConversationTag &tag,
                              const std::vector<std::string> &tagIds,
                              bool isFavorite)
{
    if (tag.isFavorites())
        return isFavorite;
    if (tag.isOther())
        return tagIds.empty();
    return hasTag(tagIds, tag.id);
}

// Is a tag filter currently satisfiable? Used to decide whether to keep showing
// a tag filter after the tag it names was deleted on another device: the filter
// id survives in QSettings, the tag does not, and without this the sidebar
// would show a permanently empty list with no way back short of editing
// settings. Callers fall back to "no filter" when this returns false.
inline bool tagFilterStillValid(const std::vector<ConversationTag> &tags,
                                const std::string &selectedTagId)
{
    if (selectedTagId.empty())
        return true; // "no filter" is always valid
    return std::any_of(tags.begin(), tags.end(),
                       [&](const ConversationTag &t) { return t.id == selectedTagId; });
}

// Toggling a tag on a conversation. The assign endpoint REPLACES the whole set,
// so a client that wants to add one tag must send every tag the conversation
// already had plus the new one — sending just the new id silently strips the
// rest. That asymmetry (a POST named "assign" that actually means "set") is the
// single most likely way to lose a user's tags, so the read-modify-write lives
// here where it is tested, not inline at the call site.
inline std::vector<std::string> toggledTagSet(const std::vector<std::string> &current,
                                              const std::string &tagId, bool on)
{
    std::vector<std::string> next;
    next.reserve(current.size() + 1);
    for (const std::string &id : current) {
        if (id != tagId)
            next.push_back(id);
    }
    if (on)
        next.push_back(tagId);
    return next;
}

} // namespace talq
