---
name: TalQ
description: A warm, calm, fast native desktop client for Nextcloud Talk. User-selectable warm themes, distinctive through palette and craft, never a blue chat app.
themeNote: "TalQ ships four user-selectable themes (Ember, Warm, Vivid, Paper). The token values below are the DEFAULT theme, Vivid. Every theme swaps the whole ramp; no theme uses cool gray. PainterTheme is the single source of truth."
colors:
  accent: "#21e3c8"
  accent-paper: "#0d9488"
  control-ink: "#06201c"
  bg-primary: "#271d12"
  bg-sidebar: "#211810"
  bg-secondary: "#312517"
  bg-surface: "#3d2e1b"
  bg-selected: "#46341d"
  bg-hover: "#342711"
  bg-message-own: "#1f5a4f"
  divider: "#4b3a23"
  text-primary: "#fcf5e7"
  text-secondary: "#c8b89a"
  text-time: "#9c8c6d"
  online: "#66dd76"
  danger: "#ff9163"
  amber: "#ffb84a"
  author-teal: "#21e3c8"
  author-clay: "#e07060"
  author-amber: "#f0a050"
  author-leaf: "#5ec76a"
  author-violet: "#9b7cd4"
  author-rose: "#e87aae"
  author-cyan: "#50b8c8"
  author-blue: "#5a9ecf"
typography:
  display:
    fontFamily: "Inter, system-ui, sans-serif"
    fontSize: "25px"
    fontWeight: 600
    lineHeight: 1.2
    letterSpacing: "-0.3px"
  title:
    fontFamily: "Inter, system-ui, sans-serif"
    fontSize: "15px"
    fontWeight: 600
    lineHeight: 1.3
    letterSpacing: "normal"
  body:
    fontFamily: "Inter, system-ui, sans-serif"
    fontSize: "14px"
    fontWeight: 400
    lineHeight: 1.45
    letterSpacing: "normal"
  label:
    fontFamily: "Inter, system-ui, sans-serif"
    fontSize: "12px"
    fontWeight: 600
    lineHeight: 1.3
    letterSpacing: "normal"
  caption:
    fontFamily: "Inter, system-ui, sans-serif"
    fontSize: "11px"
    fontWeight: 400
    lineHeight: 1.3
    letterSpacing: "0.4px"
  telemetry:
    fontFamily: "Consolas, 'Cascadia Mono', ui-monospace, monospace"
    fontSize: "11px"
    fontWeight: 600
    lineHeight: 1.3
    letterSpacing: "0.4px"
rounded:
  sm: "6px"
  md: "10px"
  control: "8px"
  card: "13px"
  pill: "999px"
spacing:
  tiny: "4px"
  sm: "8px"
  md: "12px"
  lg: "16px"
  xl: "24px"
components:
  button-primary:
    backgroundColor: "{colors.accent}"
    textColor: "{colors.control-ink}"
    typography: "{typography.label}"
    rounded: "{rounded.control}"
    padding: "8px 18px"
  button-default:
    backgroundColor: "{colors.bg-hover}"
    textColor: "{colors.text-primary}"
    typography: "{typography.label}"
    rounded: "{rounded.control}"
    padding: "8px 16px"
  button-danger:
    backgroundColor: "transparent"
    textColor: "{colors.danger}"
    rounded: "{rounded.control}"
    padding: "8px 14px"
  conversation-row:
    backgroundColor: "{colors.bg-sidebar}"
    textColor: "{colors.text-primary}"
    padding: "8px 12px"
  conversation-row-selected:
    backgroundColor: "{colors.bg-selected}"
    textColor: "{colors.text-primary}"
    padding: "8px 12px"
  message-bubble-own:
    backgroundColor: "{colors.bg-message-own}"
    textColor: "{colors.text-primary}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    padding: "8px 12px"
  message-bubble-other:
    backgroundColor: "{colors.bg-surface}"
    textColor: "{colors.text-primary}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    padding: "8px 12px"
  unread-badge:
    backgroundColor: "{colors.accent}"
    textColor: "{colors.control-ink}"
    typography: "{typography.caption}"
    rounded: "{rounded.pill}"
    padding: "1px 7px"
  status-tile:
    backgroundColor: "{colors.bg-surface}"
    textColor: "{colors.text-primary}"
    rounded: "{rounded.card}"
    padding: "13px 16px"
---

# Design System: TalQ

## 1. Overview

**Creative North Star: "Calm, Warm, Fast"**

TalQ is a quick, quiet, warm room you keep open all day. The surface stays
low-contrast and recedes so the conversation is the only thing that asks for
attention; the interactions feel immediate under the hand. Distinctiveness
comes from the warm palette and visible craft, never from a display typeface
or any costume. The v0.27.0 editorial direction ("The Field Notebook",
Instrument Serif) was tried and rejected as the wrong register for a chat
tool; this system replaces it. Do not reintroduce a serif/editorial tier.

