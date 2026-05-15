# Product

## Register

product

## Users

TalQ serves two overlapping audiences on the same surface:

- **123NET staff (primary, power users).** Keep TalQ open all day alongside other work. They live in the daily loops: scan, read, reply, switch conversations, search, manage rooms and bots. They reward speed, keyboard flow, and information density that does not get in the way.
- **123NET customers and partners (secondary, occasional).** Reach support and sales through the 123NET-branded build. They may open TalQ rarely and are not trained on it. They reward approachability, legibility, and obvious affordances.

The design must let a power user move fast without making an occasional user feel lost. Density should reveal itself with use, never confront a newcomer on first run.

## Product Purpose

TalQ is a native desktop client for Nextcloud Talk (Qt 6 / C++, QPainter-rendered, with a 123NET-branded variant). It exists to be a markedly better daily messaging surface than the stock Nextcloud Talk web UI: faster, calmer, and unmistakably crafted. Success looks like a client people *prefer* to keep open, that feels premium and trustworthy to customers, and that a power user can drive almost entirely by habit and keyboard.

## Brand Personality

Calm, warm, fast. A considered tool with a clear point of view, delivered through restraint rather than volume. The distinctiveness comes from a warm palette and visible craft, not from a display typeface or any costume; the editorial/serif experiment was tried and rejected as the wrong fit for a chat app. The voice is direct and human: plain words, no filler, no corporate hedging. Closer in temperament to Things or Linear than to Slack or Discord. The interface should feel quick under the hand and quiet in the eye.

## Anti-references

- **Generic Material / Bootstrap / "AI slop".** Cold gray neutrals, purple-on-white gradients, cookie-cutter card grids, templated component kits, evenly distributed timid palettes. If a screen could be mistaken for AI-generated boilerplate, it has failed.
- **The default Nextcloud Talk web UI.** The baseline TalQ exists to surpass. Parity is not the goal; a clear craft gap is.
- **The category reflex.** The generic cold-blue chat app (Slack/Teams clutter, Discord neon-on-charcoal). TalQ's warm identity is the deliberate escape from this and must never regress toward it.
- **The editorial costume.** Serif display faces, magazine layouts, "Letters Page" framing. Tried in 0.27.0, rejected: wrong register for a fast chat tool. Distinctiveness is warmth and craft, never a typeface worn as a badge.

## Design Principles

1. **Calm, warm, fast.** The surface is quiet and the interactions are quick. Lead with clear hierarchy and generous breathing room, delivered with restraint. Bold structure, quiet surface. Loudness is not confidence; speed and calm are.
2. **Earn the power user, welcome the newcomer.** Optimize the daily loops for speed and keyboard flow. Let density and shortcuts reveal themselves progressively; never clutter the first run to serve the thousandth.
3. **Warm identity is the moat.** The warm palette is a deliberate differentiator, not decoration. TalQ ships a small spectrum of warm themes the user picks from (three warm-dark levels and one warm-light); every one of them reinforces "this is not a generic blue chat app" and actively resists the category reflex. Cool gray is never an option in any theme.
4. **Every pixel deliberate.** Surpass the baseline through craft, not feature count: spacing rhythm, alignment, motion, and copy are intentional and consistent across the app shell.
5. **Respect attention.** Low visual and notification noise. The app should reduce anxiety, not add to it. State may glow softly to be glanceable; nothing decorates for its own sake.

## Accessibility & Inclusion

Target WCAG 2.1 AA contrast across every theme (Ember, Warm, Vivid, Paper). Honor the OS reduced-motion preference for all non-essential animation, including the ambient and presence glows. Preserve and respect user text scaling (TalQ already exposes a font scale). Do not rely on color alone to convey state (presence, unread, errors). These commitments hold even where they constrain the warm aesthetic; craft must accommodate them, not waive them.
