#pragma once
// Pure, Qt-free server-capability gating, extracted so the decision "may this
// client use feature X against THIS server?" is unit-testable in isolation
// (mirrors ChatSyncLogic.h / VersionCompare.h). Callers pass primitives; there
// are no model/network/Qt dependencies here.
//
// WHY THIS EXISTS. Until 0.65.0 TalQ fetched `cloud/capabilities` and threw the
// feature list away: AuthManager::fetchServerInfo() looped `spreed.features`
// looking only for the literal "threads" and stored a single bool. There was no
// hasCapability() anywhere in the tree (grep: zero hits), so every other
// server-version-dependent call was made unconditionally and simply 404'd or
// silently no-op'd on older servers — see the `media-caption` talkMetaData sent
// with only a comment naming the capability (MessageListModel.cpp), and
// clearChatHistory() naming `clear-history` in a comment while checking nothing.
//
// TalQ ships to two very different audiences: a branded build talks to a server
// its operator keeps current (Nextcloud 34 / Talk 24 at the time of writing),
// while the generic build talks to whatever server the user happens to run —
// frequently several major Talk versions behind. Every Talk 24 feature added in
// 0.65.0 MUST therefore be gated here and degrade to the 0.64 behaviour when the
// flag is absent.
#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace talq {

// ---------------------------------------------------------------------------
// Capability strings. Spelled once, here, so a typo is a compile error at the
// call site rather than a feature that silently never activates.
//
// TRAP, verified against Talk 24.0.4 source on 2026-08-20: the conversation
// JSON field `attributes` (which is how a client detects a voice room) is
// documented in lib/ResponseDefinitions.php:591 as
//     "only available with capability: `conversation-attributes`"
// and that capability string EXISTS NOWHERE in the Talk codebase — not in
// lib/Capabilities.php, not in docs/. It is an upstream documentation bug.
// The real gate, per docs/constants.md:76 ("Conversation attributes / Required
// capability: conversation-presets"), is CAP_CONVERSATION_PRESETS. Gating on
// the documented-but-nonexistent string would disable voice-room detection
// against EVERY server in existence, including one that fully supports it.
// ---------------------------------------------------------------------------
inline const char *const CAP_CONVERSATION_TAGS = "conversation-tags";
inline const char *const CAP_CONVERSATION_PRESETS = "conversation-presets";
inline const char *const CAP_THREADS = "threads";

// 0.65.3. Every string below was read out of Talk 24.0.4's own
// lib/Capabilities.php FEATURES list rather than out of docs/ — the tag/preset
// cycle found three places where the documentation names a capability that the
// server never advertises (see the `conversation-attributes` note above), so
// docs are a lead and lib/ is the truth.
//
// Most of these gate something that TalQ was ALREADY doing unconditionally
// (see the file header): the point of adding them is that the generic build
// talks to whatever server the user runs, and an ungated call against an older
// Talk either 404s or silently no-ops.
inline const char *const CAP_FAVORITES = "favorites";
inline const char *const CAP_NOTIFICATION_LEVELS = "notification-levels";
inline const char *const CAP_NOTIFICATION_CALLS = "notification-calls";
inline const char *const CAP_NOTE_TO_SELF = "note-to-self";
inline const char *const CAP_IMPORTANT_CONVERSATIONS = "important-conversations";
inline const char *const CAP_CHAT_GET_CONTEXT = "chat-get-context";
inline const char *const CAP_CHAT_KEEP_NOTIFICATIONS = "chat-keep-notifications";
inline const char *const CAP_DIRECT_MENTION_FLAG = "direct-mention-flag";
inline const char *const CAP_PINNED_MESSAGES = "pinned-messages";
inline const char *const CAP_ARCHIVED_CONVERSATIONS = "archived-conversations-v2";
inline const char *const CAP_INVITE_GROUPS_AND_MAILS = "invite-groups-and-mails";
inline const char *const CAP_CIRCLES_SUPPORT = "circles-support";
inline const char *const CAP_UNIFIED_SEARCH = "unified-search";
inline const char *const CAP_REACTIONS = "reactions";
inline const char *const CAP_ROOM_DESCRIPTION = "room-description";
inline const char *const CAP_START_CALL_FLAG = "start-call-flag";
inline const char *const CAP_CONVERSATION_SUBFOLDERS = "conversation-subfolders";
inline const char *const CAP_TALK_POLLS = "talk-polls";

// Bit-flags of the conversation `attributes` field (Talk 24 lib/RoomAttributes.php).
// A bit-flag, not an enum value: test with a mask, never with equality, or a
// future second attribute bit silently turns every voice room into a non-voice
// room (`attributes == 1` fails the moment the server sets bit 2 as well).
enum ConversationAttribute : int {
    AttributeNone = 0,
    AttributeVoiceRoom = 1,
};

