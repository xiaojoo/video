import QtQuick
import Playerui

Item {
    id: card
    required property real winW
    required property real winH

    property string chapterLabel: "Chapter 16"
    property string timeLabel: "02:15:29"
    property url thumb: ""

    readonly property real boxW: Theme.thumbWidthRatio * winW
    readonly property real boxH: Theme.thumbHeightRatio * winH
    readonly property real smallFont: Theme.subtitleFontRatio * winH
    readonly property real gap: smallFont * 0.45

    implicitWidth: boxW
    implicitHeight: boxH + smallFont * 1.25 * 2 + gap * 2

    Column {
        x: 0
        y: 0
        width: card.boxW
        spacing: card.gap

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: card.chapterLabel
            color: Theme.textPrimary
            font.pixelSize: card.smallFont
            font.family: Theme.uiFont
        }

        Rectangle {
            width: card.boxW
            height: card.boxH
            color: "#1C1C1E"
            border.color: Qt.rgba(1, 1, 1, 0.55)
            border.width: 1
            clip: true

            Image {
                anchors.fill: parent
                anchors.margins: 1
                source: card.thumb
                fillMode: Image.PreserveAspectCrop
                visible: status === Image.Ready
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: card.timeLabel
            color: Theme.textPrimary
            font.pixelSize: card.smallFont * 0.92
            font.family: Theme.uiFont
        }
    }
}
