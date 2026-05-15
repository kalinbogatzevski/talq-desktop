---
name: TalQ
description: A warm, paper-like native desktop client for Nextcloud Talk. Calm, content-first, unmistakably not a blue chat app.
colors:
  signal-teal: "#14b8a6"
  signal-teal-bright: "#2dd4bf"
  signal-teal-deep: "#0d9488"
  control-ink: "#0e1817"
  pressed-ink: "#141210"
  ink-sidebar: "#18140f"
  ink-raised: "#1a1613"
  ink-surface: "#221d19"
  ink-selected: "#2a211a"
  ink-hover: "#241f1a"
  own-message-teal: "#1c3330"
  warm-newsprint: "#f4efe6"
  newsprint-muted: "#a8a096"
  newsprint-faint: "#7a726a"
  newsprint-ghost: "#5a5348"
  divider-warm: "#2a241f"
  presence-green: "#5ec76a"
  alert-clay: "#e8866b"
  author-teal: "#14b8a6"
  author-clay: "#e07060"
  author-amber: "#f0a050"
  author-leaf: "#5ec76a"
  author-violet: "#9b7cd4"
  author-rose: "#e87aae"
  author-cyan: "#50b8c8"
  author-blue: "#5a9ecf"
typography:
  display:
    fontFamily: "Instrument Serif, Georgia, serif"
    fontSize: "26px"
    fontWeight: 400
    lineHeight: 1.15
    letterSpacing: "normal"
  title:
    fontFamily: "Instrument Serif, Georgia, serif"
    fontSize: "20px"
    fontWeight: 400
    lineHeight: 1.25
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
rounded:
  sm: "6px"
  md: "10px"
  control: "8px"
  pill: "999px"
spacing:
  tiny: "4px"
  sm: "8px"
  md: "12px"
  lg: "16px"
  xl: "24px"
components:
  button-primary:
    backgroundColor: "{colors.signal-teal}"
    textColor: "{colors.control-ink}"
    typography: "{typography.label}"
    rounded: "{rounded.control}"
    padding: "8px 18px"
  button-primary-hover:
    backgroundColor: "{colors.signal-teal-bright}"
    textColor: "{colors.control-ink}"
    rounded: "{rounded.control}"
    padding: "8px 18px"
  button-default:
    backgroundColor: "{colors.ink-hover}"
    textColor: "{colors.warm-newsprint}"
    typography: "{typography.label}"
    rounded: "{rounded.control}"
    padding: "8px 16px"
  button-danger:
    backgroundColor: "transparent"
    textColor: "{colors.alert-clay}"
    rounded: "{rounded.control}"
    padding: "8px 14px"
  button-ghost:
    backgroundColor: "transparent"
    textColor: "{colors.newsprint-muted}"
    rounded: "{rounded.control}"
    padding: "8px 14px"
  conversation-row:
    backgroundColor: "{colors.ink-sidebar}"
    textColor: "{colors.warm-newsprint}"
    padding: "8px 12px"
  conversation-row-selected:
    backgroundColor: "{colors.ink-selected}"
    textColor: "{colors.warm-newsprint}"
    padding: "8px 12px"
  message-bubble-own:
    backgroundColor: "{colors.own-message-teal}"
    textColor: "{colors.warm-newsprint}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    padding: "8px 12px"
  message-bubble-other:
    backgroundColor: "{colors.ink-surface}"
    textColor: "{colors.warm-newsprint}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    padding: "8px 12px"
  unread-badge:
    backgroundColor: "{colors.signal-teal}"
    textColor: "{colors.pressed-ink}"
    typography: "{typography.caption}"
    rounded: "{rounded.pill}"
    padding: "1px 7px"
---

# Design System: TalQ

## 1. Overview

**Creative North Star: "The Field Notebook"**

TalQ is a worn leather field notebook, not a glass dashboard. Surfaces are soft, warm, and paper-like. Chrome stays low-contrast and recedes so the writing (the conversation) is the only thing that asks for attention. The mood is unhurried and human: a tool you keep open all day without it raising your pulse. Depth is conveyed the way stacked paper conveys it, by warm tonal layering, never by drop shadows or glass.

The system runs on a deliberate tension. The *surface* is quiet (Field Notebook: low-contrast, content-first, calm), while the *controls* are tactile and confident (buttons, inputs, and rows have real presence and unambiguous affordances). A calm page, a pen that feels solid in the hand. This serves both audiences in PRODUCT.md: power users get a quiet surface that never shouts, occasional and external users get controls they cannot misread.

This system explicitly rejects the three anti-references from PRODUCT.md. It is not generic Material or "AI slop" (no cold gray neutrals, no purple-on-white gradients, no cookie-cutter card grids). It is not the default Nextcloud Talk web UI (parity is failure; the craft gap is the point). It is not the category reflex of a cold-blue chat app (the warmth is the entire identity and is non-negotiable).

**Key Characteristics:**
- Warm, paper-like surfaces tinted toward umber-black, never neutral gray
- A single teal "signal" accent used sparingly against ink and newsprint
- Flat by default; depth only from a four-step warm tonal ladder
- Type and spacing carry the hierarchy; ornament is near-zero
- Calm chrome, confident controls

