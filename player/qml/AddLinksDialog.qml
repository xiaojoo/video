import QtQuick
import QtQuick.Controls
import Playerui

// 新建下载: paste one or more links, see where they will land, start them.
//
// It replaces the single-line magnet box that used to sit behind a wide toolbar pill.
// The reference for this shape is the download client's own "add URLs" box: a big
// paste area, the target folder as a row under it, and one full-width action - so a
// batch of links is one paste and one click rather than one dialog per link.
Popup {
    id: sheet
    modal: true
    // No dim behind the box: the page it covers is where the links are coming from
    // (the poster wall, the download column), and the sheet is small enough to read
    // against it.
    Overlay.modal: Item { }
    padding: 0
    // Whole pixels, not the overlay's centre: the overlay is 723 px tall, an odd
    // number, so a centred card sat on a half pixel and the 1 px focus ring over the
    // paste box came out hairline along the top edge and doubled along the bottom,
    // while the sides were crisp (measured in his screenshot: top 16 #81542C,
    // bottom 166+167 #80562A, left 17 #F09630).
    readonly property Item ovl: Overlay.overlay
    // ... and hung on the overlay, because a popup's x/y count from its parent: this one
    // is declared inside the 下载 page, which starts 59 px under the window's top edge
    // (tab row + gutter), so centring on the overlay from there put the whole card 59 px
    // low - measured, the top edge came out at 342 where the centred number is 283. As the
    // overlay's own child its coordinates are the window's and the arithmetic above lands
    // it true centre. The other way in - `parent.mapToItem` to read the page's top - threw
    // the document out at load: no window, no message.
    parent: Overlay.overlay
    x: ovl ? Math.round((ovl.width - sheet.width) / 2) : 0
    y: ovl ? Math.round((ovl.height - sheet.height) / 2) : 0
    width: Math.round(refWidth * 0.42)
    height: body.implicitHeight + 2 * sheet.pad
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // What the width is a fraction of. Handed over by the host; not read off
    // Overlay.overlay, which is null while this is being built.
    property real refWidth: shellWidth
    property string saveDir: ""

    // 更改 asks the host for the folder picker: the dialog does not own that decision.
    signal dirRequested()
    // Every non-empty line, trimmed. The host adds them; this box knows nothing about
    // the download engine.
    signal submitted(var links)

    // What the host could not take, in the box's own words. A pasted line that is neither
    // a magnet nor a .torrent path used to disappear without a word: the dialog closed and
    // one of the three links simply was not in the list.
    property string problem: ""

    readonly property real pad: Math.round(shellHeight * 0.028)
    readonly property real smallFs: Math.round(0.0180 * shellHeight)
    readonly property real titleFs: Math.round(0.0194 * shellHeight)
    readonly property real fieldH: Math.round(shellHeight * 0.05)

    function links() {
        const out = [];
        for (const raw of editor.text.split("\n")) {
            const s = raw.trim();
            if (s.length > 0)
                out.push(s);
        }
        return out;
    }

    function reset() {
        editor.text = "";
        sheet.problem = "";
    }

    // The host reports back what it could not add. Those lines are put back in the box so
    // they can be fixed, and the box stays open - closing it said "done" while a third of
    // the paste had gone nowhere.
    function rejectSome(lines, added) {
        editor.text = lines.join("\n");
        const n = lines.length;
        sheet.problem = (added > 0 ? "已加入 " + added + " 个；" : "")
                        + n + " 行没加入：只认 magnet: 链接和本机的 .torrent 文件。";
        editor.forceActiveFocus();
    }

    onOpened: editor.forceActiveFocus()

    background: Rectangle {
        id: plate
        // The card takes the caret from the paste area: a press on the title, a label
        // or the empty part of the box used to leave the field focused, so the accent
        // ring stayed on as if the keyboard were still going there.
        focus: true
        radius: 6
        color: "#141418"
        border.color: "#3A3A40"
        border.width: 1

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: plate.forceActiveFocus()
        }
    }

    Column {
        id: body
        x: sheet.pad
        y: sheet.pad
        width: sheet.width - 2 * sheet.pad
        spacing: 0

        Item {
            width: parent.width
            height: Math.round(sheet.titleFs * 1.5)

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "添加链接或口令"
                color: Theme.textPrimary
                font.pixelSize: sheet.titleFs
                font.family: Theme.uiFont
            }
            Rectangle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: Math.round(shellHeight * 0.0304)
                height: width
                radius: Theme.controlRadius
                color: closeHit.containsMouse ? "#2A2A31" : "transparent"

                Icon {
                    anchors.centerIn: parent
                    width: Math.round(parent.width * 0.55)
                    height: width
                    name: "close"
                    color: Theme.glyph
                }
                MouseArea {
                    id: closeHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sheet.close()
                }
            }
        }

        Item { width: 1; height: Math.round(sheet.pad * 0.55) }

        // The paste area. A TextEdit rather than a TextInput: a batch is several lines,
        // and a one-line field scrolled the first link out of sight as the second was
        // pasted.
        Rectangle {
            id: pasteBox
            width: parent.width
            height: Math.round(shellHeight * 0.21)
            radius: Theme.controlRadius
            color: "#1B1B1F"
            border.width: editor.activeFocus ? 1 : 0
            border.color: Theme.accent
            clip: true

            TextEdit {
                id: editor
                anchors.fill: parent
                anchors.margins: 9
                wrapMode: TextEdit.Wrap
                clip: true
                selectByMouse: true
                color: Theme.textPrimary
                selectionColor: Theme.accent
                font.pixelSize: sheet.smallFs
                font.family: Theme.uiFont
            }
            Text {
                anchors.left: editor.left
                anchors.top: editor.top
                anchors.right: editor.right
                visible: editor.text.length === 0
                text: "添加多个下载链接时，请确保每行只有一个链接。"
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.pixelSize: sheet.smallFs
                font.family: Theme.uiFont
            }
        }

        // What the host refused, in the accent the job rows use for their own errors. It
        // only exists while there is something to say, so a clean paste keeps the height
        // it had.
        Text {
            visible: sheet.problem.length > 0
            width: parent.width
            topPadding: Math.round(sheet.pad * 0.5)
            text: sheet.problem
            wrapMode: Text.WordWrap
            color: Theme.accent
            font.pixelSize: sheet.smallFs
            font.family: Theme.uiFont
        }

        Item { width: 1; height: Math.round(sheet.pad * 1.25) }

        Text {
            text: "下载到："
            color: Theme.textSecondary
            font.pixelSize: sheet.smallFs
            font.family: Theme.uiFont
        }

        Item { width: 1; height: Math.round(sheet.pad * 0.4) }

        // The target, as one row: the drive it means, the folder itself, and the way to
        // change it. Only one target exists - there is no cloud side to switch to.
        Rectangle {
            width: parent.width
            height: sheet.fieldH
            radius: Theme.controlRadius
            color: "#1B1B1F"

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                Icon {
                    width: Math.round(sheet.smallFs * 1.25)
                    height: width
                    name: "folder"
                    color: Theme.textSecondary
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "电脑"
                    color: Theme.textPrimary
                    font.pixelSize: sheet.smallFs
                    font.family: Theme.uiFont
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: sheet.saveDir.length > 0 ? sheet.saveDir : "…"
                    elide: Text.ElideMiddle
                    width: Math.min(implicitWidth, Math.round(sheet.width * 0.44))
                    color: Theme.textSecondary
                    font.pixelSize: sheet.smallFs
                    font.family: Theme.uiFont
                }
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "更改"
                color: changeHit.containsMouse ? Theme.accent : Theme.textSecondary
                font.pixelSize: sheet.smallFs
                font.family: Theme.uiFont

                MouseArea {
                    id: changeHit
                    anchors.fill: parent
                    anchors.margins: -6
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sheet.dirRequested()
                }
            }
        }

        Item { width: 1; height: Math.round(sheet.pad * 0.75) }

        // The one action, the full width of the box. Dimmed rather than hidden while
        // nothing is pasted, so the empty state still shows what pressing it will do.
        //
        // The dim is two colours, not an opacity on the whole button: at 0.4 the accent
        // fill landed on #724408 and the dark label on top of it on #4A2C06 - the word
        // 立即下载 was there in the layout and invisible on screen. The plate goes to a
        // muted amber and the label to a warm grey, which still reads as "not yet" and
        // keeps the contrast (about 7:1).
        Rectangle {
            id: goBtn
            readonly property bool dead: editor.text.trim().length === 0
            width: parent.width
            height: Math.round(shellHeight * 0.064)
            radius: Theme.controlRadius
            color: dead ? "#3A2A0E"
                        : goHit.containsMouse ? "#FFA733" : Theme.accent

            Text {
                anchors.centerIn: parent
                text: {
                    const n = sheet.links().length;
                    return n > 1 ? "立即下载 " + n + " 个" : "立即下载";
                }
                color: goBtn.dead ? "#C7A96F" : "#15151A"
                font.pixelSize: Math.round(0.0194 * shellHeight)
                font.family: Theme.uiFont
            }
            MouseArea {
                id: goHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: goBtn.dead ? Qt.ArrowCursor : Qt.PointingHandCursor
                onClicked: {
                    if (goBtn.dead)
                        return;
                    sheet.submitted(sheet.links());
                }
            }
        }
    }
}
