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
    // { label, action, on } - `on` defaults to true; a row that is off is drawn dim and
    // does not answer a click, the same rule the control-row icons follow when the file
    // has nothing to offer (no audio track, no chapters).
    property var items: []
    // Which object the open menu belongs to, so the caller can act on the right row.
    property var target: null

    signal picked(string action, var target)

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

    Rectangle {
        id: card
        width: menu.labelW + 2 * menu.sidePad
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
                    Text {
                        id: label
                        anchors.verticalCenter: parent.verticalCenter
                        text: line.item ? line.item.label : ""
                        color: !line.live ? "#4A4A4E"
                               : (hit.containsMouse ? Theme.textPrimary : Theme.textSecondary)
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
                        onClicked: {
                            menu.picked(line.item.action, menu.target);
                            menu.hide();
                        }
                    }
                }
            }
        }
    }
}
