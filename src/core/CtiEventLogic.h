#pragma once

// Screen-pop card lifecycle.
//
// TalQ deliberately knows NOTHING about the phone system. A site-supplied CTI
// daemon does the listening and filtering, and sends a technology-neutral
// event:
//
//   {"type":"ring","call_id":"<opaque>","extension":"<ext>",
//    "caller":"<number>","direction":"inbound"}
//   {"type":"end","call_id":"<opaque>","reason":"answered_elsewhere"}
//
// That keeps the phone system swappable: whatever a site runs, it writes a
// daemon that speaks this and the client is unchanged. The full contract --
// pairing, events, the card payload and the security requirements -- is
// documented for third parties in docs/CTI-SCREEN-POP.md.
//
// What is left here is genuinely the client's job and genuinely fiddly: an
// agent can have several calls ringing at once (a queue call plus a direct
// dial), events can arrive out of order or duplicated, and an "end" can refer
// to a call this client never saw ring -- because TalQ was started midway
// through, or the socket dropped and reconnected.

#include <cstddef>
#include <string>
#include <vector>

namespace talq {

// What the UI should do in response to an event.
enum class CardAction {
    Show,        // new call ringing -- put a card up
    Dismiss,     // take the card down now
    MarkActive,  // keep it up: this agent answered, which is when they want it
    MarkMissed,  // keep it up briefly so a missed call stays identifiable
    Ignore,      // duplicate, or an end for a call we never showed
};

inline const char *cardActionName(CardAction a)
{
    switch (a) {
    case CardAction::Show:       return "show";
    case CardAction::Dismiss:    return "dismiss";
    case CardAction::MarkActive: return "mark-active";
    case CardAction::MarkMissed: return "mark-missed";
    case CardAction::Ignore:     return "ignore";
    }
    return "ignore";
}

// A card that is currently on screen.
struct ActiveCard {
    std::string callId;
    std::string caller;
    std::string extension;
};

// How long a card lingers after the call ends, in milliseconds.
//
// A card is NEVER dismissed on a timer while the phone is still ringing --
// that was the single most important UI rule to get right. A card that
// vanishes after five seconds while the caller is still waiting is worse than
// no card at all, because the agent has already started relying on it.
inline int lingerMsForAction(CardAction action)
{
    switch (action) {
    case CardAction::MarkMissed: return 15000;  // long enough to read and act on
    case CardAction::MarkActive: return 0;      // 0 = stay until the user dismisses
    case CardAction::Show:       return 0;      // ringing: no timer at all
    case CardAction::Dismiss:
    case CardAction::Ignore:     break;
    }
    return 0;
}

// Tracks which calls currently have a card. Pure -- no Qt, no I/O -- so the
// ordering and duplication rules can be tested exhaustively.
class CtiCardStore
{
public:
    // A ring for a call we already show is a duplicate. The daemon already
    // collapses the two DialBegin events Asterisk emits per ringing phone, but
    // a reconnect can legitimately replay one, and popping a second identical
    // card would look like two calls.
    CardAction onRing(const std::string &callId,
                      const std::string &caller,
                      const std::string &extension)
    {
        if (callId.empty())
            return CardAction::Ignore;
        if (indexOf(callId) != npos())
            return CardAction::Ignore;
        m_cards.push_back(ActiveCard{callId, caller, extension});
        return CardAction::Show;
    }

    // `reason` comes straight off the wire.
    CardAction onEnd(const std::string &callId, const std::string &reason)
    {
        const std::size_t idx = indexOf(callId);
        if (idx == npos())
            return CardAction::Ignore;   // never showed it; nothing to do

        // This agent picked up: keep the card. This is the moment its content
        // is most useful, and it is also when "open in browser" gets clicked.
        if (reason == "answered_by_me") {
            return CardAction::MarkActive;
        }

        // Someone else in the ring group took it, or the caller gave up. The
        // card is now noise on this desk.
        if (reason == "answered_elsewhere" || reason == "cancelled") {
            m_cards.erase(m_cards.begin() + static_cast<long>(idx));
            return CardAction::Dismiss;
        }

        // Rang out or was busy. Worth keeping visible briefly -- a missed
        // customer call is something the agent may want to return.
        m_cards.erase(m_cards.begin() + static_cast<long>(idx));
        return CardAction::MarkMissed;
    }

