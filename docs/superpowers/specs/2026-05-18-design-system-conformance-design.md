# TalQ Design-System Conformance — Design

Date: 2026-05-18
Status: Implemented (0.30.x line, post call-bar redesign)

## Problem

TalQ has a formal design guide ("calm, warm, fast" + 4-theme, embodied in
code by `PainterTheme`), but widget chrome was hand-styled per dialog: 14
files carried hardcoded-hex `setStyleSheet` blocks (`#141210`, `#14b8a6`,
…) that did not track the 4 themes and each drifted. The call control bar
was the only surface doing it right (vector icons + state tints from
`PainterTheme`). Goal: the entire app conforms to the guide from a single
source of truth — "not one button one style, another button another".

## Principle

Zero hardcoded colour/radius in `src/ui`. Every chrome surface is
*generated* from `PainterTheme` (the code embodiment of the guide). One
place defines it; everything inherits; all 4 themes track automatically;
nothing can drift per-widget again.

## Architecture (hybrid: shared QSS + reusable painted button)

### Component A — `src/ui/AppStyle.{h,cpp}`
`AppStyle::sheet(const PainterTheme&)` returns the single app-wide
stylesheet for all generic chrome (QDialog, QPushButton, QToolButton,
QLineEdit, QComboBox, QCheckBox, QListWidget, QMenu, QScrollBar, QToolTip,
QProgressBar, QGroupBox, QTabBar, QFrame separators). Never bare `QWidget`
(QPainter surfaces paint their own ground). Applied via
`qApp->setStyleSheet()` inside `MainWindow::restyleChrome()` (runs at init
and on every `applyThemeId`). Semantics, not colours, at call sites via
dynamic properties: `variant` = primary|danger|ghost on buttons; `role` =
secondary|muted|title|danger|success on labels; `QFrame` `role`=hint|card.
`AppStyle::installRepolishFilter()` re-polishes a widget when its
`variant`/`role` changes at runtime.

### Component B — `src/painter/VectorIcons.h` + `src/ui/TalqIconButton.{h,cpp}`
`VectorIcons::draw()` is the 24-unit vector icon set lifted out of
CallStage's file-local `drawCallIcon` (CallStage now includes it; de-dup),
extended with send/attach/emoji/search/close/plus/back/gear/copy/forward/
trash/belloff. `TalqIconButton` is the call-bar chip generalised: a
`QAbstractButton` painting a `VectorIcons` glyph on a palette-driven
rounded chip with the same state language (hover wash / active accent
chip / off → dim + slash / danger). Composer attach/emoji/send use it.

### Bespoke components
Frameless/stateful widgets that need their own shape (StatusPopover,
EmojiPickerWidget, TopicTabBar, NewChatDialog, ConversationInfoDialog,
ComposerWidget) keep a small **palette-driven** sheet built in an
`applyChrome()` that re-runs from `changeEvent(QEvent::PaletteChange)`.
`QApplication::setPalette` is set app-wide from `PainterTheme` in
`MainWindow::applyDarkPalette()`, so `palette()` is the theme.

## Anti-drift guardrail

Review rule: no raw hex / ad-hoc colour `setStyleSheet` in `src/ui`.
Verify: `grep -rnE 'setStyleSheet\("[^"]*#[0-9a-fA-F]{6}' src/ui/*.cpp`
must be empty. `CallDialog.cpp` is dead (not in the CMake target) — ignore.

## Decisions / exceptions

- ImageViewerDialog stays **black** — neutral true-colour ground for
  photos (every OS image viewer does this). Justified, not drift.
- Side-stripe `border-left` accents removed (SettingsDialog hint box,
  composer reply bar) → full callouts / leading glyphs. Banned pattern.

## Files

New: `src/ui/AppStyle.{h,cpp}`, `src/ui/TalqIconButton.{h,cpp}`,
`src/painter/VectorIcons.h`. Modified: `MainWindow.cpp` (qApp sheet in
restyleChrome, upload bar, palette HighlightedText), `CallStage.cpp` (use
VectorIcons), `ComposerWidget.{h,cpp}` (TalqIconButton + applyChrome),
StatusPopover/EmojiPickerWidget/TopicTabBar/NewChatDialog/
ConversationInfoDialog/SettingsDialog/SharePickerDialog/
NextcloudFilePickerDialog/ScheduledMessagesDialog/UpcomingRemindersDialog/
SelectionBarWidget/LoginWidget (strip hardcoded chrome, tag variants/roles
or palette-driven), `CMakeLists.txt`.

## Verification

Debug build green; app runs; the hex grep above is empty across all built
UI files. Recommended follow-up: `impeccable critique` against
PRODUCT.md/DESIGN.md, then targeted `polish`.
