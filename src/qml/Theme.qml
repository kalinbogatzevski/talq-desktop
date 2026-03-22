pragma Singleton
import QtQuick

QtObject {
    // ─── Theme Mode ───
    property bool darkMode: true

    // ─── Backgrounds (Warm Carbon) ───
    readonly property color bgPrimary: darkMode ? "#121210" : "#ffffff"
    readonly property color bgSecondary: darkMode ? "#1a1a16" : "#f5f5f2"
    readonly property color bgSidebar: darkMode ? "#181814" : "#f0f0ed"
    readonly property color bgMessage: darkMode ? "transparent" : "transparent"
    readonly property color bgMessageOwn: darkMode ? "#1a302e" : "#d4f0ed"
    readonly property color bgHover: darkMode ? "#2c2c28" : "#eeeee8"
    readonly property color bgSelected: darkMode ? "#33332e" : "#e4e4de"
    readonly property color bgInput: darkMode ? "#1e1e1c" : "#f0f0ed"
    readonly property color bgSurface: darkMode ? "#222220" : "#ffffff"
    readonly property color bgOverlay: darkMode ? "#0a0a08" : "#00000040"

    // ─── Text (Warm Grays) ───
    readonly property color textPrimary: darkMode ? "#e4e0da" : "#1a1a16"
    readonly property color textSecondary: darkMode ? "#8a8680" : "#6b6860"
    readonly property color textTime: darkMode ? "#6a665e" : "#9a968e"
    readonly property color textMuted: darkMode ? "#5a5850" : "#b0aca5"

    // ─── Topic Colors ───
    readonly property var topicPalette: ["#2ec4b6", "#e07060", "#f0a050", "#5ec76a", "#9b7cd4", "#e87aae"]
    function topicColor(index) { return topicPalette[Math.abs(index) % topicPalette.length] }
    function stringHash(str) {
        var h = 0
        for (var i = 0; i < str.length; i++) { h = ((h << 5) - h) + str.charCodeAt(i); h = h & h }
        return h
    }

    // ─── Accents ───
    readonly property color accent: darkMode ? "#2ec4b6" : "#1aab9d"
    readonly property color accentHover: darkMode ? "#3dd4c6" : "#22bfb0"
    readonly property color accentPressed: darkMode ? "#25a99d" : "#159488"
    readonly property color unreadBadge: darkMode ? "#2ec4b6" : "#1aab9d"

    // ─── Semantic Colors ───
    readonly property color success: "#5ec76a"
    readonly property color online: "#5ec76a"
    readonly property color danger: "#e06060"
    readonly property color dangerHover: "#c94545"
    readonly property color warning: "#f0a050"
    readonly property color info: "#5b9bd5"
    readonly property color systemMsg: darkMode ? "#6a665e" : "#9a968e"

    // ─── Borders & Dividers (Warm) ───
    readonly property color border: darkMode ? "#2a2a26" : "#e5e2dc"
    readonly property color divider: darkMode ? "#222220" : "#eeeee8"
    readonly property color borderFocused: darkMode ? "#2ec4b6" : "#1aab9d"
    readonly property color selectionBar: accent

    // ─── Alpha Helpers ───
    readonly property real alphaHover: 0.06
    readonly property real alphaActive: 0.10
    readonly property real alphaDisabled: 0.35

    // ─── Font Scale (Ctrl+Plus / Ctrl+Minus) ───
    property real fontScale: 1.0

    // ─── Typography ───
    readonly property int fontSizeXSmall: Math.round(9 * fontScale)
    readonly property int fontSizeTiny: Math.round(11 * fontScale)
    readonly property int fontSizeSmall: Math.round(12 * fontScale)
    readonly property int fontSizeNormal: Math.round(14 * fontScale)
    readonly property int fontSizeLarge: Math.round(16 * fontScale)
    readonly property int fontSizeXLarge: Math.round(20 * fontScale)
    readonly property int fontSizeTitle: Math.round(22 * fontScale)
    readonly property int fontSizeHero: Math.round(36 * fontScale)

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

    // ─── Button Sizes ───
    readonly property int buttonSizeSmall: 28
    readonly property int buttonSizeMedium: 36
    readonly property int buttonSizeLarge: 48

    // ─── Icon Sizes ───
    readonly property int iconSizeSmall: 16
    readonly property int iconSizeMedium: 20
    readonly property int iconSizeLarge: 24

    // ─── Avatar Sizes ───
    readonly property int avatarSizeTiny: 24
    readonly property int avatarSizeSmall: 32
    readonly property int avatarSize: 44
    readonly property int avatarSizeLarge: 52

    // ─── Status Indicators ───
    readonly property int statusDotSize: 10
    readonly property int badgeHeight: 18
    readonly property int badgeFontSize: 10

    // ─── Border Widths ───
    readonly property real borderWidthThin: 0.5
    readonly property int borderWidthNormal: 1
    readonly property int borderWidthThick: 2

    // ─── Scrollbar ───
    readonly property int scrollbarWidth: 4

    // ─── Layout Dimensions ───
    readonly property int headerHeight: 54
    readonly property int conversationHeight: 76
    readonly property int composerMinHeight: 56
}
