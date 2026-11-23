#pragma once

// Text from the server shown in a tooltip, exactly as written (test: tooltip-text-test).
//
// QToolTip has no text-format setting: its label decides per string with
// Qt::mightBeRichText(), which says "rich" when the first line holds a tag Qt
// knows (<b>, <img>, <a>, ...) or the entity "&lt;". So a conversation named
// "<b>x</b>" passed raw showed a bold "x", an "<img ...>" became a picture
// placeholder and an "<a ...>" a link. Escaping alone is not enough either: an
// escaped "R&D" is "R&amp;D", which Qt does NOT take for rich text, so the tip
// would show the entity. plainTooltipHtml() therefore escapes the text AND wraps
// it in an element, which makes it rich text for every input, shown verbatim.
//
// `white-space:pre` keeps it looking like the plain-text tooltip it replaces
// (one line, repeated spaces and line breaks kept), measured against the real
// QToolTip on Qt 6.10. One difference remains: Qt wraps a PLAIN tooltip that is
// wider than the screen, but a rich one in white-space normal, nowrap or pre stays
// on one line however long it is (pre-wrap does wrap, at a width far too narrow
// to use), so a name wider than the whole screen runs off its edge.
//
// For a name, a title or any other single string. A tooltip built from several
// pieces escapes each piece itself (see ReactionWhoLogic.h).

#include <QString>

namespace talq {

// `text` as tooltip HTML that shows every character as written. Empty stays
// empty, so QToolTip::showText still hides the tip for it.
inline QString plainTooltipHtml(const QString &text)
{
    if (text.isEmpty())
        return {};
    return QStringLiteral("<div style='white-space:pre'>") + text.toHtmlEscaped()
           + QStringLiteral("</div>");
}

} // namespace talq
