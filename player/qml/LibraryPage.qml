import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import Playerui

Item {
    id: page

    readonly property real colW: Math.max(120, page.width * 0.13)
    // 横屏: a card is a frame off the film, so the box keeps the frame's own 16:9
    // and the still fills it instead of being sliced into a poster.
    readonly property real artH: colW * 9 / 16

    function fmtDuration(sec) {
        const s = Math.round(sec)
        const pad = (n) => String(n).padStart(2, "0")
        const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), ss = s % 60
        return h > 0 ? h + ":" + pad(m) + ":" + pad(ss) : m + ":" + pad(ss)
    }

    // The card's right-click menu. One instance for the whole grid, so two cards cannot
    // be open at once; the action is applied to whichever card opened it. A card hidden
    // or renamed reloads the grid from C++ (Library::rowsChanged).
    readonly property var cardItems: [
        { label: "打开文件所在文件夹", action: "reveal" },
        { label: "重命名",              action: "rename" },
        { label: "从媒体库移除",         action: "hide" }
    ]

    Rectangle {
        anchors.fill: parent
        color: "#0A0A0C"
    }

    // A cell is the art plus the text under it, and that text height is measured
    // from two hidden items carrying the delegate's own font rather than guessed
    // from a window ratio: the ratio that fits at 723 px clips the year line at 225.
    Text {
        id: titleMetric
        visible: false
        text: "M\nM"
        font.pixelSize: page.height * 0.021
        font.family: Theme.uiFont
    }
    Text {
        id: yearMetric
        visible: false
        text: "M"
        font.pixelSize: page.height * 0.019
        font.family: Theme.uiFont
    }
    readonly property real textBlock: 9 + titleMetric.height + yearMetric.height + 6

    Column {
        id: header
        x: page.width * 0.02
        y: page.height * 0.035
        spacing: page.height * 0.006

        Text {
            text: "媒体库"
            color: Theme.textPrimary
            font.pixelSize: Theme.titleFontRatio * page.height
            font.family: Theme.uiFont
        }
        Text {
            text: library.mediaCount + " 部 · " + library.fileCount + " 个文件 · 时长 "
                  + library.durationCount + "/" + library.fileCount + " · 未解析 "
                  + library.unparsedCount + " · 上次扫描 " + library.lastScanMs + " ms"
            color: Theme.textSecondary
            font.pixelSize: Theme.subtitleFontRatio * page.height
            font.family: Theme.uiFont
        }
    }

    Rectangle {
        id: rescan
        anchors.right: parent.right
        anchors.rightMargin: page.width * 0.02
        y: header.y + header.height * 0.15
        width: rescanText.implicitWidth + page.width * 0.02
        height: rescanText.height + page.height * 0.018
        radius: height / 2
        color: over.containsMouse ? "#2A2A2E" : "#1B1B1F"
        border.color: "#3A3A40"
        border.width: 1

        Text {
            id: rescanText
            anchors.centerIn: parent
            text: "重新扫描"
            color: Theme.textPrimary
            font.pixelSize: Theme.subtitleFontRatio * page.height
            font.family: Theme.uiFont
        }
        MouseArea {
            id: over
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: library.scan()
        }
    }

    Component.onCompleted: {
        if (shellAutoPick)
            folderPick.open();
        // Opening the page from the player never scanned, so a catalogue that was
        // already in the database showed up as an empty grid.
        else if (library.lastScanMs === 0)
            library.scan();
    }

    FolderDialog {
        id: folderPick
        title: "选择媒体目录"
        onAccepted: {
            library.addRoot(folderPick.selectedFolder)
            library.scan()
        }
    }

    Rectangle {
        id: pick
        anchors.right: rescan.left
        anchors.rightMargin: page.width * 0.012
        y: rescan.y
        width: pickText.implicitWidth + page.width * 0.02
        height: rescan.height
        radius: height / 2
        color: pickOver.containsMouse ? "#2A2A2E" : "#1B1B1F"
        border.color: "#3A3A40"
        border.width: 1

        Text {
            id: pickText
            anchors.centerIn: parent
            text: "选择文件夹"
            color: Theme.textPrimary
            font.pixelSize: Theme.subtitleFontRatio * page.height
            font.family: Theme.uiFont
        }
        MouseArea {
            id: pickOver
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: folderPick.open()
        }
    }

    // The library covers the whole window and hides the transport row, so this
    // page owns its own exit — an empty library has no card to click.
    Rectangle {
        id: backToPlayer
        anchors.right: pick.left
        anchors.rightMargin: page.width * 0.012
        y: pick.y
        width: backText.implicitWidth + page.width * 0.02
        height: pick.height
        radius: height / 2
        color: backOver.containsMouse ? "#2A2A2E" : "#1B1B1F"
        border.color: "#3A3A40"
        border.width: 1

        Text {
            id: backText
            anchors.centerIn: parent
            text: "返回播放"
            color: Theme.textPrimary
            font.pixelSize: Theme.subtitleFontRatio * page.height
            font.family: Theme.uiFont
        }
        MouseArea {
            id: backOver
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: win.page = "player"
        }
    }

    GridView {
        id: grid
        x: page.width * 0.02
        y: header.y + header.height + page.height * 0.03
        width: page.width - 2 * page.width * 0.02
        height: page.height - y - page.height * 0.03
        clip: true
        cellWidth: page.colW
        cellHeight: page.artH + page.textBlock
        boundsBehavior: Flickable.StopAtBounds
        model: libraryModel
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        delegate: Item {
            id: cell
            required property string title
            required property int year
            required property string path
            required property bool parsed
            required property string kind
            required property double duration
            required property string poster
            required property var mediaId
            // The rename is edited in place, on the card's own label - not in a dialog.
            property bool renaming: false
            // One "the pointer is on this card" for everything the card shows. Qt hands a
            // mouse-over to exactly one item, so while the corner ✕ is hovered the card's
            // own area reports false - and the ✕ used to be shown *by* that flag, which
            // meant moving onto it hid it, which gave the hover back, which showed it
            // again. Measured: the plate's colour never changed under the pointer.
            readonly property bool hovered: mouse.containsMouse || hideHit.containsMouse

            function startRename() {
                renameField.text = cell.title
                cell.renaming = true
                renameField.forceActiveFocus()
                renameField.select(0, renameField.text.length)
            }
            function commit() {
                const t = renameField.text.trim()
                if (t.length > 0 && t !== cell.title)
                    library.setTitle(cell.mediaId, t)
                cell.renaming = false
            }
            function cancel() {
                cell.renaming = false
            }
            width: grid.cellWidth
            height: grid.cellHeight

            Rectangle {
                id: art
                x: 4
                y: 4
                width: parent.width - 8
                height: page.artH
                radius: 4
                color: "#17171B"
                border.width: cell.hovered ? 2 : 1
                border.color: cell.hovered ? Theme.accent : "#2C2C31"

                // The first frame, cropped to the card by the box it fills. Two
                // pixels inside the card: at one pixel the image's square corner
                // still sticks 1 px outside the card's 4 px arc, and an image is
                // a plain quad that no radius reaches.
                Image {
                    id: cover
                    anchors.fill: parent
                    anchors.margins: 2
                    source: poster
                    fillMode: Image.PreserveAspectCrop
                    visible: status === Image.Ready
                }

                // The badge and the 未识别 tag read off this, not off the still:
                // a white opening frame drops #B8B8B8 text to 1.9:1 on its own.
                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: parent.height * 0.30
                    visible: poster.length > 0
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#000A0A0C" }
                        GradientStop { position: 1.0; color: "#E60A0A0C" }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    text: kind === "audio" ? "♪" : ""
                    color: "#3A3A40"
                    font.pixelSize: parent.height * 0.3
                    visible: text.length > 0
                }
                Text {
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 6
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: parsed ? "" : "未识别"
                    color: Theme.accent
                    font.pixelSize: page.height * 0.018
                    font.family: Theme.uiFont
                }
                // Corner badge: the approved title/year rows keep their rhythm.
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    text: duration > 0 ? page.fmtDuration(duration) : ""
                    color: Theme.textSecondary
                    font.pixelSize: page.height * 0.016
                    font.family: Theme.uiFont
                }
            }

            Text {
                id: nameText
                x: 4
                y: art.y + art.height + 4
                width: parent.width - 8
                padding: 0
                text: title
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.WordWrap
                color: Theme.textPrimary
                font.pixelSize: page.height * 0.021
                font.family: Theme.uiFont
            }
            // Year gets its own line: sharing one item let a long title push it out.
            Text {
                x: 4
                y: nameText.y + nameText.height + 1
                text: year > 0 ? String(year) : " "
                color: Theme.textSecondary
                font.pixelSize: page.height * 0.019
                font.family: Theme.uiFont
            }

            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: (m) => {
                    if (m.button === Qt.RightButton) {
                        // The menu's id is in this component's scope; `page.cardMenu`
                        // would be undefined, because an id is not a property of the
                        // object it sits on.
                        const at = cell.mapToItem(page, m.x, m.y);
                        cardMenu.showAt(at.x, at.y, page.cardItems, cell);
                        return;
                    }
                    playerCore.load(path); win.page = "player";
                }
            }

            // Remove from the library. Only on hover, and only clickable while shown:
            // an affordance nobody can see must not be a hit target. Declared after the
            // card's own MouseArea so it is the one that gets the click.
            //
            // A plate, not a loose glyph: the card is a frame off the film, and the
            // ✕ used to sit directly on it - white on hover over a bright frame read as
            // nothing at all. The plate is wider than the glyph so the mark that can be
            // hit is the mark that is drawn.
            Rectangle {
                id: hideBadge
                z: 2
                anchors.top: art.top
                anchors.right: art.right
                anchors.margins: 8
                width: hideGlyph.width + hideGlyph.height * 1.1
                height: hideGlyph.height * 1.5
                radius: 4
                color: hideHit.containsMouse ? "#F2000000" : "#B3000000"
                visible: cell.hovered && !cell.renaming

                Text {
                    id: hideGlyph
                    anchors.centerIn: parent
                    // The plate is sized and placed off the glyph's *line box*, and the
                    // ✕'s ink does not sit in the middle of that box - measured on the
                    // plate over a white part of the frame (its edges are a hard
                    // transition there): plate rows 111..142, ink rows 123..133, so the
                    // mark lay 1.5 px below the plate's centre at a 17.35 px font.
                    // Centring the box cannot fix that, so the box goes up by the same
                    // 0.086 of the type size, which keeps the mark centred at every
                    // window size.
                    anchors.verticalCenterOffset: -font.pixelSize * 0.086
                    text: "✕"
                    color: hideHit.containsMouse ? Theme.textPrimary : Theme.glyph
                    font.pixelSize: page.height * 0.024
                    font.family: Theme.uiFont
                }
                MouseArea {
                    id: hideHit
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: hideBadge.visible
                    cursorShape: Qt.PointingHandCursor
                    onClicked: library.hideMedia(cell.mediaId)
                }
            }

            // The same slot the title occupies, one line taller than the glyph: the
            // label is replaced where it stands rather than edited in a window elsewhere.
            Rectangle {
                id: renameBox
                z: 3
                x: 4
                y: nameText.y - 2
                width: parent.width - 8
                height: nameText.height + 4
                radius: 3
                color: "#17171B"
                border.color: Theme.accent
                border.width: 1
                visible: cell.renaming
                TextInput {
                    id: renameField
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: Theme.textPrimary
                    font.pixelSize: nameText.font.pixelSize
                    font.family: Theme.uiFont
                    selectionColor: Theme.accent
                    onAccepted: cell.commit()
                    Keys.onEscapePressed: cell.cancel()
                    // Losing the field without Enter - a click on another card, or on the
                    // window's own buttons - abandons the edit. Without this the card
                    // stays in edit mode and the next click both closes it and starts the
                    // film.
                    onActiveFocusChanged: if (!activeFocus && cell.renaming)
                        cell.cancel()
                }
            }
        }
    }

    // The card's menu, and the layer that dismisses it. Both above the grid: with a menu
    // open, a click anywhere but on a row is a dismissal, not a request to start playing
    // the card underneath it.
    MouseArea {
        z: cardMenu.z - 1
        anchors.fill: parent
        visible: cardMenu.visible
        enabled: cardMenu.visible
        // A right-click somewhere else while a menu is open moves the menu to that
        // card, so both buttons have to reach this layer.
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: cardMenu.hide()
    }

    ContextMenu {
        id: cardMenu
        z: 20
        space: page
        refH: shellHeight
        onPicked: (action, target) => {
            if (!target)
                return;
            if (action === "reveal")
                library.revealInFolder(target.path);
            else if (action === "rename")
                target.startRename();
            else if (action === "hide")
                library.hideMedia(target.mediaId);
        }
    }
}
