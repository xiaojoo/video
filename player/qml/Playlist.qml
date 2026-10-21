import QtQuick
import QtQuick.Controls
import Playerui

// The playlist dock: two lists in the column the window grows to fit - the files in the
// same folder as what is playing, and the chapters of the file itself. There is no
// chapter drawer any more and no chapter button in the control row: one cell opens this
// column, and the two tabs inside it are the whole surface.
//
// The sizes are ratios of the launch height rather than the pixels the drawer hardcoded,
// because this column's width is a ratio too: at 4K the two must still agree.
Rectangle {
    id: pl

    // The launch height. The dock is the reason the window got wider, so it sizes
    // itself by the number the rest of the pinned chrome uses, never by the width it
    // just changed.
    property real refH: 723
    // What PlayerCore is holding. That is a file system path, not a file:// url -
    // `PlayerCore::load` folds the url down to a path and keys the strip cache, the
    // duration write-back and the catalogue on that one string - so the rows are
    // matched by their path and loaded by their url.
    property string mediaPath: ""
    // Library::itemsBeside, and the chapter edges and labels the header line and the
    // hover bubble already read. All handed over by the caller, so no two surfaces can
    // disagree about what this file has.
    property var items: []
    property var edges: []
    property var labels: []
    property int chapter: -1
    // Which list is showing: "files", "chapters" or "lists". The caller owns it, because
    // the cell and `P` only open and close the column.
    property string tab: "files"
    // ---- 我的播单 ----
    // library.playlists() and the entries of the one that is open, plus which one is
    // open (-1 for none) and which entry is currently playing. All read by the caller:
    // the dock draws what it is handed and reports what was clicked, so the database
    // stays behind one door.
    property var lists: []
    property var entries: []
    property int openList: -1
    property string queuePath: ""
    // Which delete is waiting for its second click. One number, because only one row can
    // be asking at a time; the rename is not, because it lives on its own row.
    property int pendingDelete: -1
    // The row whose name is being typed, as the row itself rather than as an id: the
    // answer has to survive the model being rebuilt under it.
    property Item editingRow: null
    // A press anywhere that is not the text field ends the edit. The field does not
    // reliably lose the active focus when another item takes the press, so waiting for
    // that left the row sitting in its box with no way out - the row's own click area is
    // off while it is being edited, so the very click meant to leave could not be heard.
    function endEdit() {
        editingRow = null
    }

    signal picked(string url)
    signal chapterPicked(int index)
    signal dismissed()
    signal tabPicked(string tab)
    signal createRequested()
    signal listClicked(int id)
    signal listAddRequested(int id)
    signal listRenamed(int id, string name)
    signal listDeleteRequested(int id)
    signal listDeleteConfirmed(int id)
    signal entryClicked(int id, string path, string url)
    signal entryRemoved(int id, string path)
    signal entryMoved(int id, string path, int dir)

    readonly property real pad: Math.round(0.0221 * refH)          // 16 px at 723
    readonly property real thumbW: Math.round(0.1328 * refH)        // 96
    readonly property real thumbH: Math.round(thumbW * 9 / 16)      // 54
    readonly property real rowH: thumbH + 2 * Math.round(0.0111 * refH)   // 54 + 16 = 70
    readonly property real chapRowH: Math.round(0.047 * refH)       // 34, the drawer's row
    readonly property real titleFs: Math.round(0.0194 * refH)       // 14
    readonly property real smallFs: Math.round(0.0180 * refH)       // 13
    readonly property real headFs: Math.round(0.0249 * refH)        // 18
    readonly property real tabH: Math.round(0.047 * refH)           // 34
    // Where the text column starts and how wide it is: the cover, then a gap.
    readonly property real textX: pad + thumbW + Math.round(0.0138 * refH)   // 16 + 96 + 10
    readonly property real textW: Math.max(40, width - textX - pad)
    // The chapter rows keep the drawer's column: a fixed time on the right, the name
    // eats the rest.
    readonly property real chapTimeW: Math.round(0.094 * refH)      // 68

    // A file the catalogue never probed has duration 0, and 0:00 would be a claim; the
    // row hides that column instead. Chapter one starts at 0 and has to read 0:00.
    function fmt(t) {
        if (!isFinite(t) || t < 0)
            return "";
        const s = Math.floor(t % 60), m = Math.floor(t / 60) % 60, h = Math.floor(t / 3600);
        const p = (v, n) => String(v).padStart(n, "0");
        return h > 0 ? h + ":" + p(m, 2) + ":" + p(s, 2) : p(m, 1) + ":" + p(s, 2);
    }
    // The library's own name for the file when the user typed one, the file name
    // otherwise - the same rule the player header follows, so the two surfaces cannot
    // call the same file two things.
    function label(it) {
        return it.title.length > 0 ? it.title : it.name;
    }
    function name(i) {
        return labels.length > i ? labels[i] : "";
    }

    readonly property real glyphS: Math.round(0.0194 * refH)        // 14 px row glyph
    readonly property real hitS: Math.round(0.0304 * refH)          // 22 px hit

    // The playlists and the entries of the open one, flattened into the rows of a single
    // list. One list, because two nested scrollers inside a 333 px column is how you get
    // a wheel that scrolls the wrong thing, and because the open playlist has to sit
    // between its neighbours the way the reference does.
    readonly property var listRows: {
        const out = []
        for (let i = 0; i < lists.length; ++i) {
            const l = lists[i]
            out.push({ kind: "head", id: l.id, name: l.name, count: l.count,
                       open: l.id === openList })
            if (l.id !== openList)
                continue
            for (let k = 0; k < entries.length; ++k) {
                const e = entries[k]
                out.push({ kind: "item", id: l.id, path: e.path, url: e.url,
                           label: label(e), duration: e.duration, poster: e.poster,
                           first: k === 0, last: k === entries.length - 1 })
            }
        }
        return out
    }

    width: Math.round(Theme.playlistWidthRatio * refH)
    height: parent ? parent.height : 0
    x: parent ? parent.width - width : 0
    // Opaque, flush, square: this is a column of the shell, not a card floating over
    // the film. The tone is the old drawer's, so the two read as one family.
    // Whether it is on screen at all is the caller's `plOpen`.
    color: "#FF0D0D10"

    Item {
        id: headRow
        x: 0
        y: Math.round(pl.refH * 0.0166)
        width: parent.width
        // A fixed band rather than `Math.max(headText.height, ...)`: a binding on an
        // object that reads its own children declared below it is the shape that has
        // silently never re-evaluated in this Qt.
        height: Math.round(pl.refH * 0.047)

        Text {
            id: headText
            x: pl.pad
            anchors.verticalCenter: parent.verticalCenter
            text: "播放列表"
            color: Theme.textPrimary
            font.pixelSize: pl.headFs
            font.family: Theme.uiFont
        }

        Text {
            id: closeGlyph
            anchors.right: parent.right
            anchors.rightMargin: Math.round(pl.refH * 0.0194)
            anchors.verticalCenter: parent.verticalCenter
            text: "✕"
            color: Theme.textSecondary
            font.pixelSize: Math.round(pl.refH * 0.0221)
            font.family: Theme.uiFont
            MouseArea {
                anchors.fill: parent
                anchors.margins: -6
                cursorShape: Qt.PointingHandCursor
                onClicked: pl.dismissed()
            }
        }
    }

    // Two lists one column apart, so the switch is a tab strip rather than two surfaces
    // stacked over each other: each label centred in its own half, the active one
    // underlined - the shape of the reference.
    Item {
        id: tabs
        x: 0
        y: headRow.y + headRow.height + Math.round(pl.refH * 0.0083)
        width: parent.width
        height: pl.tabH

        Repeater {
            model: [{ key: "files", text: "文件" }, { key: "chapters", text: "章节" },
                    { key: "lists", text: "我的播单" }]
            delegate: Item {
                required property var modelData
                required property int index
                readonly property bool active: pl.tab === modelData.key
                width: tabs.width / 3
                height: tabs.height
                x: index * width

                Text {
                    id: tabLabel
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.text
                    color: parent.active ? Theme.textPrimary : Theme.textSecondary
                    font.pixelSize: pl.titleFs
                    font.family: Theme.uiFont
                }
                Rectangle {
                    width: Math.round(tabLabel.implicitWidth * 0.65)
                    height: Math.max(2, Math.round(pl.refH * 0.0028))
                    radius: height / 2
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    color: Theme.accent
                    visible: parent.active
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: pl.tabPicked(modelData.key)
                }
            }
        }
    }

    // ---- the folder's files ----
    ListView {
        id: fileList
        x: 0
        y: tabs.y + tabs.height
        width: parent.width
        height: parent.height - y
        clip: true
        visible: pl.tab === "files"
        enabled: visible
        boundsBehavior: Flickable.StopAtBounds
        model: pl.items
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        // Both empty cases say what is missing rather than showing a blank column: a
        // file that is not from a folder (a disc), and a folder with nothing else.
        Text {
            visible: pl.items.length === 0
            x: pl.pad
            y: Math.round(pl.refH * 0.0249)
            width: fileList.width - 2 * pl.pad
            text: pl.mediaPath.length === 0 ? "没有正在播放的文件" : "这个文件夹里没有其他可播放的文件"
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            font.pixelSize: pl.smallFs
            font.family: Theme.uiFont
        }

        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property bool playing: modelData.path === pl.mediaPath
            width: fileList.width
            height: pl.rowH

            Rectangle {
                anchors.fill: parent
                color: rowHit.containsMouse ? "#22222A" : "transparent"
            }

            Rectangle {
                id: art
                x: pl.pad
                anchors.verticalCenter: parent.verticalCenter
                width: pl.thumbW
                height: pl.thumbH
                radius: 4
                color: "#17171B"
                // Two pixels inside: at one, the image's square corner still sticks out
                // of the 4 px arc, and a plain quad is beyond the reach of any radius.
                Image {
                    anchors.fill: parent
                    anchors.margins: 2
                    source: row.modelData.poster
                    fillMode: Image.PreserveAspectCrop
                    visible: status === Image.Ready
                }
            }

            Text {
                id: title
                x: pl.textX
                y: Math.round(pl.refH * 0.0111)
                width: pl.textW
                text: pl.label(row.modelData)
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                color: row.playing ? Theme.accent : Theme.textPrimary
                font.pixelSize: pl.titleFs
                font.family: Theme.uiFont
            }

            Text {
                id: nowPlaying
                visible: row.playing
                x: pl.textX
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Math.round(pl.refH * 0.0111)
                text: "正在播放"
                color: Theme.accent
                font.pixelSize: pl.smallFs
                font.family: Theme.uiFont
            }

            Text {
                x: nowPlaying.visible ? nowPlaying.x + nowPlaying.width
                                         + Math.round(pl.refH * 0.0111) : pl.textX
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Math.round(pl.refH * 0.0111)
                visible: row.modelData.duration > 0
                text: pl.fmt(row.modelData.duration)
                color: Theme.textSecondary
                font.pixelSize: pl.smallFs
                font.family: Theme.uiFont
            }

            MouseArea {
                id: rowHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: pl.picked(row.modelData.url)
            }
        }
    }

    // ---- this file's chapters ----
    ListView {
        id: chapList
        x: 0
        y: tabs.y + tabs.height
        width: parent.width
        height: parent.height - y
        clip: true
        visible: pl.tab === "chapters"
        enabled: visible
        boundsBehavior: Flickable.StopAtBounds
        model: pl.edges
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Text {
            visible: pl.edges.length === 0
            x: pl.pad
            y: Math.round(pl.refH * 0.0249)
            width: chapList.width - 2 * pl.pad
            text: "此文件没有章节"
            color: Theme.textSecondary
            font.pixelSize: pl.smallFs
            font.family: Theme.uiFont
        }

        delegate: Item {
            required property var modelData
            required property int index
            width: chapList.width
            height: pl.chapRowH

            Rectangle {
                anchors.fill: parent
                color: chapHit.containsMouse ? "#22222A" : "transparent"
            }
            Text {
                x: pl.pad
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 2 * pl.pad - pl.chapTimeW
                text: (index + 1) + ". " + pl.name(index)
                elide: Text.ElideRight
                color: pl.chapter === index ? Theme.accent : Theme.textPrimary
                font.pixelSize: pl.titleFs
                font.family: Theme.uiFont
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: pl.pad
                anchors.verticalCenter: parent.verticalCenter
                text: pl.fmt(modelData)
                color: Theme.textSecondary
                font.pixelSize: pl.smallFs
                font.family: Theme.uiFont
            }
            MouseArea {
                id: chapHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: pl.chapterPicked(index)
            }
        }
    }

    // One of the small marks a row shows while the pointer is on it. `live` greys a mark
    // that has nothing to do (the first row's up arrow) instead of hiding it, so the
    // marks that are live never slide sideways between rows.
    component RowGlyph: Item {
        id: glyph
        property string icon: ""
        property bool live: true
        // Told to the row by the catcher below, not read from this item's own mouse area.
        // The marks' visibility is what the row decides, and a hidden item cannot report
        // the pointer inside it - so a binding through here closed the loop Qt then
        // reported as "Binding loop detected for property visible", and moving onto a mark
        // hid it, which gave the hover back, which showed it again.
        property bool hot: false
        signal clicked
        width: pl.hitS
        height: pl.hitS
        Icon {
            anchors.centerIn: parent
            width: pl.glyphS
            height: pl.glyphS
            name: glyph.icon
            color: !glyph.live ? "#4A4A4E"
                               : glyph.hot ? Theme.textPrimary : Theme.textSecondary
        }
        MouseArea {
            anchors.fill: parent
            enabled: glyph.live
            cursorShape: Qt.PointingHandCursor
            onClicked: glyph.clicked()
        }
    }

    // ---- 我的播单 ----
    ListView {
        id: listList
        x: 0
        y: tabs.y + tabs.height
        width: parent.width
        height: parent.height - y - addRow.height - Math.round(pl.refH * 0.0111)
        clip: true
        visible: pl.tab === "lists"
        enabled: visible
        boundsBehavior: Flickable.StopAtBounds
        model: pl.listRows
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Text {
            visible: pl.lists.length === 0
            x: pl.pad
            y: Math.round(pl.refH * 0.0249)
            width: listList.width - 2 * pl.pad
            text: "还没有播单，点右下角新建一个"
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            font.pixelSize: pl.smallFs
            font.family: Theme.uiFont
        }

        delegate: Item {
            id: r
            required property var modelData
            width: listList.width
            height: pl.chapRowH
            readonly property bool head: modelData.kind === "head"
            // The glyphs and the row share one "is the pointer here": a mark that only
            // appears while the row is hovered must not vanish the moment the pointer
            // reaches it, which is the bug the media library card already taught.
            // Qt hands a mouse-over to exactly one item, so the marks' own strip is
            // watched as well - but by the always-there catcher beside them, not by the
            // marks themselves: their visibility is the thing being decided here, and
            // reading it back through them is the loop Qt reported on `visible`.
            readonly property bool shown: headHit.containsMouse || marksHover.containsMouse
            readonly property bool pending: pl.pendingDelete === modelData.id
            // Which row is being renamed is the dock's, but it names the row object rather
            // than an id from the model: the model is rebuilt the moment a name is
            // committed, and a binding that compared `modelData.id` did not answer the
            // write that ended the edit - the row stayed in its box.
            readonly property bool editing: pl.editingRow === r
            readonly property real countW: Math.round(pl.refH * 0.072)     // 共0个
            readonly property real indent: pl.pad + Math.round(pl.refH * 0.0166)
            // Select-all so the first typed character replaces the name, the way the
            // library card's rename does it.
            function beginRename() {
                // Written, not bound. A `text: modelData.name` binding keeps living
                // through what the user types - typing is not a QML write - so the next
                // refresh of the list replaced the half-typed name with the old one and
                // Enter committed that instead. Measured: typed FINAL, Enter, the row went
                // back to showing SUMMER and the database kept SUMMER.
                renameField.text = modelData.name
                renameField.forceActiveFocus()
                renameField.select(0, renameField.text.length)
            }
            // Covered both ways in: the row that starts the edit while it is already
            // built, and one that is built while the edit is already open (the model is
            // rebuilt under the list whenever any playlist changes).
            Component.onCompleted: if (editing) beginRename()

            Rectangle {
                anchors.fill: parent
                color: r.shown ? "#22222A" : "transparent"
            }

            // The disclosure mark: the same character turned a quarter way when the
            // playlist is open, which is cheaper than two glyphs that could disagree.
            Text {
                id: arrow
                visible: r.head
                x: pl.pad
                anchors.verticalCenter: parent.verticalCenter
                text: "›"
                rotation: modelData.open ? 90 : 0
                Behavior on rotation { NumberAnimation { duration: 110 } }
                color: Theme.textSecondary
                font.pixelSize: pl.titleFs
                font.family: Theme.uiFont
            }

            Text {
                id: label
                visible: !r.editing
                x: r.indent
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - r.indent - pl.pad - r.countW
                       - (r.shown && !r.pending ? 3 * pl.hitS : 0)
                text: r.pending ? "删除这个播单？" : (r.head ? modelData.name : modelData.label)
                elide: Text.ElideRight
                color: r.pending ? Theme.accent
                                 : (!r.head && modelData.path === pl.queuePath ? Theme.accent
                                                                                : Theme.textPrimary)
                font.pixelSize: pl.titleFs
                font.family: Theme.uiFont
            }

            // Renaming happens on the row itself, exactly like the library card: the
            // name is typed where it was read, no second window. Same two parts as there,
            // a Rectangle and a TextInput rather than a TextField: this app runs the
            // native style, which refuses a customised control background and said so
            // once per row ("The current style does not support customization of this
            // control"), leaving a system-white box in the middle of a dark panel.
            Rectangle {
                id: renameBox
                visible: r.editing
                x: r.indent - 4
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - r.indent - pl.pad
                height: pl.chapRowH - 6
                radius: 3
                color: "#22222A"
                border.width: renameField.activeFocus ? 1 : 0
                border.color: Theme.accent
                onVisibleChanged: if (visible) r.beginRename()

                TextInput {
                    id: renameField
                    // No `text` binding: see beginRename. The seed is written when the
                    // edit opens, and the row is empty for the moment between.
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: Theme.textPrimary
                    font.pixelSize: pl.titleFs
                    font.family: Theme.uiFont
                    selectionColor: Theme.accent
                    // Enter commits; Escape, a click away, and losing the field to the
                    // window's own buttons all abandon, which is what the card does. The
                    // guard stops a commit from a row that is no longer the one being
                    // edited - a recycled row can still hold this field.
                    onAccepted: {
                        if (pl.editingRow === r) {
                            pl.listRenamed(modelData.id, text)
                            pl.editingRow = null
                        }
                    }
                    Keys.onEscapePressed: pl.editingRow = null
                    onActiveFocusChanged: if (!activeFocus && pl.editingRow === r)
                        pl.editingRow = null
                }
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: pl.pad
                anchors.verticalCenter: parent.verticalCenter
                visible: !r.pending && !r.editing
                text: r.head ? "共" + modelData.count + "个" : pl.fmt(modelData.duration)
                color: Theme.textSecondary
                font.pixelSize: pl.smallFs
                font.family: Theme.uiFont
            }

            Row {
                id: glyphZone
                visible: r.shown && !r.pending && !r.editing
                anchors.right: parent.right
                anchors.rightMargin: pl.pad + r.countW
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                RowGlyph {
                    id: gA
                    icon: r.head ? "plus" : "up"
                    live: r.head || !modelData.first
                    // The catcher shares its left edge with this row, so its x is directly
                    // comparable with a mark's x inside the row.
                    hot: marksHover.containsMouse && marksHover.mouseX >= gA.x
                         && marksHover.mouseX < gA.x + gA.width
                    onClicked: r.head ? pl.listAddRequested(modelData.id)
                                       : pl.entryMoved(modelData.id, modelData.path, -1)
                }
                RowGlyph {
                    id: gB
                    icon: r.head ? "pencil" : "down"
                    live: r.head || !modelData.last
                    hot: marksHover.containsMouse && marksHover.mouseX >= gB.x
                         && marksHover.mouseX < gB.x + gB.width
                    onClicked: {
                        if (r.head) {
                            pl.editingRow = r
                            r.beginRename()
                        } else {
                            pl.entryMoved(modelData.id, modelData.path, 1)
                        }
                    }
                }
                RowGlyph {
                    id: gC
                    icon: "close"
                    hot: marksHover.containsMouse && marksHover.mouseX >= gC.x
                         && marksHover.mouseX < gC.x + gC.width
                    onClicked: r.head ? pl.listDeleteRequested(modelData.id)
                                       : pl.entryRemoved(modelData.id, modelData.path)
                }
            }

            // The marks' strip, watched by something that is always there. It sits over
            // the same rectangle as the row above and takes no button at all, so the
            // marks keep their clicks while this keeps the mouse-over - which is what
            // lets the row ask "is the pointer on my marks" without the answer depending
            // on whether the marks are currently drawn.
            MouseArea {
                id: marksHover
                anchors.right: parent.right
                anchors.rightMargin: pl.pad + r.countW
                anchors.verticalCenter: parent.verticalCenter
                width: 3 * pl.hitS
                height: pl.hitS
                acceptedButtons: Qt.NoButton
                hoverEnabled: true
            }

            // The two answers to the delete question, in the same place the glyphs were.
            Row {
                visible: r.pending
                anchors.right: parent.right
                anchors.rightMargin: pl.pad
                anchors.verticalCenter: parent.verticalCenter
                spacing: Math.round(pl.refH * 0.0111)
                Text {
                    text: "删除"
                    color: Theme.accent
                    font.pixelSize: pl.smallFs
                    font.family: Theme.uiFont
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -6
                        cursorShape: Qt.PointingHandCursor
                        onClicked: pl.listDeleteConfirmed(modelData.id)
                    }
                }
                Text {
                    text: "取消"
                    color: Theme.textSecondary
                    font.pixelSize: pl.smallFs
                    font.family: Theme.uiFont
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -6
                        cursorShape: Qt.PointingHandCursor
                        onClicked: pl.pendingDelete = -1
                    }
                }
            }

            MouseArea {
                id: headHit
                // The bottom-most sibling, not the last one declared. This fills the
                // whole row, so left on top of its siblings it took every press that
                // belonged to the marks beside it or to the rename box - measured: a
                // press on the up-arrow played the row instead, and the row order never
                // changed. The marks and the box are what the pointer is offered, so they
                // answer first and this only gets what they do not take.
                z: -1
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                hoverEnabled: true
                enabled: !r.editing
                cursorShape: Qt.PointingHandCursor
                // Pressing any other row leaves the rename, whether or not that press
                // then opens or plays the row it landed on.
                onPressed: pl.endEdit()
                onClicked: {
                    if (r.head)
                        pl.listClicked(modelData.id)
                    else
                        pl.entryClicked(modelData.id, modelData.path, modelData.url)
                }
            }
        }
    }

    // The blank strip under the rows. The list's own flickable takes a press there, so
    // the dock's bottom-most catcher never hears it - measured: clicking below the last
    // row was the one "somewhere else" that left the rename open. This covers exactly the
    // empty part of the viewport and nothing else, so no row loses its own click.
    MouseArea {
        x: listList.x
        y: listList.y + Math.min(listList.contentHeight, listList.height)
        width: listList.width
        height: Math.max(0, listList.height - listList.contentHeight)
        visible: pl.tab === "lists" && height > 0
        enabled: visible
        onPressed: pl.endEdit()
    }

    // The floating mark from the reference: bottom-right, and the only way in to a list
    // that does not exist yet.
    Item {
        id: addRow
        anchors.right: parent.right
        anchors.rightMargin: pl.pad
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.round(pl.refH * 0.0221)
        width: Math.round(pl.refH * 0.0664)        // 48 px plate
        height: width
        visible: pl.tab === "lists"

        Rectangle {
            anchors.fill: parent
            radius: 8
            color: addHit.containsMouse ? "#2A2A31" : "#1F1F25"
            border.width: 1
            border.color: "#3A3A40"
        }
        Icon {
            anchors.centerIn: parent
            // The plate's own centre is a quarter of it above the dock's bottom margin,
            // so the mark sits on that rather than on the row it is drawn in.
            width: Math.round(pl.refH * 0.0304)
            height: width
            name: "playlist"
            color: addHit.containsMouse ? Theme.textPrimary : Theme.glyph
        }
        MouseArea {
            id: addHit
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: pl.createRequested()
        }
    }

    // Right-clicks that land here must not fall through to the film's context menu,
    // which is a window-wide layer under this one. Declared last with a negative z so
    // the rows keep their own clicks; this only catches what they do not take.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        z: -1
        // What the rows did not take - the empty column below them, or the body of the
        // row being renamed, whose own click area is off while it is being edited - is a
        // click elsewhere too, so it ends the rename.
        onPressed: pl.endEdit()
    }
}
