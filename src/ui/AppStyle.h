#pragma once

#include <QString>
#include "painter/PainterTheme.h"

// ── TalQ single source of truth for widget chrome ───────────────────────
//
// The whole app's Qt-widget styling (dialogs, buttons, inputs, combos,
// menus, scrollbars, tooltips, lists) is GENERATED from PainterTheme — the
// code embodiment of the "calm, warm, fast" + 4-theme design guide. There
// is exactly one stylesheet, applied via qApp->setStyleSheet() from
// MainWindow::restyleChrome() at startup and on every theme change, so all
// four themes track automatically and nothing can drift per-widget.
//
// RULE (anti-drift): do NOT put raw hex or ad-hoc setStyleSheet() colour in
// src/ui. Express intent with a dynamic property and let AppStyle paint it:
//
//     btn->setProperty("variant", "primary");   // accent call-to-action
//     btn->setProperty("variant", "danger");    // destructive (outline)
//     btn->setProperty("variant", "ghost");     // quiet text button
//     lbl->setProperty("role",    "secondary"); // dimmer caption text
//     lbl->setProperty("role",    "muted");     // faintest hint text
//
// QSS variant/role selectors only re-evaluate on a style re-polish; AppStyle
// installs an app-wide event filter so a setProperty() on an already-shown
// widget repaints without each call site needing unpolish/polish.

namespace AppStyle {

// The complete app stylesheet for the active theme.
QString sheet(const PainterTheme &t);

// Install the dynamic-property repolish filter (idempotent; call once).
void installRepolishFilter();

} // namespace AppStyle
