import QtQuick
import Playerui

// A scrollbar this app draws itself. The platform style paints its own white groove no
// matter what `background` says - the same reason every text field here is a Rectangle
// with a TextInput inside it - and on a near-black page that groove reads as a band of
// another window lying on top of this one. So: a thumb over a transparent track, and the
// track is only there to be grabbed.
//
// It is not a child of the Flickable, because a Flickable adopts its children into the
// scrolling content; the caller places it beside the view and hands over the target.
Item {
    id: bar

    property Flickable target
    // The visible mark. The item itself stays wider so the thumb is easy to catch.
    property real thickness: 4
    // For a view that is itself hidden some of the time: overriding `visible` here would
    // drop the "is there anything to scroll" test, so callers set this instead.
    property bool wantVisible: true

    width: 14
    visible: wantVisible && target !== null && target.contentHeight > target.height + 1
    enabled: visible


    // The mark's own travel, and the range it drives. Both the thumb's position and the
    // drag are read off this pair, so the mark is where the pointer put it.
    readonly property real travel: Math.max(1, height - thumb.height)
    readonly property real scrollable: target ? Math.max(1, target.contentHeight - target.height) : 1

    Rectangle {
        id: thumb

        x: (bar.width - bar.thickness) / 2
        width: bar.thickness
        radius: width / 2
        color: hold.pressed ? Theme.accent : (hold.containsMouse ? "#4A4A52" : "#3A3A40")
        // Where the view is, mapped onto the track. `Flickable.visibleArea` is an attached
        // property and does not read from outside the flickable, so this is the same
        // fraction taken from contentY/contentHeight.
        readonly property real span: bar.target ? Math.max(1, bar.target.contentHeight) : 1
        y: bar.target ? Math.round(bar.target.contentY / bar.scrollable * bar.travel) : 0
        height: bar.target
                ? Math.round(Math.max(28, Math.min(bar.height,
                                                   bar.height / span * bar.height))) : 0
    }

    MouseArea {
        id: hold

        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        // How far below the thumb's top the press landed. Holding on to it is what keeps
        // the mark under the pointer; the mapping before this one put the *middle of the
        // viewport* under the cursor instead, so grabbing the top edge of the 368 px thumb
        // of the magnet list (11 seeds, 432 px of track, 74 px scrollable) moved the
        // content by 185 px of thumb travel in one step - the list lurched, and after that
        // the mark and the pointer never agreed again.
        property int grab: 0
        onPressed: (mouse) => {
            grab = Math.max(0, Math.min(thumb.height, mouse.y - thumb.y));
            bar.setFromTrack(mouse.y - grab);
        }
        onPositionChanged: (mouse) => { if (pressed) bar.setFromTrack(mouse.y - grab); }
    }

    // A whole pixel: a fractional contentY leaves every row between two device rows, and
    // the separators and the text resample as the drag moves.
    function setFromTrack(trackY) {
        const t = target;
        if (!t)
            return;
        const f = Math.max(0, Math.min(1, trackY / travel));
        t.contentY = Math.round(f * scrollable);
    }
}
