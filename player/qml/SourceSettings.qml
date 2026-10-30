import QtQuick
import QtQuick.Controls
import Playerui

// The window's 设置 door. It lives in Main.qml rather than on a page because the door is
// in the window's caption, which is on screen everywhere - a player page with no sources
// still has to be able to open it.
//
// It is the one surface in the app laid out as a settings sheet rather than a page, but
// it is coloured like everything else: the palette below is made of the same charcoal
// literals the 新建下载 dialog, the context menu and the download rows already use.
Popup {
    id: sheet
    modal: true
    // No dim behind the card: the window underneath stays readable, and the card is a
    // panel that belongs to that page rather than a modal step.
    Overlay.modal: Item { }
    // Centred, but on whole pixels. `anchors.centerIn` left this card on a half-pixel y at
    // the launch size, and every horizontal hairline in it - the card's own edge, the two
    // section cards, the source rows, the fields - then straddled two rows and came out at
    // half the strength of the vertical edges: the four sides of one border were not the
    // same line. `Overlay.overlay` is null until the popup has an overlay to sit in, which
    // is also why the window's width is handed in rather than read from it.
    x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
    y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
    padding: 0
    background: null
    width: card.width
    height: card.height
    closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
    // What the card's width is a fraction of. Main.qml hands over the window's own width;
    // the fallback is the launch size. It is not read off Overlay.overlay: that is null
    // while this is being built, and the binding threw there ("Cannot read property
    // 'width' of null") before the window had drawn anything at all.
    property real refWidth: shellWidth
    // Where new downloads land; the 下载任务 section shows it and can change it.
    property string dlDir: ""
    // "sources" or "downloads". The gear keeps the section it was last left on; the
    // download toolbar's panel mark opens straight onto 下载.
    property string tab: "sources"

    function openAt(what) {
        sheet.tab = what;
        sheet.open();
    }

    // The folder picker is the window's (Main.qml owns it, and so does the library's own
    // 选择目录); this card only asks for it.
    signal dirRequested()

    Rectangle {
        id: card
        width: Math.round(sheet.refWidth * 0.58)
        // Tall enough for whichever section is up, and never shorter than 0.46 of the
        // window: the two sections are a header, one card and a list, and at the launch
        // size that is ~320 px against the 206 px the old sheet was - the ask was "加高一点".
        // The mock's own panel is 580 tall at 1427 wide (0.41, i.e. ~303 here); this keeps
        // the 333 and spends the difference on the empty tail the mock also has.
        height: Math.min(card.capH, Math.max(card.minH,
                                            Math.max(card.srcContentH, card.dlContentH)))
        radius: 5
        color: card.bg
        border.width: 1
        border.color: card.line
        clip: true
        focus: true

        // ---- palette ----
        // Every value here is one the rest of the app already spends, so the sheet is the
        // same charcoal as the 新建下载 dialog, the context menu and the download rows:
        // panel #141418 (AddLinksDialog), plates #1B1B1F and fields one step under them,
        // the picked nav row #3A2A0E (the accent-tinted fill the dialog uses), hovers
        // #22222A / #2A2A2E / #2A2A31, dim text #4A4A4E. The two steps that carry the
        // depth - plate over panel and field under it - are both 11/255, because the
        // mock's own 3-level step vanished into this palette's black floor.
        //
        // One edge for the whole sheet: the card, the two section cards, the source rows,
        // every field and every button are all `line`, so "the outline is the same
        // everywhere" is one value rather than a family of near-greys.
        readonly property color bg: "#141418"          // what every dialog in the app is
        readonly property color plate: "#1B1B1F"       // cards, bg +11
        readonly property color line: "#3A3A40"        // the only edge in the sheet
        readonly property color fill: "#141418"        // fields, plate -11
        readonly property color textHi: Theme.glyph              // headings, row names
        readonly property color textMid: Theme.textSecondary     // button labels
        readonly property color textDim: "#B3B8B8B8"    // subtitles, field labels (70%)
        readonly property color textFaint: "#8CB8B8B8"  // urls, placeholders (55%)
        readonly property color navIdle: Theme.textSecondary
        readonly property color navSel: "#3A2A0E"       // the app's accent-tinted fill
        readonly property color navHover: "#22222A"     // the context menu's hovered row
        readonly property color btnHover: "#2A2A2E"     // plate +15, as on the library buttons
        readonly property color accentHover: "#3A2A0E"  // the dialog's own accent fill
        readonly property color boxHover: "#2A2A31"     // the ✕, as in 新建下载
        readonly property color boxHoverLine: "#4A4A4E"
        readonly property color swOff: "#141418"
        readonly property color swOffKnob: "#4A4A4E"

        // ---- the sheet's grid, taken off his mock ----
        // The mock is a 1478x647 render of a 1427x581 panel, and this sheet is 744 wide at
        // the launch size - half - so the mock's 57 px fields are 30 here, its 114 px row
        // cards 59, and its buttons 62x29 rather than the 75x38 the first pass drew.
        readonly property real pad: 15
        readonly property real navTop: 8
        readonly property real navW: Math.round(card.width * 0.210)          // 156
        readonly property real navH: 38
        readonly property real headH: 41
        readonly property real headIcon: 26
        // The section's heading and the nav row that names it are one size on purpose -
        // the nav says which page is open, so it is not a caption of the heading. 13 read
        // as too small against the sheet and 16 as too big; this is the one between.
        readonly property real headFs: 14
        readonly property real smallFs: 13
        readonly property real bodyFs: 12
        readonly property real labelFs: 12
        readonly property real fieldH: 30
        readonly property real rowH: 59
        readonly property real rowGap: 13
        readonly property real boxPad: 15
        readonly property real cardGap: 13
        readonly property real paneGap: 12
        // The page starts one gap to the right of the nav column, and that gap is the same
        // 13 the column is inset from the sheet's own left edge.
        readonly property real paneX: card.navW + 13                         // 169
        readonly property real paneW: card.width - card.paneX - card.pad     // 560
        // Both section cards are now one field line and nothing else - the titled line and
        // the label line are gone, so the placeholders say what each box takes - and being
        // the same sum they are one number.
        readonly property real blockH: boxPad + fieldH + boxPad              // 60
        // The unfolded source row: two fields and the 保存/取消 line, with its padding.
        readonly property real editRowH: 2 * 12 + 3 * card.fieldH + 2 * 8
        readonly property real minH: Math.round(shellHeight * 0.46)
        readonly property real capH: Math.round(shellHeight * 0.80)

        // Rows fold at six and the list scrolls; the unfolded row is as tall as it is.
        readonly property int shownRows: Math.min(srcList.count, 6)
        readonly property real rowsFull: card.shownRows <= 0 ? 0
            : (srcPane.editingRow >= 0
               ? (card.shownRows - 1) * card.rowH + (card.shownRows - 1) * card.rowGap
                 + card.editRowH
               : card.shownRows * card.rowH + (card.shownRows - 1) * card.rowGap)
        // What is left for the list inside a sheet that has to stay on screen.
        readonly property real listRoom: card.capH - 2 * card.pad - card.headH
                                         - card.paneGap - card.blockH - card.cardGap
        readonly property real listH: Math.min(card.rowsFull, card.listRoom)
        readonly property real srcContentH: 2 * card.pad + card.headH + card.paneGap
                                            + card.blockH
                                            + (srcList.count > 0 ? card.cardGap + card.listH : 0)
        readonly property real dlContentH: 2 * card.pad + card.headH + card.paneGap
                                           + card.blockH

        // The card swallows presses on its own body: a Rectangle passes them through to
        // whatever is behind the popup, and a press there used to open a poster's magnets.
        // It also takes the caret, so a field does not keep its accent border after a
        // press somewhere else in the box.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: card.forceActiveFocus()
        }

        // ---- the sections ----
        // The nav column is not a plate of its own, has no rule beside it and no frame
        // around it: the panel is one surface top to bottom, and the picked row is the
        // fill under its own glyph and label. It is inset 13 on the left so that fill does
        // not run into the sheet's own edge, and flush to the column's right, which is what
        // leaves the seam to the page at 20 rather than 33.
        Column {
            id: nav
            x: 13
            y: card.navTop
            width: card.navW - 12
            // A gap between the two rows: with none, the picked row's orange fill ran
            // into the idle row's plate and the pair read as one block.
            spacing: 8

            Repeater {
                model: [
                    { key: "downloads", text: "下载任务", icon: "folder" },
                    { key: "sources", text: "在线源", icon: "cloudDown" }
                ]
                delegate: Rectangle {
                    id: navItem
                    required property var modelData
                    width: nav.width
                    height: card.navH
                    radius: 3
                    readonly property bool cur: sheet.tab === modelData.key
                    color: navItem.cur ? card.navSel : (navHit.containsMouse ? card.navHover : "transparent")

                    // The picked row is the fill and the orange glyph and label; the bar on
                    // the panel's edge and the chevron on its right are gone.
                    Icon {
                        x: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20
                        height: 20
                        name: navItem.modelData.icon
                        color: navItem.cur ? Theme.accent : card.navIdle
                    }

                    Text {
                        x: 40
                        anchors.verticalCenter: parent.verticalCenter
                        text: navItem.modelData.text
                        color: navItem.cur ? Theme.accent : card.navIdle
                        // The same size as the page title on the right, not one step
                        // under it: the two say the same thing - which section is open.
                        font.pixelSize: card.headFs
                        font.family: Theme.uiFont
                        // Colour only marks the picked row. DemiBold made the label grow
                        // a shade wider the moment it was clicked.
                        font.weight: Font.Normal
                    }

                    MouseArea {
                        id: navHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: sheet.tab = navItem.modelData.key
                    }
                }
            }
        }

        // ---- the page's own header: glyph, title, one line of what this page is ----
        Item {
            id: head
            x: card.paneX
            y: card.pad
            width: card.paneW
            height: card.headH

            Icon {
                id: headIcon
                x: 0
                y: 2
                width: card.headIcon
                height: card.headIcon
                name: sheet.tab === "sources" ? "cloudDown" : "folder"
                color: Theme.accent
                weight: 1.7
            }

            Text {
                anchors.left: headIcon.right
                anchors.leftMargin: 10
                anchors.verticalCenter: headIcon.verticalCenter
                text: sheet.tab === "sources" ? "在线源" : "下载任务"
                color: card.textHi
                font.pixelSize: card.headFs
                font.family: Theme.uiFont
                font.weight: Font.DemiBold
            }

            Text {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.rightMargin: 48
                anchors.top: headIcon.bottom
                anchors.topMargin: 2
                text: sheet.tab === "sources"
                      ? "添加在线媒体源，支持多种视频解析网站，方便您下载喜欢的内容。"
                      : "新任务默认存到这里，已经在下载的任务不会搬走。"
                color: card.textDim
                font.pixelSize: card.bodyFs
                font.family: Theme.uiFont
                elide: Text.ElideRight
            }
        }

        // The way out, in the corner: the popup covers the gear that opened it.
        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            width: 22
            height: 22
            radius: 4
            color: scHit.containsMouse ? card.boxHover : "transparent"
            border.width: 1
            border.color: scHit.containsMouse ? card.boxHoverLine : card.line

            Icon {
                anchors.centerIn: parent
                width: 10
                height: 10
                name: "close"
                color: card.textMid
                weight: 1.5
            }
            MouseArea {
                id: scHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: sheet.close()
            }
        }

        // ---- 下载任务 ----
        Rectangle {
            id: dlCard
            visible: sheet.tab === "downloads"
            x: card.paneX
            y: card.pad + card.headH + card.paneGap
            width: card.paneW
            height: card.blockH
            radius: 4
            color: card.plate
            border.width: 1
            border.color: card.line

            readonly property real btnW: Math.round(shellHeight * 0.135)

            Column {
                x: card.boxPad
                y: card.boxPad
                width: parent.width - 2 * card.boxPad
                spacing: 0

                Row {
                    width: parent.width
                    height: card.fieldH
                    spacing: 10

                    Rectangle {
                        width: parent.width - dlCard.btnW * 2 - 20
                        height: parent.height
                        radius: 3
                        color: card.fill
                        border.width: 1
                        border.color: card.line

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: sheet.dlDir.length > 0 ? sheet.dlDir : "…"
                            elide: Text.ElideMiddle
                            color: card.textMid
                            font.pixelSize: card.bodyFs
                            font.family: Theme.uiFont
                        }
                    }

                    Rectangle {
                        id: pick
                        width: dlCard.btnW
                        height: parent.height
                        radius: 3
                        color: pickHit.containsMouse ? card.accentHover : "transparent"
                        border.width: 1
                        border.color: pickHit.containsMouse ? "#FFA733" : Theme.accent

                        Text {
                            anchors.centerIn: parent
                            text: "选择文件夹"
                            color: Theme.accent
                            font.pixelSize: card.bodyFs
                            font.family: Theme.uiFont
                        }
                        MouseArea {
                            id: pickHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: sheet.dirRequested()
                        }
                    }

                    Rectangle {
                        id: openPick
                        width: dlCard.btnW
                        height: parent.height
                        radius: 3
                        color: openHit.containsMouse ? card.btnHover : "transparent"
                        border.width: 1
                        border.color: card.line

                        Text {
                            anchors.centerIn: parent
                            text: "打开"
                            color: card.textMid
                            font.pixelSize: card.bodyFs
                            font.family: Theme.uiFont
                        }
                        MouseArea {
                            id: openHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (sheet.dlDir.length > 0)
                                           Qt.openUrlExternally("file:///"
                                               + sheet.dlDir.replace(/\\/g, "/"))
                        }
                    }
                }
            }
        }

        // ---- 在线源 ----
        Rectangle {
            id: addCard
            visible: sheet.tab === "sources"
            x: card.paneX
            y: card.pad + card.headH + card.paneGap
            width: card.paneW
            height: card.blockH
            radius: 4
            color: card.plate
            border.width: 1
            border.color: card.line

            readonly property real innerW: width - 2 * card.boxPad
            readonly property real btnW: Math.round(shellHeight * 0.115)                 // 83
            readonly property real nameW: Math.round((innerW - btnW - 28) * 0.512)       // 211
            readonly property real urlW: innerW - btnW - 28 - nameW                      // 201

            Column {
                x: card.boxPad
                y: card.boxPad
                width: addCard.innerW
                spacing: 0

                Row {
                    width: parent.width
                    height: card.fieldH
                    spacing: 14

                    MyField {
                        id: nameField
                        width: addCard.nameW
                        height: parent.height
                        fontSize: card.bodyFs
                        placeholder: "请输入源名称，如：sidhub.cc"
                        fill: card.fill
                        edgeColor: card.line
                        hintColor: card.textFaint
                        corner: 3
                        inset: 14
                    }
                    MyField {
                        id: urlField
                        width: addCard.urlW
                        height: parent.height
                        fontSize: card.bodyFs
                        placeholder: "https://"
                        fill: card.fill
                        edgeColor: card.line
                        hintColor: card.textFaint
                        corner: 3
                        inset: 14
                        onCommitted: srcAdd.doAdd()
                    }
                    Rectangle {
                        id: srcAdd
                        width: addCard.btnW
                        height: parent.height
                        radius: 3
                        color: sh2.containsMouse ? card.accentHover : "transparent"
                        border.width: 1
                        border.color: sh2.containsMouse ? "#FFA733" : Theme.accent

                        function doAdd() {
                            const u = urlField.text.trim();
                            if (u.length === 0)
                                return;
                            online.addSource(nameField.text, u);
                            nameField.text = "";
                            urlField.text = "";
                            // Adding a source and then still having to click it in the menu
                            // row to see anything is one step too many, and the settings box
                            // would sit on top of the wall it just made.
                            if (online.currentUrl.length === 0)
                                online.open(u);
                            sheet.close();
                        }

                        Row {
                            anchors.centerIn: parent
                            spacing: 5
                            height: 16
                            Icon {
                                width: 12
                                height: 12
                                y: 2
                                name: "plus"
                                color: Theme.accent
                                weight: 1.8
                            }
                            Text {
                                height: parent.height
                                verticalAlignment: Text.AlignVCenter
                                text: "添加源"
                                color: Theme.accent
                                font.pixelSize: card.bodyFs
                                font.family: Theme.uiFont
                            }
                        }
                        MouseArea {
                            id: sh2
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: srcAdd.doAdd()
                        }
                    }
                }
            }
        }

        ListView {
            id: srcList
            visible: sheet.tab === "sources" && srcList.count > 0
            x: card.paneX
            y: addCard.y + addCard.height + card.cardGap
            width: card.paneW
            height: card.listH
            clip: true
            spacing: card.rowGap
            boundsBehavior: Flickable.StopAtBounds
            model: online.sources

            delegate: Rectangle {
                id: srow
                required property var modelData
                required property int index
                property bool editing: srcPane.editingRow === index
                width: srcList.width
                height: editing ? card.editRowH : card.rowH
                radius: 4
                color: card.plate
                border.width: 1
                border.color: card.line

                function save() {
                    online.updateSource(srow.index, editName.text, editUrl.text);
                    srcPane.editingRow = -1;
                }

                Rectangle {
                    id: ring
                    visible: !srow.editing
                    x: 13
                    anchors.verticalCenter: parent.verticalCenter
                    width: 26
                    height: 26
                    radius: 13
                    color: "transparent"
                    border.width: 1
                    border.color: card.line

                    Icon {
                        anchors.centerIn: parent
                        width: 13
                        height: 13
                        name: "link"
                        color: card.textMid
                        weight: 1.6
                    }
                }

                Column {
                    visible: !srow.editing
                    anchors.left: ring.right
                    anchors.leftMargin: 10
                    anchors.right: acts.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    Text {
                        width: parent.width
                        text: srow.modelData.name
                        elide: Text.ElideRight
                        color: srow.modelData.enabled ? card.textHi : card.textFaint
                        font.pixelSize: card.smallFs
                        font.family: Theme.uiFont
                    }
                    Text {
                        width: parent.width
                        text: srow.modelData.url
                        elide: Text.ElideMiddle
                        color: card.textFaint
                        font.pixelSize: card.labelFs
                        font.family: Theme.uiFont
                    }
                }

                Row {
                    id: acts
                    visible: !srow.editing
                    anchors.right: parent.right
                    anchors.rightMargin: 13
                    anchors.verticalCenter: parent.verticalCenter
                    height: 29
                    spacing: 9

                    // Positioners ignore anchors, so the switch is dropped into the middle
                    // of the 29 px line by its own y rather than by an anchor. Its cell is
                    // 8 px wider than the pill, which is the mock's gap next to 修改.
                    Item {
                        width: 38
                        height: 29
                        Rectangle {
                            id: sw
                            y: 5
                            width: 30
                            height: 19
                            radius: 9.5
                            color: srow.modelData.enabled ? Theme.accent : card.swOff
                            border.width: 1
                            border.color: srow.modelData.enabled ? Theme.accent : card.line

                            // White knob while on, as in the mock; it used to be the panel's
                            // own near-black, which read as a hole in the pill.
                            Rectangle {
                                width: 15
                                height: 15
                                radius: 7.5
                                y: 2
                                x: srow.modelData.enabled ? parent.width - width - 2 : 2
                                color: srow.modelData.enabled ? "#FFFFFF" : card.swOffKnob
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                // Only the flag moves here. The wall notices that the source
                                // it was showing went away and moves itself on - which it has
                                // to do however the switch got flipped.
                                onClicked: online.setSourceEnabled(srow.index,
                                                                  !srow.modelData.enabled)
                            }
                        }
                    }

                    Rectangle {
                        id: editBtn
                        y: 0
                        width: Math.round(shellHeight * 0.086)      // 62
                        height: 29
                        radius: 3
                        color: editBtnHit.containsMouse ? card.btnHover : "transparent"
                        border.width: 1
                        border.color: editBtnHit.containsMouse ? card.boxHoverLine : card.line

                        Row {
                            anchors.centerIn: parent
                            spacing: 5
                            height: 16
                            Icon {
                                width: 12
                                height: 12
                                y: 2
                                name: "pencil"
                                color: card.textMid
                                weight: 1.5
                            }
                            Text {
                                height: parent.height
                                verticalAlignment: Text.AlignVCenter
                                text: "修改"
                                color: card.textMid
                                font.pixelSize: card.bodyFs
                                font.family: Theme.uiFont
                            }
                        }
                        MouseArea {
                            id: editBtnHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                editName.text = srow.modelData.name;
                                editUrl.text = srow.modelData.url;
                                srcPane.editingRow = srow.index;
                            }
                        }
                    }

                    Rectangle {
                        id: delBtn
                        y: 0
                        width: Math.round(shellHeight * 0.086)
                        height: 29
                        radius: 3
                        color: delBtnHit.containsMouse ? card.btnHover : "transparent"
                        border.width: 1
                        border.color: delBtnHit.containsMouse ? card.boxHoverLine : card.line

                        Row {
                            anchors.centerIn: parent
                            spacing: 5
                            height: 16
                            Icon {
                                width: 12
                                height: 12
                                y: 2
                                name: "trash"
                                color: card.textMid
                                weight: 1.5
                            }
                            Text {
                                height: parent.height
                                verticalAlignment: Text.AlignVCenter
                                text: "删除"
                                color: card.textMid
                                font.pixelSize: card.bodyFs
                                font.family: Theme.uiFont
                            }
                        }
                        MouseArea {
                            id: delBtnHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: online.removeSource(srow.index)
                        }
                    }
                }

                Column {
                    id: editCol
                    visible: srow.editing
                    x: card.boxPad
                    y: 12
                    width: parent.width - 2 * card.boxPad
                    spacing: 8

                    MyField {
                        id: editName
                        width: parent.width
                        height: card.fieldH
                        fontSize: card.bodyFs
                        placeholder: "名字"
                        framed: true
                        fill: card.fill
                        edgeColor: card.line
                        hintColor: card.textFaint
                        corner: 3
                        inset: 14
                    }
                    MyField {
                        id: editUrl
                        width: parent.width
                        height: card.fieldH
                        fontSize: card.bodyFs
                        placeholder: "https://站点/"
                        framed: true
                        fill: card.fill
                        edgeColor: card.line
                        hintColor: card.textFaint
                        corner: 3
                        inset: 14
                        onCommitted: srow.save()
                    }
                    Row {
                        spacing: 8
                        height: card.fieldH

                        Repeater {
                            model: [
                                { key: "save", text: "保存" },
                                { key: "cancel", text: "取消" }
                            ]
                            delegate: Rectangle {
                                required property var modelData
                                width: Math.round(shellHeight * 0.086)
                                height: card.fieldH
                                radius: 3
                                color: abh.containsMouse ? card.btnHover : "transparent"
                                border.width: 1
                                border.color: modelData.key === "save" ? Theme.accent : card.line
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.text
                                    color: modelData.key === "save" ? Theme.accent : card.textMid
                                    font.pixelSize: card.bodyFs
                                    font.family: Theme.uiFont
                                }
                                MouseArea {
                                    id: abh
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (modelData.key === "save")
                                            srow.save();
                                        else
                                            srcPane.editingRow = -1;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // The unfolded row needs somewhere to store which row is open, and the list's own
        // delegates are not addressable from the card. An invisible holder also keeps the
        // id `srcPane` that the sizing above reads.
        Item {
            id: srcPane
            visible: false
            property int editingRow: -1
        }
    }
}
