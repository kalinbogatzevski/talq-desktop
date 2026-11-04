#pragma once

#include "painter/PainterTheme.h"

#include <QColor>
#include <QString>

class QLineEdit;

// A search box's clear affordance: an "x" inside the right end of the field, shown only while
// the field has text, and Esc clearing the text while the field has focus.
//
// Both go through QLineEdit::clear(), so the field's own textChanged handler is the only thing
// that reacts: the chat list's filter is reset exactly as if the user had deleted the text
// (one textChanged, no second path into the filter). The field keeps (or gets) the focus, so
// the next query can be typed at once. An EMPTY field leaves Esc alone: whatever the window did
// with it before still happens (in a dialog, Esc still closes it).
//
// The "x" is VectorIcons' "close" glyph, tinted from PainterTheme, not the style's clear
// button: that icon is the platform style's own (a different one offscreen than on Windows 11)
// and ignores the theme. Used by the chat-list search (MainWindow) and the add-people search in
// the Conversation info dialog. Test: search-field-clear-test; its colours are scored in
// theme-conformance-test through colorsFor().
namespace talq::searchclear {

struct Colors {
    QColor glyph;          // at rest
    QColor glyphPressed;   // while the mouse button is down on it
};

// The neighbouring icon buttons' inks (the sidebar's Home and Filter glyphs: textSecondary,
// textPrimary on hover). Both hold 4.5:1 on every ground a search field has.
inline Colors colorsFor(const PainterTheme &t) { return { t.textSecondary, t.textPrimary }; }

// Adds the affordance to `field`. Calling it again on the same field does nothing.
void attach(QLineEdit *field, const PainterTheme &theme);

// Re-tints the "x" for a theme change (its icon is baked, like SelectionBarWidget's).
void retint(QLineEdit *field, const PainterTheme &theme);

// The chat list's pill-shaped search field, from theme tokens. Lives here so the test renders
// exactly the sheet MainWindow::restyleChrome() applies.
QString sidebarPillStyleSheet(const PainterTheme &t);

} // namespace talq::searchclear
