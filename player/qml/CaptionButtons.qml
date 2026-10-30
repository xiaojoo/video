import QtQuick
import Playerui

Item {
    id: root
    property color glyph: "#E6E6E6"
    // Measured off his reference: three separate bumps, not one band. The hit area
    // is the bump itself - no invisible halo around it.
    readonly property real tileW: 40
    readonly property real tileH: 28
    readonly property real gap: 6
    readonly property real artSize: 11
    // The space between the bumps is not the space to the window's edges: the group sits
    // on the page's own inset, the same 13 px the tab row leaves above and to the left
    // (round(shellWidth * sideInsetWRatio)). It was 6 px, and the two rows started on
    // two different lines - measured, tiles at y 6..33 and x up to 1275 against tabs at
    // 13..46 with their left edge at 13.
    readonly property real edge: Math.round(shellWidth * Theme.sideInsetWRatio)
    // The gear sits on its own side of a hairline and is not one of the three window
    // buttons, so its tile is the same size and its ink a little larger: a ring and its
    // teeth read smaller than a line or a box at the same box size.
    readonly property real gearSize: 12
    property bool fullscreen: false
    // Lit while the card the gear opens is on screen - the tile's own plate, the same one
    // hover uses, so the glyph does not move.
    property bool settingsOn: false
    signal picked(string what)

    // The group's extent carries the edge margin, not the 6 px it uses between bumps.
    width: btnRow.width + edge
    height: btnRow.height + edge

    Row {
        id: btnRow
        anchors.right: parent.right
        anchors.rightMargin: root.edge
        anchors.top: parent.top
        anchors.topMargin: root.edge
        spacing: root.gap
        height: root.tileH

        // ---- 设置: its own tile, then the rule that parts it from min/max/close ----
        Item {
            id: gearCell
            width: root.tileW
            height: root.tileH

            readonly property bool hot: gearArea.containsMouse

            Rectangle {
                anchors.fill: parent
                radius: 4
                color: "#B8131821"
            }

            Rectangle {
                anchors.fill: parent
                radius: 4
                color: gearCell.hot || root.settingsOn ? Qt.rgba(1, 1, 1, 0.13) : "transparent"
            }

            // A thick ring, eight teeth and a solid hub. The first pass was a 1 px ring with
            // 1 px spikes, which at 12 px read as a sun and sat fainter than the window
            // glyphs beside it; six variants were rendered at this size on this plate and
            // this one is the only one that says "gear" at a glance.
            Canvas {
                id: gearArt
                anchors.centerIn: parent
                width: root.gearSize
                height: root.gearSize
                onPaint: {
                    const g = getContext("2d");
                    g.reset();
                    g.strokeStyle = root.glyph;
                    g.fillStyle = root.glyph;
                    g.lineCap = "round";
                    g.translate(width / 2, height / 2);
                    g.lineWidth = 1.8;
                    g.beginPath();
                    g.arc(0, 0, 3.0, 0, 2 * Math.PI);
                    g.stroke();
                    g.lineWidth = 1.6;
                    for (let i = 0; i < 8; i++) {
                        g.save();
                        g.rotate(i * Math.PI / 4);
                        g.beginPath();
                        g.moveTo(0, -3.6);
                        g.lineTo(0, -5.1);
                        g.stroke();
                        g.restore();
                    }
                    g.beginPath();
                    g.arc(0, 0, 1.0, 0, 2 * Math.PI);
                    g.fill();
                }
            }

            MouseArea {
                id: gearArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.picked("settings")
            }
        }

        // The rule between the gear and the window buttons: a hairline the height of the
        // glyphs, not of the tile, so it reads as a separator rather than a fourth tile.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 1
            height: Math.round(root.tileH * 0.55)
            color: "#3F3F47"
        }

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
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.picked(cell.key)
                }
            }
        }
    }
}