## 2. Colors

A warm, low-chroma paper-and-ink palette with one saturated teal signal. Every neutral is tinted toward umber; nothing in this system is a true gray.

### Primary
- **Signal Teal** (#14b8a6): The single accent. Unread badges, the active conversation marker, primary buttons, focus, links, the TalQ peer marker. In dark theme it is #14b8a6; in light theme it deepens to **Signal Teal Deep** (#0d9488) for contrast on paper. **Signal Teal Bright** (#2dd4bf) is hover only. Its rarity is what makes it read as signal.

### Neutral (the paper-and-ink ladder)
- **Pressed Ink** (#141210): The base application background. Warm near-black, never #000.
- **Ink Sidebar** (#18140f): The conversation list ground, one step up from base.
- **Ink Raised** (#1a1613): Secondary panels, dialog grounds.
- **Ink Surface** (#221d19): The topmost resting surface (incoming message bubbles, input wells).
- **Ink Selected** (#2a211a) / **Ink Hover** (#241f1a): Row selection and hover, warm-tinted so they never read as gray highlight.
- **Own Message Teal** (#1c3330): Sent-message bubble. A desaturated teal-tinted ink, so "mine" reads by hue, not by a loud fill.
- **Warm Newsprint** (#f4efe6): Primary text. Warm cream on ink, never #fff.
- **Newsprint Muted** (#a8a096): Secondary text, previews.
- **Newsprint Faint** (#7a726a): Timestamps, system messages.
- **Newsprint Ghost** (#5a5348): Placeholders, disabled, the faintest tier.
- **Divider Warm** (#2a241f): Hairline separators, warm-tinted.

### Tertiary (state + identity)
- **Presence Green** (#5ec76a): Online presence and success only. Theme-independent.
- **Alert Clay** (#e8866b): Destructive actions and errors. A muted clay, not a fire-engine red, so warnings stay calm.
- **Author palette** (8 fixed hues, hashed from actor id: Author Teal #14b8a6, Clay #e07060, Amber #f0a050, Leaf #5ec76a, Violet #9b7cd4, Rose #e87aae, Cyan #50b8c8, Blue #5a9ecf): group-chat author names and avatar fallbacks. The 6-color topic palette is the first six of these.

### Named Rules
**The One Signal Rule.** Signal Teal covers at most ~10% of any screen. It marks exactly one thing: "this needs you" (unread, active, primary action). If two teal things compete on screen, one of them is wrong.

**The No-Gray Rule.** There is no neutral gray anywhere. Every surface, text, and divider value is tinted toward the umber/newsprint hue. A value that reads as gray is a bug, not a neutral.

## 3. Typography

**Display / Title Font:** Instrument Serif (bundled, SIL OFL, with Georgia, serif fallback).
**Body / Label Font:** Inter (bundled, with system-ui, sans-serif fallback).

**Character:** A two-face editorial pairing. Instrument Serif carries the confident, distinct personality at the display and title tiers (header conversation title, welcome). Inter remains the calm, screen-optimised workhorse for the body and all chrome, so the reading surface stays legible and unhurried at the small sizes a chat client lives at. The serif is where identity speaks; the body is deliberately quiet.

### Hierarchy
- **Display** (Instrument Serif 400, 26px, line-height 1.15): The welcome name and the largest editorial moment.
- **Title** (Instrument Serif 400, 20px, line-height 1.25): The header conversation title. The serif's character is the confidence; it does not need bolding.
- **Body** (Inter Regular 400, 14px, line-height 1.45): Message text. The reading tier; everything else is chrome.
- **Label** (Inter SemiBold 600, 12px, line-height 1.3): Sidebar conversation names, secondary labels, member roles, the "(bot)" tag, button text. Dense list rows stay Inter for legibility, never the serif.
- **Caption** (Inter Regular 400, 11px, +0.4px tracking, uppercased in dialog eyebrows): Timestamps, system messages, section eyebrows.

### Named Rules
**The Two-Lever Rule.** Within a face, hierarchy is built from size and weight only (Inter 400 vs 600). No italics for hierarchy, no third weight for chrome, no color-as-hierarchy. The serif/sans split is the one sanctioned third axis, and only at the display/title tiers.

**The Serif-Is-For-Headlines Rule.** Instrument Serif appears only at the display and title tiers (header title, welcome). It must never leak into body, dense list rows, labels, or controls; those are always Inter. A serif sidebar row is a bug.

**The Scale Honesty Rule.** A headline needs a real larger size in the display face, never bolded Inter body text standing in for one.

## 4. Elevation

This system is flat. There are no drop shadows, no glass, no blur on any resting surface. Depth is conveyed exclusively by the warm tonal ladder: Pressed Ink (#141210) at the base, Ink Sidebar (#18140f), Ink Raised (#1a1613), Ink Surface (#221d19) on top. Each step up the ladder is a lighter, warmer tint, so a raised surface reads as raised the way a sheet of paper on a desk reads as raised: by tone, not by a cast shadow. This is intrinsic to the QPainter rendering and to the Field Notebook calm.

### Named Rules
**The Flat-Paper Rule.** Surfaces are flat at rest, always. Forbidden: `box-shadow` on cards, panels, rows, bubbles, or dialogs. Depth is a tonal-ladder choice, never a shadow.

**The Ladder Rule.** A surface is raised by moving exactly one step up the ladder (base to sidebar to raised to surface). Never skip a step, never invent an intermediate value, never raise by shadow instead.

## 5. Components

Components are tactile and confident: visible presence, real weight, unambiguous affordance, set against the quiet paper ground.

### Buttons
- **Shape:** Gently rounded (8px control radius). Never pill, never square.
- **Primary:** Signal Teal (#14b8a6) fill, Control Ink (#0e1817) text, SemiBold, padding 8px 18px. The only filled control; reserved for the one primary action on a surface. Hover lifts to Signal Teal Bright (#2dd4bf); pressed sinks to Signal Teal Deep (#0d9488).
- **Default:** Ink Hover (#241f1a) fill, Warm Newsprint text, padding 8px 16px. The quiet workhorse button.
- **Danger:** Transparent fill, Alert Clay (#e8866b) text, hairline clay border. Destructive actions stay calm and recessive until hovered.
- **Ghost:** Transparent fill, Newsprint Muted text. Lowest-emphasis dismissals.
- **Self-contained styling rule:** a control's full visual style must live on the control itself, never inherited from an ancestor's selector-less stylesheet. (This is a real, fixed defect class in TalQ; see Do's and Don'ts.)

### Conversation Row (signature component)
The heart of the app. Avatar (36px circle) + name (Title) + last-message preview (Newsprint Muted) + timestamp (Caption) + optional unread badge, favorite dot, and presence dot. Resting ground is Ink Sidebar; hover is Ink Hover; selected is Ink Selected with the Signal Teal active marker. Row height and internal rhythm use the 8/12px spacing steps. Density is high but never cramped: this is the screen power users stare at all day.

### Message Bubble (signature component)
- **Own:** Own Message Teal (#1c3330), right-aligned, 10px radius. "Mine" reads by warm hue, not a loud fill.
- **Other:** Ink Surface (#221d19), left-aligned, 10px radius, preceded by avatar and author-color name in group chats.
- **Grouping:** Consecutive messages from one author tighten to 6px spacing (vs 10px between authors) and drop the repeated avatar and name. The thread should read as paragraphs, not stamped cards.

### Inputs / Fields
Ink Surface or Ink Raised well, hairline Divider Warm stroke, 8px radius, Warm Newsprint text, Newsprint Ghost placeholder. Focus is a Signal Teal border, never an outer glow (glow is shadow; shadow is forbidden).

### Badges and Pills
Unread badge: Signal Teal pill, Pressed Ink text, Caption type. Date and unread separators are warm-tinted pills, not full-width rules. Presence dot is a single tonal dot, bottom-right of the avatar (the Signal/peer "Q" marker sits top-right so the two never collide).

### Header Bar
54px tall. Small avatar (30px) + Title + a single Caption subtitle that carries one fact at a time (presence, or typing, or the peer's TalQ version), with calm icon buttons (call, search, info) at the trailing edge.

## 6. Do's and Don'ts

### Do:
- **Do** keep Signal Teal under ~10% of any screen (The One Signal Rule). Its scarcity is the design.
- **Do** tint every neutral toward umber/newsprint (The No-Gray Rule). Pure or cool gray is a bug.
- **Do** build hierarchy from size and weight only (The Two-Lever Rule).
- **Do** raise surfaces one step up the warm tonal ladder (The Ladder Rule), never with a shadow.
- **Do** give every control its full style on the control itself, immune to ancestor stylesheets.
- **Do** honor reduced-motion and user font scaling, and keep dark and light at WCAG AA contrast (PRODUCT.md accessibility line).

### Don't:
- **Don't** use `#000` or `#fff` anywhere. This includes the light-theme surface: the current pure `#ffffff` light `bgSurface` violates the No-Gray Rule and is a standing fix, not a precedent.
- **Don't** add `box-shadow`, glassmorphism, blur, or gradient text to any surface (The Flat-Paper Rule, and the impeccable absolute bans).
- **Don't** use a colored side-stripe (`border-left`/`border-right` > 1px) as an accent on rows, bubbles, or callouts. Use full hairline borders, a tonal-ladder step, or nothing.
- **Don't** drift toward generic Material or "AI slop": cold gray neutrals, purple-on-white gradients, cookie-cutter identical card grids, templated component kits (PRODUCT.md anti-reference, verbatim).
- **Don't** settle for parity with the default Nextcloud Talk web UI; the visible craft gap is the point (PRODUCT.md anti-reference, verbatim).
- **Don't** regress toward the cold-blue chat-app category reflex (Slack/Teams clutter, Discord neon-on-charcoal). The warmth is the identity (PRODUCT.md anti-reference, verbatim).
- **Don't** fake editorial presence by bolding 14px text in place of a real larger size (The Scale Honesty Rule).
