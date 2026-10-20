import QtQuick
import Playerui

// The dropdown for the cells that hold one of several values: rotation, frame
// ratio, playback rate. Qt's own Menu is a separate native window, which is the
// same reason the bubble is ours - it outranks the player it belongs to, cannot
// be styled to match the chrome, and lands in the wrong place on a frameless
// tool window. One shared instance also means two of these cannot open at once.
Item {
    id: picker

    property Item space: parent
    // The size the chrome was designed at (the window's launch rect), so the dropdown's
    // rows keep the type size they have where the window started.
    property real refH: 720
    property var options: []
    property int current: -1
    property Item target: null
    // "above" is the control row's own dropdown: it stands over the glyph. "right" is the
    // same list used as a submenu of the right-click menu, hanging off the row that owns
    // it - the menu stays open, so the two read as one cascade.
    property string side: "above"
    // How much of the anchor's width to give back, so a submenu measured from a row that
    // stops short of its card can hug the card instead of the row.
    property real rightInset: 0

    signal picked(int index)

    readonly property real fs: Theme.subtitleFontRatio * refH * 0.85
    readonly property real pad: fs * 0.62
    readonly property real rowH: fs * 2.0
    readonly property real tick: fs * 1.35
    readonly property real standOff: fs * 0.55
    readonly property int count: options ? options.length : 0

    width: card.width
    // The box reaches `reach` below the card, over the gap between the card and
    // the icon. HoverHandler has no padding on this Qt, so the only way to keep
    // that crossing from counting as "left" is to make it part of the hovered
    // area. Nothing but the hover handler lives out there, so the extra strip
    // still passes clicks through.
    height: card.height + reach
    visible: false
    readonly property real reach: standOff + 8

    // Watches the whole card, including the padding between rows, which no row's
    // own mouse area claims.
    HoverHandler { id: over }
    readonly property bool pointerOn: over.hovered

    function show(item, list, cur, at, inset) {
        if (!item || !list || list.length === 0)
            return;
        target = item;
        options = list;
        current = cur;
        side = at === undefined ? "above" : at;
        rightInset = inset === undefined ? 0 : inset;
        visible = true;
        // Reposition here rather than relying on onVisibleChanged: switching from
        // one menu to another leaves `visible` true, so that handler never fires
        // and the second card keeps the first one's y.
        place();
    }

    function hide() {
        visible = false;
        target = null;
    }

    // Pinned above the *glyph*: the speed cell is wider than its icon because it
    // carries a readout, and centring on the cell slid the menu off the icon.
    function place() {
        if (!target || !space)
            return;
        const p = target.mapToItem(space, 0, 0);
        if (side === "right") {
            // Hanging off the row that owns it. `rightInset` is the row's distance to the
            // card's edge, added back so the gap is measured from the card and not from
            // the text column inside it; go left when the right side has no room. The
            // first option lands on the parent row's own line, which costs the padding.
            let x = p.x + target.width + rightInset + standOff;
            if (x + card.width > space.width - 6)
                x = p.x - rightInset - card.width - standOff;
            picker.x = Math.max(6, x);
            picker.y = Math.max(6, Math.min(space.height - card.height - 6, p.y - pad));
            return;
        }
        let x = p.x + target.width / 2 - card.width / 2;
        x = Math.max(6, Math.min(space.width - card.width - 6, x));
        let y = p.y - card.height - standOff;
        if (y < 4)
            y = p.y + target.height + standOff;
        picker.x = x;
        picker.y = y;
    }

    onVisibleChanged: if (visible)
        place()

    // The size lands a beat after `visible` does, so placing only on show reads
    // the *previous* menu's height: open 8 rows then 4 and the card sits 100 px
    // too high, open 4 then 8 and it hangs down over the icons. Reposition when
    // the card's own geometry catches up.
    Connections {
        target: card
        function onHeightChanged() { if (picker.visible) picker.place(); }
        function onWidthChanged() { if (picker.visible) picker.place(); }
    }

    // The card is as wide as the widest label really lays out. FontMetrics
    // advanceWidth was tried and is wrong for mixed-script labels: it does not
    // resolve the fallbacks the Text item does, so "音轨 2 · eac3" painted ~9 px
    // wider than the number said and the right gap came out 18 against 25.
    readonly property real labelW: {
        let w = 0
        for (let i = 0; i < rows.children.length; ++i) {
            const c = rows.children[i]
            if (c && c.labelW !== undefined)
                w = Math.max(w, c.labelW)
        }
        return w
    }

    Rectangle {
        id: card
        // The dot gutter is paid for on both sides, so the labels get the same
        // clear space left and right instead of hugging the edge. No extra
        // allowance: implicitWidth already carries the glyph side bearings, and
        // the fs * 0.25 that advanceWidth needed now pushed the right gap 4 px
        // past the left.
        width: picker.labelW + 2 * (picker.pad + picker.tick)
        height: picker.count * picker.rowH + 2 * picker.pad
        radius: 4
        color: "#141418"

        Column {
            id: rows
            x: picker.pad
            y: picker.pad
            width: parent.width - 2 * picker.pad

            Repeater {
                model: picker.count
                delegate: Item {
                    id: line
                    required property int index
                    property real labelW: label.implicitWidth
                    width: rows.width
                    height: picker.rowH

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 1
                        radius: 3
                        // Hover moves colour only, never geometry.
                        color: hit.containsMouse ? "#22222A" : "transparent"
                    }
                    Rectangle {
                        visible: line.index === picker.current
                        // Inset by the card's own padding as well: sitting at the
                        // row's x=0 made the dot the only thing touching the edge.
                        x: picker.pad
                        anchors.verticalCenter: parent.verticalCenter
                        width: picker.fs * 0.34
                        height: width
                        radius: width / 2
                        color: Theme.accent
                    }
                    Text {
                        id: label
                        x: picker.tick
                        anchors.verticalCenter: parent.verticalCenter
                        text: picker.options[line.index]
                        color: line.index === picker.current ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: picker.fs
                        font.family: Theme.uiFont
                    }
                    MouseArea {
                        id: hit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: picker.picked(line.index)
                    }
                }
            }
        }
    }
}
