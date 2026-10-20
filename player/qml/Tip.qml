import QtQuick
import Playerui

// Our own callout. Qt's ToolTip is a separate native popup window: it cannot be
// styled to match the chrome, it outranks the player window it belongs to, and
// on a frameless tool window it lands in the wrong place. One shared instance is
// also what keeps two bubbles from ever opening at once - whoever is hovered
// last owns it.
Item {
    id: tip

    // The item the x/y below are expressed in. Defaults to our own parent, which
    // is the layer we are placed on.
    property Item space: parent
    // The size the chrome was designed at (the window's launch rect), so the bubble
    // keeps the type size it has where the window started instead of scaling with it.
    property real refH: 720
    property string side: "above"      // "above" | "below"

    property Item target: null
    property string label: ""
    property string keys: ""
    property real caretX: 0
    property int delayMs: 420

    readonly property real fs: Theme.subtitleFontRatio * refH * 0.85
    readonly property real pad: fs * 0.62
    readonly property real caret: Math.max(4, fs * 0.40)
    readonly property real standOff: caret + fs * 0.30

    width: card.width
    height: card.height
    visible: false
    opacity: 0

    function show(item, text, keyHint, atSide) {
        if (!item || text.length === 0)
            return;
        tip.target = item;
        tip.label = text;
        tip.keys = keyHint;
        tip.side = atSide;
        place();                 // so the size is right the moment it appears
        timer.restart();
    }

    function hide() {
        timer.stop();
        tip.visible = false;
        tip.opacity = 0;
    }

    // A jump from one icon to the next delivers the new `entered` *before* the
    // old `exited`, so the outgoing cell must not be allowed to cancel the
    // bubble that has already been claimed by the incoming one.
    function hideFor(item) {
        if (item === target)
            hide();
    }

    // Centred on the target, then pushed back inside the window; the caret keeps
    // following the target, so a clamped bubble still points at the right icon.
    function place() {
        if (!target || !space)
            return;
        const p = target.mapToItem(space, 0, 0);
        const cx = p.x + target.width / 2;
        const w = card.width, h = card.height;
        let x = cx - w / 2;
        x = Math.max(6, Math.min(space.width - w - 6, x));
        let y = side === "above" ? p.y - h - standOff : p.y + target.height + standOff;
        if (side === "above" && y < 4)
            y = p.y + target.height + standOff;
        tip.x = x;
        tip.y = y;
        caretX = Math.max(caret + 6, Math.min(w - caret - 6, cx - x));
    }

    onVisibleChanged: if (visible)
        place()

    Timer {
        id: timer
        interval: tip.delayMs
        onTriggered: {
            tip.place();
            tip.visible = true;
            tip.opacity = 1;
        }
    }

    // The pointer tip sits behind the card, so only its two outer edges - and
    // their share of the border - stick out.
    Rectangle {
        id: pointy
        width: tip.caret * 1.42
        height: width
        rotation: 45
        x: tip.caretX - width / 2
        y: tip.side === "above" ? card.height - height / 2 : -height / 2
        color: card.color
        z: -1
    }

    Rectangle {
        id: card
        width: line.width + 2 * tip.pad
        height: line.height + 2 * tip.pad
        radius: Math.min(4, height * 0.25)
        color: "#141418"

        Row {
            id: line
            x: tip.pad
            y: tip.pad
            spacing: tip.pad * 0.75

            Text {
                text: tip.label
                color: Theme.textPrimary
                font.pixelSize: tip.fs
                font.family: Theme.uiFont
            }
            Text {
                visible: tip.keys.length > 0
                text: tip.keys
                color: Theme.textSecondary
                font.pixelSize: tip.fs
                font.family: Theme.uiFont
            }
        }
    }
}
