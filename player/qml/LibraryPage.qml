import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import Playerui

Item {
    id: page

    // Two numbers, both taken from the player's control row: `gutter` is what its glyphs
    // sit in from the window's edge, `gap` the distance between two neighbours' ink. The
    // four outer sides all use the first, so the cards line up with the title above them
    // and with the window's bottom; the inner spacing uses the second, like the row of
    // buttons under the film.
    readonly property real gutter: Math.round(shellWidth * Theme.sideInsetWRatio)
    readonly property real gap: Math.round(shellHeight * Theme.controlGapRatio)
    // Half the gap per side, so two neighbours sit one gap apart and the outermost cards
    // still land on the gutter itself.
    readonly property real cardInset: Math.round(gap / 2)
    readonly property int perRow: 7
    // A card is a frame off the film, so the box keeps the frame's own 16:9 and the still
    // fills it instead of being sliced into a poster. The width is the row divided by
    // seven, plus the card's own inset on each side, so the first card starts where the
    // title starts and the last ends where the pill ends - and seven always fit.
    readonly property real colW: Math.max(120, (page.width - 2 * gutter
                                                + 2 * cardInset) / perRow - 0.01)
    // 16:9 of the *card*, not of the cell: the card sits `cardInset` in from each side of
    // its cell, and taking the ratio off the cell made the box 1.75:1 at 1100 px wide and
    // 1.87:1 at 1920 - the still stretched as the window grew, which is what reads as the
    // cards "not keeping their proportion" while dragging.
    readonly property real artH: (colW - 2 * cardInset) * 9 / 16

    function fmtDuration(sec) {
        const s = Math.round(sec)
        const pad = (n) => String(n).padStart(2, "0")
        const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), ss = s % 60
        return h > 0 ? h + ":" + pad(m) + ":" + pad(ss) : m + ":" + pad(ss)
    }

    // The card's right-click menu. One instance for the whole grid, so two cards cannot
    // be open at once; the action is applied to whichever card opened it. A card hidden
    // or renamed reloads the grid from C++ (Library::rowsChanged).
    // Built when the menu opens, not bound: the playlists can change while this page is
    // up, and a function call in a binding would answer with whatever the list was the
    // first time it was read.
    function cardItems() {
        const out = [
            { label: "打开文件所在文件夹", action: "reveal" },
            { label: "重命名",              action: "rename" },
            { label: "从媒体库移除",         action: "hide" }
        ];
        const ls = library.playlists();
        for (let i = 0; i < ls.length; ++i)
            out.push({ label: "加到「" + ls[i].name + "」", action: "pl:" + ls[i].id });
        // With no list to add to the row says so and stays grey, rather than vanishing:
        // the action exists, the target does not yet.
        if (ls.length === 0)
            out.push({ label: "加到播单（还没有播单）", action: "", on: false });
        return out;
    }

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
        font.pixelSize: shellHeight * 0.021
        font.family: Theme.uiFont
    }
    Text {
        id: yearMetric
        visible: false
        text: "M"
        font.pixelSize: shellHeight * 0.019
        font.family: Theme.uiFont
    }
    readonly property real textBlock: page.cardInset + titleMetric.height
                                       + yearMetric.height + page.cardInset
    // Which of the two things this page is: the local catalogue, or the online
    // wall. The caller owns it, the same way it owns the playlist dock's tab.
    property string mode: "local"

    // Passed straight up to Main.qml, which owns the one bubble.
    signal tipFor(var item, string text, string keys)
    signal tipOff(var item)
    // The folder picker lives in Main.qml; this page is a component, so it cannot reach
    // that id and hands the ask up instead. The 设置 card is reachable from here only
    // through the window's own gear, which is on screen everywhere.
    signal dirRequested()
    // Where a job from the wall or the 新建 dialog lands. Main.qml owns it (it is the
    // window that stores the choice), so it arrives as a plain value like every other
    // piece of chrome geometry on this page.
    property string saveDir: ""
    // A card's play, the tab row's 返回播放: both leave this page for the player, and
    // the page object cannot touch the window's `page` property itself.
    signal playerRequested()

    // The page's chrome line, at the left where the page title used to be: the tab pair
    // and the way back to the player. The library's own row sits under it.
    readonly property real rowY: page.gutter

    // The library's own row: the two actions, then what they last produced. Left aligned on
    // the page's gutter, because that is the slot the 在线媒体 tab uses for its source row.
    Row {
        id: libRow
        x: page.gutter
        y: page.rowY + height + page.gutter
        spacing: page.gap
        visible: page.mode === "local"
        readonly property real pillH: rescanText.height + shellHeight * 0.018

        Rectangle {
            id: pick
            width: pickText.implicitWidth + shellHeight * 0.028
            height: libRow.pillH
            radius: Theme.controlRadius
            color: pickOver.containsMouse ? "#2A2A2E" : "#1B1B1F"
            border.color: "#3A3A40"
            border.width: 1

            Text {
                id: pickText
                anchors.centerIn: parent
                text: "选择文件夹"
                color: Theme.textPrimary
                font.pixelSize: Theme.subtitleFontRatio * shellHeight
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

        Rectangle {
            id: rescan
            width: rescanText.implicitWidth + shellHeight * 0.028
            height: libRow.pillH
            radius: Theme.controlRadius
            color: over.containsMouse ? "#2A2A2E" : "#1B1B1F"
            border.color: "#3A3A40"
            border.width: 1

            Text {
                id: rescanText
                anchors.centerIn: parent
                text: "重新扫描"
                color: Theme.textPrimary
                font.pixelSize: Theme.subtitleFontRatio * shellHeight
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

        Text {
            id: stats
            anchors.verticalCenter: parent.verticalCenter
            text: library.mediaCount + " 部 · " + library.fileCount + " 个文件 · 时长 "
                  + library.durationCount + "/" + library.fileCount + " · 未解析 "
                  + library.unparsedCount + " · 上次扫描 " + library.lastScanMs + " ms"
            color: Theme.textSecondary
            font.pixelSize: Theme.subtitleFontRatio * shellHeight
            font.family: Theme.uiFont
        }
    }

    Component.onCompleted: {
        if (shellAutoPick)
            folderPick.open();
        // Opening the page from the player never scanned, so a catalogue that was
        // already in the database showed up as an empty grid.
        else if (library.lastScanMs === 0)
            library.scan();
        // A film can have been watched and closed since the grid was last read, and
        // that is the only thing that changes the "where you stopped" bar. One SELECT
        // with a limit on entry, so the card does not wait for a scan.
        else
            libraryModel.reload();
    }

    FolderDialog {
        id: folderPick
        title: "选择媒体目录"
        onAccepted: {
            library.addRoot(folderPick.selectedFolder)
            library.scan()
        }
    }

    // 本地 / 在线媒体: the chrome line opens at the page's gutter, where the page title used
    // to be. 返回播放 rides along at the right end of it - the library covers the whole
    // window and hides the transport row, so this page owns its own exit, and an empty
    // library has no card to click.
    Row {
        id: modeTabs
        x: page.gutter
        y: page.rowY
        height: libRow.pillH
        spacing: page.gap

        Repeater {
            // The download list is the third page, not a view inside the online one: the
            // tab sits in the chrome line with the other two, so a job is one click away
            // from anywhere. Its count is the row's own badge and only appears when there
            // is something to count.
            model: [{ key: "local", text: "本地媒体库" },
                    { key: "online", text: "在线媒体" },
                    { key: "jobs", text: jobsPage.jobs.length > 0
                                   ? "下载中 " + jobsPage.jobs.length : "下载中" }]
            delegate: Rectangle {
                required property var modelData
                width: mt.implicitWidth + shellHeight * 0.028
                height: parent.height
                radius: Theme.controlRadius
                color: page.mode === modelData.key ? Theme.accent
                       : (mth.containsMouse ? "#2A2A2E" : "#1B1B1F")
                border.color: "#3A3A40"
                border.width: 1
                Text {
                    id: mt
                    anchors.centerIn: parent
                    text: modelData.text
                    color: page.mode === modelData.key ? "#15151A" : Theme.textPrimary
                    font.pixelSize: Theme.subtitleFontRatio * shellHeight
                    font.family: Theme.uiFont
                }
                MouseArea {
                    id: mth
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.mode = modelData.key
                }
            }
        }

        Rectangle {
            id: backToPlayer
            width: backText.implicitWidth + shellHeight * 0.028
            height: modeTabs.height
            radius: Theme.controlRadius
            color: backOver.containsMouse ? "#2A2A2E" : "#1B1B1F"
            border.color: "#3A3A40"
            border.width: 1

            Text {
                id: backText
                anchors.centerIn: parent
                text: "返回播放"
                color: Theme.textPrimary
                font.pixelSize: Theme.subtitleFontRatio * shellHeight
                font.family: Theme.uiFont
            }
            MouseArea {
                id: backOver
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: page.playerRequested()
            }
        }
    }

    OnlinePage {
        x: 0
        // Up by its own pad, so the page's first row of pills lands on the same line the
        // library's row occupies: the two tabs share their second line instead of one of
        // them carrying an empty band.
        y: libRow.y - page.gutter
        width: page.width
        // Down to the window's own bottom: this page keeps its own gutter on all four
        // sides, and stopping where the grid stops would charge for it twice - the bottom
        // margin measured 26 px instead of 13.
        height: page.height - y
        visible: page.mode === "online"
        // The same gutter, so the posters and the pills above them start on one line
        // rather than on two ratios of two different heights.
        pad: page.gutter
        gap: page.gap
        saveDir: page.saveDir
        onTipFor: (item, text, keys) => page.tipFor(item, text, keys)
        onTipOff: (item) => page.tipOff(item)
        // A poster's menu joined a download; the list it lands in is the page beside this
        // one, so the host does the switching.
        onJobsRequested: page.mode = "jobs"
    }

    DownloadsPage {
        id: jobsPage
        x: 0
        // One gutter under the tab row. This page has no second row of its own - the
        // sources belong to the online page - so its list starts where the library's grid
        // would start, one gutter below the row above it.
        y: modeTabs.y + modeTabs.height + page.gutter
        width: page.width
        height: page.height - y - page.gutter
        visible: page.mode === "jobs"
        enabled: visible
        saveDir: page.saveDir
        onDirRequested: page.dirRequested()
        onPlayerRequested: page.playerRequested()
        onTipFor: (item, text, keys) => page.tipFor(item, text, keys)
        onTipOff: (item) => page.tipOff(item)
    }

    GridView {
        id: grid
        visible: page.mode === "local"
        enabled: visible
        // -4: a card's art is inset 4 px inside its cell, and it is the art's edges
        // that should line up with the two pill rows above, not the invisible cell.
        x: page.gutter - page.cardInset
        // The card's own inset comes back out of the margin, so what is 13 px from the row
        // above is the art's edge - the same reading the 在线媒体 tab gives between its two
        // pill rows and its first poster.
        y: libRow.y + libRow.height + page.gutter - page.cardInset
        width: page.width - 2 * (page.gutter - page.cardInset)
        height: page.height - y - page.gutter
        clip: true
        cellWidth: page.colW
        cellHeight: page.artH + page.textBlock
        boundsBehavior: Flickable.StopAtBounds
        model: libraryModel

        delegate: Item {
            id: cell
            required property string title
            required property int year
            required property string path
            required property bool parsed
            required property string kind
            required property double duration
            required property double pos
            required property string poster
            required property var mediaId
            // The rename is edited in place, on the card's own label - not in a dialog.
            property bool renaming: false
            // One "the pointer is on this card" for everything the card shows. It used to
            // also watch the corner ✕, because Qt hands a mouse-over to exactly one item
            // and hovering that ✕ took the card's own flag away - which hid the ✕, which
            // gave the flag back. The mark is gone, so there is nothing left to keep lit.
            readonly property bool hovered: mouse.containsMouse

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
                x: page.cardInset
                y: page.cardInset
                width: parent.width - 2 * page.cardInset
                height: page.artH
                radius: 4
                color: "#17171B"
                border.width: 1
                border.color: "#2C2C31"

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
                    text: duration > 0 ? page.fmtDuration(duration)
                                        + (cell.pos > 0 ? " · " + Math.round(cell.pos / duration * 100) + "%" : "")
                          : ""
                    color: Theme.textSecondary
                    font.pixelSize: page.height * 0.016
                    font.family: Theme.uiFont
                }

                // Where this one stopped, on the bottom edge of the still. Only a
                // film that is partway through gets one: at 0 or at the end there is
                // nothing to come back to, and a full-width bar would read as a
                // second frame around the card.
                Rectangle {
                    visible: cell.pos > 0 && duration > 0
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 3
                    color: Theme.track

                    Rectangle {
                        x: 0
                        y: 0
                        width: parent.width * Math.min(1, cell.pos / duration)
                        height: parent.height
                        color: Theme.accent
                    }
                }

                // The hover frame, three sides. The fourth would have lain on top of the
                // resume bar that lives on this card's bottom edge - two accent lines, one
                // above the other. A Rectangle can only border all four sides, so the ring
                // is drawn 4 px taller than the card and cut back to it: its bottom band
                // falls under the cut, and the top corners keep the card's own 4 px arc.
                Item {
                    anchors.fill: parent
                    visible: cell.hovered
                    clip: true
                    Rectangle {
                        width: parent.width
                        height: parent.height + 4
                        radius: 4
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.accent
                    }
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
                font.pixelSize: shellHeight * 0.021
                font.family: Theme.uiFont
            }
            // Year gets its own line: sharing one item let a long title push it out.
            Text {
                x: 4
                y: nameText.y + nameText.height + 1
                text: year > 0 ? String(year) : " "
                color: Theme.textSecondary
                font.pixelSize: shellHeight * 0.019
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
                        cardMenu.showAt(at.x, at.y, page.cardItems(), cell);
                        return;
                    }
                    playerCore.load(path); page.playerRequested();
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

    // The grid's bar, beside the grid rather than inside it: a Flickable adopts its
    // children into the scrolling content, and this must not scroll.
    ThinScrollBar {
        target: grid
        wantVisible: grid.visible
        x: 0 + grid.width - width + 2
        y: grid.y
        height: grid.height
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
            else if (action.indexOf("pl:") === 0)
                library.addToPlaylist(Number(action.slice(3)), [target.path]);
        }
    }
}
