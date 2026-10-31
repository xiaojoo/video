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
    readonly property real gutter: Math.round(Theme.refW * Theme.sideInsetWRatio)
    readonly property real gap: Math.round(Theme.refH * Theme.controlGapRatio)
    // Half the gap per side, so two neighbours sit one gap apart and the outermost cards
    // still land on the gutter itself.
    readonly property real cardInset: Math.round(gap / 2)
    // The span the grid is laid out in, and the one card size that gets repeated across it.
    readonly property real gridW: page.width - 2 * (gutter - cardInset)
    // A card is a frame off the film, so the box keeps the frame's own 16:9 and the still
    // fills it instead of being sliced into a poster. 194 px is the *minimum* card - the same
    // box the online wall's posters use - and the window decides how many of them fit. What
    // is left over at the end of a row is by definition not enough for one more card, so the
    // cards now eat it instead of leaving an empty band: the row is divided by the column
    // count that minimum produced. That bounds the growth at one card's worth - a six-column
    // row gains at most (194+18)/6 = 35 px per card - and it falls back to exactly 194 the
    // moment the next column fits. Dividing the row by a *fixed* column count is still wrong:
    // that made every card grow with the window (194 px at 1282, 387 px at 2400).
    readonly property real artW: 194
    readonly property real colW: artW + 2 * cardInset
    // Whole cards only: the row is divided by this count, so a column is counted when its
    // own trailing inset is paid for (see the online wall's `columns`).
    readonly property int perRow: Math.max(1, Math.floor(gridW / colW))
    readonly property real cellW: Math.floor(gridW / perRow)
    // 16:9 of the *card*, not of the cell: the card sits `cardInset` in from each side of
    // its cell, and taking the ratio off the cell made the box 1.75:1 at 1100 px wide and
    // 1.87:1 at 1920 - the still stretched as the window grew, which is what reads as the
    // cards "not keeping their proportion" while dragging. Rounded to whole pixels: 194's
    // exact 9/16 is 109.125, and a fractional cell height puts every row on a quarter
    // pixel, which resamples the title under it.
    readonly property real artH: Math.round((cellW - 2 * cardInset) * 9 / 16)

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

    // The card under a point in page coordinates, asked to open its menu. A right-click
    // while a menu is already up means that menu is on the wrong card: it is closed first,
    // then the card under the pointer takes its place. itemAt() wants content coordinates,
    // so the grid's own position and scroll come back out of the point.
    function openCardMenuAt(px, py) {
        const item = grid.itemAt(px - grid.x + grid.contentX, py - grid.y + grid.contentY)
        if (item)
            item.menuAt(px, py)
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
        font.pixelSize: Math.round(Theme.refH * 0.021)
        font.family: Theme.uiFont
    }
    Text {
        id: yearMetric
        visible: false
        text: "M"
        font.pixelSize: Math.round(Theme.refH * 0.019)
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
    // Which surface the online page's magnet board uses, and the ask to make the window
    // give a column of its own to it. Both belong to the host: one is the stored setting,
    // the other is the window's rect.
    property bool magnetPanel: false
    property bool magnetRight: false
    signal magnetDockAsked(bool on)
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
        readonly property real pillH: rescanText.height + Theme.refH * 0.018

        Rectangle {
            id: pick
            width: pickText.implicitWidth + Theme.refH * 0.028
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
                font.pixelSize: Math.round(Theme.subtitleFontRatio * Theme.refH)
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
            width: rescanText.implicitWidth + Theme.refH * 0.028
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
                font.pixelSize: Math.round(Theme.subtitleFontRatio * Theme.refH)
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
            font.pixelSize: Math.round(Theme.subtitleFontRatio * Theme.refH)
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
                width: mt.implicitWidth + Theme.refH * 0.028
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
                    font.pixelSize: Math.round(Theme.subtitleFontRatio * Theme.refH)
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
            width: backText.implicitWidth + Theme.refH * 0.028
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
                font.pixelSize: Math.round(Theme.subtitleFontRatio * Theme.refH)
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
        magnetPanel: page.magnetPanel
        magnetRight: page.magnetRight
        onMagnetDockAsked: (on) => page.magnetDockAsked(on)
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
        width: page.gridW
        height: page.height - y - page.gutter
        clip: true
        cellWidth: page.cellW
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
            // Opens the card's menu at a point in page coordinates. A function rather than
            // the code inline in the mouse handler, because the page has to open the menu
            // for the card *under the pointer* when a right-click lands while another
            // card's menu is up.
            function menuAt(px, py) {
                cardMenu.showAt(px, py, page.cardItems(), cell)
            }
            width: grid.cellWidth
            height: grid.cellHeight

            // The card's chrome, taken from the reference he sent: a chamfered luminous
            // frame, the still set into it, three cut bars at the top-right corner, and a
            // black band along the bottom carrying the progress and the times. The
            // reference's two cards differ in nothing but the frame colour - one blue, one
            // amber - so blue is the resting state and the app's accent is the hover.
            Item {
                id: art
                x: page.cardInset
                y: page.cardInset
                width: parent.width - 2 * page.cardInset
                height: page.artH
                clip: true

                // Measured on the reference and cut down to this card's 194 px: its frame
                // clips the corner at 26 px of 741 (0.035), its still at 68 (0.092), and
                // the black gap between the line and the still is 4-5 px (0.006 - one and
                // a half pixels here, so the gap is 2).
                readonly property real chamfer: 7
                readonly property real cut: 17
                // The line plus that gap: the still's window starts this far inside.
                readonly property real inset: 2
                // The band holding the progress bar and the two times. The reference's
                // strip is 45 px of 363 (0.124); 16 keeps a 10 px line of type inside it.
                readonly property real bandH: 16
                // The line itself is a tinted near-white on both reference cards - (212,
                // 242, 255) on the blue one, (233, 215, 189) on the amber - and the card's
                // colour lives in the corner bars and the percentage, not in the line.
                readonly property color stroke: cell.hovered ? "#F0DCC0" : "#D2ECFF"
                readonly property color ink: cell.hovered ? Theme.accent : Theme.cool
                readonly property color plate: "#0E0E11"

                Rectangle {
                    anchors.fill: art
                    color: art.plate
                }

                // The first frame, cropped to the 190 x 105 window it is given. Its four
                // square corners are covered by the canvas above, which is what turns
                // them into the reference's diagonals.
                Image {
                    id: cover
                    x: art.inset
                    y: art.inset
                    width: art.width - 2 * art.inset
                    height: art.height - 2 * art.inset
                    source: poster
                    fillMode: Image.PreserveAspectCrop
                    visible: status === Image.Ready
                }

                // Everything the reference draws *over* the picture - the four corner
                // cuts, the band, the line and the three bars - in one painter, so the
                // corners cannot get out of step with the line that joins them.
                Canvas {
                    id: chrome
                    readonly property color stroke: art.stroke
                    readonly property color ink: art.ink
                    anchors.fill: art
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onStrokeChanged: requestPaint()
                    onInkChanged: requestPaint()
                    onPaint: {
                        const g = getContext("2d");
                        g.reset();
                        const w = width, h = height;
                        const c = art.chamfer, cut = art.cut, d = art.inset;

                        g.fillStyle = art.plate;
                        g.fillRect(d, h - d - art.bandH, w - 2 * d, art.bandH);

                        // Four triangles in the plate's colour: the still's cut corner and
                        // the black gap between it and the frame line.
                        g.beginPath();
                        g.moveTo(0, 0); g.lineTo(cut, 0); g.lineTo(0, cut); g.closePath();
                        g.moveTo(w, 0); g.lineTo(w - cut, 0); g.lineTo(w, cut); g.closePath();
                        g.moveTo(0, h); g.lineTo(cut, h); g.lineTo(0, h - cut); g.closePath();
                        g.moveTo(w, h); g.lineTo(w - cut, h); g.lineTo(w, h - cut); g.closePath();
                        g.fill();

                        // The frame: a 3 px halo of the card's colour, then the near-white
                        // line over it. One stroke of each, so the two cannot drift apart;
                        // the reference's line is luminous with no hard outer edge.
                        const o = 0.5;
                        g.lineWidth = 1;
                        g.beginPath();
                        g.moveTo(c, o);
                        g.lineTo(w - c, o);
                        g.lineTo(w - o, c);
                        g.lineTo(w - o, h - c);
                        g.lineTo(w - c, h - o);
                        g.lineTo(c, h - o);
                        g.lineTo(o, h - c);
                        g.lineTo(o, c);
                        g.closePath();
                        const k = art.ink;
                        g.strokeStyle = "rgba(" + Math.round(k.r * 255) + ","
                                        + Math.round(k.g * 255) + ","
                                        + Math.round(k.b * 255) + ",0.22)";
                        g.lineWidth = 3;
                        g.stroke();
                        g.strokeStyle = chrome.stroke;
                        g.lineWidth = 1;
                        g.stroke();

                        // Three cut bars at the top-right, the reference's one ornament:
                        // small parallelograms leaning with the corner, straddling the
                        // still's cut edge exactly as they do there.
                        g.fillStyle = k;
                        for (let i = 0; i < 3; ++i) {
                            const x = w - 22 + i * 5;
                            g.beginPath();
                            g.moveTo(x, 8); g.lineTo(x + 3, 8);
                            g.lineTo(x + 5, 3); g.lineTo(x + 2, 3);
                            g.closePath(); g.fill();
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    text: kind === "audio" ? "♪" : ""
                    color: "#3A3A40"
                    font.pixelSize: parent.height * 0.3
                    visible: text.length > 0
                }

                // The reference's bottom strip: a progress bar and the two times, on the
                // plate's own colour. Solid rather than a scrim, so a white first frame
                // cannot lift the time to 1.9:1 the way the old gradient let it.
                Item {
                    id: band
                    x: art.inset
                    y: art.height - art.inset - art.bandH
                    width: art.width - 2 * art.inset
                    height: art.bandH

                    // The reference's bar is 12 px of 363 tall (0.033), filled to the
                    // percentage; the unfilled rest is (42,45,51). It stops where the
                    // duration starts, not at the card's edge.
                    Rectangle {
                        id: track
                        visible: cell.pos > 0 && duration > 0
                        x: 6
                        y: 7
                        width: Math.max(0, times.x - 12)
                        height: 3
                        radius: height / 2
                        color: "#2E3138"

                        Rectangle {
                            width: parent.width * Math.min(1, cell.pos / duration)
                            height: parent.height
                            radius: parent.radius
                            color: art.ink
                        }
                    }

                    Row {
                        id: times
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 0

                        // Duration is grey on both reference cards, (182,189,200); only
                        // the percentage takes the card's colour.
                        Text {
                            text: duration > 0 ? page.fmtDuration(duration) : ""
                            color: "#B6BDC8"
                            font.pixelSize: Math.round(Theme.refH * 0.014)
                            font.family: Theme.uiFont
                        }
                        Text {
                            text: duration > 0 && cell.pos > 0
                                  ? " · " + Math.round(cell.pos / duration * 100) + "%" : ""
                            color: art.ink
                            font.pixelSize: Math.round(Theme.refH * 0.014)
                            font.family: Theme.uiFont
                        }
                    }
                }

                Text {
                    anchors.verticalCenter: band.verticalCenter
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: parsed ? "" : "未识别"
                    color: Theme.accent
                    font.pixelSize: Math.round(Theme.refH * 0.018)
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
                font.pixelSize: Math.round(Theme.refH * 0.021)
                font.family: Theme.uiFont
            }
            // Year gets its own line: sharing one item let a long title push it out.
            Text {
                x: 4
                y: nameText.y + nameText.height + 1
                text: year > 0 ? String(year) : " "
                color: Theme.textSecondary
                font.pixelSize: Math.round(Theme.refH * 0.019)
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
                        cell.menuAt(at.x, at.y);
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
        // A right-click somewhere else while a menu is open means the menu is on the
        // wrong card, so both buttons have to reach this layer: the left one only
        // dismisses, the right one dismisses and re-opens on what it landed on.
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (m) => {
            const right = m.button === Qt.RightButton
            // This layer is the page, so its own coordinates are the page's.
            const px = m.x, py = m.y
            cardMenu.hide()
            if (right)
                page.openCardMenuAt(px, py)
        }
    }

    ContextMenu {
        id: cardMenu
        z: 20
        space: page
        refH: Theme.refH
        // The one menu stands for whichever card was hit, so a right-click on the menu
        // itself - it can lie over a neighbouring card - moves it to that card rather
        // than leaving it turned towards the one that is gone.
        retarget: true
        onRightPressed: (x, y) => {
            cardMenu.hide()
            page.openCardMenuAt(x, y)
        }
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
