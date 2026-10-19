import QtQuick
import Playerui

Item {
    id: bar

    property real progress: 0.0431
    property real hoverPos: -1.0
    property bool hovering: false
    property bool scrubbing: false
    signal scrubbed(real frac)

    readonly property real lineY: height / 2
    // The bar's own height is fixed at the launch size by Main.qml, so everything
    // here is sized off it and stays the same pixels at any window size. 0.030 is the
    // ratio Main.qml sizes the bar with, so the division gives back that height.
    readonly property real winH: height / 0.030
    readonly property real lineH: Math.max(2, Theme.barLineRatio * winH)
    readonly property real knobD: Theme.knobDiameterRatio * winH

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
        height: bar.lineH
        radius: height / 2
        color: Theme.trackFlat
    }

    // Played portion: the same colour as the icons, per his ask.
    Rectangle {
        x: 0
        y: bar.lineY - bar.lineH / 2
        width: bar.width * bar.shownFrac
        height: bar.lineH
        radius: height / 2
        color: Theme.glyph
    }

    // Play head.
    Rectangle {
        x: bar.width * bar.shownFrac - bar.knobD / 2
        y: bar.lineY - bar.knobD / 2
        width: bar.knobD
        height: bar.knobD
        radius: bar.knobD / 2
        color: Theme.glyph
    }

    // Hover marker on the track.
    Rectangle {
        visible: bar.hovering && !bar.scrubbing
        x: bar.width * bar.hoverPos - bar.knobD / 2
        y: bar.lineY - bar.knobD / 2
        width: bar.knobD
        height: bar.knobD
        radius: bar.knobD / 2
        color: Theme.textPrimary
    }
}