The system runs on a deliberate tension. The *surface* is quiet (low-contrast,
content-first, calm), while the *controls* are tactile and confident (buttons,
inputs, rows have real presence and unambiguous affordance). A calm page, a
pen that feels solid in the hand. This serves both PRODUCT.md audiences: power
users get a surface that never shouts, occasional and external users get
controls they cannot misread.

It explicitly rejects the four PRODUCT.md anti-references: generic
Material/"AI slop", the default Nextcloud Talk web UI, the cold-blue chat-app
category reflex, and the editorial costume.

**Key Characteristics:**
- Warm surfaces tinted toward umber/amber, never neutral or cool gray, in
  every theme
- One saturated accent (teal family) used sparingly as the single signal
- Flat at rest; depth from a warm tonal ladder, with a sanctioned soft glow
  reserved for *state* (active, hover, presence, ambient)
- Inter only; type and spacing carry the hierarchy
- Calm chrome, confident controls, fast transitions

## 2. Colors (the four-theme system)

Color is not a single palette. `PainterTheme` is an `enum class Theme { Ember,
Warm, Vivid, Paper }` and every color in the app is a token resolved from the
active theme. The user picks the theme (Ctrl+D, the sidebar swatch, or
Settings) and it persists. The painters re-tint live; the Mission Control home
rebuilds itself on theme change.

- **Ember** - the most restrained warm dark; base lifted just off black.
- **Warm** - richer warm-dark with more color presence.
- **Vivid** - the **default**: bold and alive, maximum warm flavour, calm
  structure. The token values in this file's frontmatter are Vivid's.
- **Paper** - warm light, bright by nature, for daylight use.

