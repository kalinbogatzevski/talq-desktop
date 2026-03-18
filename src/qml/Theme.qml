pragma Singleton
import QtQuick

QtObject {
    // ─── Backgrounds ───
    readonly property color bgPrimary: "#17212b"
    readonly property color bgSecondary: "#0e1621"
    readonly property color bgSidebar: "#17212b"
    readonly property color bgMessage: "#182533"
    readonly property color bgMessageOwn: "#2b5278"
    readonly property color bgHover: "#1e2a36"
    readonly property color bgSelected: "#2b5278"
    readonly property color bgInput: "#222e3a"
    readonly property color bgSurface: "#1c2836"        // cards, elevated panels
    readonly property color bgOverlay: "#0a0f14"        // modal overlays

    // ─── Text ───
    readonly property color textPrimary: "#e8edf2"
    readonly property color textSecondary: "#6b8299"
    readonly property color textTime: "#5c7389"
    readonly property color textMuted: "#465a6e"

    // ─── Accents ───
    readonly property color accent: "#5eb5f7"
    readonly property color accentHover: "#78c4ff"
    readonly property color accentPressed: "#4a9de0"
    readonly property color unreadBadge: "#4082bc"
    readonly property color online: "#5ec76a"
    readonly property color danger: "#e74c3c"
    readonly property color dangerHover: "#c0392b"
    readonly property color warning: "#faa05a"
    readonly property color systemMsg: "#5c7389"

    // ─── Borders & Dividers ───
    readonly property color border: "#1c2936"
    readonly property color divider: "#162230"
    readonly property color selectionBar: accent

    // ─── Typography ───
    readonly property int fontSizeTiny: 11
    readonly property int fontSizeSmall: 12
    readonly property int fontSizeNormal: 14
    readonly property int fontSizeLarge: 16
    readonly property int fontSizeTitle: 22
    readonly property int fontSizeHero: 36

    // ─── Spacing ───
    readonly property int spacingTiny: 4
    readonly property int spacingSmall: 8
    readonly property int spacingNormal: 12
    readonly property int spacingLarge: 16
    readonly property int spacingXLarge: 24

    // ─── Radii ───
    readonly property int radiusSmall: 6
    readonly property int radiusNormal: 10
    readonly property int radiusLarge: 14
    readonly property int radiusRound: 100

    // ─── Animation ───
    readonly property int animFast: 120
    readonly property int animNormal: 200
    readonly property int animSlow: 350

    // ─── Dimensions ───
    readonly property int headerHeight: 54
    readonly property int avatarSize: 46
    readonly property int conversationHeight: 68
    readonly property int composerMinHeight: 52
}
