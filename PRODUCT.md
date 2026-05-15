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

Confident, editorial, distinct. Typography-led with deliberate hierarchy and a clear point of view, delivered with calm and restraint rather than volume. Warm, not cold. The voice is direct and human: plain words, no filler, no corporate hedging. It should feel like a considered tool made by people with taste, closer in temperament to Things or Notion than to Slack or Discord.

## Anti-references

- **Generic Material / Bootstrap / "AI slop".** Cold gray neutrals, purple-on-white gradients, cookie-cutter card grids, templated component kits, evenly distributed timid palettes. If a screen could be mistaken for AI-generated boilerplate, it has failed.
- **The default Nextcloud Talk web UI.** The baseline TalQ exists to surpass. Parity is not the goal; a clear craft gap is.
- **The category reflex.** The generic cold-blue chat app (Slack/Teams clutter, Discord neon-on-charcoal). TalQ's warm/paper identity is the deliberate escape from this and must never regress toward it.

## Design Principles

1. **Editorial confidence, calm execution.** Lead with strong typographic hierarchy and a clear point of view, but deliver it with restraint and generous breathing room. Bold structure, quiet surface. Loudness is not confidence.
2. **Earn the power user, welcome the newcomer.** Optimize the daily loops for speed and keyboard flow. Let density and shortcuts reveal themselves progressively; never clutter the first run to serve the thousandth.
3. **Warm identity is the moat.** The warm, paper-like palette is a deliberate differentiator, not decoration. Every choice should reinforce "this is not a generic blue chat app" and actively resist the category reflex.
4. **Every pixel deliberate.** Surpass the baseline through craft, not feature count: spacing rhythm, alignment, motion, and copy are all intentional and consistent across the app shell.
5. **Respect attention.** Low visual and notification noise. The app should reduce anxiety, not add to it.

## Accessibility & Inclusion

Target WCAG 2.1 AA contrast across both dark and light themes. Honor the OS reduced-motion preference for all non-essential animation. Preserve and respect user text scaling (TalQ already exposes a font scale). Do not rely on color alone to convey state (presence, unread, errors). These commitments hold even where they constrain the warm/paper aesthetic; craft must accommodate them, not waive them.
