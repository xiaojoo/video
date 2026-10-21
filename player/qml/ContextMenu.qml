import QtQuick
import Playerui

// A right-click menu. Qt's own Menu is a separate native window: it outranks the player
// it belongs to, cannot be styled to match the chrome, and lands in the wrong place on a
// frameless tool window - the same three reasons the bubble and the dropdown are ours.
// Each surface gets its own instance, and one instance is shared by everything on that
// surface, so two of its menus cannot be open at once.
Item {
    id: menu

    property Item space: parent
    // The size the chrome was designed at (the window's launch rect), so the rows keep
    // the type size they have where the window started.
    property real refH: 720
    // { label, action, on, icon, keys } - `on` defaults to true; a row that is off is
    // drawn dim and does not answer a click, the same rule the control-row icons follow
    // when the file has nothing to offer (no audio track, no chapters). `icon` is an
    // Icon.qml name and `keys` the hint the bubble shows; both columns appear only where
    // some row actually carries them, so the library card's menu keeps the geometry it
    // was approved at.
    property var items: []
    // Which object the open menu belongs to, so the caller can act on the right row.
    property var target: null

    signal picked(string action, var target)
    // Emitted when the pointer lands on a row: the action of a row that owns a submenu,
    // "" for every other row. The shell cannot tell a submenu from a command on its own,
    // and the menu has no business opening lists it knows nothing about.
    signal hovered(string action, var row)

    // Qt hands a mouse-over to exactly one item, so the menu counts its own rows the way
    // the control row counts its cells: the shell has to know the pointer is still inside
    // this box when it decides whether the submenu beside it should close.
    property int hoverRows: 0
    readonly property bool pointerInside: hoverRows > 0

    readonly property real fs: Theme.subtitleFontRatio * refH * 0.85
    readonly property real pad: fs * 0.62
    // The card's left and right padding. It carries two things at once: the room the
    // label has inside the highlight, and the room the highlight has inside the card -
    // so the hover gap cannot grow without this growing too. Raised from the vertical
    // pad (7.8 px) to 15.6 px to make that gap 11.7 px instead of 3.9. The top and bottom
    // stay on `pad`: only the sides were asked for.
    readonly property real sidePad: fs * 1.24
    readonly property real rowH: fs * 2.0
    readonly property int count: items ? items.length : 0

    readonly property bool hasIcons: {
        for (let i = 0; i < count; ++i)
            if (items[i].icon)
                return true;
        return false;
    }
    readonly property bool hasKeys: {
        for (let i = 0; i < count; ++i)
            if (items[i].keys)
                return true;
        return false;
    }
    // The glyph column. The icons are drawn on a 24-unit grid that fills its box, and
    // the widest of them (menu, cc) ink right up to the box edge - a gutter of 1.35 x the
    // type size left 1.2 px between such a glyph and the first character. 1.75 gives the
    // tightest pair 8.3 px, and the glyph sits at the column's left so the gap is all on
    // the side that needs it.
    readonly property real tick: hasIcons ? fs * 1.75 : 0
    readonly property real glyphSize: fs * 1.15
    // The gap between a label and its key hint. No number was given, so it is stated:
    // 1.2 x the type size, enough that the two columns never read as one phrase.
    readonly property real keyGap: fs * 1.2

    visible: false
    width: card.width
    height: card.height

    function showAt(px, py, list, item) {
        if (!space || !list || list.length === 0)
            return;
        items = list;
        target = item;
        visible = true;
        // The pointer is inside the card it opened on, so the box goes down-right from
        // there and flips when that would hang it off either edge.
        let x = px, y = py;
        if (x + card.width > space.width - 6)
            x = space.width - card.width - 6;
        if (y + card.height > space.height - 6)
            y = space.height - card.height - 6;
        menu.x = Math.max(6, x);
        menu.y = Math.max(6, y);
    }

    function hide() {
        visible = false;
        target = null;
        items = [];
        // The pointer does not send an exit when the box under it simply goes away, and a
        // count left behind would keep the submenu alive forever on the next dropdown.
        hoverRows = 0;
    }

    // The card is as wide as the widest label really lays out - the same reason the
    // dropdown does not measure text itself: FontMetrics does not resolve the fallbacks
    // a Text item does, so mixed-script rows come out narrower than they paint.
    readonly property real labelW: {
        let w = 0;
        for (let i = 0; i < rows.children.length; ++i) {
            const c = rows.children[i];
            if (c && c.labelW !== undefined)
                w = Math.max(w, c.labelW);
        }
        return w;
    }
    // The key column is as wide as its widest entry, and every row right-aligns to it,
    // so the hints line up on one edge instead of chasing the label they sit after.
    readonly property real keyW: {
        let w = 0;
        for (let i = 0; i < rows.children.length; ++i) {
            const c = rows.children[i];
            if (c && c.keyW !== undefined)
                w = Math.max(w, c.keyW);
        }
        return w;
    }

    Rectangle {
        id: card
        width: menu.tick + menu.labelW
               + (menu.hasKeys ? menu.keyGap + menu.keyW : 0) + 2 * menu.sidePad
        height: menu.count * menu.rowH + 2 * menu.pad
        radius: 4
        color: "#141418"

        Column {
            id: rows
            x: menu.sidePad
            y: menu.pad
            width: parent.width - 2 * menu.sidePad

            Repeater {
                model: menu.count
                delegate: Item {
                    id: line
                    required property int index
                    readonly property var item: line.index < menu.count ? menu.items[line.index] : null
                    // A row that does not say otherwise is live; only an explicit
                    // `on: false` greys it. Written as a test against false rather than
                    // `item.on` because a row list without the key at all is the normal
                    // case, and undefined is not true.
                    readonly property bool live: item !== null && item.on !== false
                    property real labelW: label.implicitWidth
                    property real keyW: keys.implicitWidth
                    width: rows.width
                    height: menu.rowH

                    Rectangle {
                        anchors.fill: parent
                        anchors.topMargin: 1
                        anchors.bottomMargin: 1
                        // The gap: the highlight stops short of the card's sides, so a
                        // hovered row reads as a band lying inside the box. No number
                        // given, so the coefficient is a quarter of the padding - 3.9 px
                        // taken off *outward*, which is what keeps the label inside the
                        // band: a positive margin starts the band to the right of the
                        // first glyph and pushes it outside the highlight (measured:
                        // band 612..667 against ink starting at 610).
                        anchors.leftMargin: -menu.sidePad / 4
                        anchors.rightMargin: -menu.sidePad / 4
                        radius: 3
                        // Hover moves colour only, never geometry.
                        color: hit.containsMouse && line.live ? "#22222A" : "transparent"
                    }
                    Icon {
                        // The control's own glyph, the same one the icon under the bar
                        // draws, so the row and the bubble describe one thing twice
                        // rather than two things that look alike.
                        anchors.verticalCenter: parent.verticalCenter
                        x: 0
                        width: menu.glyphSize
                        height: menu.glyphSize
                        visible: menu.hasIcons
                        name: line.item && line.item.icon ? line.item.icon : ""
                        color: !line.live ? "#4A4A4E"
                               : (hit.containsMouse ? Theme.textPrimary : Theme.textSecondary)
                    }
                    Text {
                        id: label
                        x: menu.tick
                        anchors.verticalCenter: parent.verticalCenter
                        text: line.item ? line.item.label : ""
                        color: !line.live ? "#4A4A4E"
                               : (hit.containsMouse ? Theme.textPrimary : Theme.textSecondary)
                        font.pixelSize: menu.fs
                        font.family: Theme.uiFont
                    }
                    Text {
                        id: keys
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        // A row list that carries no key column at all is the normal case,
                        // not an error: the media library's card menu has no icons and no
                        // shortcuts. `undefined` into a QString property is what the
                        // assignment complained about, ten times per card menu.
                        text: line.item && line.item.keys ? line.item.keys : ""
                        visible: menu.hasKeys && text.length > 0
                        // Stays on the secondary grey even under hover: the label is the
                        // thing being pointed at, and whitening both would say so twice.
                        color: line.live ? Theme.textSecondary : "#4A4A4E"
                        font.pixelSize: menu.fs
                        font.family: Theme.uiFont
                    }
                    MouseArea {
                        id: hit
                        anchors.fill: parent
                        hoverEnabled: true
                        // A row that is off still shows - it has to explain itself - but
                        // it does not answer the click, which is how the greyed icons on
                        // the control row behave.
                        cursorShape: line.live ? Qt.PointingHandCursor : Qt.ArrowCursor
                        enabled: line.live
                        onEntered: {
                            menu.hoverRows = menu.hoverRows + 1;
                            menu.hovered(line.live && line.item && line.item.sub
                                         ? line.item.action : "", line);
                        }
                        // Only the count comes off an exit; the submenu itself is closed
                        // by the *next* row entered, so crossing the few pixels between a
                        // row and the list beside it does not dismiss the pair.
                        onExited: menu.hoverRows = menu.hoverRows - 1
                        onClicked: {
                            menu.picked(line.item.action, menu.target);
                            // A row that owns a submenu keeps the menu open: the list
                            // belongs beside it, and closing the parent would close the
                            // pair. Every other row is a command, and the menu is done.
                            if (!line.item.sub)
                                menu.hide();
                        }
                    }
                }
            }
        }
    }
}