    // The socket dropped. Anything still ringing is now unknowable -- the call
    // may have been answered by a colleague minutes ago -- so the cards are
    // stale and must go rather than sit there lying.
    void clear() { m_cards.clear(); }

    bool isShowing(const std::string &callId) const { return indexOf(callId) != npos(); }
    std::size_t count() const { return m_cards.size(); }
    const std::vector<ActiveCard> &cards() const { return m_cards; }

private:
    static std::size_t npos() { return static_cast<std::size_t>(-1); }

    std::size_t indexOf(const std::string &callId) const
    {
        for (std::size_t i = 0; i < m_cards.size(); ++i)
            if (m_cards[i].callId == callId)
                return i;
        return npos();
    }

    std::vector<ActiveCard> m_cards;
};

// ── Card content: a CLOSED style vocabulary ─────────────────────────────────

// The server describes its own card, but it does NOT get to choose colours.
// A hex value or an unrecognised name from the server would walk straight past
// the AA contrast guarantees the theme suite enforces, so the wire carries a
// small fixed vocabulary and anything outside it degrades to Normal. Each of
// these maps to a pair the conformance suite already scores.
enum class CardStyle {
    Normal,    // textPrimary
    Muted,     // textSecondary
    Warning,   // amber      (amber-as-text is AA-registered)
    Danger,    // danger     (danger-as-text is AA-registered)
};

inline CardStyle cardStyleFromWire(const std::string &style)
{
    if (style == "muted")   return CardStyle::Muted;
    if (style == "warning") return CardStyle::Warning;
    if (style == "danger")  return CardStyle::Danger;
    // "normal", empty, or anything this build has never heard of. Degrading
    // rather than rejecting is deliberate: a newer server adding a style must
    // not blank out an older client's card.
    return CardStyle::Normal;
}

// How many rows fit is the SERVER's call, not a client constant -- otherwise
// deciding to show one more contract would mean shipping a new desktop build,
// which is the whole thing this design exists to avoid.
//
// The client keeps only a hard ceiling. A server that asks for 500 rows is
// either mistaken or hostile, and either way a card taller than the screen is
// unusable and unclosable.
inline constexpr int kDefaultMaxFields = 6;    // when the server says nothing
inline constexpr int kHardMaxFields    = 14;   // the server may never exceed this

// `serverLimit` <= 0 means "not specified" -- an older server, or one that
// does not care.
inline int resolveFieldLimit(int serverLimit)
{
    if (serverLimit <= 0)
        return kDefaultMaxFields;
    return serverLimit < kHardMaxFields ? serverLimit : kHardMaxFields;
}

inline int visibleFieldCount(int total, int serverLimit = 0)
{
    if (total <= 0) return 0;
    const int limit = resolveFieldLimit(serverLimit);
    return total < limit ? total : limit;
}

inline int hiddenFieldCount(int total, int serverLimit = 0)
{
    const int shown = visibleFieldCount(total, serverLimit);
    return total > shown ? total - shown : 0;
}

// ── Wire-event validation ───────────────────────────────────────────────────

// The daemon is trusted, but it is still a separate process across a socket,
// and a half-written or truncated frame must not put a blank card on screen.
inline bool ringEventIsUsable(const std::string &callId, const std::string &extension)
{
    return !callId.empty() && !extension.empty();
}

// What to render when the ERP lookup fails, times out, or the caller is not a
// known customer. Showing the bare number is deliberately still worth doing:
// "someone is calling from 082..." beats no card, and the lookup failing is
// exactly when the agent has least information.
inline std::string fallbackCardTitle(const std::string &caller)
{
    if (caller.empty())
        return "Incoming call";
    return caller;
}

} // namespace talq
