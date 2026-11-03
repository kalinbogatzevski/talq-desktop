#pragma once
// Which grouping the sidebar opens in: the startup decision, and nothing else.
//
// The sidebar can be grouped by None, by Tags (Talk 24 server tags) or by Folders (local, see
// FolderLogic.h). Two stored values can answer "which one did the user last choose":
//
//   Sidebar/groupMode    int, written by builds that have folders: 0 None, 1 Tags, 2 Folders
//   Sidebar/groupByTag   bool, the only value older builds wrote and still read
//
// A new build keeps writing BOTH, in one batch (groupByTag = "the mode is Tags"), so that a
// downgrade still behaves. That is why the two keys can only DISAGREE after an older build has
// written groupByTag on its own (a downgrade, a tick of "Group by tag", an upgrade again): the
// legacy value is then the NEWER choice, and the stored mode is the one that went out of date.
// So a disagreement is settled for the legacy bool; the stored mode is what tells Folders from
// the rest, which the bool cannot say, so it decides whenever the two agree.
//
//   a stored groupMode in 0..2, legacy missing or agreeing (legacy == (mode is Tags))
//                                         -> that mode (so mode 2 with legacy false stays Folders)
//   a stored groupMode in 0..2, legacy present and disagreeing
//                                         -> the legacy bool: true = Tags, false = None
//   missing / not a number / out of range -> the legacy bool: true = Tags, false or missing = None
//
// The result is always 0, 1 or 2. The values are the same numbers as SidebarPainter::GroupMode
// (MainWindow.cpp static_asserts it), kept here as plain ints so this header needs neither Qt nor
// the painter, and so tests/sidebar_group_mode_test.cpp can run without either.
//
// The CALLER turns a QSettings value into the two optionals: an absent key, or one that does not
// parse as a number, is std::nullopt. (A garbage mode string is therefore the same as a missing
// one, and falls back to the legacy bool instead of silently choosing None.)

#include <optional>

namespace talq {

constexpr int kSidebarGroupNone    = 0;
constexpr int kSidebarGroupTags    = 1;
constexpr int kSidebarGroupFolders = 2;

inline int startupSidebarGroupMode(std::optional<long long> storedMode,
                                   std::optional<bool> legacyGroupByTag)
{
    if (storedMode && *storedMode >= kSidebarGroupNone && *storedMode <= kSidebarGroupFolders) {
        const int mode = static_cast<int>(*storedMode);
        // The new build writes both keys together, so a legacy value that differs from what
        // the mode implies was written later, by an older build: it is the newer choice.
        if (legacyGroupByTag && *legacyGroupByTag != (mode == kSidebarGroupTags))
            return *legacyGroupByTag ? kSidebarGroupTags : kSidebarGroupNone;
        return mode;
    }
    return (legacyGroupByTag && *legacyGroupByTag) ? kSidebarGroupTags : kSidebarGroupNone;
}

} // namespace talq
