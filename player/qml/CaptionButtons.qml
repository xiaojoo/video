import QtQuick

Item {
    id: root
    property color glyph: "#E6E6E6"
    property real hitW: 46
    property real hitH: 32
    property bool fullscreen: false
    signal picked(string what)
    signal tipFor(var item, string text, string keys)
    signal tipOff(var item)

    readonly property var labels: ({
        min: ["最小化", ""],
        max: [fullscreen ? "还原" : "全屏", "F"],
        close: ["关闭", ""]
    })

    // The window's top drag strip sizes itself from this item, so it has to
    // report the Row's extent: a bare Item defaults to 0x0 and the strip was
    // then an invisible, unhittable rectangle.
    width: btnRow.width
    height: btnRow.height

    Row {
        id: btnRow
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.hitH

        Repeater {
            model: ["min", "max", "close"]
            delegate: Item {
                id: cell
                required property string modelData
                readonly property string key: modelData
                width: root.hitW
                height: root.hitH

                readonly property bool hot: area.containsMouse
                readonly property bool isClose: key === "close"

                Rectangle {
                    anchors.fill: parent
                    color: cell.hot ? (cell.isClose ? "#C42B1C" : Qt.rgba(1, 1, 1, 0.13)) : "transparent"
                }

                Canvas {
                    id: art
                    anchors.centerIn: parent
                    width: 11
                    height: 11
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
                    onEntered: {
                        const t = root.labels[cell.key];
                        if (t)
                            root.tipFor(cell, t[0], t[1]);
                    }
                    onExited: root.tipOff(cell)
                    onPressed: root.tipOff(cell)
                    onClicked: root.picked(cell.key)
                }
            }
        }
    }
}
