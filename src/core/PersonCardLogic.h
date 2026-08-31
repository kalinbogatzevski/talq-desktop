#pragma once

// Pure decisions behind the colleague person card.
//
// The card answers "who is this, and are they actually working right now?" and
// it is assembled from three independent sources -- Nextcloud identity, the
// shift service, and whatever a site's card endpoint chose to send. Any of the
// three can be missing, slow or wrong, so the decisions about what to SAY when
// a piece is absent are the ones worth pinning down without a running app.
//
// Kept free of Qt on purpose: the failure modes that matter here are a bot id
// reaching the ERP, a blank headline, and a chip that claims something it does
// not know. The widget converts to and from QString at the edge.

#include "core/ShiftStatusLogic.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace talq {

namespace person_detail {

inline std::string trimCopy(const std::string &s)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    auto b = std::find_if(s.begin(), s.end(), notSpace);
    auto e = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    return (b < e) ? std::string(b, e) : std::string();
}

// Only ever applied to a state this build has never heard of. The known states
// are spelled out below because "dnd" -> "Dnd" would be wrong, but blanking an
// unrecognised one would make a newer server look broken on an older build.
inline std::string capitaliseFirst(const std::string &s)
{
    if (s.empty()) return s;
    std::string out = s;
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

} // namespace person_detail

// Whether this actor id may be sent to the ERP.
//
// A bot's actorId can carry a "bots/" prefix. Pasted into a request path it is
// at best a junk lookup and at worst a path walk made with the user's OWN
// credential, so anything containing a separator is refused outright.
//
// Guests deliberately pass: their id is an opaque session hash with no '/', so
// it is sent and simply comes back unknown. That is the accepted cost of not
// plumbing an actor type through MessageListModel, MessageLayout, LayoutEngine
// and the layout cache key for one call site.
inline bool personCardEligibleForErp(const std::string &actorId)
{
    const std::string id = person_detail::trimCopy(actorId);
    if (id.empty()) return false;
    return id.find('/') == std::string::npos;
}

// The headline: a display name if there is one, else the raw id, else a word.
// A card with an empty title reads as a rendering bug rather than as missing
// data, which is why there is no empty result.
inline std::string personCardTitle(const std::string &displayName,
                                   const std::string &actorId)
{
    const std::string name = person_detail::trimCopy(displayName);
    if (!name.empty()) return name;
    const std::string id = person_detail::trimCopy(actorId);
    if (!id.empty()) return id;
    return "Unknown";
}

// Presence as one line: "Online \xC2\xB7 in a mtg".
//
// An empty state means presence was never fetched -- which is NOT the same as
// being offline, and must render as nothing at all rather than as a claim that
// happens to be wrong. The presence hash is populated from the conversation
// list, so a colleague in a group room the user has never messaged privately
// legitimately has no entry.
inline std::string personPresenceLine(const std::string &state,
                                      const std::string &message)
{
    const std::string st = person_detail::trimCopy(state);
    if (st.empty()) return {};

    std::string word;
    if      (st == "online")    word = "Online";
    else if (st == "away")      word = "Away";
    else if (st == "invisible") word = "Offline";   // NC reports hidden as invisible
    else if (st == "offline")   word = "Offline";
    else if (st == "dnd")       word = "Do not disturb";
    else                        word = person_detail::capitaliseFirst(st);

    const std::string msg = person_detail::trimCopy(message);
    if (msg.empty()) return word;
    return word + " \xC2\xB7 " + msg;   // U+00B7 MIDDLE DOT
}

// The chip's words. A server label always wins for a KNOWN state -- that is
// what lets a deployment reword "On shift" to "Rostered", or translate it,
// without anyone shipping a build.
//
// Unknown says nothing at all, and no server label can make it speak. Unknown
// is deliberately indistinguishable from a refused request and from never
// having asked, and a label would break that.
inline std::string personShiftChipText(ShiftState state,
                                       const std::string &serverLabel)
{
    if (state == ShiftState::Unknown) return {};
    const std::string label = person_detail::trimCopy(serverLabel);
    if (!label.empty()) return label;
    switch (state) {
    case ShiftState::OnShift:  return "On shift";
    case ShiftState::OnBreak:  return "On break";
    case ShiftState::OffShift: return "Off shift";
    case ShiftState::Unknown:  break;
    }
    return {};
}

// A card is a DETAIL surface, so every known state draws.
//
// This deliberately differs from shiftMarksExceptionInList(), which marks only
// the exceptions because badging the normal case on every row of a long list
// is noise that teaches people to stop looking. Here the user asked about this
// one person, and "yes, they are on shift" is still an answer.
inline bool personShiftChipIsDrawable(ShiftState state)
{
    return state != ShiftState::Unknown;
}

} // namespace talq