// A server's advertised Talk feature set.
//
// Deliberately fails CLOSED: a default-constructed registry (capabilities never
// fetched, or the fetch failed) reports false for everything. AuthManager's
// capability callback bails on error with a bare `if (!ok) return;`, so a
// transient network failure at login leaves this empty — and "empty" must mean
// "assume an old server and use the 0.64 code paths", never "assume support".
class ServerCapabilities {
public:
    // Replaces the whole set. MUST NOT merge into the previous contents: the
    // same TalQ process can log out of a Talk 24 server and into an older one,
    // and a merge would carry the newer server's flags across and re-enable
    // endpoints the new server answers with 404.
    void setFeatures(const std::vector<std::string> &features)
    {
        m_features.clear();
        m_features.insert(features.begin(), features.end());
        m_fetched = true;
    }

    // Clears back to the fail-closed state. Call on logout so a stale set can
    // never gate a decision made against a different server.
    void reset()
    {
        m_features.clear();
        m_fetched = false;
    }

    bool has(const std::string &feature) const
    {
        return m_features.find(feature) != m_features.end();
    }

    // True once a capabilities payload has actually been parsed. Distinguishes
    // "this server advertises no features" from "we never asked" — only useful
    // for diagnostics; gating decisions must use has() and treat both as false.
    bool fetched() const { return m_fetched; }

    std::size_t size() const { return m_features.size(); }

    // --- Talk 24 feature gates -------------------------------------------
    bool supportsConversationTags() const { return has(CAP_CONVERSATION_TAGS); }
    bool supportsConversationPresets() const { return has(CAP_CONVERSATION_PRESETS); }

    // Voice rooms are not a capability of their own — they are a room PRESET
    // (Talk 24 lib/RoomPresets/VoiceRoom.php, identifier "voiceroom"), and the
    // `attributes` bit that marks one is gated on the presets capability. So
    // the presets flag is what licenses BOTH the create-time preset picker and
    // reading `attributes` off an existing conversation.
    bool supportsVoiceRooms() const { return supportsConversationPresets(); }

    // --- 0.65.3 gates -----------------------------------------------------
    bool supportsFavorites() const { return has(CAP_FAVORITES); }
    bool supportsNotificationLevels() const { return has(CAP_NOTIFICATION_LEVELS); }
    bool supportsNotificationCalls() const { return has(CAP_NOTIFICATION_CALLS); }
    bool supportsNoteToSelf() const { return has(CAP_NOTE_TO_SELF); }
    bool supportsImportantConversations() const { return has(CAP_IMPORTANT_CONVERSATIONS); }
    bool supportsChatGetContext() const { return has(CAP_CHAT_GET_CONTEXT); }
    bool supportsKeepNotifications() const { return has(CAP_CHAT_KEEP_NOTIFICATIONS); }
    bool supportsDirectMentionFlag() const { return has(CAP_DIRECT_MENTION_FLAG); }
    bool supportsPinnedMessages() const { return has(CAP_PINNED_MESSAGES); }
    bool supportsArchivedConversations() const { return has(CAP_ARCHIVED_CONVERSATIONS); }
    bool supportsGroupInvites() const { return has(CAP_INVITE_GROUPS_AND_MAILS); }
    bool supportsCircles() const { return has(CAP_CIRCLES_SUPPORT); }
    bool supportsUnifiedSearch() const { return has(CAP_UNIFIED_SEARCH); }
    bool supportsReactions() const { return has(CAP_REACTIONS); }
    bool supportsRoomDescription() const { return has(CAP_ROOM_DESCRIPTION); }
    bool supportsStartCallFlag() const { return has(CAP_START_CALL_FLAG); }
    bool supportsPolls() const { return has(CAP_TALK_POLLS); }

private:
    std::set<std::string> m_features;
    bool m_fetched = false;
};

// True iff this conversation is a voice room, i.e. one the client should join
// the call in automatically on open ("Voice rooms - Join call when joining
// conversation", Talk 24 docs/constants.md:79).
//
// `capable` is ServerCapabilities::supportsVoiceRooms(). It is a parameter
// rather than an implicit global because an older server does not send
// `attributes` at all: Conversation::fromJson then parses a MISSING key, which
// QJsonValue::toInt() reports as 0 — harmless here, but if a future server
// reuses the field for something else, an ungated read would auto-join calls
// the user never asked for. Auto-joining a call by mistake is about the most
// user-hostile bug this client can have (hot mic), so it stays gated.
inline bool isVoiceRoom(int attributes, bool capable)
{
    return capable && (attributes & AttributeVoiceRoom) != 0;
}

// Whether opening `token` should auto-join its call.
//
// Guards the three ways auto-join goes wrong:
//   - not a voice room, or server too old            -> never
//   - already in a call (any room)                   -> never; joining a second
//     call would tear down the first, and re-opening the room you are already
//     called into would rejoin in a loop
//   - already auto-joined this token this session    -> never; onConversationSelected
//     fires again on every sidebar refresh/reselect, and without this the user
//     could not hang up and stay in the room — the next refresh would drag them
//     straight back into the call.
inline bool shouldAutoJoinCall(int attributes, bool capable, bool alreadyInACall,
                               bool alreadyAutoJoinedThisToken)
{
    return isVoiceRoom(attributes, capable) && !alreadyInACall
           && !alreadyAutoJoinedThisToken;
}

} // namespace talq
