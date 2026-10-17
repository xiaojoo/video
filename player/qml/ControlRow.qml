import QtQuick
import Playerui

Item {
    id: row

    readonly property real winH: height / Theme.controlRowHeightRatio
    readonly property real winW: width
    readonly property real icon: Theme.iconRatio * winH
    readonly property real pitchL: Theme.leftGroupPitchWRatio * winW
    readonly property real pitchC: Theme.centerPitchWRatio * winW
    readonly property color glyph: Theme.glyph
    property string timeText: "07:46 / 03:00:22"
    property bool playing: false
    property real volume: 100
    // What the rotate/aspect cells currently hold, so the bubble can read the
    // state out instead of only naming the control.
    property int rotateDeg: 0
    property string aspectLabel: "自动"
    property string speedLabel: "1×"
    // How many choices the two track menus actually have. Zero greys the cell: a
    // dead icon that still explains itself beats one that eats a click.
    property int audioCount: 0
    property int subCount: 0

    signal action(string what)
    signal volumePicked(real percent)
    signal tipFor(var item, string text, string keys)
    signal tipOff(var item)
    // Cells that hold one of several values ask the shell for the dropdown
    // instead of cycling: reaching 1.25x by clicking past six others is not a
    // way to pick a value.
    signal pickFor(string name, var item)

    // Qt gives a mouse-over to one item only, so the row counts how many of its
    // own hit areas currently hold it. The shell uses this to keep the chrome
    // from hiding while the pointer is parked on a button.
    property int hoverCells: 0
    readonly property bool pointerInside: hoverCells > 0

    readonly property real lineH: Theme.lineThicknessRatio * winH

    component Cell: Item {
        id: cell
        required property string name
        property string tipText: ""
        property string tipKeys: ""
        // Drawn beside the glyph, for the one control whose value has to be read
        // without hovering first: the rate the film is actually running at.
        property string labelText: ""
        // Greyed-out but still there: the glyph must not vanish, and the cell
        // must still answer a hover, because the whole point of the bubble is to
        // say what the dead icon would have done. Only the click is withheld.
        property bool live: true
        property bool hasMenu: false
        width: row.icon
        height: row.icon
        Icon {
            id: glyph
            anchors.verticalCenter: parent.verticalCenter
            x: cell.labelText.length > 0 ? 0 : (parent.width - parent.height) / 2
            width: parent.height
            height: parent.height
            name: cell.name
            color: !cell.live ? "#4A4A4E"
                   : hit.containsMouse ? Theme.textPrimary : row.glyph
        }
        Text {
            visible: cell.labelText.length > 0
            x: glyph.x + glyph.width + cell.height * 0.06
            anchors.verticalCenter: parent.verticalCenter
            text: cell.labelText
            color: !cell.live ? "#4A4A4E"
                   : hit.containsMouse ? Theme.textPrimary : row.glyph
            font.pixelSize: cell.height * 0.42
            font.family: Theme.uiFont
        }
        MouseArea {
            id: hit
            anchors.fill: parent
            anchors.margins: -4
            hoverEnabled: true
            cursorShape: cell.live ? Qt.PointingHandCursor : Qt.ArrowCursor
            onEntered: {
                row.hoverCells = row.hoverCells + 1;
                if (cell.tipText.length > 0)
                    row.tipFor(cell, cell.tipText, cell.tipKeys);
            }
            onExited: {
                row.hoverCells = row.hoverCells - 1;
                row.tipOff(cell);
            }
            onPressed: row.tipOff(cell)
            onClicked: {
                if (!cell.live)
                    return;
                if (cell.hasMenu)
                    row.pickFor(cell.name, glyph);
                else
                    row.action(cell.name);
            }
        }
    }

    // ---- left group: menu / cast / CC / volume + slider / timecode ----
    Item {
        x: Theme.sideInsetWRatio * row.winW
        anchors.verticalCenter: parent.verticalCenter
        height: row.icon
        width: row.pitchL * 4 + 0.066 * row.winW + 0.13 * row.winW

        Cell { name: "menu";   x: 0; tipText: "打开媒体库" }
        Cell { name: "cast";   x: row.pitchL; live: false
               tipText: "投屏"; tipKeys: "暂未接入" }
        Cell { name: "cc";     x: row.pitchL * 2; hasMenu: true; live: row.subCount > 0
               tipText: "字幕"; tipKeys: row.subCount > 0 ? "S" : "本片无字幕" }
        Cell { name: "volume"; x: row.pitchL * 3; tipText: "静音"; tipKeys: "M" }

        Item {
            id: vol
            x: row.pitchL * 3 + row.icon + 0.012 * row.winW
            width: 0.066 * row.winW
            height: row.icon
            anchors.verticalCenter: parent.verticalCenter

            readonly property real frac: Math.max(0, Math.min(1, row.volume / 100))

            Rectangle {
                y: parent.height / 2 - Math.max(2, row.lineH) / 2
                width: parent.width
                height: Math.max(2, row.lineH)
                radius: height / 2
                color: Qt.rgba(1, 1, 1, 0.32)
            }
            Rectangle {
                y: parent.height / 2 - Math.max(2, row.lineH) / 2
                width: parent.width * vol.frac
                height: Math.max(2, row.lineH)
                radius: height / 2
                color: row.glyph
            }
            Rectangle {
                x: parent.width * vol.frac - row.icon * 0.19
                y: parent.height / 2 - row.icon * 0.19
                width: row.icon * 0.38
                height: row.icon * 0.38
                radius: width / 2
                color: row.glyph
            }

            MouseArea {
                anchors.fill: parent
                anchors.margins: -6
                hoverEnabled: true
                onEntered: row.hoverCells = row.hoverCells + 1
                onExited: row.hoverCells = row.hoverCells - 1
                onClicked: (m) => row.volumePicked(100 * m.x / width)
                onPositionChanged: (m) => { if (pressed) row.volumePicked(100 * m.x / width) }
            }
        }

        Text {
            x: vol.x + vol.width + 0.012 * row.winW
            anchors.verticalCenter: parent.verticalCenter
            text: row.timeText
            color: row.glyph
            font.pixelSize: Theme.subtitleFontRatio * row.winH
            font.family: Theme.uiFont
        }
    }

    // ---- centre transport ----
    Item {
        id: center
        x: row.width / 2 - row.pitchC * 2
        anchors.verticalCenter: parent.verticalCenter
        height: row.icon * 1.35
        width: row.pitchC * 4 + row.icon * 1.35

        Cell { name: "prevChapter"; x: 0; anchors.verticalCenter: parent.verticalCenter
               tipText: "上一个章节"; tipKeys: "J" }
        Cell { name: "back10"; x: row.pitchC; width: row.icon * 1.15; height: row.icon * 1.15
               anchors.verticalCenter: parent.verticalCenter
               tipText: "后退 10 秒"; tipKeys: "←" }
        Cell { name: row.playing ? "pause" : "play"
               x: row.pitchC * 2 + row.icon * 0.18; width: row.icon * 1.35; height: row.icon * 1.35
               anchors.verticalCenter: parent.verticalCenter
               tipText: row.playing ? "暂停" : "播放"; tipKeys: "空格" }
        Cell { name: "fwd10"; x: row.pitchC * 3; width: row.icon * 1.15; height: row.icon * 1.15
               anchors.verticalCenter: parent.verticalCenter
               tipText: "前进 10 秒"; tipKeys: "→" }
        Cell { name: "nextChapter"; x: row.pitchC * 4; anchors.verticalCenter: parent.verticalCenter
               tipText: "下一个章节"; tipKeys: "L" }

        // The skip glyphs carry a numeral inside the arc. The two offsets are the
        // measured gap between the ink of "10" and the ring's centre (the ink is
        // not at the centre of its own layout box), expressed as a fraction of the
        // font size so it holds at every window size.
        Repeater {
            model: [row.pitchC, row.pitchC * 3]
            delegate: Text {
                required property var modelData
                readonly property real fs: row.icon * 0.42
                x: modelData + row.icon * 0.575 - width / 2 + fs * 0.033
                y: center.height / 2 - height / 2 - fs * 0.037
                text: "10"
                color: row.glyph
                font.pixelSize: fs
                font.family: Theme.uiFont
            }
        }
    }

    // ---- right group ----
    // The speed cell is wider than a glyph because it carries its own readout.
    // The 4 px at the end is the hit margin the neighbours' mouse areas push
    // back by, so the two do not overlap on the boundary.
    readonly property real speedW: icon * 2.05 + 4
    Item {
        id: right
        x: row.width - Theme.sideInsetWRatio * row.winW - (row.pitchL * 5 + row.speedW + row.icon)
        anchors.verticalCenter: parent.verticalCenter
        height: row.icon
        width: row.pitchL * 5 + row.speedW + row.icon

        Cell { name: "audioTrack"; x: 0; hasMenu: true; live: row.audioCount > 0
               tipText: "音轨"; tipKeys: row.audioCount > 0 ? "A" : "无音轨" }
        Cell { name: "chapters";   x: row.pitchL; tipText: "章节列表"; tipKeys: "T" }
        Cell { name: "rotate";     x: row.pitchL * 2; hasMenu: true; tipText: "画面旋转"
               tipKeys: row.rotateDeg + "°" }
        Cell { name: "aspect";     x: row.pitchL * 3; hasMenu: true; tipText: "画面比例"
               tipKeys: row.aspectLabel }
        Cell { name: "speed"; x: row.pitchL * 4; width: row.speedW; hasMenu: true
               labelText: row.speedLabel; tipText: "倍速播放"; tipKeys: row.speedLabel }
        Cell { name: "pip";        x: row.pitchL * 4 + row.speedW; tipText: "小窗播放" }
        Cell { name: "collapse";   x: row.pitchL * 5 + row.speedW; tipText: "全屏"; tipKeys: "F" }
    }

}
