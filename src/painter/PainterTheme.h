#pragma once

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QImage>
#include <QString>

/**
 * Warm, calm, fast visual system for QPainter rendering. Ships four
 * user-selectable themes (three warm-dark levels + one warm light). The
 * legacy bool ctor is kept as a compatibility shim (true→Vivid, false→Paper)
 * so existing call sites keep working while the theme picker is wired up.
 */
class PainterTheme
{
public:
    enum class Theme { Ember, Warm, Vivid, Paper };

    PainterTheme(Theme theme, qreal fontScale = 1.0);
    PainterTheme(bool darkMode = true, qreal fontScale = 1.0);  // shim

    // Theme registry for the picker / cycling / persistence.
    static QString themeId(Theme t);          // "ember" | "warm" | "vivid" | "paper"
    static QString themeLabel(Theme t);       // "Ember" | "Warm" | ...
    static Theme   themeFromId(const QString &id, Theme fallback = Theme::Vivid);
    static Theme   cycle(Theme t);            // next theme (Settings + Ctrl+D)

    Theme theme = Theme::Vivid;
    bool  isLight = false;

    // ── Backgrounds ──
    QColor bgPrimary;
    QColor bgSecondary;
    QColor bgSidebar;
    QColor bgSelected;
    QColor bgMessageOwn;
    QColor bgSurface;
    QColor bgHover;

    // ── Text ──
    QColor textPrimary;
    QColor textSecondary;
    QColor textTime;
    QColor textMuted;

    // ── Accents ──
    QColor accent;
    QColor controlInk;   // ink/glyph color on a colored fill (per theme)
    QColor amber;        // warm secondary: favorite, away, accents
    QColor glow;         // colored halo for state (send hover, active row)
    QColor ambient;      // soft accent tint painted behind the thread
    QColor unreadBadge;
    QColor online;
    QColor danger;
    QColor success;
    QColor systemMsg;

    // ── Rich-text message body ──
    // Message HTML (Message::fromJson) used to bake these three colours into
    // inline style= attributes, which made them theme-blind: an inline colour
    // beats both the default style sheet and the paint-time palette, so the
    // one dark-tuned value shipped to every theme. On "Paper" that put
    // near-black textPrimary on a #1f2937 slate code background -- 1.22:1,
    // i.e. invisible -- and dropped mentions/links to ~2-2.9:1. They are
    // tokens now, applied per theme via richTextStyleSheet().
    QColor mention;   // @user / file-reference emphasis inside a message
    QColor link;      // hyperlink text inside a message
    QColor codeBg;    // fill behind `inline code` and ```code blocks```

    // Accent, made readable AS TEXT. The accent is painted as text in ~19
    // places (sidebar unread timestamp, QLabel[role="success"], ...), not only
    // as a fill -- and on Paper the fill-calibrated value scores 2.85:1 on the
    // selected sidebar row. Darkening the accent ITSELF was the wrong lever:
    // it feeds badges and the primary button, which already pass because their
    // ink goes through inkOn(). So the fill keeps its value and text gets this.
    // On the three dark themes the accent already clears AA as text, so this
    // resolves to the accent unchanged and nothing moves.
    QColor accentText;

    // Accent as text on the two fills that are NOT chrome grounds, so
    // accentText (calibrated against bgSelected) does not cover them. Both are
    // precomputed rather than corrected at paint time: their fills are fixed
    // per theme, so there is nothing to decide per frame.
    QColor accentSoft;      // the calm accent tint: selected / unread topic chip
    QColor accentSoftInk;   // accent, readable ON accentSoft
    QColor quoteInkPeer;    // reply-quote author, on the inset over a peer bubble
    QColor quoteInkOwn;     // ...and over your own bubble, which is the darker case

    // ── Borders ──
    QColor divider;

    // ── Font sizes (scaled) ──
    int fontSizeTiny;
    int fontSizeSmall;
    int fontSizeNormal;
    int fontSizeLarge;

    // ── Spacing ──
    static constexpr int spacingTiny = 4;
    static constexpr int spacingSmall = 8;
    static constexpr int spacingNormal = 12;
    static constexpr int spacingLarge = 16;
    static constexpr int spacingXLarge = 24;

    // ── Radii ──
    static constexpr int radiusSmall = 6;
    static constexpr int radiusControl = 8;    // DESIGN.md "control" — buttons, inputs
    static constexpr int radiusNormal = 10;
    static constexpr int radiusCard = 13;      // DESIGN.md "card" — panels, tiles

    // ── Avatar ──
    static constexpr int avatarSize = 36;
    static constexpr int avatarGap = 8;

    // ── Unread badge (the stadium pill) ── was independently redefined as
    // BadgeHeight=18 / BadgeFontSize=10 in SidebarPainter.h, ThreadsPainter.h
    // and a local static in TopicTabBar.cpp -- three copies of one constant
    // pair, promoted here so a future resize has one home.
    static constexpr int badgeHeight = 18;
    static constexpr int badgeFontSize = 10;