Token roles (resolved per theme; Vivid values shown):
- **accent** (#21e3c8; Paper #0d9488): the single signal. Unread, active
  conversation marker, primary buttons, focus, links, the TalQ peer marker,
  the brand "Q". Its scarcity is what makes it read as signal.
- **control-ink** (#06201c): ink/glyph color on an accent fill.
- **Ladder** bg-primary → bg-sidebar → bg-secondary → bg-surface, plus
  bg-selected / bg-hover: warm-tinted grounds, each step a lighter warm tint.
- **text-primary / text-secondary / text-time**: warm cream → muted →
  faintest. Never #fff, never cool gray.
- **bg-message-own** (#1f5a4f): sent bubble; "mine" reads by hue, not a loud
  fill.
- **online** (#66dd76) success/presence, **danger** (#ff9163) destructive and
  errors (calm clay, not fire-engine red), **amber** (#ffb84a) warm secondary
  (favorite, away, accents).
- **Author palette** (8 fixed hues hashed from actor id): group-chat author
  names and avatar fallbacks. Topic palette is the first six.

### Named Rules
**The One Signal Rule.** The accent covers at most ~10% of any screen and
marks exactly one meaning: "this needs you" (unread, active, primary action).
Two competing accent things on one screen means one is wrong.

**The No-Gray Rule.** No neutral or cool gray exists in any theme. Every
surface, text, and divider value is tinted toward the theme's warm hue. A
value that reads as gray is a bug, not a neutral.

## 3. Typography

**One face: Inter** (bundled, with system-ui, sans-serif fallback) for
everything. **Telemetry** chrome (Mission Control values, tags, footer) uses a
monospace (Consolas / Cascadia Mono) purely for tabular control-room legibility,
never for prose.

**Character:** quiet and screen-optimised at the small sizes a chat client
lives at. Identity is the warmth and the craft, not the typeface. There is no
display/serif tier.

### Hierarchy
- **Display** (Inter 600, 25px): the welcome greeting, the largest moment.
- **Title** (Inter 600, 15px): header conversation title, sidebar names.
- **Body** (Inter 400, 14px): message text. The reading tier.
- **Label** (Inter 600, 12px): secondary labels, roles, button text.
- **Caption** (Inter 400, 11px, +0.4px, often uppercased): timestamps, system
  messages, section eyebrows, Mission Control tile keys.

### Named Rules
**The Two-Lever Rule.** Hierarchy is built from size and weight only (Inter
400 vs 600). No italics for hierarchy, no third weight for chrome, no
color-as-hierarchy.

**The Scale Honesty Rule.** A headline needs a real larger size, never bolded
14px body text standing in for one.

## 4. Elevation

Flat at rest. Depth comes from the warm tonal ladder: a raised surface is one
step up the ladder (lighter, warmer tint), the way paper on a desk reads as
raised by tone, not by a cast shadow.

One thing is sanctioned beyond flat: a soft, theme-colored **glow reserved for
state**, never for decoration. It appears as the ambient accent wash behind
the thread, the halo on the active conversation row and the send button on
hover, and the breathing presence dot. These communicate live state at a
glance and are part of the warmth; they honor reduced-motion.

### Named Rules
**The Flat-But-Stateful Rule.** Surfaces are flat at rest. Forbidden:
decorative `box-shadow`, glassmorphism, blur, or gradient text on any surface.
*Permitted:* a soft accent glow that encodes state (active/hover/presence/
ambient), tasteful and low-opacity, honoring reduced-motion. If a glow is not
carrying state, it is decoration and is forbidden.

**The Ladder Rule.** Raise a surface by exactly one step up the ladder
(primary → sidebar → secondary → surface). Never skip a step, never invent an
intermediate value, never raise by shadow.

## 5. Components

Controls are tactile and confident against the quiet ground; chrome is calm.

### Buttons
- **Shape:** 8px control radius. Never pill, never square.
- **Primary:** accent fill, control-ink text, SemiBold, 8px 18px. The only
  filled control; the one primary action on a surface. Hover/pressed shift
  within the accent.
- **Default:** bg-hover fill, text-primary, 8px 16px. The quiet workhorse.
- **Danger:** transparent fill, danger text, hairline danger border. Calm
  until hovered.
- **Self-contained styling rule:** a control's full visual style lives on the
  control itself, never inherited from an ancestor's selector-less stylesheet
  (a real, fixed defect class in TalQ; see Do's and Don'ts).

### Conversation Row (signature)
Avatar (circle) + name (Title) + preview (text-secondary) + time (Caption) +
optional unread badge, favorite dot, presence dot. Ground bg-sidebar; hover
bg-hover with a soft accent halo; selected bg-selected with the accent active
marker. High density, never cramped: the screen power users stare at all day.

### Message Bubble (signature)
- **Own:** bg-message-own, right-aligned, 10px radius. "Mine" by hue.
- **Other:** bg-surface, left-aligned, 10px radius, author-color name in
  group chats.
- **Grouping:** consecutive messages from one author tighten and drop the
  repeated avatar/name. The thread reads as paragraphs, not stamped cards.

### Inputs / Fields
bg-surface or bg-secondary well, hairline divider stroke, 8px radius,
text-primary, faint placeholder. Focus is an accent border.

### Mission Control (the empty-state home, signature)
When no conversation is selected the home is a live status board, not a blank
slate or a wall of diagnostics:
- a command bar (brand wordmark, BUILD/brand tags, a breathing system-status
  pill that goes amber when a subsystem drops);
- a greeting that still states the empty state and what to do next;
- a telemetry grid of tiles (server, signaling, push, Nextcloud, Talk, GPU),
  each a status LED + uppercase key + value + monospace sub-line;
- a SUBSYSTEMS strip of GStreamer codec/transport chips, green/danger;
- a FLIGHT LOG panel: the changelog framed as a mission log, fills height;
- a monospace footer readout.
All theme-tokenized and rebuilt on theme change. It is the clearest
expression of "calm surface, confident control": glanceable, not noisy.

### Badges, Pills, Header
Unread badge: accent pill, control-ink text, Caption. Date/unread separators
are warm pills, not full-width rules. Header bar: small avatar + Title + a
single Caption subtitle carrying one fact (presence, typing, or the peer's
TalQ version), with calm icon buttons at the trailing edge.

## 6. Do's and Don'ts

### Do:
- **Do** keep the accent under ~10% of any screen (One Signal). Scarcity is
  the design.
- **Do** resolve every color from the active theme token; tint every neutral
  warm (No-Gray), in all four themes.
- **Do** build hierarchy from size and weight only, Inter throughout (Two-Lever).
- **Do** raise surfaces one step up the warm ladder (Ladder), never by shadow.
- **Do** use the soft glow only to encode state, and honor reduced-motion.
- **Do** give every control its full style on the control itself, immune to
  ancestor stylesheets.
- **Do** keep every theme at WCAG AA contrast and respect user font scaling.

### Don't:
- **Don't** use `#000` or `#fff` anywhere, including Paper's surfaces.
- **Don't** add decorative `box-shadow`, glassmorphism, blur, or gradient
  text (Flat-But-Stateful, and the impeccable absolute bans). A glow that
  isn't state is decoration.
- **Don't** use a colored side-stripe (`border-left`/`border-right` > 1px) as
  an accent on rows, bubbles, or callouts. Full hairline borders, a ladder
  step, or nothing.
- **Don't** reintroduce a serif or editorial display tier; the costume was
  rejected. Distinctiveness is warmth and craft.
- **Don't** drift toward generic Material / "AI slop", settle for parity with
  the Nextcloud Talk web UI, or regress toward the cold-blue category reflex
  (PRODUCT.md anti-references, verbatim).
- **Don't** fake a headline by bolding 14px text (Scale Honesty).
