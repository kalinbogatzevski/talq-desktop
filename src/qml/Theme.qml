pragma Singleton
import QtQuick

QtObject {
    // ─── Theme Mode ───
    property bool darkMode: true

    // ─── Backgrounds ───
    readonly property color bgPrimary: darkMode ? "#1a1d24" : "#ffffff"
    readonly property color bgSecondary: darkMode ? "#14171d" : "#f5f6f8"
    readonly property color bgSidebar: darkMode ? "#1e2128" : "#f0f1f4"
    readonly property color bgMessage: darkMode ? "transparent" : "transparent"  // flat for others
    readonly property color bgMessageOwn: darkMode ? "#1a3a3a" : "#d4f0ed"
    readonly property color bgHover: darkMode ? "#262a33" : "#e8eaee"
    readonly property color bgSelected: darkMode ? "#2a2f3a" : "#e0e4ea"
    readonly property color bgInput: darkMode ? "#262a33" : "#f0f1f4"
    readonly property color bgSurface: darkMode ? "#232830" : "#ffffff"
    readonly property color bgOverlay: darkMode ? "#0a0c10" : "#00000040"

    // ─── Text ───
    readonly property color textPrimary: darkMode ? "#e8eaed" : "#1a1d24"
    readonly property color textSecondary: darkMode ? "#8b919a" : "#6b7280"
    readonly property color textTime: darkMode ? "#6b7280" : "#9ca3af"
    readonly property color textMuted: darkMode ? "#565c66" : "#b0b6c0"

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
    readonly property color online: "#5ec76a"
    readonly property color danger: "#e06060"
    readonly property color dangerHover: "#c94545"
    readonly property color warning: "#f0a050"
    readonly property color systemMsg: darkMode ? "#6b7280" : "#9ca3af"

    // ─── Borders & Dividers ───
    readonly property color border: darkMode ? "#2a2f3a" : "#e5e7eb"
    readonly property color divider: darkMode ? "#22262e" : "#eeeff2"
    readonly property color selectionBar: accent

    // ─── Font Scale (Ctrl+Plus / Ctrl+Minus) ───
    property real fontScale: 1.0

    // ─── Typography ───
    readonly property int fontSizeTiny: Math.round(11 * fontScale)
    readonly property int fontSizeSmall: Math.round(12 * fontScale)
    readonly property int fontSizeNormal: Math.round(14 * fontScale)
    readonly property int fontSizeLarge: Math.round(16 * fontScale)
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

    // ─── Dimensions ───
    readonly property int headerHeight: 54
    readonly property int avatarSize: 44
    readonly property int avatarSizeSmall: 32
    readonly property int conversationHeight: 76
    readonly property int composerMinHeight: 56
}
