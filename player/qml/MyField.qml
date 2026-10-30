import QtQuick
import Playerui

// A text field the app draws itself. The native style refuses a customised control
// background and would drop a system-white box into a dark panel, so every input here
// is a Rectangle plus a TextInput - the same pair the library card's rename uses.
Rectangle {
    id: box

    property alias text: input.text
    property string placeholder: ""
    readonly property bool active: input.activeFocus
    // The page that places a field knows its type scale. Both this and the height have
    // to come from the caller: they used to be fractions of `parent.height`, which
    // inside a 36 px toolbar row made a 2 px box with 1 px text in it.
    property int fontSize: Math.round(box.height * 0.36)

    // Enter, or the field losing focus with something in it.
    signal committed()

    // A field standing on a plate that is already almost the field's own grey
    // (#1B1B1F on #17171C is a Δ4 step) reads as text lying on the plate. Those callers
    // draw the edge instead of pretending the fill will carry it.
    property bool framed: false
    // The settings card is navy rather than the app's charcoal, so a page there hands
    // over its own four greys. Every other caller keeps the defaults below.
    property color fill: "#1B1B1F"
    property color edgeColor: "#3A3A40"
    property color hintColor: Theme.textSecondary
    property real corner: 4
    property real inset: 8

    height: 32
    radius: box.corner
    color: box.fill
    border.width: box.active || box.framed ? 1 : 0
    border.color: box.active ? Theme.accent : box.edgeColor

    TextInput {
        id: input
        anchors.fill: parent
        anchors.leftMargin: box.inset
        anchors.rightMargin: box.inset
        verticalAlignment: TextInput.AlignVCenter
        clip: true
        color: Theme.textPrimary
        font.pixelSize: box.fontSize
        font.family: Theme.uiFont
        selectionColor: Theme.accent
        onAccepted: box.committed()
    }

    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.leftMargin: box.inset
        visible: input.text.length === 0 && !input.activeFocus
        text: box.placeholder
        color: box.hintColor
        font.pixelSize: input.font.pixelSize
        font.family: Theme.uiFont
    }
}
