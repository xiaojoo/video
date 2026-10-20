import QtQuick
import Playerui

Rectangle {
    id: panel
    property bool open: false
    // The launch width, so the drawer keeps the width it was designed at instead of
    // becoming a 900 px column on a 3840 screen - the same rule the rest of the chrome
    // follows now.
    property real refW: 1282
    // The gap left above and below the drawer - one number, because the top is asked to
    // read like the bottom, and the bottom one is the control band.
    property real gap: 0
    // The gap at its right edge. The caller feeds it the same number the window's close
    // button uses, so the two surfaces line up on one margin instead of two guesses.
    property real edgeGap: 0
    // Chapter start times and the names that go with them, both handed over by the
    // caller: the drawer, the header line and the hover bubble must not disagree about
    // what the chapters of this file are.
    property var edges: []
    property var labels: []

    function fmt(t) {
        if (!isFinite(t) || t < 0)
            t = 0;
        const s = Math.floor(t % 60), m = Math.floor(t / 60) % 60, h = Math.floor(t / 3600);
        const p = (v, n) => String(v).padStart(n, "0");
        return h > 0 ? h + ":" + p(m, 2) + ":" + p(s, 2) : p(m, 1) + ":" + p(s, 2);
    }
    function name(i) {
        return labels.length > i ? labels[i] : "";
    }
    signal picked(int index)
    signal dismissed()

    readonly property real drawerW: Theme.drawerWidthRatio * refW
    width: drawerW
    // Held off the right edge by `edgeGap`, no slide: it is either out or it is not.
    x: (parent ? parent.width : 0) - drawerW - edgeGap
    y: gap
    height: Math.max(0, (parent ? parent.height : 480) - 2 * gap)
    radius: 10
    // Opaque: this is a surface the list is read on, not a tooltip over the picture.
    color: "#FF0D0D10"
    border.width: 0
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
            cursorShape: Qt.PointingHandCursor
            onClicked: panel.dismissed()
        }
    }

    // A file with no chapters opens an empty drawer, and says so rather than showing
    // nothing at all.
    Text {
        visible: panel.edges.length === 0
        x: 16
        y: header.y + header.height + 18
        text: "此文件没有章节"
        color: Theme.textSecondary
        font.pixelSize: 14
        font.family: Theme.uiFont
    }

    ListView {
        id: list
        x: 0
        y: header.height + 18
        width: parent.width
        height: parent.height - y
        clip: true
        model: panel.edges

        delegate: Item {
            required property int index
            required property var modelData
            width: list.width
            height: 34
            Rectangle {
                anchors.fill: parent
                radius: 0
                color: rowHit.containsMouse ? "#22222A" : "transparent"
            }
            Text {
                x: 16
                anchors.verticalCenter: parent.verticalCenter
                // The time column is a fixed 68 px on the right; the title eats the rest.
                width: parent.width - 84
                text: (index + 1) + ". " + panel.name(index)
                elide: Text.ElideRight
                color: playerCore.chapter === index ? Theme.accent : Theme.textPrimary
                font.pixelSize: 14
                font.family: Theme.uiFont
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: panel.fmt(modelData)
                color: Theme.textSecondary
                font.pixelSize: 13
                font.family: Theme.uiFont
            }
            MouseArea {
                id: rowHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: panel.picked(index)
            }
        }
    }
}
