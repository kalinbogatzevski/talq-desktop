import QtQuick
import QtQuick.Shapes
import TalkQt

// Geometric thin-stroke SVG icon component.
// Usage: TqIcon { name: "phone"; size: 20; color: Theme.textSecondary }
//
// All icons are drawn as Shape paths at native resolution — crisp at any size.
// Stroke weight scales proportionally. No raster assets needed.

Item {
    id: root

    property string name: ""
    property int size: Theme.iconSizeMedium
    property color color: Theme.textSecondary
    property real strokeWidth: Math.max(1.2, size / 12)

    width: size
    height: size

    Shape {
        anchors.fill: parent
        layer.enabled: true
        layer.samples: 4  // anti-aliasing

        ShapePath {
            strokeColor: root.color
            strokeWidth: root.strokeWidth
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            // Scale all paths from a 24x24 viewbox to actual size
            property real s: root.size / 24.0

            // Icon paths — each draws in a 24x24 coordinate space
            PathSvg {
                path: {
                    var sc = parent.s
                    switch (root.name) {

                    // --- Navigation ---
                    case "arrow-left":
                        return "M" + (15*sc) + " " + (5*sc) + " L" + (9*sc) + " " + (12*sc) + " L" + (15*sc) + " " + (19*sc)

                    case "arrow-down":
                        return "M" + (5*sc) + " " + (9*sc) + " L" + (12*sc) + " " + (15*sc) + " L" + (19*sc) + " " + (9*sc)

                    case "chevron-left":
                        return "M" + (14*sc) + " " + (6*sc) + " L" + (9*sc) + " " + (12*sc) + " L" + (14*sc) + " " + (18*sc)

                    case "chevron-right":
                        return "M" + (10*sc) + " " + (6*sc) + " L" + (15*sc) + " " + (12*sc) + " L" + (10*sc) + " " + (18*sc)

                    case "close":
                        return "M" + (6*sc) + " " + (6*sc) + " L" + (18*sc) + " " + (18*sc) + " M" + (18*sc) + " " + (6*sc) + " L" + (6*sc) + " " + (18*sc)

                    // --- Communication ---
                    case "phone":
                        return "M" + (5*sc) + " " + (4*sc)
                             + " C" + (5*sc) + " " + (4*sc) + " " + (8*sc) + " " + (4*sc) + " " + (8*sc) + " " + (7*sc)
                             + " C" + (8*sc) + " " + (9*sc) + " " + (6*sc) + " " + (11*sc) + " " + (8*sc) + " " + (13*sc)
                             + " C" + (10*sc) + " " + (15*sc) + " " + (13*sc) + " " + (17*sc) + " " + (15*sc) + " " + (16*sc)
                             + " C" + (17*sc) + " " + (15*sc) + " " + (20*sc) + " " + (16*sc) + " " + (20*sc) + " " + (19*sc)
                             + " C" + (20*sc) + " " + (20*sc) + " " + (19*sc) + " " + (21*sc) + " " + (17*sc) + " " + (21*sc)
                             + " C" + (10*sc) + " " + (21*sc) + " " + (3*sc) + " " + (14*sc) + " " + (3*sc) + " " + (7*sc)
                             + " C" + (3*sc) + " " + (5*sc) + " " + (4*sc) + " " + (4*sc) + " " + (5*sc) + " " + (4*sc)

                    case "video":
                        return "M" + (3*sc) + " " + (7*sc)
                             + " L" + (3*sc) + " " + (17*sc)
                             + " L" + (15*sc) + " " + (17*sc)
                             + " L" + (15*sc) + " " + (7*sc) + " Z"
                             + " M" + (15*sc) + " " + (10*sc)
                             + " L" + (21*sc) + " " + (7*sc)
                             + " L" + (21*sc) + " " + (17*sc)
                             + " L" + (15*sc) + " " + (14*sc)

                    case "send":
                        return "M" + (4*sc) + " " + (12*sc)
                             + " L" + (20*sc) + " " + (12*sc)
                             + " M" + (15*sc) + " " + (7*sc)
                             + " L" + (20*sc) + " " + (12*sc)
                             + " L" + (15*sc) + " " + (17*sc)

                    case "chat":
                        return "M" + (4*sc) + " " + (5*sc)
                             + " L" + (20*sc) + " " + (5*sc)
                             + " L" + (20*sc) + " " + (16*sc)
                             + " L" + (12*sc) + " " + (16*sc)
                             + " L" + (8*sc) + " " + (20*sc)
                             + " L" + (8*sc) + " " + (16*sc)
                             + " L" + (4*sc) + " " + (16*sc) + " Z"

                    // --- Actions ---
                    case "attach":
                        return "M" + (12*sc) + " " + (3*sc)
                             + " C" + (16*sc) + " " + (3*sc) + " " + (19*sc) + " " + (6*sc) + " " + (19*sc) + " " + (10*sc)
                             + " L" + (19*sc) + " " + (16*sc)
                             + " C" + (19*sc) + " " + (19*sc) + " " + (16*sc) + " " + (21*sc) + " " + (12*sc) + " " + (21*sc)
                             + " C" + (8*sc) + " " + (21*sc) + " " + (5*sc) + " " + (19*sc) + " " + (5*sc) + " " + (16*sc)
                             + " L" + (5*sc) + " " + (9*sc)
                             + " C" + (5*sc) + " " + (7*sc) + " " + (7*sc) + " " + (5*sc) + " " + (9*sc) + " " + (5*sc)
                             + " C" + (11*sc) + " " + (5*sc) + " " + (13*sc) + " " + (7*sc) + " " + (13*sc) + " " + (9*sc)
                             + " L" + (13*sc) + " " + (16*sc)

                    case "reply":
                        return "M" + (4*sc) + " " + (12*sc)
                             + " L" + (10*sc) + " " + (6*sc)
                             + " L" + (10*sc) + " " + (10*sc)
                             + " C" + (15*sc) + " " + (10*sc) + " " + (20*sc) + " " + (12*sc) + " " + (20*sc) + " " + (18*sc)
                             + " C" + (18*sc) + " " + (14*sc) + " " + (15*sc) + " " + (14*sc) + " " + (10*sc) + " " + (14*sc)
                             + " L" + (10*sc) + " " + (18*sc) + " Z"

                    case "download":
                        return "M" + (12*sc) + " " + (3*sc) + " L" + (12*sc) + " " + (16*sc)
                             + " M" + (7*sc) + " " + (12*sc) + " L" + (12*sc) + " " + (16*sc) + " L" + (17*sc) + " " + (12*sc)
                             + " M" + (4*sc) + " " + (20*sc) + " L" + (20*sc) + " " + (20*sc)

                    case "refresh":
                        return "M" + (4*sc) + " " + (12*sc)
                             + " A" + (8*sc) + " " + (8*sc) + " 0 1 1 " + (12*sc) + " " + (20*sc)
                             + " A" + (8*sc) + " " + (8*sc) + " 0 0 0 " + (19*sc) + " " + (8*sc)
                             + " M" + (15*sc) + " " + (4*sc) + " L" + (19*sc) + " " + (8*sc) + " L" + (23*sc) + " " + (4*sc)

                    case "power":
                        return "M" + (12*sc) + " " + (3*sc) + " L" + (12*sc) + " " + (12*sc)
                             + " M" + (7*sc) + " " + (5*sc) + " A" + (8*sc) + " " + (8*sc) + " 0 1 0 " + (17*sc) + " " + (5*sc)

                    // --- Media ---
                    case "mic":
                        return "M" + (12*sc) + " " + (3*sc)
                             + " C" + (14*sc) + " " + (3*sc) + " " + (15*sc) + " " + (4*sc) + " " + (15*sc) + " " + (6*sc)
                             + " L" + (15*sc) + " " + (12*sc)
                             + " C" + (15*sc) + " " + (14*sc) + " " + (14*sc) + " " + (15*sc) + " " + (12*sc) + " " + (15*sc)
                             + " C" + (10*sc) + " " + (15*sc) + " " + (9*sc) + " " + (14*sc) + " " + (9*sc) + " " + (12*sc)
                             + " L" + (9*sc) + " " + (6*sc)
                             + " C" + (9*sc) + " " + (4*sc) + " " + (10*sc) + " " + (3*sc) + " " + (12*sc) + " " + (3*sc)
                             + " M" + (6*sc) + " " + (11*sc) + " C" + (6*sc) + " " + (15*sc) + " " + (9*sc) + " " + (18*sc) + " " + (12*sc) + " " + (18*sc)
                             + " C" + (15*sc) + " " + (18*sc) + " " + (18*sc) + " " + (15*sc) + " " + (18*sc) + " " + (11*sc)
                             + " M" + (12*sc) + " " + (18*sc) + " L" + (12*sc) + " " + (21*sc)

                    case "mic-off":
                        return "M" + (15*sc) + " " + (10*sc) + " L" + (15*sc) + " " + (6*sc)
                             + " C" + (15*sc) + " " + (4*sc) + " " + (14*sc) + " " + (3*sc) + " " + (12*sc) + " " + (3*sc)
                             + " C" + (10*sc) + " " + (3*sc) + " " + (9*sc) + " " + (4*sc) + " " + (9*sc) + " " + (6*sc)
                             + " L" + (9*sc) + " " + (8*sc)
                             + " M" + (9*sc) + " " + (12*sc) + " C" + (9*sc) + " " + (14*sc) + " " + (10*sc) + " " + (15*sc) + " " + (12*sc) + " " + (15*sc)
                             + " C" + (14*sc) + " " + (15*sc) + " " + (15*sc) + " " + (14*sc) + " " + (15*sc) + " " + (12*sc)
                             + " M" + (4*sc) + " " + (4*sc) + " L" + (20*sc) + " " + (20*sc)

                    case "hangup":
                        return "M" + (3*sc) + " " + (11*sc)
                             + " C" + (3*sc) + " " + (9*sc) + " " + (7*sc) + " " + (8*sc) + " " + (12*sc) + " " + (8*sc)
                             + " C" + (17*sc) + " " + (8*sc) + " " + (21*sc) + " " + (9*sc) + " " + (21*sc) + " " + (11*sc)
                             + " L" + (21*sc) + " " + (14*sc)
                             + " C" + (21*sc) + " " + (15*sc) + " " + (20*sc) + " " + (16*sc) + " " + (18*sc) + " " + (15*sc)
                             + " L" + (16*sc) + " " + (14*sc)
                             + " L" + (16*sc) + " " + (11*sc)
                             + " L" + (8*sc) + " " + (11*sc)
                             + " L" + (8*sc) + " " + (14*sc)
                             + " L" + (6*sc) + " " + (15*sc)
                             + " C" + (4*sc) + " " + (16*sc) + " " + (3*sc) + " " + (15*sc) + " " + (3*sc) + " " + (14*sc) + " Z"

                    // --- Theme ---
                    case "sun":
                        return "M" + (12*sc) + " " + (3*sc) + " L" + (12*sc) + " " + (5*sc)
                             + " M" + (12*sc) + " " + (19*sc) + " L" + (12*sc) + " " + (21*sc)
                             + " M" + (5*sc) + " " + (12*sc) + " L" + (3*sc) + " " + (12*sc)
                             + " M" + (19*sc) + " " + (12*sc) + " L" + (21*sc) + " " + (12*sc)
                             + " M" + (6.3*sc) + " " + (6.3*sc) + " L" + (7.8*sc) + " " + (7.8*sc)
                             + " M" + (16.2*sc) + " " + (16.2*sc) + " L" + (17.7*sc) + " " + (17.7*sc)
                             + " M" + (6.3*sc) + " " + (17.7*sc) + " L" + (7.8*sc) + " " + (16.2*sc)
                             + " M" + (16.2*sc) + " " + (7.8*sc) + " L" + (17.7*sc) + " " + (6.3*sc)
                             + " M" + (16*sc) + " " + (12*sc) + " A" + (4*sc) + " " + (4*sc) + " 0 1 1 " + (8*sc) + " " + (12*sc)
                             + " A" + (4*sc) + " " + (4*sc) + " 0 1 1 " + (16*sc) + " " + (12*sc)

                    case "moon":
                        return "M" + (18*sc) + " " + (12*sc)
                             + " A" + (7*sc) + " " + (7*sc) + " 0 1 1 " + (12*sc) + " " + (5*sc)
                             + " A" + (9*sc) + " " + (9*sc) + " 0 0 0 " + (18*sc) + " " + (12*sc)

                    // --- Files ---
                    case "cloud":
                        return "M" + (6*sc) + " " + (17*sc)
                             + " C" + (3*sc) + " " + (17*sc) + " " + (3*sc) + " " + (12*sc) + " " + (6*sc) + " " + (12*sc)
                             + " C" + (6*sc) + " " + (9*sc) + " " + (8*sc) + " " + (7*sc) + " " + (11*sc) + " " + (7*sc)
                             + " C" + (14*sc) + " " + (7*sc) + " " + (16*sc) + " " + (8*sc) + " " + (17*sc) + " " + (10*sc)
                             + " C" + (20*sc) + " " + (10*sc) + " " + (21*sc) + " " + (13*sc) + " " + (19*sc) + " " + (15*sc)
                             + " C" + (21*sc) + " " + (17*sc) + " " + (19*sc) + " " + (17*sc) + " " + (18*sc) + " " + (17*sc) + " Z"

                    // --- Misc ---
                    case "search":
                        return "M" + (15*sc) + " " + (15*sc) + " L" + (20*sc) + " " + (20*sc)
                             + " M" + (10*sc) + " " + (16*sc) + " A" + (6*sc) + " " + (6*sc) + " 0 1 1 " + (10*sc) + " " + (4*sc)
                             + " A" + (6*sc) + " " + (6*sc) + " 0 1 1 " + (10*sc) + " " + (16*sc)

                    case "settings":
                        return "M" + (12*sc) + " " + (15*sc)
                             + " A" + (3*sc) + " " + (3*sc) + " 0 1 1 " + (12*sc) + " " + (9*sc)
                             + " A" + (3*sc) + " " + (3*sc) + " 0 1 1 " + (12*sc) + " " + (15*sc)
                             + " M" + (12*sc) + " " + (2*sc) + " L" + (12*sc) + " " + (5*sc)
                             + " M" + (12*sc) + " " + (19*sc) + " L" + (12*sc) + " " + (22*sc)
                             + " M" + (4.2*sc) + " " + (7.5*sc) + " L" + (6.8*sc) + " " + (9*sc)
                             + " M" + (17.2*sc) + " " + (15*sc) + " L" + (19.8*sc) + " " + (16.5*sc)
                             + " M" + (4.2*sc) + " " + (16.5*sc) + " L" + (6.8*sc) + " " + (15*sc)
                             + " M" + (17.2*sc) + " " + (9*sc) + " L" + (19.8*sc) + " " + (7.5*sc)

                    case "bolt":
                        return "M" + (13*sc) + " " + (3*sc)
                             + " L" + (7*sc) + " " + (13*sc)
                             + " L" + (12*sc) + " " + (13*sc)
                             + " L" + (11*sc) + " " + (21*sc)
                             + " L" + (17*sc) + " " + (11*sc)
                             + " L" + (12*sc) + " " + (11*sc) + " Z"

                    default:
                        return ""
                    }
                }
            }
        }
    }
}
