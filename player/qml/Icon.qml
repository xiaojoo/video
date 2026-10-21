import QtQuick
import QtQuick.Shapes

Item {
    id: root
    property string name: "play"
    property color color: "#E6E6E6"
    property real weight: 1.6
    implicitWidth: 24
    implicitHeight: 24
    // A name the table does not have - "" above all, which is what a menu row with no
    // glyph column carries - used to reach `table[name].filled` and throw "Value is
    // undefined and could not be converted to an object" once per binding per row. An
    // unknown name draws nothing; that is the whole job here.
    readonly property var spec: root.table[root.name] || ({ d: "", filled: false, cap: "" })

    // An arrowhead sitting on the end of an arc: an isoceles triangle whose axis
    // is the arc's own direction of travel. The previous versions used right
    // triangles placed by eye, which is what read as a blob on the skip glyphs
    // and as a right-angle stub on rotate.
    function head(px, py, dx, dy, len, half, back) {
        const n = Math.sqrt(dx * dx + dy * dy) || 1;
        dx /= n;
        dy /= n;
        const bx = px - dx * back, by = py - dy * back;
        const r = v => Math.round(v * 100) / 100;
        return "M" + r(px + dx * len) + " " + r(py + dy * len)
                + "L" + r(bx - dy * half) + " " + r(by + dx * half)
                + "L" + r(bx + dy * half) + " " + r(by - dx * half) + "z";
    }

    // 24x24 grid. `filled` marks shapes that must be closed and filled.
    //
    // The arcs are unchanged; only the heads are generated, and each one sits at
    // its arc's free end pointing along the direction of travel there - putting
    // it at the arc's start makes it point back into the ring and read as a flag.
    readonly property var table: ({
        "menu":         { d: "M3 6h18M3 12h18M3 18h18", filled: false },
        "cast":         { d: "M2 16.1a5.9 5.9 0 0 1 5.9 5.9M2 12.1a9.9 9.9 0 0 1 9.9 9.9M2 8V6a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2h-6", filled: false },
        // Asked for smaller letters inside the same box. The two C's are arcs, so the
        // pair is rebuilt rather than scaled in place: radius 2.6 -> 2.1 with the same
        // opening ratio (the chord stays at 0.73 of the radius), the separation scaled
        // by the same factor, and the chords' midpoint put on the box's own centre line -
        // the box is drawn from y 5 to y 19, so that midpoint is 12, and the first pass
        // here left it at 12.5, which measured 0.5 px low.
        "cc":           { d: "M4 5h16a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1zM10.6 10.47a2.1 2.1 0 1 0 0 3.06M16.25 10.47a2.1 2.1 0 1 0 0 3.06", filled: false },
        "volume":       { d: "M4 9.5h3L11.5 6v12L7 14.5H4zM15 9a4.5 4.5 0 0 1 0 6M17.8 6.6a8 8 0 0 1 0 10.8", filled: false },
        // Asked for "a slash on the icon it has now", so the speaker and its waves stay
        // and one stroke goes across them. The slash is inset by its own width so the two
        // do not fuse where they cross the speaker's outline.
        "volumeMuted":  { d: "M4 9.5h3L11.5 6v12L7 14.5H4zM15 9a4.5 4.5 0 0 1 0 6M17.8 6.6a8 8 0 0 1 0 10.8M4.2 4.2l15.6 15.6", filled: false },
        "prevChapter":  { d: "M6 5.5v13M19 6l-9 6 9 6z", filled: true },
        "nextChapter":  { d: "M18 5.5v13M5 6l9 6-9 6z", filled: true },
        "play":         { d: "M8 5l12 7-12 7z", filled: true },
        "pause":        { d: "M8.5 5.5h3.2v13H8.5zM14.3 5.5h3.2v13h-3.2z", filled: true },
        "back10":       { d: "M12 5a7 7 0 1 0 6.9 8.4", filled: false, cap: head(18.9, 13.4, 0.199, -0.98, 3.2, 2.0, 1.0) },
        "fwd10":        { d: "M12 5a7 7 0 1 1-6.9 8.4", filled: false, cap: head(5.1, 13.4, -0.199, -0.98, 3.2, 2.0, 1.0) },
        "chapters":     { d: "M4 6h16M4 12h16M4 18h10", filled: false },
        // Three list rules and a play head beside them. The pair is laid out so the ink
        // of the whole glyph - the round caps included - spans x 2.7..21.5 and y
        // 5.7..18.3, i.e. centred in the 24-unit box like its neighbours; a play
        // triangle drawn at the box's own centre would read 1 px right of them.
        "playlist":     { d: "M3.5 6.5h8M3.5 12h8M3.5 17.5h8", filled: false,
                          cap: "M15 8.5l6.5 3.5-6.5 3.5z" },
        // The four small marks the playlist rows use. All drawn on the same 24 grid and
        // with the same 1.6 stroke as the rest, so a 14 px row glyph matches a 25 px
        // control-row glyph instead of looking thin next to it.
        "plus":         { d: "M12 5.5v13M5.5 12h13", filled: false },
        "up":           { d: "M6.5 14.5L12 9l5.5 5.5", filled: false },
        "down":         { d: "M6.5 9.5L12 15l5.5-5.5", filled: false },
        // A pencil at 45°: the body, the ferrule line, and the tip as its own filled
        // wedge so it still reads at 14 px.
        "pencil":       { d: "M6 18l1.2-4.1L15.6 5.5l3 3-8.4 8.4zM14 7.2l2.8 2.8",
                          filled: false, cap: "M5.2 19.4l1.6-4.2 2.6 2.6z" },
        "audioTrack":   { d: "M4 13a8 8 0 1 1 16 0M4 13v3a2 2 0 0 0 2 2h1v-5H6a2 2 0 0 0-2 2zm16 0v3a2 2 0 0 1-2 2h-1v-5h1a2 2 0 0 1 2 2z", filled: false },
        "rotate":       { d: "M20 11a8 8 0 1 0-2.3 6", filled: false, cap: head(17.7, 17, 0.725, -0.689, 3.2, 2.0, 1.0) },
        "aspect":       { d: "M3 6h18v12H3zM8 9.5v5M16 9.5v5", filled: false },
        "pip":          { d: "M3 5h18v14H3zM13 12h6v5h-6z", filled: false },
        // A dial with the needle off the vertical: the pivot dot is the filled
        // cap so the needle can stay a stroke.
        "speed":        { d: "M4 17a8 8 0 1 1 16 0M12 17L16.2 11.4", filled: false,
                          cap: "M10.5 17a1.5 1.5 0 1 0 3 0a1.5 1.5 0 1 0-3 0z" },
        "info":         { d: "M12 3.5a8.5 8.5 0 1 0 0 17 8.5 8.5 0 0 0 0-17zM12 11v5.5M12 7.6v.9", filled: false },
        "collapse":     { d: "M9 9L4 4M4 9V4h5M15 9l5-5M20 9V4h-5M9 15l-5 5M4 15v5h5M15 15l5 5M20 15v5h-5", filled: false },
        "min":          { d: "M4 12h16", filled: false },
        "max":          { d: "M5 5h14v14H5z", filled: false },
        "close":        { d: "M5 5l14 14M19 5L5 19", filled: false }
    })

    Shape {
        id: shape
        width: 24
        height: 24
        // The 24x24 grid is the icons' coordinate system, so the item stays 24
        // wide and is scaled to the cell. fillMode: Stretch was tried and is
        // wrong here: it stretches the path's *bounding box* (back10's ink only
        // spans 14x16.6 of the grid) onto the item, which skews the glyph and
        // moves its centre by several pixels.
        transform: Scale {
            xScale: root.width / 24
            yScale: root.height / 24
        }
        // The antialiasing is this renderer's doing: without it the glyph edges
        // stay hard and blocky (8 distinct luminances across the glyph, 32 with).
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeWidth: root.spec.filled ? 1.2 : root.weight
            strokeColor: root.color
            fillColor: root.spec.filled ? root.color : "transparent"
            strokeStyle: ShapePath.SolidLine
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: root.spec.d }
        }

        ShapePath {
            strokeWidth: 0
            strokeColor: "transparent"
            fillColor: root.color
            PathSvg { path: root.spec.cap || "" }
        }
    }
}