    // ── Layout ──
    static constexpr int messageSpacingGrouped = 6;
    static constexpr int messageSpacingNormal = 10;
    static constexpr int dateSepHeight = 40;
    static constexpr int datePillHeight = 26;
    static constexpr int unreadSepHeight = 28;
    static constexpr int unreadPillHeight = 22;

    // ── Author color from actorId (djb2 hash -> 8-color palette) ──
    // This is the IDENTITY hue: correct for an avatar FILL, where inkOn()
    // then picks the ink. Do NOT use it for the author's NAME -- see below.
    static QColor authorColor(const QString &actorId);

    // The same author identity, made readable as TEXT on this theme's message
    // bubble. ChatPainter paints the group-chat author name with the raw hue,
    // and measured against the bubble fill that is 2.09-3.31:1 for ALL EIGHT
    // hues on Paper (and 3.88-4.30:1 for two of them on the dark themes) --
    // the name above every message in every group conversation.
    //
    // Hue and saturation are preserved and only lightness moves, so "who said
    // this" still reads by colour exactly as before; the hue is the identity,
    // its lightness never was. Precomputed per theme in the constructor, so a
    // paint pass is a lookup. A hue that already clears AA is returned
    // untouched, which is why the dark themes barely move.
    QColor authorInk(const QString &actorId) const;

    // The identity hash, shared by authorColor() and authorInk() so the two
    // can never index different palette entries for the same actor. Exposed
    // so tests can assert that agreement directly.
    static int authorPaletteIndex(const QString &actorId);

    // The corrected author ink by palette index, mirroring authorPaletteAt()
    // for the raw hue. Lets a test check the whole table without going
    // through the hash, and check the hash separately.
    QColor authorInkAt(int index) const;

    // Lightness-only correction of `c` until it clears `minRatio` against
    // `ground`, preserving hue and saturation. Returns `c` unchanged when it
    // already passes. This is the general form of the rule above; inkOn()
    // remains the right tool when you may swap to a different ink entirely.
    static QColor readableOn(const QColor &c, const QColor &ground,
                             double minRatio = 4.5);

    // ── Author-palette introspection, for exhaustive contrast tests ──
    static int authorPaletteSize();
    static QColor authorPaletteAt(int index);

    // Ink for text drawn ON an arbitrary coloured fill (an author-palette
    // avatar, an icon backdrop, ...). controlInk is calibrated for the
    // accent-teal fill alone and fails AA (4.5:1) against several
    // author-palette hues on the light theme (measured ~2.1-3.3:1, 6 of 8
    // hues below even the 3:1 large-text bar). Score both of this theme's
    // own near-black/near-white ink tokens (controlInk, textPrimary --
    // they're a matched near-black/near-white pair in every shipped theme,
    // swapped on Paper) by WCAG relative-luminance contrast against the
    // actual fill and return whichever wins. Not static: the candidates are
    // per-theme tokens, not pure #000/#fff, so the choice needs instance
    // data -- every call site already holds a PainterTheme.
    QColor inkOn(const QColor &fill) const;

    // ── Topic color from palette index (6-color palette) ──
    static QColor topicColor(int index);
    // Introspection for exhaustive contrast tests, same idiom as
    // authorPaletteSize() -- topicColor(index) already takes a direct index
    // (unlike authorColor's hashed QString), so no separate "At" accessor
    // is needed, just the count to iterate.
    static int topicPaletteSize();

    // ── Crop an image to a circle at the given size ──
    static QImage cropToCircle(const QImage &source, int size);

    // ── Format a Unix timestamp as "HH:mm", "Yesterday", or "dd MMM" ──
    static QString formatRelativeTime(qint64 epochSecs);

    // ── Build preview text from author + message ──
    static QString formatPreviewText(const QString &author, const QString &message);

    // ── Rich-text style sheet for message body documents ──
    // Applied with QTextDocument::setDefaultStyleSheet BEFORE setHtml (Qt
    // resolves the sheet at parse time; setting it after is a no-op). Carries
    // the per-theme colour for <pre>/<code>/<a>/.mention so the message HTML
    // itself can stay colourless and theme-independent -- the body doc is
    // rebuilt on every theme change (ChatPainter::setTheme clears the layout
    // cache), so this tracks the live theme.
    QString richTextStyleSheet() const;

    // ── Fonts ──
    QFont bodyFont() const;
    QFont nameFont() const;
    QFont timeFont() const;
    QFont systemFont() const;
    QFont dateSepFont() const;

private:
    qreal m_fontScale = 1.0;
    QColor m_authorInk[8];   // authorColor palette, corrected for this theme
};
