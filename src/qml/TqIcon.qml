import QtQuick
import QtQuick.Shapes
import TalkQt

Item {
    id: root

    property string name: ""
    property int size: Theme.iconSizeMedium
    property color color: Theme.textSecondary

    width: size
    height: size

    Shape {
        id: shape
        width: 24
        height: 24
        anchors.centerIn: parent
        transform: Scale {
            origin.x: 12; origin.y: 12
            xScale: root.size / 24.0
            yScale: root.size / 24.0
        }
        layer.enabled: true
        layer.samples: 4

        ShapePath {
            strokeColor: root.color
            strokeWidth: 1.5
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathSvg { path: root.svgPath }
        }
    }

    readonly property string svgPath: {
        switch (name) {
        case "arrow-left":
            return "M15 5 L9 12 L15 19"
        case "arrow-down":
            return "M5 9 L12 15 L19 9"
        case "chevron-left":
            return "M14 6 L9 12 L14 18"
        case "chevron-right":
            return "M10 6 L15 12 L10 18"
        case "close":
            return "M6 6 L18 18 M18 6 L6 18"
        case "phone":
            return "M5 4 C5 4 8 4 8 7 C8 9 6 11 8 13 C10 15 13 17 15 16 C17 15 20 16 20 19 C20 20 19 21 17 21 C10 21 3 14 3 7 C3 5 4 4 5 4"
        case "video":
            return "M3 7 L3 17 L15 17 L15 7 Z M15 10 L21 7 L21 17 L15 14"
        case "send":
            return "M4 12 L20 12 M15 7 L20 12 L15 17"
        case "chat":
            return "M4 5 L20 5 L20 16 L12 16 L8 20 L8 16 L4 16 Z"
        case "attach":
            return "M12 3 C16 3 19 6 19 10 L19 16 C19 19 16 21 12 21 C8 21 5 19 5 16 L5 9 C5 7 7 5 9 5 C11 5 13 7 13 9 L13 16"
        case "reply":
            return "M10 6 L4 12 L10 18 M4 12 L16 12 C19 12 20 14 20 17"
        case "download":
            return "M12 3 L12 16 M7 12 L12 16 L17 12 M4 20 L20 20"
        case "refresh":
            return "M4 12 A8 8 0 1 1 12 20 A8 8 0 0 0 19 8 M15 4 L19 8 L23 4"
        case "power":
            return "M12 3 L12 12 M7 5 A8 8 0 1 0 17 5"
        case "mic":
            return "M12 3 C14 3 15 4 15 6 L15 12 C15 14 14 15 12 15 C10 15 9 14 9 12 L9 6 C9 4 10 3 12 3 M6 11 C6 15 9 18 12 18 C15 18 18 15 18 11 M12 18 L12 21"
        case "mic-off":
            return "M15 10 L15 6 C15 4 14 3 12 3 C10 3 9 4 9 6 L9 8 M9 12 C9 14 10 15 12 15 C14 15 15 14 15 12 M4 4 L20 20"
        case "hangup":
            return "M3 11 C3 9 7 8 12 8 C17 8 21 9 21 11 L21 14 C21 15 20 16 18 15 L16 14 L16 11 L8 11 L8 14 L6 15 C4 16 3 15 3 14 Z"
        case "sun":
            return "M12 3 L12 5 M12 19 L12 21 M5 12 L3 12 M19 12 L21 12 M6.3 6.3 L7.8 7.8 M16.2 16.2 L17.7 17.7 M6.3 17.7 L7.8 16.2 M16.2 7.8 L17.7 6.3 M16 12 A4 4 0 1 1 8 12 A4 4 0 1 1 16 12"
        case "moon":
            return "M18 12 A7 7 0 1 1 12 5 A9 9 0 0 0 18 12"
        case "cloud":
            return "M6 17 C3 17 3 12 6 12 C6 9 8 7 11 7 C14 7 16 8 17 10 C20 10 21 13 19 15 C21 17 19 17 18 17 Z"
        case "search":
            return "M15 15 L20 20 M10 16 A6 6 0 1 1 10 4 A6 6 0 1 1 10 16"
        case "settings":
            return "M12 15 A3 3 0 1 1 12 9 A3 3 0 1 1 12 15 M12 2 L12 5 M12 19 L12 22 M4.2 7.5 L6.8 9 M17.2 15 L19.8 16.5 M4.2 16.5 L6.8 15 M17.2 9 L19.8 7.5"
        case "bolt":
            return "M13 3 L7 13 L12 13 L11 21 L17 11 L12 11 Z"
        default:
            return ""
        }
    }
}
