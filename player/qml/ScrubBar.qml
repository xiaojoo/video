import QtQuick
import Playerui

Item {
    id: bar

    property real progress: 0.0431
    property real hoverPos: -1.0
    property bool hovering: false
    property bool scrubbing: false
    signal scrubbed(real frac)

    // Chapter starts as fractions of the timeline, read off the reference ticks.
    property var chapterTicks: [
        0.0951, 0.1537, 0.2016, 0.2390, 0.2919, 0.3325, 0.3927, 0.4309,
        0.4659, 0.5309, 0.5691, 0.6138, 0.6634, 0.7114, 0.7553, 0.8374,
        0.9228, 0.9585
    ]

    // A Canvas repaints only when it is dirtied, and the tick list lives on this
    // item rather than on the Canvas, so the real chapters arriving from mpv
    // would otherwise never replace the mock ones.
    onChapterTicksChanged: ticks.requestPaint()

    readonly property real lineY: height / 2
    readonly property real lineH: Theme.lineThicknessRatio * parent.height
    readonly property real knobD: Theme.knobDiameterRatio * parent.height
    readonly property real halfBase: Theme.tickHalfBaseRatio * parent.width
    readonly property real tickIn: Theme.tickInnerRatio * parent.height
    readonly property real tickOut: Theme.tickOuterRatio * parent.height

    readonly property real shownFrac: scrubbing ? hoverPos : progress

    // Widened hit area: the visible bar is only a few pixels tall.
    MouseArea {
        id: hit
        anchors.verticalCenter: parent.verticalCenter
        height: Math.max(bar.height, bar.parent.height * 0.032)
        width: bar.width
        x: 0
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton

        function frac(mx) { return Math.max(0, Math.min(1, mx / Math.max(1, width))) }

        onPositionChanged: (m) => {
            bar.hoverPos = frac(m.x);
            bar.hovering = true;
        }
        // onEntered carries the position too: without it the first entry leaves
        // hoverPos at -1, and the card's visible test (hoverPos >= 0) stays false
        // until a second move happens to land inside the bar. entered() has no
        // parameter at all, so the position comes from MouseArea.mouseX.
        onEntered: {
            bar.hoverPos = frac(mouseX);
            bar.hovering = true;
        }
        onExited: if (!bar.scrubbing) bar.hovering = false
        onPressed: (m) => {
            bar.scrubbing = true;
            bar.hoverPos = frac(m.x);
            bar.hovering = true;
        }
        onReleased: (m) => {
            bar.scrubbing = false;
            bar.scrubbed(frac(m.x));
        }
    }

    // Unplayed track.
    Rectangle {
        x: 0
        y: bar.lineY - bar.lineH / 2
        width: parent.width
        height: Math.max(2, bar.lineH)
        radius: height / 2
        color: Theme.track
    }

    // Played portion.
    Rectangle {
        x: 0
        y: bar.lineY - bar.lineH / 2
        width: bar.width * bar.shownFrac
        height: Math.max(2, bar.lineH)
        radius: height / 2
        color: Theme.accent
    }

    Canvas {
        id: ticks
        anchors.fill: parent
        onPaint: {
            const g = getContext("2d");
            g.reset();
            g.fillStyle = Theme.accent;
            const hw = bar.halfBase, cy = bar.lineY;
            const tIn = bar.tickIn, tOut = bar.tickOut;
            for (const t of bar.chapterTicks) {
                const cx = t * width;
                // Base sits on the outer edge, tip points at the line, and the
                // pair never touches the track itself.
                g.beginPath();
                g.moveTo(cx - hw, cy - tOut);
                g.lineTo(cx + hw, cy - tOut);
                g.lineTo(cx, cy - tIn);
                g.closePath();
                g.fill();
                g.beginPath();
                g.moveTo(cx - hw, cy + tOut);
                g.lineTo(cx + hw, cy + tOut);
                g.lineTo(cx, cy + tIn);
                g.closePath();
                g.fill();
            }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    // Play head.
    Rectangle {
        x: bar.width * bar.shownFrac - bar.knobD / 2
        y: bar.lineY - bar.knobD / 2
        width: bar.knobD
        height: bar.knobD
        radius: bar.knobD / 2
        color: Theme.accent
    }

    // Hover marker on the track.
    Rectangle {
        visible: bar.hovering && !bar.scrubbing
        x: bar.width * bar.hoverPos - bar.knobD / 2
        y: bar.lineY - bar.knobD / 2
        width: bar.knobD
        height: bar.knobD
        radius: bar.knobD / 2
        color: Theme.accent
    }
}
