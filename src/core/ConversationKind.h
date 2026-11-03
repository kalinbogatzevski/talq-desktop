#pragma once
// Which conversations are drawn as a ROUNDED SQUARE rather than a circle.
//
// A 1:1 chat keeps a circle (it IS a person), a group chat gets a rounded
// square, so the two read apart in the sidebar at a glance. This is the ONE
// predicate: the sidebar row, the chat header and the conversation-info dialog
// all ask it, so they can never disagree. (The codebase already has two other
// "group" predicates for other purposes -- {2,3} for the Groups filter and "not
// type 1" for where a picture is fetched from -- and this one deliberately
// matches the first.)
//
// Conversation::Type (src/models/Conversation.h): 1 OneToOne, 2 Group,
// 3 Public, 4 Changelog, 5 FormerOneToOne, 6 NoteToSelf. Types 1, 4, 5 and 6
// stay circles: 5 is a person, 6 is the user's own bookmark disc, 4 is a
// system room.
//
// Pure and Qt-free so a test can pin the whole truth table.
// Spec: docs/superpowers/specs/2026-11-02-sidebar-folders-design.md (D10).

namespace talq {

constexpr bool isGroupConversation(int conversationType)
{
    return conversationType == 2 || conversationType == 3;
}

} // namespace talq
