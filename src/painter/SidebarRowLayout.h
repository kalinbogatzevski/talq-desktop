#pragma once
// Pure, Qt-free sidebar row geometry, extracted so the switch from a
// uniform-row-height list to a list that also contains TAG SECTION HEADERS is
// unit-testable (mirrors ChatSyncLogic.h / ServerCapabilities.h).
//
// WHY THIS EXISTS. Until 0.65.1 the sidebar was uniform-row-height BY
// CONSTRUCTION: every position was `visibleIdx * rowH` and the inverse was
// `rowAtY() = canvasY / rowH`, spread across ~17 call sites in
// SidebarPainter.cpp. `m_visibleIndices` held nothing but conversation indices
// — there was no concept of a row that is not a conversation. Tag grouping
// needs headers, which are a different height, so that arithmetic has to
// become a table.
//
// THE REGRESSION THIS GUARDS. With no headers the table MUST reproduce the old
// arithmetic exactly, or every existing behaviour that depends on it — hit
// testing, hover, the scrollbar, viewport culling — shifts by a row. That
// equivalence is the first thing the test pins.
#include <cstddef>
#include <vector>

namespace talq {

// The built-in "Other" folder section (conversations filed nowhere). Not a real
// folder, so it has no index into the folder list: -1 is "no folder section at
// all", >= 0 is a folder, and this is the third thing a row can be in.
constexpr int kFolderOther = -2;

// One row in the visible list: either a conversation or a section header.
struct SidebarRow {
    int layoutIndex = -1;   // index into SidebarPainter::m_layouts; <0 = header
    int tagIndex = -1;      // index into the tag list (tag headers, and rows under one)
    // Folder grouping, kept apart from tagIndex ON PURPOSE: every reader of
    // tagIndex (the header paint, the click branch that PUTs a collapse to the
    // Talk server) assumes a tag, so a folder header that reused it would either
    // paint nothing or send a folder id to the server. -1 = not in a folder
    // section; >= 0 = the folder at that index in the folder list;
    // kFolderOther = the built-in Other section. Set on a folder HEADER and on
    // every conversation row under it, so a drop onto either resolves the same.
    int folderIndex = -1;
    double y = 0;           // top edge in CONTENT coordinates (not viewport)
    int height = 0;

    bool isHeader() const { return layoutIndex < 0; }
    // Header kinds. A header is exactly one of: tag header (tagIndex only),
    // folder header (folderIndex only). Test the KIND before reading either index.
    bool isFolderHeader() const { return isHeader() && folderIndex != -1; }
    bool isTagHeader() const { return isHeader() && folderIndex == -1; }
};

// Stamp y/height onto every row, top to bottom. Headers take headerH,
// conversations convH. Call after the row list is built and ordered; nothing
// else may set y.
inline void assignRowGeometry(std::vector<SidebarRow> &rows, int convH, int headerH)
{
    double y = 0;
    for (SidebarRow &r : rows) {
        r.height = r.isHeader() ? headerH : convH;
        r.y = y;
        y += r.height;
    }
}

// Total scrollable content height.
inline double totalHeight(const std::vector<SidebarRow> &rows)
{
    if (rows.empty())
        return 0;
    return rows.back().y + rows.back().height;
}

// Row index at a CONTENT-coordinate y, or -1 when outside the content.
//
// Deliberately a scan rather than `y / rowH`: once headers exist the rows are
// no longer a single height, and the old division silently returns a wrong row
// instead of failing. Rows are few (a sidebar holds tens, not thousands) and
// this runs on click/hover, so the linear walk is not worth optimising away —
// correctness here is worth more than the microseconds.
inline int rowAtY(const std::vector<SidebarRow> &rows, double canvasY)
{
    if (canvasY < 0)
        return -1;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (canvasY < rows[i].y + rows[i].height)
            return static_cast<int>(i);
    }
    return -1;   // below the last row
}

// Does this row list contain any header? Used to decide whether the flat
// (0.64-identical) paths can be taken.
inline bool hasHeaders(const std::vector<SidebarRow> &rows)
{
    for (const SidebarRow &r : rows) {
        if (r.isHeader())
            return true;
    }
    return false;
}

