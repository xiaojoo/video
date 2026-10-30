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
        y: bar.target ? Math.min(bar.height - height,
                                 bar.target.contentY / span * bar.height) : 0
        height: bar.target
                ? Math.max(28, Math.min(bar.height, bar.height / span * bar.height)) : 0
    }

    MouseArea {
        id: hold

        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: (mouse) => bar.moveTo(at(mouse.y))
        onPositionChanged: (mouse) => { if (pressed) bar.moveTo(at(mouse.y)); }

        // Where along the track a pointer position is. This cannot be a property binding:
        // `mouse` only exists inside the signal handlers, and a binding that names it
        // throws ReferenceError every time the item repaints.
        function at(y) {
            return height > 0 ? Math.max(0, Math.min(1, y / height)) : 0;
        }
    }

    // Put the clicked point of the track under the middle of the viewport.
    function moveTo(f) {
        const t = target;
        if (!t)
            return;
        const last = Math.max(0, t.contentHeight - t.height);
        t.contentY = Math.max(0, Math.min(last, f * t.contentHeight - t.height / 2));
    }
}
