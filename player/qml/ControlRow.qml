import QtQuick
import Playerui

Item {
    id: row

    readonly property real winH: height / Theme.controlRowHeightRatio
    // The window's launch width. `row.width` is the live one, and the side margin read
    // off it came out 13 px at 1282 and 39 px at 3840 - the gap he asked to hold still.
    readonly property real refW: shellWidth
    readonly property real icon: Theme.iconRatio * winH
    // Fixed gaps between the icons, so maximising leaves them exactly as wide as at
    // the small window. See Theme.leftGroupPitchHRatio for the arithmetic.
    readonly property real pitchL: Theme.leftGroupPitchHRatio * winH
    readonly property real pitchC: Theme.centerPitchHRatio * winH
    readonly property color glyph: Theme.glyph
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
    // With no chapters in the file there is nothing for the two chapter buttons to jump
    // to, so they go grey where that is the reason - the same treatment the audio and
    // subtitle cells get when the track list is empty.
    property bool hasChapters: true

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

    readonly property real lineH: Math.max(2, Theme.barLineRatio * winH)

    // Three tiers of the same row, because a 480 px window cannot carry 15 controls and
    // a 620 px one should not have to. The tier is a ratio of the window's own height,
    // so it does not move when the window is maximised: measured at 480x270 the three
    // groups end at x=100, 293 and 473 of 480 - nothing overlaps, but the right group
    // finishes 7 px from the edge and the rate readout sits against its neighbour.
    // Mini is pinned to the narrow tier by definition; a window dragged narrower than
    // 1.15 x its height gets it too. 1.45 is where the wide set stops having a pitch of
    // slack between its groups.
    // 8 px of slack on each edge, so a pointer parked on the boundary cannot make a
    // whole group of icons blink. It is exactly the width of the grab band: the drag
    // moves the edge by one band per step, and without the slack the last step of a
    // drag that ends on the threshold decides the layout.
    readonly property real tierHyst: 8
    property int miniTier: -1
    property int tier: 2

    // Which side the boundary is crossed from is the whole point of a hysteresis, so
    // this cannot be a binding: it has to see the tier it is leaving.
    //
    // It reads width and height directly, never refW/winH/narrowEdge/wideEdge. Those are
    // bindings off the same signals, and the declared handler is connected before they
    // are installed, so on the event that fires this function they still hold the
    // previous size. Measured on the stale reading: one jump from 1032 to 1282 left the
    // row on the 4-cell group and the jump back left it on the 7-cell one - the tier was
    // decided by the width the window had *before* the drag step, so a single big resize
    // never caught up until the next one.
    function retier() {
        if (miniTier >= 0) {
            tier = miniTier;
            return;
        }
        const w = width;
        const h = height / Theme.controlRowHeightRatio;
        const narrow = 1.15 * h;
        const wide = 1.45 * h;
        if (tier === 2)
            tier = w < wide - tierHyst ? (w < narrow - tierHyst ? 0 : 1) : 2;
        else if (tier === 0)
            tier = w > narrow + tierHyst ? (w > wide + tierHyst ? 2 : 1) : 0;
        else
            tier = w > wide + tierHyst ? 2 : (w < narrow - tierHyst ? 0 : 1);
    }
    onWidthChanged: retier()
    onHeightChanged: retier()
    onMiniTierChanged: retier()
    Component.onCompleted: retier()

    readonly property var leftOrder:  [["menu", "volume"], ["menu", "volume"],
                                       ["menu", "cast", "cc", "volume"]]
    readonly property var rightOrder: [["pip", "collapse"],
                                       ["chapters", "speed", "pip", "collapse"],
                                       ["audioTrack", "chapters", "rotate", "aspect",
                                        "speed", "pip", "collapse"]]
    // Narrow tier is play plus the two chapter jumps: in a window that small, skipping to
    // the next chapter is the thing that has no other handle, while a ten-second nudge
    // still has the scrub bar under the pointer.
    readonly property var centerOrder: [["prevChapter", "play", "nextChapter"],
                                        ["prevChapter", "back10", "play", "fwd10", "nextChapter"],
                                        ["prevChapter", "back10", "play", "fwd10", "nextChapter"]]

    // Left edges, walking the tier's own list: a step is one pitch, except after the
    // rate cell, which is wider than a glyph and whose neighbour starts where it ends.
    // At the top tier this reproduces the fixed positions the row has always had.
    // How wide a cell is drawn: most are one glyph, the rate cell carries its own
    // readout beside it, and the transport glyphs are stepped up.
    function cellW(name) {
        if (name === "speed")
            return row.speedW;
        if (name === "play" || name === "pause")
            return row.icon * 1.35;
        if (name === "back10" || name === "fwd10")
            return row.icon * 1.15;
        return row.icon;
    }
    // Left edges, walking the tier's own list. A step is one pitch plus whatever the
    // cell being left behind is wider than a glyph, so the gap between two neighbours'
    // ink is pitch - icon for every pair. Stepping by a fixed pitch instead - or by the
    // wide cell's own width, which is what this used to do - makes the transport group
    // uneven around the play button: measured 47 / 52 / 26 / 43 px.
    function xOf(list, name, pitch) {
        let x = 0;
        for (let i = 0; i < list.length; ++i) {
            if (list[i] === name)
                return x;
            // The rate cell is the one exception: its box is sized for the longest label
            // and holds the short ones left-aligned, so its neighbour already starts
            // where the box ends. Adding the box's surplus on top of that opens a 40 px
            // hole after it (measured) where the row had 22 before.
            x += list[i] === "speed" ? row.speedW
                                     : pitch + cellW(list[i]) - row.icon;
        }
        return 0;
    }
    function shown(list, name) {
        return list.indexOf(name) >= 0;
    }

    // The cells by name. The shell's right-click menu reaches the five value cells
    // through this: their command is "open this dropdown from this glyph", so a menu
    // row that stands in for one has to anchor on the same icon, not on the row. It is
    // also where the menu takes its label and its key hint, so the bubble and the menu
    // cannot name the same control two different ways.
    property var cells: ({})
    function groupW(list, pitch) {
        if (list.length === 0)
            return 0;
        return xOf(list, list[list.length - 1], pitch) + cellW(list[list.length - 1]);
    }

    readonly property real leftW: groupW(leftOrder[tier], pitchL)
            + (tier === 2 ? Theme.volGapHRatio * winH + Theme.volWidthHRatio * winH : 0)
    readonly property real rightW: groupW(rightOrder[tier], pitchL)
    readonly property real centerW: groupW(centerOrder[tier], pitchC)

    component Cell: Item {
        id: cell
        required property string name
        // The name the tier lists use, which is what the shell looks up. Only the play
        // cell differs from what it draws: its glyph swaps with the state, and a map
        // keyed on the drawn glyph would lose that row the moment the film started.
        property string key: cell.name
        // The glyph, handed out because an id inside an inline component is not a
        // property of the component - `cell.glyph` reads undefined.
        readonly property Item anchor: glyph
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
        Component.onCompleted: row.cells[cell.key] = cell
    }

    // ---- left group: menu / cast / CC / volume + slider ----
    // The timecode moved to the two ends of the scrub bar, so this group stops
    // where the slider does. Below the top tier the slider and the two track cells
    // are the first to go: the rate and the chapter list stay.
    Item {
        x: Theme.sideInsetWRatio * row.refW
        anchors.verticalCenter: parent.verticalCenter
        height: row.icon
        width: row.leftW

        Cell { name: "menu";   x: row.xOf(row.leftOrder[row.tier], "menu", row.pitchL); tipText: "打开媒体库" }
        Cell { name: "cast";   visible: row.shown(row.leftOrder[row.tier], "cast")
               x: row.xOf(row.leftOrder[row.tier], "cast", row.pitchL); live: false
               tipText: "投屏"; tipKeys: "暂未接入" }
        Cell { name: "cc";     visible: row.shown(row.leftOrder[row.tier], "cc")
               x: row.xOf(row.leftOrder[row.tier], "cc", row.pitchL); hasMenu: true
               live: row.subCount > 0
               tipText: "字幕"; tipKeys: row.subCount > 0 ? "S" : "本片无字幕" }
        Cell { name: "volume"; x: row.xOf(row.leftOrder[row.tier], "volume", row.pitchL)
               tipText: "静音"; tipKeys: "M" }

        Item {
            id: vol
            visible: row.tier === 2
            x: row.xOf(row.leftOrder[row.tier], "volume", row.pitchL) + row.icon
               + Theme.volGapHRatio * row.winH
            width: Theme.volWidthHRatio * row.winH
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

    }

    // ---- centre transport ----
    // The group is centred on its own laid-out width, so adding or dropping a cell moves
    // the play button not at all.
    Item {
        id: center
        x: row.width / 2 - row.centerW / 2
        anchors.verticalCenter: parent.verticalCenter
        height: row.icon * 1.35
        width: row.centerW

        Cell { name: "prevChapter"; visible: row.shown(row.centerOrder[row.tier], "prevChapter")
               x: row.xOf(row.centerOrder[row.tier], "prevChapter", row.pitchC)
               anchors.verticalCenter: parent.verticalCenter
               tipText: "上一个章节"; tipKeys: "J"; live: row.hasChapters }
        Cell { name: "back10"; visible: row.shown(row.centerOrder[row.tier], "back10")
               x: row.xOf(row.centerOrder[row.tier], "back10", row.pitchC)
               width: row.icon * 1.15; height: row.icon * 1.15
               anchors.verticalCenter: parent.verticalCenter
               tipText: "后退 10 秒"; tipKeys: "←" }
        // The play and pause glyphs' ink boxes sit 2 and 1 units right of the centre of
        // the 24-unit box they are drawn in (their path data: M8 5l12 7-12 7z spans x
        // 8..20, the two bars span 8.5..17.5). The cell goes back by exactly that, which
        // puts the glyph on the group's centre line *and* makes the two gaps beside it
        // equal - measured with the old +0.18 push, 56 px left of play against 41 right.
        readonly property real playInkOff: (row.playing ? 1.0 : 2.0) / 24 * row.icon * 1.35
        Cell { name: row.playing ? "pause" : "play"; key: "play"
               x: row.xOf(row.centerOrder[row.tier], "play", row.pitchC) - center.playInkOff
               width: row.icon * 1.35; height: row.icon * 1.35
               anchors.verticalCenter: parent.verticalCenter
               tipText: row.playing ? "暂停" : "播放"; tipKeys: "空格" }
        Cell { name: "fwd10"; visible: row.shown(row.centerOrder[row.tier], "fwd10")
               x: row.xOf(row.centerOrder[row.tier], "fwd10", row.pitchC)
               width: row.icon * 1.15; height: row.icon * 1.15
               anchors.verticalCenter: parent.verticalCenter
               tipText: "前进 10 秒"; tipKeys: "→" }
        Cell { name: "nextChapter"; visible: row.shown(row.centerOrder[row.tier], "nextChapter")
               x: row.xOf(row.centerOrder[row.tier], "nextChapter", row.pitchC)
               anchors.verticalCenter: parent.verticalCenter
               tipText: "下一个章节"; tipKeys: "L"; live: row.hasChapters }

        // The skip glyphs carry a numeral inside the ring. The two offsets are the
        // measured gap between the ink of "10" and the ring's centre (the ink is
        // not at the centre of its own layout box), expressed as a fraction of the
        // font size so it holds at every window size. The narrow tier has no skips,
        // and xOf answers 0 for a name its list does not hold, so the pair has to be
        // switched off with them - otherwise both numerals land on the first cell.
        Repeater {
            // A Repeater is not an Item, so it has no visible: the empty list is what
            // keeps the two numerals off the narrow tier's first cell.
            model: row.shown(row.centerOrder[row.tier], "back10")
                   ? [row.xOf(row.centerOrder[row.tier], "back10", row.pitchC),
                      row.xOf(row.centerOrder[row.tier], "fwd10", row.pitchC)]
                   : []
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
        x: row.width - Theme.sideInsetWRatio * row.refW - row.rightW
        anchors.verticalCenter: parent.verticalCenter
        height: row.icon
        width: row.rightW

        Cell { name: "audioTrack"; visible: row.shown(row.rightOrder[row.tier], "audioTrack")
               x: row.xOf(row.rightOrder[row.tier], "audioTrack", row.pitchL)
               hasMenu: true; live: row.audioCount > 0
               tipText: "音轨"; tipKeys: row.audioCount > 0 ? "A" : "无音轨" }
        Cell { name: "chapters";   visible: row.shown(row.rightOrder[row.tier], "chapters")
               x: row.xOf(row.rightOrder[row.tier], "chapters", row.pitchL)
               tipText: "章节列表"; tipKeys: "T" }
        Cell { name: "rotate";     visible: row.shown(row.rightOrder[row.tier], "rotate")
               x: row.xOf(row.rightOrder[row.tier], "rotate", row.pitchL)
               hasMenu: true; tipText: "画面旋转"; tipKeys: row.rotateDeg + "°" }
        Cell { name: "aspect";     visible: row.shown(row.rightOrder[row.tier], "aspect")
               x: row.xOf(row.rightOrder[row.tier], "aspect", row.pitchL)
               hasMenu: true; tipText: "画面比例"; tipKeys: row.aspectLabel }
        Cell { name: "speed"; visible: row.shown(row.rightOrder[row.tier], "speed")
               x: row.xOf(row.rightOrder[row.tier], "speed", row.pitchL)
               width: row.speedW; hasMenu: true
               labelText: row.speedLabel; tipText: "倍速播放"; tipKeys: row.speedLabel }
        Cell { name: "pip";   x: row.xOf(row.rightOrder[row.tier], "pip", row.pitchL)
               tipText: "小窗播放" }
        // Same effect as F now: the true fullscreen with the snapshot cover. The
        // system maximise stays reachable through the caption button at the top.
        Cell { name: "collapse"; x: row.xOf(row.rightOrder[row.tier], "collapse", row.pitchL)
               tipText: "全屏"; tipKeys: "F" }
    }

}