// A section is worth rendering only if it has at least one conversation.
// Upstream hides empty tag sections, and a screen of headers with nothing
// under them is worse than no grouping at all — particularly "Other", which is
// empty exactly when the user has tagged everything.
//
// `counts` is per-tag conversation counts; returns true if a header for
// `tagIndex` should be emitted.
inline bool sectionVisible(const std::vector<int> &counts, int tagIndex)
{
    if (tagIndex < 0 || tagIndex >= static_cast<int>(counts.size()))
        return false;
    return counts[static_cast<std::size_t>(tagIndex)] > 0;
}

// ── Folder sections (drag and drop, reveal, header layout) ──

// The folder section a row belongs to, for resolving a drop: a folder header or
// any conversation row inside that folder gives the folder's index; the Other
// header or a row in Other gives kFolderOther; -1 when there is no row there or
// the row is outside every folder section (flat list, tag header).
inline int folderSectionOfRow(const std::vector<SidebarRow> &rows, int row)
{
    if (row < 0 || row >= static_cast<int>(rows.size()))
        return -1;
    return rows[static_cast<std::size_t>(row)].folderIndex;
}

// Would dropping a conversation on `targetSection` change anything? `current`
// is the section the conversation is in now (unfiled, or filed under a folder
// that no longer exists, is kFolderOther). A drop on its own section is a
// no-op, and a drop with no target (-1) is refused.
inline bool dropChangesFolder(int targetSection, int currentSection)
{
    return targetSection != -1 && targetSection != currentSection;
}

// Scroll position that brings [top, bottom] fully into a view `viewH` tall,
// moving as little as possible; unchanged when it is already fully visible. A
// row taller than the view is aligned to its top. The caller still clamps to
// the content height.
inline double scrollToReveal(double scrollY, double top, double bottom, double viewH)
{
    if (top < scrollY)
        return top;
    if (bottom > scrollY + viewH)
        return (bottom - top > viewH) ? top : bottom - viewH;
    return scrollY;
}

// Signed pixels to scroll on one auto-scroll tick while a drag hovers at
// viewport y. Zero in the middle of the view; inside `zone` px of the top or
// bottom edge it scrolls toward that edge, 1 px at the zone's inner border up
// to `maxStep` at (and beyond) the edge. Negative = up. The zone is capped at
// half the view so the top and bottom zones can never overlap.
inline double dragAutoScrollStep(double y, double viewH, double zone, double maxStep)
{
    if (viewH <= 0 || zone <= 0 || maxStep <= 0)
        return 0;
    if (zone > viewH / 2)
        zone = viewH / 2;
    double depth = 0;     // 0 at the zone's inner border .. 1 at the edge
    double sign = 0;
    if (y < zone) {
        depth = (zone - y) / zone;
        sign = -1;
    } else if (y > viewH - zone) {
        depth = (y - (viewH - zone)) / zone;
        sign = 1;
    } else {
        return 0;
    }
    if (depth > 1)
        depth = 1;
    return sign * (1 + depth * (maxStep - 1));
}

// Where the pieces of a folder header sit, left to right: [triangle] name ...
// count [badge]. The badge (closed folder with unread) and the count are
// placed from the RIGHT edge first and the name takes what is left, so a long
// folder name is elided against them rather than painted through them. That
// ordering is the point: eliding first and placing the badge after would let a
// long name run under it.
struct FolderHeaderSpans {
    double nameW = 0;     // width the name may use, starting at textX
    double countX = 0;    // left edge of the count box (countW wide)
    double badgeX = 0;    // left edge of the unread badge; meaningful only when badgeW > 0
};
inline FolderHeaderSpans folderHeaderSpans(double totalW, double textX, double padRight,
                                           double countW, double badgeW, double gap)
{
    FolderHeaderSpans s;
    double right = totalW - padRight;
    if (badgeW > 0) {
        s.badgeX = right - badgeW;
        right = s.badgeX - gap;
    }
    s.countX = right - countW;
    s.nameW = s.countX - textX;
    if (s.nameW < 0)
        s.nameW = 0;
    return s;
}

} // namespace talq
