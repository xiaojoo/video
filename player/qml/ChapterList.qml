import QtQuick
import Playerui

Rectangle {
    id: panel
    property bool open: false

    function fmt(t) {
        if (!isFinite(t) || t < 0)
            t = 0;
        const s = Math.floor(t % 60), m = Math.floor(t / 60) % 60, h = Math.floor(t / 3600);
        const p = (v, n) => String(v).padStart(n, "0");
        return h > 0 ? h + ":" + p(m, 2) + ":" + p(s, 2) : p(m, 1) + ":" + p(s, 2);
    }
    signal picked(int index)
    signal dismissed()

    width: parent ? parent.width * 0.24 : 300
    height: Math.min(list.contentHeight + header.height + 24, parent ? parent.height * 0.7 : 500)
    anchors.right: parent.right
    anchors.rightMargin: parent.width * 0.02
    anchors.verticalCenter: parent.verticalCenter
    radius: 10
    color: "#E60D0D10"
    border.color: "#2C2C31"
    border.width: 1
    visible: open
    clip: true

    Text {
        id: header
        x: 16
        y: 12
        text: "章节"
        color: Theme.textPrimary
        font.pixelSize: 18
        font.family: Theme.uiFont
    }

    Text {
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: header.verticalCenter
        text: "✕"
        color: Theme.textSecondary
        font.pixelSize: 16
        MouseArea {
            anchors.fill: parent
            anchors.margins: -6
            onClicked: panel.dismissed()
        }
    }

    ListView {
        id: list
        x: 8
        y: header.height + 18
        width: parent.width - 16
        height: parent.height - y - 12
        clip: true
        model: playerCore.chapterTitles

        delegate: Item {
            required property int index
            required property var modelData
            width: list.width
            height: 34
            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                radius: 6
                color: rowHit.containsMouse ? "#22222A" : "transparent"
            }
            Text {
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 130
                text: (index + 1) + ". " + (modelData.length ? modelData : "章节 " + (index + 1))
                elide: Text.ElideRight
                color: playerCore.chapter === index ? Theme.accent : Theme.textPrimary
                font.pixelSize: 14
                font.family: Theme.uiFont
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: panel.fmt(playerCore.chapterStarts[index] || 0)
                color: Theme.textSecondary
                font.pixelSize: 13
                font.family: Theme.uiFont
            }
            MouseArea {
                id: rowHit
                anchors.fill: parent
                hoverEnabled: true
                onClicked: panel.picked(index)
            }
        }
    }
}
