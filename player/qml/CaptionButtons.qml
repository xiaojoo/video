import QtQuick

Item {
    id: root
    property color glyph: "#E6E6E6"
    // Measured off his reference: three separate bumps, not one band. The hit area
    // is the bump itself - no invisible halo around it.
    readonly property real tileW: 40
    readonly property real tileH: 28
    readonly property real gap: 6
    readonly property real artSize: 11
    property bool fullscreen: false
    signal picked(string what)

    // The same 6 px that separates the bumps also separates the group from the
    // window's top and right edges, so the extent carries that margin.
    width: btnRow.width + gap
    height: btnRow.height + gap

    Row {
        id: btnRow
        anchors.right: parent.right
        anchors.rightMargin: root.gap
        anchors.top: parent.top
        anchors.topMargin: root.gap
        spacing: root.gap
        height: root.tileH

        Repeater {
            model: ["min", "max", "close"]
            delegate: Item {
                id: cell
                required property string modelData
                readonly property string key: modelData
                width: root.tileW
                height: root.tileH

                readonly property bool hot: area.containsMouse
                readonly property bool isClose: key === "close"

                // 72% of a near-black blue over whatever is behind it, which is how
                // the reference reads: two bumps over different parts of the same
                // photo measure (61,63,74) and (79,82,95).
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: "#B8131821"
                }

                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: cell.hot ? (cell.isClose ? "#C42B1C" : Qt.rgba(1, 1, 1, 0.13)) : "transparent"
                }

                Canvas {
                    id: art
                    anchors.centerIn: parent
                    width: root.artSize
                    height: root.artSize
                    onPaint: {
                        const g = getContext("2d");
                        g.reset();
                        g.strokeStyle = cell.hot && cell.isClose ? "#FFFFFF" : root.glyph;
                        g.lineWidth = 1;
                        g.beginPath();
                        if (cell.key === "min") {
                            g.moveTo(0, 5.5); g.lineTo(10, 5.5);
                        } else if (cell.key === "max") {
                            g.rect(0.5, 0.5, 9, 9);
                        } else {
                            g.moveTo(0.5, 0.5); g.lineTo(9.5, 9.5);
                            g.moveTo(9.5, 0.5); g.lineTo(0.5, 9.5);
                        }
                        g.stroke();
                    }
                    onWidthChanged: requestPaint()
                }

                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.picked(cell.key)
                }
            }
        }
    }
}
