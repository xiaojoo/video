import QtQuick
import QtQuick.Controls
import Playerui

// The window's 设置 door. It lives in Main.qml rather than on a page because the door is
// in the window's caption, which is on screen everywhere - a player page with no sources
// still has to be able to open it.
//
// One page, every option on it. The strip across the top is not a set of tabs that swap
// content: it jumps to the section it names, and it follows the scroll back. The layout is
// the shape of the settings sheets he sent - a bold title outside a rounded card, rows
// inside it split by full-width hairlines, the control of a row flush right as a pill - and
// the colours stay the app's own charcoal, the same literals the 新建下载 dialog, the
// context menu and the download rows already spend.
Popup {
    id: sheet
    modal: true
    // No dim behind the card: the window underneath stays readable, and the card is a
    // panel that belongs to that page rather than a modal step.
    Overlay.modal: Item { }
    // Centred, but on whole pixels. `anchors.centerIn` left this card on a half-pixel y at
    // the launch size, and every horizontal hairline in it - the card's own edge, the two
    // group cards, the rules between rows, the fields - then straddled two rows and came
    // out at half the strength of the vertical edges. `Overlay.overlay` is null until the
    // popup has an overlay to sit in, which is also why the window's width is handed in
    // rather than read from it.
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
    property real refWidth: Theme.refW
    // Where new downloads land; the 下载任务 section shows it and can change it.
    property string dlDir: ""
    // Which section the strip stands on. A click moves it and scrolls to it; the scroll
    // moves it back, but only when there was something to scroll - with two sources the
    // page fits and the second section still has to be selectable.
    property string tab: "sources"
    // Which source row has its 更多 menu up, and which one is unfolded into its edit
    // fields. Both are the card's business rather than a row's: one menu for the whole
    // list, and one open row.
    property int menuRow: -1
    // Whether the 添加源 card is showing its fields. The card is always there; only what
    // it costs in height follows this.
    property bool adding: false
    property int editingRow: -1

    function openAt(what) {
        sheet.tab = what;
        sheet.open();
    }

    // The folder picker is the window's (Main.qml owns it, and so does the library's own
    // 选择目录); this card only asks for it.
    signal dirRequested()
    // Which surface the online page's magnet board uses, and the ask to change it. The
    // choice is stored by the window; this card only shows and sets it.
    property string magnetPanel: "popup"
    signal magnetPanelPicked(string which)
    // How many connections a 网盘 direct link is pulled with. Same arrangement as the
    // board's position: the card shows it, the window stores it and hands it to the engine.
    property int dlConns: 8
    signal connsPicked(int n)

    // The primary pill: an accent edge, an accent wash that comes in from the icon side,
    // and a halo reaching ~6 px past the edge. A real blur needs QtQuick.Effects and
    // effectsplugin.dll is not in the package, so the halo is three 2 px steps of accent -
    // and they are siblings of the face rather than children of it, because a child is
    // painted over its parent's own fill whatever its z. The face, the label and the hit
    // area each therefore carry a z above them.
    component WashButton: Item {
        id: wb
        property alias label: wbText.text
        property alias glyph: wbIcon.name
        readonly property bool hot: wbHit.containsMouse
        signal clicked()
        width: wbRow.implicitWidth + 2 * card.btnPadX
        height: card.btnH
        readonly property real corner: height / 2

        Repeater {
            model: [card.glowNear, card.glowMid, card.glowFar]
            delegate: Rectangle {
                required property var modelData
                required property int index
                x: -(index + 1) * 2
                y: -(index + 1) * 2
                width: wb.width + (index + 1) * 4
                height: wb.height + (index + 1) * 4
                radius: wb.corner + (index + 1) * 2
                color: modelData
            }
        }

        Rectangle {
            anchors.fill: parent
            z: 1
            radius: wb.corner
            border.width: 1
            border.color: wb.hot ? "#FFA733" : Theme.accent
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0
                               color: wb.hot ? card.washInHot : card.washIn }
                GradientStop { position: 1.0
                               color: wb.hot ? card.washOutHot : card.washOut }
            }
        }

        Row {
            id: wbRow
            anchors.centerIn: parent
            z: 2
            spacing: 6
            height: 16
            Icon {
                id: wbIcon
                width: 14
                height: 14
                y: 1
                color: Theme.accent
                weight: 1.8
            }
            Text {
                id: wbText
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                color: Theme.accent
                font.pixelSize: card.bodyFs
                font.family: Theme.uiFont
            }
        }

        MouseArea {
            id: wbHit
            anchors.fill: parent
            z: 3
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: wb.clicked()
        }
    }

    // Every other pill on the page - 打开文件夹, 更多, 保存, 取消. An edge, and on a card a
    // fill one step above the card so the edge has something to sit on.
    component EdgeButton: Rectangle {
        id: eb
        property alias label: ebText.text
        property string glyph: ""
        property color rest: "transparent"
        property color ink: card.textHi
        signal clicked()
        width: ebRow.implicitWidth + 2 * card.btnPadX
        height: card.btnH
        radius: height / 2
        color: ebHit.containsMouse ? card.btnHover : eb.rest
        border.width: 1
        border.color: ebHit.containsMouse ? card.boxHoverLine : card.line

        Row {
            id: ebRow
            anchors.centerIn: parent
            spacing: 6
            height: 16
            Icon {
                visible: eb.glyph.length > 0
                width: 14
                height: 14
                y: 1
                name: eb.glyph
                color: eb.ink
                weight: 1.6
            }
            Text {
                id: ebText
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                color: eb.ink
                font.pixelSize: card.bodyFs
                font.family: Theme.uiFont
            }
        }

        MouseArea {
            id: ebHit
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: eb.clicked()
        }
    }

    // A section's name, standing outside the card it introduces: the reference puts the
    // title above the card rather than inside it, with the sentence that explains the
    // section under a 26 px glyph.
    // A section's name, standing outside the card it introduces. The reference's own
    // glyph and the sentence under it are gone: the title says which section this is, and
    // the strip above the page says it again.
    component SectionHead: Item {
        id: sh
        property alias title: shTitle.text
        height: card.secHead

        Text {
            id: shTitle
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: card.textHi
            font.pixelSize: card.headFs
            font.family: Theme.uiFont
            font.weight: Font.DemiBold
        }
    }

    Rectangle {
        id: card
        width: Math.round(sheet.refWidth * 0.58)
        // It grows with what is on the page - every source is shown, so a long list is a
        // tall card - up to 0.86 of the window, and past that the page scrolls inside it.
        // The floor is what the page costs with nothing to scroll, so the card never
        // collapses to a strip when the list is short.
        height: Math.min(capH, Math.max(minH, chromeH + pageH + pad))
        radius: 5
        color: card.bg
        border.width: 1
        border.color: card.line
        clip: true
        focus: true

        // ---- palette ----
        // Every value here is one the rest of the app already spends, so the sheet is the
        // same charcoal as the 新建下载 dialog, the context menu and the download rows:
        // panel #141418 (AddLinksDialog), group cards #1B1B1F and fields one step under
        // them, the picked tab the bare accent (what the chrome's own page tabs do), hovers
        // #2A2A2E / #2A2A31, dim text #4A4A4E. The two steps that carry the depth - card
        // over panel and field under it - are both 11/255, because the reference's own
        // 3-level step vanished into this palette's black floor.
        //
        // One edge for the whole sheet: the card, the two group cards, every field and
        // every pill are all `line`, so "the outline is the same everywhere" is one value
        // rather than a family of near-greys.
        readonly property color bg: "#141418"          // what every dialog in the app is
        readonly property color plate: "#1B1B1F"       // the group cards, bg +11
        readonly property color line: "#3A3A40"        // the only edge in the sheet
        readonly property color fill: "#141418"        // fields, plate -11
        readonly property color textHi: Theme.glyph              // headings, row names
        readonly property color textMid: Theme.textSecondary     // values
        readonly property color textDim: "#B3B8B8B8"    // subtitles, field labels (70%)
        readonly property color textFaint: "#8CB8B8B8"  // urls, placeholders (55%)
        readonly property color btnHover: "#2A2A2E"     // plate +15, as on the library tabs
        readonly property color boxHover: "#2A2A31"     // the ✕, as in 新建下载
        readonly property color boxHoverLine: "#4A4A4E"
        readonly property color swOff: "#141418"
        readonly property color swOffKnob: "#4A4A4E"
        readonly property color danger: "#C42B1C"       // the caption's own close red
        // Both link glyphs are the reference's other accent, not a grey. `cool` is the
        // token the library card's corner bars already spend, so this is not a new colour.
        readonly property color lead: Theme.cool
        // The primary pill's halo. Where all three steps overlap they add up to the mock's
        // +26/255 just outside the edge; the outer two and the outer one land on its +15
        // and +6.
        readonly property color glowNear: Qt.rgba(Theme.accent.r, Theme.accent.g,
                                                  Theme.accent.b, 0.06)
        readonly property color glowMid: Qt.rgba(Theme.accent.r, Theme.accent.g,
                                                 Theme.accent.b, 0.04)
        readonly property color glowFar: Qt.rgba(Theme.accent.r, Theme.accent.g,
                                                 Theme.accent.b, 0.025)
        // Its fill is not flat: +32% at the left edge and +6% at the right, so the wash
        // comes in from the side the icon sits on. Both stops are pre-composited onto the
        // panel rather than drawn as translucent accent - a translucent face lets the halo
        // bleed up through the whole pill, which measured +45% flat against the +32 there.
        readonly property color washIn: "#5B3811"
        readonly property color washOut: "#241C16"
        readonly property color washInHot: "#6D410F"
        readonly property color washOutHot: "#302215"

        // ---- the sheet's grid ----
        // Off the two settings sheets he sent, which are 1x: a group card's two-line row is
        // 64 there and its pill 91x27 with the label 12 either side, the rules between rows
        // run the whole width of the card, and the title stands outside it.
        readonly property real pad: 15
        readonly property real stripH: 38
        readonly property real pageGap: 12
        readonly property real chromeH: pad + stripH + pageGap
        readonly property real paneW: card.width - 2 * pad
        readonly property real headFs: 14
        readonly property real smallFs: 13
        readonly property real bodyFs: 12
        readonly property real labelFs: 12
        readonly property real fieldH: 30
        // The pills one step under the fields: at 30 they were as tall as the box you type
        // into, which made 添加源 read as the loudest thing on its line.
        readonly property real btnH: 26
        readonly property real btnPadX: 9
        readonly property real rowH: 64
        readonly property real secHead: 38
        readonly property real secGap: 20
        // The unfolded source row: two fields and the 保存/取消 line, with its padding.
        readonly property real editRowH: 2 * 12 + 3 * card.fieldH + 2 * 8
        readonly property real minH: Math.round(Theme.refH * 0.72)
        readonly property real capH: Math.round(Theme.refH * 0.86)
        // The list's own count. `online.sources` is a QJsonArray, and a QJsonArray handed
        // to QML has no `length` - reading it into an int property came out 0 and the whole
        // 在线源 section collapsed. The Repeater is the thing that actually knows.
        readonly property int srcCount: srcRows.count
        // What the list costs: one line per source, plus the fold if one of them is open.
        readonly property real rowsH: srcCount <= 0 ? 0
            : srcCount * rowH + (sheet.editingRow >= 0 ? editRowH - rowH : 0)
        readonly property real srcCardH: rowsH
        readonly property real pageCap: capH - chromeH - pad
        readonly property real pageH: Math.min(pageCap, body.implicitHeight)

        // The card swallows presses on its own body: a Rectangle passes them through to
        // whatever is behind the popup, and a press there used to open a poster's magnets.
        // It also takes the caret, so a field does not keep its accent border after a
        // press somewhere else in the box.
        MouseArea {
            anchors.fill: parent
            onPressed: card.forceActiveFocus()
        }

        // ---- the strip across the top ----
        // The same plate the chrome's own page tabs are: the picked one is filled with the
        // accent and carries near-black text, the rest sit on #1B1B1F with a grey edge.
        Row {
            id: strip
            x: card.pad
            y: card.pad
            height: card.stripH
            spacing: 8

            Repeater {
                model: [
                    { key: "sources", text: "在线源" },
                    { key: "downloads", text: "下载任务" }
                ]
                delegate: Rectangle {
                    id: stab
                    required property var modelData
                    required property int index
                    width: st.implicitWidth + Theme.refH * 0.028
                    height: strip.height
                    radius: Theme.controlRadius
                    color: sheet.tab === modelData.key ? Theme.accent
                           : (sth.containsMouse ? "#2A2A2E" : "#1B1B1F")
                    border.width: 1
                    border.color: "#3A3A40"

                    Text {
                        id: st
                        anchors.centerIn: parent
                        text: modelData.text
                        color: sheet.tab === modelData.key ? "#15151A" : Theme.textPrimary
                        font.pixelSize: Math.round(Theme.subtitleFontRatio * Theme.refH)
                        font.family: Theme.uiFont
                    }

                    MouseArea {
                        id: sth
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // `stab.modelData`, not bare `modelData`: this handler is a
                            // nested item's, and a bare name there throws at run time -
                            // qmllint stays quiet and the tab just stops responding.
                            sheet.tab = stab.modelData.key;
                            page.goTo(stab.index);
                        }
                    }
                }
            }
        }

        Rectangle {
            x: card.pad
            y: strip.y + strip.height + 5
            width: card.paneW
            height: 1
            color: card.line
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

        // ---- the page ----
        // One Flickable with a Column of blocks inside it. The Column is what the Flickable
        // adopts as its scrolling content, and a Column's implicitHeight is its own business
        // - sizing the content by hand off the children's positions made the Flickable take
        // that binding over, which left every section stacked at y 0. The scrollbar is a
        // sibling of the Flickable, so it stays put.
        Flickable {
            id: page
            x: card.pad
            y: card.chromeH
            // The page owns the whole inner width: the cards have to sit the same distance
            // from both walls, and the rule under the tabs - which does span the pane - was
            // the tell that they did not. The scrollbar is not paid for out of this width;
            // it lives in the right padding (below).
            width: card.paneW
            height: card.pageH
            contentWidth: width
            contentHeight: body.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            // The strip follows the page, but only when the page can actually move. The
            // last section wins once the page is at the bottom: with a short list its top
            // can never be pushed up to the viewport's edge, and a rule that asks for that
            // would leave the second tab unselectable - the click would scroll, the spy
            // would immediately move the mark back, and the tab would look dead.
            onContentYChanged: {
                if (contentHeight > height + 1)
                    sheet.tab = contentY + 4 >= Math.min(dlHead.y, contentHeight - height)
                            ? "downloads" : "sources";
            }

            function goTo(which) {
                page.contentY = which === 1
                        ? Math.max(0, Math.min(dlHead.y, contentHeight - height)) : 0;
            }

            Column {
                id: body
                width: page.width
                spacing: 0

                SectionHead {
                    width: body.width
                    title: "在线源"
                }

                // 添加源 on a card of its own, ahead of the list: the sentence that says
                // what a source is on the left, the button that asks for one on the right,
                // and only once that button has been pressed, the two fields and the line
                // that commits them.
                Rectangle {
                    id: addCard
                    width: body.width
                    height: sheet.adding ? 132 : 44
                    radius: 6
                    color: card.plate
                    border.width: 1
                    border.color: card.line
                    clip: true

                    readonly property real pad: 15
                    readonly property real gap: 12
                    readonly property real freeW: width - 2 * pad - gap
                    readonly property real nameW: Math.round(freeW * 0.52)

                    function doAdd() {
                        const u = addUrl.text.trim();
                        if (u.length === 0)
                            return;
                        online.addSource(addName.text, u);
                        addName.text = "";
                        addUrl.text = "";
                        sheet.adding = false;
                    }

                    Text {
                        x: addCard.pad
                        y: Math.round((44 - height) / 2)
                        width: addToggle.x - 20 - x
                        text: "添加在线媒体源，支持多种视频解析网站，方便您下载喜欢的内容。"
                        color: card.textDim
                        font.pixelSize: card.bodyFs
                        font.family: Theme.uiFont
                        elide: Text.ElideRight
                    }

                    WashButton {
                        id: addToggle
                        anchors.right: parent.right
                        anchors.rightMargin: addCard.pad
                        y: Math.round((44 - height) / 2)
                        label: sheet.adding ? "收起" : "添加源"
                        glyph: sheet.adding ? "up" : "plus"
                        onClicked: {
                            sheet.adding = !sheet.adding;
                            sheet.menuRow = -1;
                        }
                    }

                    Rectangle {
                        visible: sheet.adding
                        anchors.top: parent.top
                        anchors.topMargin: 44
                        width: parent.width
                        height: 1
                        color: card.line
                    }

                    MyField {
                        id: addName
                        visible: sheet.adding
                        x: addCard.pad
                        y: 54
                        width: addCard.nameW
                        height: card.fieldH
                        fontSize: card.bodyFs
                        placeholder: "请输入源名称，如：sidhub.cc"
                        fill: card.fill
                        framed: true
                        edgeColor: card.line
                        hintColor: card.textFaint
                        corner: 3
                        // The link mark lives inside the field, behind a hairline.
                        inset: 45

                        Icon {
                            x: 13
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16
                            height: 16
                            name: "link"
                            color: card.lead
                            weight: 1.7
                        }
                        Rectangle {
                            x: 35
                            y: 9
                            width: 1
                            height: 12
                            color: Qt.alpha(card.line, 0.55)
                        }
                    }
                    MyField {
                        id: addUrl
                        visible: sheet.adding
                        x: addName.x + addName.width + addCard.gap
                        y: 54
                        width: addCard.freeW - addCard.nameW
                        height: card.fieldH
                        fontSize: card.bodyFs
                        placeholder: "https://"
                        fill: card.fill
                        framed: true
                        edgeColor: card.line
                        hintColor: card.textFaint
                        corner: 3
                        inset: 14
                        onCommitted: addCard.doAdd()
                    }
                    Row {
                        visible: sheet.adding
                        x: addCard.pad
                        y: 94
                        spacing: 8

                        EdgeButton {
                            label: "添加"
                            onClicked: addCard.doAdd()
                        }
                        EdgeButton {
                            label: "取消"
                            onClicked: sheet.adding = false
                        }
                    }
                }

                Item {
                    // The seam between the section's two cards. A positioner drops an
                    // invisible child, so with nothing in the list the section is one card.
                    visible: card.srcCount > 0
                    width: body.width
                    height: 13
                }


                Rectangle {
                    id: srcCard
                    width: body.width
                    height: card.srcCardH
                    radius: 6
                    color: card.plate
                    border.width: 1
                    border.color: card.line
                    clip: true

                    Repeater {
                        id: srcRows
                        model: online.sources
                        delegate: Item {
                            id: srow
                            required property var modelData
                            required property int index
                            property bool editing: sheet.editingRow === index
                            x: 0
                            // Everything above this row, plus the fold of the row that is
                            // open in edit mode when that row is above it.
                            y: index * card.rowH
                               + (sheet.editingRow >= 0 && sheet.editingRow < index
                                  ? card.editRowH - card.rowH : 0)
                            width: srcCard.width
                            height: editing ? card.editRowH : card.rowH

                            function save() {
                                online.updateSource(srow.index, editName.text, editUrl.text);
                                sheet.editingRow = -1;
                            }

                            // The reference's rows are split by a rule that runs the whole
                            // width of the card, not inset to its padding. The first row
                            // has none: a child's clip is the bounding box, so its rule
                            // runs square through the card's rounded top corners instead
                            // of following them, which made the top edge read as a second,
                            // extra line. There the card's own border is the rule.
                            Rectangle {
                                visible: !srow.editing && srow.index > 0
                                anchors.top: parent.top
                                width: parent.width
                                height: 1
                                color: card.line
                            }

                            Rectangle {
                                id: ring
                                visible: !srow.editing
                                x: 15
                                anchors.verticalCenter: parent.verticalCenter
                                width: 32
                                height: 32
                                radius: 16
                                color: "transparent"
                                border.width: 1
                                border.color: card.line

                                Icon {
                                    anchors.centerIn: parent
                                    width: 14
                                    height: 14
                                    name: "link"
                                    color: card.lead
                                    weight: 1.7
                                }
                            }

                            Column {
                                id: sCol
                                visible: !srow.editing
                                x: 63
                                y: Math.round((srow.height - height) / 2)
                                width: moreBtn.x - 12 - x
                                spacing: 4

                                // The row's own state marker: the switch came off the line
                                // when the three actions went into the box, so a source
                                // that is off had nothing but a paler name to say so.
                                Row {
                                    id: sNameRow
                                    width: sCol.width
                                    height: sName.implicitHeight
                                    spacing: 6

                                    Text {
                                        id: sName
                                        // The tag hugs the name's ink, so the name has to
                                        // stop at its own width and give the tag the rest
                                        // of the line rather than push it to the far end.
                                        // A ternary, not Math.min: a width binding that
                                        // calls a function stops seeing the other item's
                                        // geometry change.
                                        readonly property real room: sCol.width
                                                - (sOff.visible ? sOff.width + sNameRow.spacing : 0)
                                        width: implicitWidth < room ? implicitWidth : room
                                        text: srow.modelData.name
                                        elide: Text.ElideRight
                                        color: srow.modelData.enabled ? card.textHi
                                                                       : card.textFaint
                                        font.pixelSize: card.smallFs
                                        font.family: Theme.uiFont
                                        // The name line is the only DemiBold in the row: the url
                                        // under it is one step smaller, so weight is what
                                        // separates them.
                                        font.weight: Font.DemiBold
                                    }

                                    Rectangle {
                                        id: sOff
                                        visible: !srow.modelData.enabled
                                        // The name's ink does not sit in the middle of its
                                        // own line box: measured on the rendered row its
                                        // centre is 9.6px below the top of the 19px box.
                                        // Centring this 20px tag on the box instead put its
                                        // ink 5.2px under the name it follows.
                                        y: 10 - height / 2
                                        width: sOffText.implicitWidth + 10
                                        height: sOffText.implicitHeight + 4
                                        radius: 2
                                        color: "transparent"
                                        border.width: 1
                                        border.color: card.line

                                        Text {
                                            id: sOffText
                                            anchors.centerIn: parent
                                            text: "已禁用"
                                            color: card.textFaint
                                            font.pixelSize: card.labelFs
                                            font.family: Theme.uiFont
                                        }
                                    }
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

                            // One control on the line: 启用/禁用, 修改 and 删除 all live in
                            // the box it opens.
                            EdgeButton {
                                id: moreBtn
                                visible: !srow.editing
                                label: "更多"
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                onClicked: {
                                    // Where the box hangs, in the card's own coordinates:
                                    // the row scrolls, so its position in the card is not
                                    // the row's y plus the page's.
                                    const p = moreBtn.mapToItem(Overlay.overlay, 0,
                                                               moreBtn.height);
                                    more.x = Math.max(4, Math.min(p.x - more.padding,
                                                                  Overlay.overlay.width
                                                                  - more.width - 4));
                                    more.y = Math.min(p.y + 4 - more.padding,
                                                      Overlay.overlay.height - more.height - 4);
                                    sheet.menuRow = srow.index;
                                    more.open();
                                }
                            }

                            // Unfolded in place: the row grows and the rows under it move.
                            Column {
                                id: editCol
                                visible: srow.editing
                                x: 15
                                y: 12
                                width: parent.width - 30
                                spacing: 8

                                MyField {
                                    id: editName
                                    text: srow.editing ? srow.modelData.name : ""
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
                                    text: srow.editing ? srow.modelData.url : ""
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
                                    height: card.btnH

                                    Repeater {
                                        model: [
                                            { key: "save", text: "保存" },
                                            { key: "cancel", text: "取消" }
                                        ]
                                        delegate: EdgeButton {
                                            required property var modelData
                                            label: modelData.text
                                            onClicked: {
                                                if (modelData.key === "save")
                                                    srow.save();
                                                else
                                                    sheet.editingRow = -1;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ---- 海报位置 ----
                // The same board, two surfaces. The card's own page tabs already draw a
                // picked state as a filled accent pill, so this row borrows that rather
                // than introducing a second switch style.
                Item { width: body.width; height: card.secGap }

                SectionHead {
                    width: body.width
                    title: "海报位置"
                }

                Rectangle {
                    id: panelCard
                    width: body.width
                    height: 44
                    radius: 6
                    color: card.plate
                    border.width: 1
                    border.color: card.line

                    readonly property real pad: 15

                    Text {
                        x: panelCard.pad
                        anchors.verticalCenter: parent.verticalCenter
                        text: "点开海报后的磁力列表"
                        color: card.textHi
                        font.pixelSize: card.bodyFs
                        font.family: Theme.uiFont
                    }

                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: panelCard.pad
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6

                        Repeater {
                            model: [ { key: "popup", text: "弹框" },
                                     { key: "dock", text: "左栏" },
                                     { key: "right", text: "右栏" } ]

                            delegate: Rectangle {
                                id: opt
                                required property var modelData
                                readonly property bool on: sheet.magnetPanel === modelData.key
                                width: optText.implicitWidth + 18
                                height: card.btnH
                                radius: Theme.controlRadius
                                color: opt.on ? Theme.accent
                                       : (optHit.containsMouse ? card.btnHover : card.fill)
                                border.width: 1
                                border.color: opt.on ? Theme.accent : card.line

                                Text {
                                    id: optText
                                    anchors.centerIn: parent
                                    text: opt.modelData.text
                                    color: opt.on ? "#15151A" : card.textMid
                                    font.pixelSize: card.bodyFs
                                    font.family: Theme.uiFont
                                }
                                MouseArea {
                                    id: optHit
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: sheet.magnetPanelPicked(opt.modelData.key)
                                }
                            }
                        }
                    }
                }

                // ---- 下载任务 ----
                Item { width: body.width; height: card.secGap }

                SectionHead {
                    id: dlHead
                    width: body.width
                    title: "下载任务"
                }

                Rectangle {
                    id: dlCard
                    width: body.width
                    height: 2 * card.rowH
                    radius: 6
                    color: card.plate
                    border.width: 1
                    border.color: card.line

                    // One hairline for the pair, drawn by the card that owns both rows: two
                    // stacked cards would put two 1 px edges on the same line.
                    Rectangle {
                        y: card.rowH
                        width: parent.width
                        height: 1
                        color: card.line
                    }

                    Column {
                        width: parent.width

                        Item {
                            width: parent.width
                            height: card.rowH

                            Column {
                                x: 15
                                y: Math.round((parent.height - height) / 2)
                                width: 420
                                spacing: 4

                                Text {
                                    width: parent.width
                                    text: "保存位置"
                                    color: card.textHi
                                    font.pixelSize: card.smallFs
                                    font.family: Theme.uiFont
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    width: parent.width
                                    text: sheet.dlDir.length > 0 ? sheet.dlDir : "…"
                                    elide: Text.ElideMiddle
                                    color: card.textFaint
                                    font.pixelSize: card.labelFs
                                    font.family: Theme.uiFont
                                }
                            }

                            Row {
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                height: card.btnH
                                spacing: 12

                                WashButton {
                                    label: "更改存储路径"
                                    glyph: "folder"
                                    onClicked: sheet.dirRequested()
                                }
                                EdgeButton {
                                    label: "打开文件夹"
                                    glyph: "play"
                                    onClicked: if (sheet.dlDir.length > 0)
                                                   Qt.openUrlExternally("file:///"
                                                       + sheet.dlDir.replace(/\\/g, "/"))
                                }
                            }
                        }

                        // ---- how many connections a 网盘 link is pulled with ----
                        // Presets rather than a typed number: the curve is measured (1/6/12
                        // connections = 359/705/1152 KB/s on the CDN), so the only choices
                        // that mean anything are the ones on either side of the knee.
                        Item {
                            width: parent.width
                            height: card.rowH

                            Column {
                                x: 15
                                y: Math.round((parent.height - height) / 2)
                                width: 420
                                spacing: 4

                                Text {
                                    width: parent.width
                                    text: "网盘并发下载"
                                    color: card.textHi
                                    font.pixelSize: card.smallFs
                                    font.family: Theme.uiFont
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    width: parent.width
                                    text: "同时拉的连接数；只影响新开始的任务"
                                    color: card.textFaint
                                    font.pixelSize: card.labelFs
                                    font.family: Theme.uiFont
                                }
                            }

                            Row {
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                height: card.btnH
                                spacing: 6

                                Repeater {
                                    model: [1, 2, 4, 6, 8, 12, 16]

                                    delegate: Rectangle {
                                        id: copt
                                        required property var modelData
                                        readonly property bool on: sheet.dlConns === modelData
                                        width: coptText.implicitWidth + 18
                                        height: card.btnH
                                        radius: Theme.controlRadius
                                        color: copt.on ? Theme.accent
                                               : (coptHit.containsMouse ? card.btnHover : card.fill)
                                        border.width: 1
                                        border.color: copt.on ? Theme.accent : card.line

                                        Text {
                                            id: coptText
                                            anchors.centerIn: parent
                                            text: copt.modelData
                                            color: copt.on ? "#15151A" : card.textMid
                                            font.pixelSize: card.bodyFs
                                            font.family: Theme.uiFont
                                        }
                                        MouseArea {
                                            id: coptHit
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: sheet.connsPicked(copt.modelData)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ---- 网盘登录 ----
                // The three drives whose ladder needs a session he has to type into, and all
                // three now work the same way: 去登录 brings up a browser of ours in its own
                // profile, the recorder waits until the drive itself says he is in, and what
                // lands is a key file this row can clear. 迅雷 signs every request in its page
                // and 百度's endpoints want BDUSS; 夸克's own api is what answers for it,
                // because none of its cookie names tells a session from an anonymous jar. UC
                // stays off the list - no ladder yet, and a row that cannot do anything is a
                // dead control.
                Item { width: body.width; height: card.secGap }

                SectionHead {
                    id: panHead
                    width: body.width
                    title: "网盘登录"
                }

                Rectangle {
                    id: panCard
                    width: body.width
                    height: panRows.count * card.rowH
                    radius: 6
                    color: card.plate
                    border.width: 1
                    border.color: card.line

                    // One hairline per join, drawn by the card that owns the rows - the rows
                    // each carrying their own top edge is how two adjacent lines became a fat
                    // one.
                    Repeater {
                        id: panJoins
                        model: panRows.count - 1

                        Rectangle {
                            required property int index
                            y: (index + 1) * card.rowH
                            width: parent.width
                            height: 1
                            color: card.line
                        }
                    }

                    Column {
                        width: parent.width

                        Repeater {
                            id: panRows
                            model: ["quark", "xunlei", "baidu"]

                            delegate: Item {
                                id: prow
                                required property var modelData
                                width: parent.width
                                height: card.rowH
                                // A property, never a function call: a binding that asks for
                                // stateText() keeps the answer it got before he signed in.
                                readonly property string state: modelData === "quark"
                                        ? panlogin.quarkState
                                        : (modelData === "baidu" ? panlogin.baiduState
                                                                  : panlogin.xunleiState)
                                readonly property bool signedIn: state.indexOf("已登录") === 0
                                // The recorder holds one busy flag for whichever drive it is waiting
                                // on, so a row only claims to be waiting when the sentence it was given
                                // is the waiting sentence.
                                readonly property bool waiting: state.indexOf("正在等你") === 0

                                Column {
                                    x: 15
                                    y: Math.round((parent.height - height) / 2)
                                    width: 420
                                    spacing: 4

                                    Text {
                                        width: parent.width
                                        text: prow.modelData === "baidu" ? "百度网盘"
                                            : (prow.modelData === "quark" ? "夸克网盘" : "迅雷网盘")
                                        color: card.textHi
                                        font.pixelSize: card.smallFs
                                        font.family: Theme.uiFont
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        width: parent.width
                                        text: prow.state
                                        elide: Text.ElideMiddle
                                        color: card.textFaint
                                        font.pixelSize: card.labelFs
                                        font.family: Theme.uiFont
                                    }
                                }

                                WashButton {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    // One shape for all three rows: the recorder answers for
                                    // every drive now, so the button either forgets his key
                                    // file or goes and makes one.
                                    label: prow.waiting ? "等待登录中"
                                         : (prow.signedIn ? "退出登录" : "去登录")
                                    glyph: prow.signedIn ? "logout" : "link"
                                    onClicked: {
                                        if (prow.signedIn)
                                            panlogin.logout(prow.modelData);
                                        else
                                            panlogin.login(prow.modelData);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }


        ThinScrollBar {
            target: page
            // Parked in the right padding, flush with the page's edge: the track is 14
            // wide and the padding is 15, so the 4 px mark lands about 5 px inside the
            // margin instead of standing on a card whose width it no longer eats.
            x: card.width - width - 1
            y: page.y
            height: page.height
        }

    }

    // The row's actions, in a box rather than two buttons on the line: 禁用/启用, 修改 and
    // 删除. It is a Popup of the window's overlay, not a child of the card: as a card child
    // it had to share the sheet with a full-card "press outside to close" MouseArea, and
    // that layer took the press before any row of the box ever saw it. One box for the
    // whole list, told which row by `menuRow`, so opening the next row's box moves this one.
    Popup {
        id: more
        parent: Overlay.overlay
        width: 108
        height: moreCol.implicitHeight + 2 * padding
        modal: false
        padding: 6
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
        onAboutToHide: sheet.menuRow = -1

        background: Rectangle {
            radius: 4
            color: card.bg
            border.width: 1
            border.color: card.line
        }

        // The action itself, run here instead of in the delegate's own handler: a nested
        // item's handler cannot name `modelData` - that throws ReferenceError the moment
        // the press lands, after the box has already closed, and the row does nothing.
        function pick(key) {
            const row = sheet.menuRow;
            more.close();
            if (row < 0)
                return;
            if (key === "tog")
                online.setSourceEnabled(row, !online.sources[row].enabled);
            else if (key === "edit")
                // The row seeds its own fields when it opens: those fields belong to a
                // delegate, and a delegate that is not unfolded does not exist to be
                // written from here.
                sheet.editingRow = row;
            else
                online.removeSource(row);
        }

        Column {
            id: moreCol
            width: more.availableWidth

            // The switch left the line with the other two actions, so the box says which
            // way it stands: 禁用 while the source is on, 启用 while it is off.
            readonly property bool on: sheet.menuRow >= 0
                    ? online.sources[sheet.menuRow].enabled : false

            Repeater {
                model: [
                    { key: "tog", text: moreCol.on ? "禁用" : "启用", danger: false },
                    { key: "edit", text: "修改", danger: false },
                    { key: "del", text: "删除", danger: true }
                ]
                delegate: Rectangle {
                    id: moreItem
                    required property var modelData
                    width: moreCol.width
                    height: 32
                    radius: 3
                    color: moreHit.containsMouse ? "#22222A" : "transparent"

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.text
                        color: modelData.danger ? card.danger : card.textHi
                        font.pixelSize: card.bodyFs
                        font.family: Theme.uiFont
                    }

                    MouseArea {
                        id: moreHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: more.pick(moreItem.modelData.key)
                    }
                }
            }
        }
    }
}

