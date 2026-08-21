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

// One row in the visible list: either a conversation or a section header.
struct SidebarRow {
    int layoutIndex = -1;   // index into SidebarPainter::m_layouts; <0 = header
    int tagIndex = -1;      // index into the tag list (headers only)
    double y = 0;           // top edge in CONTENT coordinates (not viewport)
    int height = 0;

    bool isHeader() const { return layoutIndex < 0; }
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

} // namespace talq
