import QtQuick
import QtQuick.Controls
import Playerui

// 在线媒体: a magnet-index site as a poster wall. The download jobs used to share this
// page and have a tab of their own beside it now; what is left here is the wall.
//
// Like the library card menu, this page talks to the context object (online) directly;
// the alternative was a signal per action and a handler in Main.qml that only forwarded it.
Item {
    id: page

    property string saveDir: ""
    // The poster whose magnets are being listed, or null.
    property var detail: null
    // A page hop asks for a whole new list, so the wall goes back to the top first - the
    // new page is read from its first row, not from wherever the last one was left.
    signal hopToTop()
    // A download was added from a poster's menu. The job list is its own page now, so the
    // switch belongs to the host that owns both pages.
    signal jobsRequested()
    // The shared callout lives in Main.qml; every mark that wants one hands its item up
    // through here and the library page.
    signal tipFor(var item, string text, string keys)
    signal tipOff(var item)

    // The page's gutter. The media library hands over the same value its own header uses,
    // so the posters start on the pills' line; on its own it reads off the player control
    // row's gap, which is what that alignment is defined by.
    property real pad: Math.round(shellWidth * Theme.sideInsetWRatio)
    // The distance between two neighbours - pills, chips, posters. Kept apart from `pad` for
    // the same reason the player's row keeps them apart: one is the margin to the window,
    // the other is the gap between things.
    property real gap: Math.round(shellHeight * Theme.controlGapRatio)
    // Half of the gutter per side: two neighbours sit one gutter apart and the outermost
    // posters still land on the gutter.
    readonly property real inset: Math.round(gap / 2)
    // The block under each poster that holds one or two lines of title, with the same
    // inset above and below it as between the posters side to side.
    readonly property real lineH: Math.round(smallFs * 1.35)
    readonly property real labelBlock: 2 * inset + 2 * lineH
    // How wide a poster wants to be. How many fit is a fact about the window, and the row
    // is divided by that number - so a wider window gains a column instead of stretching
    // the ones it already has.
    readonly property real targetCol: 200
    readonly property real smallFs: Math.round(0.0180 * shellHeight)
    readonly property real titleFs: Math.round(0.0194 * shellHeight)
    // The bottom edge of the menu line: the sources and the categories share one row now,
    // so the wall starts one line under it.
    readonly property real menuBottom: tabs.y + tabs.height
    // Where the content block starts. The status line only counts while it is actually
    // on screen: an invisible Text keeps its implicit height, and hanging the download
    // panel off that leftover 18 px left it 44 px under the menu while the wall sat 13 px
    // under it.
    readonly property real contentTop: page.menuBottom + page.pad
                                       + (status.visible ? status.height + page.pad : 0)

    // The sources the wall can actually be pointed at. A row switched off in the settings
    // card keeps its place there and stops being a tab.
    readonly property var liveSources: {
        const out = [];
        for (const s of online.sources)
            if (s.enabled)
                out.push(s);
        return out;
    }

    // The site a url belongs to, ignoring the path - a category page is still the source
    // that published it.
    function shows(s) {
        const cur = online.currentUrl;
        return cur.length > 0 && s.url.length > 0
               && (cur === s.url || cur.startsWith(s.url.replace(/\/+$/, "") + "/"));
    }

    Component.onCompleted: {
        online.loadSources();
        // Read off the engine rather than off `liveSources`: this runs in the same tick as
        // the load, and a binding asked for before the list exists keeps that answer.
        const live = online.sources.filter(s => s.enabled);
        if (live.length > 0)
            online.open(live[0].url);
    }

    Connections {
        target: page
        function onHopToTop() {
            // Stop whatever flick was still running, then go to the top. A bare
            // `contentY = 0` lands *during* the old view's deceleration, and the flick
            // finished the movement afterwards - the reported "the bar scrolls by itself"
            // on every page click.
            wall.flick(0, 0);
            wall.positionViewAtBeginning();
        }
    }

    // The window's 设置 card can switch a source off from anywhere, including while this
    // page is the one on screen. A wall left pointing at a source that has no pill any
    // more is a page with nothing to go back to, so it moves to a source that is still on.
    // Nothing else about a row concerns the wall: one whose address was edited is
    // re-opened by the model itself.
    Connections {
        target: online
        function onSourcesChanged() {
            if (online.currentUrl.length === 0)
                return;
            for (const s of online.sources) {
                if (!page.shows(s))
                    continue;
                if (s.enabled)
                    return;
                page.hopToTop();
                for (const live of online.sources)
                    if (live.enabled) {
                        online.open(live.url);
                        break;
                    }
                return;
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#0A0A0C"
    }

    // ---- content: the whole width, because the menu is a row at the top now ----
    Item {
        anchors.fill: parent
        id: content

        Row {
            id: tabs
            x: page.pad
            y: page.pad
            spacing: page.gap
            height: Math.round(shellHeight * 0.05)

            // One pill per online source that is switched on, carrying the name it was
            // added with - the tab says what you are looking at, and the categories of
            // whichever is lit run on to its right on this same line.
            Repeater {
                model: page.liveSources
                delegate: Rectangle {
                    id: src
                    required property var modelData
                    // Lit for every page of this source, not just its root: the category
                    // row under it belongs to the source, so picking 电影 must not put the
                    // source's own pill out.
                    readonly property bool cur: page.shows(modelData)
                    width: t.implicitWidth + Math.round(shellHeight * 0.028)
                    height: tabs.height
                    radius: Theme.controlRadius
                    color: src.cur ? Theme.accent
                           : (th.containsMouse ? "#2A2A2E" : "#1B1B1F")
                    border.color: "#3A3A40"
                    border.width: 1
                    Text {
                        id: t
                        anchors.centerIn: parent
                        text: modelData.name
                        color: src.cur ? "#15151A" : Theme.textPrimary
                        font.pixelSize: page.smallFs
                        font.family: Theme.uiFont
                    }
                    MouseArea {
                        id: th
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            page.hopToTop();
                            online.open(modelData.url);
                        }
                    }
                }
            }

        }

        // ---- the categories the selected source publishes, from the same fetch that
        // filled the wall. They sit on the sources' own line, to the right of the last
        // pill, and are whatever is left of the width - a second source takes its place
        // in the queue and pushes these along rather than getting a line of its own. ----
        ListView {
            id: chips
            x: tabs.x + (tabs.width > 0 ? tabs.width + page.gap : 0)
            y: tabs.y
            width: Math.max(0, parent.width - x - page.pad)
            height: tabs.height
            orientation: ListView.Horizontal
            spacing: page.gap
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: {
                // 首页 is dropped: it is the source's own root url, and the source pill to
                // the left opens exactly that. Matched on the url, not the label, so a
                // site whose home link is called something else loses the same button.
                const roots = [];
                for (const s of online.sources)
                    roots.push(s.url.replace(/\/+$/, ""));
                const out = [];
                for (const n of online.nav)
                    if (!roots.includes(n.url.replace(/\/+$/, "")))
                        out.push(n);
                return out;
            }
            // No bar on this row: it is one line tall, the wheel scrolls it, and the
            // style would put a white groove at its end.

            delegate: Rectangle {
                required property var modelData
                readonly property bool cur: online.currentUrl === modelData.url
                width: ct.implicitWidth + Math.round(shellHeight * 0.028)
                height: ListView.view.height
                radius: Theme.controlRadius
                color: ch.containsMouse ? "#2A2A2E" : "#1B1B1F"
                border.width: 1
                border.color: cur ? Theme.accent : "#3A3A40"
                Text {
                    id: ct
                    anchors.centerIn: parent
                    text: modelData.name
                    elide: Text.ElideRight
                    color: cur ? Theme.accent : Theme.textPrimary
                    font.pixelSize: page.smallFs
                    font.family: Theme.uiFont
                }
                MouseArea {
                    id: ch
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        page.hopToTop();
                        online.open(modelData.url);
                    }
                }
            }
        }

        Text {
            id: status
            x: page.pad
            y: page.menuBottom + page.pad
            width: parent.width - 2 * page.pad
            elide: Text.ElideRight
            // The count is not shown any more - the pager at the bottom of the wall says
            // where in the site this is - so the line only exists while it has something
            // to say. An empty Text would still take its row and push the wall down.
            visible: text.length > 0
            color: online.error.length > 0 ? Theme.accent : Theme.textSecondary
            font.pixelSize: page.smallFs
            font.family: Theme.uiFont
            // No "正在读取…" branch here: that text would appear for the duration of every
            // request, and the wall's y hangs off this line's height - so the whole grid
            // and its bar slid 30 px down and back on each page click. The skeleton and
            // the dots say "loading"; this line is only for things that went wrong.
            text: {
                if (online.error.length > 0)
                    return online.error;
                if (page.liveSources.length === 0)
                    return "上面还没有可用的在线源，点窗口右上角的齿轮加一个。";
                return "";
            }
        }

        GridView {
            id: wall
            // -5: a poster's art is inset 5 px inside its cell, and it is the art's edges
            // that should meet the page's gutter. Dividing that span by the column count -
            // rather than picking a fraction of the width and letting the grid take as many
            // as fit - is what leaves no void at the right: 0.155 gave 6 columns and a
            // 85 px gap on this window.
            x: page.pad - page.inset
            y: page.contentTop
            width: parent.width - 2 * (page.pad - page.inset)
            // The pager is part of the page, not an overlay: the last row of posters has
            // to stop above it rather than behind it.
            height: parent.height - y - page.pad
                    - (pager.visible ? pager.height + page.inset : 0)
            clip: true
            // The skeleton takes the area over while a page is being fetched: leaving the
            // old posters up made a page hop look like nothing had happened.
            visible: !online.busy && page.liveSources.length > 0
            enabled: visible
            // -0.01 so that floor(width / cellWidth) cannot come out one column short
            // through rounding: without it a window that fits 6 asks for 5.
            readonly property int columns: Math.max(1, Math.floor(width / page.targetCol))
            cellWidth: Math.max(120, width / columns - 0.01)
            // The poster keeps its own 1.72 ratio and the title block is added below it,
            // rather than the cell being 1.72 with the label squeezed into a leftover 17%
            // - that leftover made the art-to-art distance 46 px while the side-to-side
            // gap was 18.
            cellHeight: Math.round((cellWidth - 2 * page.inset) * 1.72) + page.labelBlock
            boundsBehavior: Flickable.StopAtBounds
            model: online.items

            delegate: Item {
                required property var modelData
                width: wall.cellWidth
                height: wall.cellHeight

                Rectangle {
                    id: art
                    x: page.inset
                    y: 0
                    width: parent.width - 2 * page.inset
                    height: parent.height - page.labelBlock
                    radius: 4
                    color: "#141418"
                    clip: true

                    Image {
                        anchors.fill: parent
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        source: modelData.poster
                        cache: true
                        // No sourceSize: these posters come off the site about 270 px wide,
                        // so asking for `art.width * 2` asked for more than exists *and* made
                        // the decode size depend on the cell - every resize then dropped all
                        // 20 images back to the flat card and re-fetched them (measured: the
                        // placeholder colour went from 267 px to 401,399 px, and it took
                        // three seconds to draw them again).
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: parent.radius
                        color: "transparent"
                        border.width: wh.containsMouse ? 2 : 0
                        border.color: Theme.accent
                    }
                }
                Text {
                    x: page.inset
                    y: art.y + art.height + page.inset
                    width: parent.width - 2 * page.inset
                    text: modelData.title
                    elide: Text.ElideRight
                    maximumLineCount: 2
                    // WrapAnywhere let a Latin title break mid-word on screen
                    // ("Resident Ev / il"); the library card, the playlist and the job
                    // row all wrap by word.
                    wrapMode: Text.WordWrap
                    color: wh.containsMouse ? Theme.accent : Theme.textPrimary
                    font.pixelSize: page.smallFs
                    font.family: Theme.uiFont
                }
                MouseArea {
                    id: wh
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    // One click lists the magnets behind the poster. Nothing is
                    // downloaded until one of them is chosen.
                    onClicked: {
                        magPopup.links = [];
                        page.detail = modelData;
                        online.magnets(modelData.href);
                    }
                }
            }
        }

        // ---- the wait, made visible: the wall's own shape, empty ----
        // The same grid, the same cells, the same gaps - so the page does not move when
        // the posters arrive. The wave runs across the columns so it reads as "fetching"
        // rather than as a picture that failed to load.
        Grid {
            id: skeleton
            x: wall.x
            y: wall.y
            columns: wall.columns
            spacing: 0
            visible: online.busy

            Repeater {
                model: wall.columns * 2

                delegate: Item {
                    id: sk
                    required property int index
                    width: wall.cellWidth
                    height: wall.cellHeight
                    readonly property real phase: (index % wall.columns) * 110

                    // The whole cell breathes, so the poster's box and the line under it
                    // stay one object rather than two animations that can drift.
                    SequentialAnimation on opacity {
                        running: skeleton.visible
                        loops: Animation.Infinite
                        PauseAnimation { duration: sk.phase }
                        NumberAnimation { from: 0.45; to: 1; duration: 420
                                            easing.type: Easing.InOutQuad }
                        NumberAnimation { from: 1; to: 0.45; duration: 420
                                          easing.type: Easing.InOutQuad }
                    }

                    Rectangle {
                        id: skArt
                        x: page.inset
                        y: page.inset
                        width: parent.width - 2 * page.inset
                        height: Math.round(width * 1.72)
                        radius: Theme.controlRadius
                        color: "#141418"
                    }
                    Rectangle {
                        x: page.inset
                        y: skArt.y + skArt.height + page.inset
                        width: Math.round(skArt.width * 0.55)
                        height: page.smallFs
                        radius: 2
                        color: "#141418"
                        opacity: 0.75
                    }
                }
            }
        }

        // ---- the request animation: three dots, in the middle of the screen ----
        Row {
            id: busyDots
            anchors.centerIn: parent
            spacing: Math.round(page.gap * 0.55)
            visible: online.busy
            z: 6

            Repeater {
                model: 3

                delegate: Rectangle {
                    id: dot
                    required property int index
                    width: Math.round(page.gap * 0.5)
                    height: width
                    radius: width / 2
                    color: Theme.glyph
                    SequentialAnimation on opacity {
                        running: busyDots.visible
                        loops: Animation.Infinite
                        PauseAnimation { duration: index * 180 }
                        NumberAnimation { from: 0.25; to: 1; duration: 300 }
                        NumberAnimation { from: 1; to: 0.25; duration: 300 }
                    }
                }
            }
        }

        // The wall's bar, beside the grid rather than inside it: a Flickable adopts its
        // children into the scrolling content, and this must not scroll.
        ThinScrollBar {
            target: wall
            wantVisible: wall.visible
            x: wall.x + wall.width - width + 2
            y: wall.y
            height: wall.height
        }

        // ---- the page hop: the source's own pagination, along the bottom ----
        Row {
            id: pager
            x: Math.max(page.pad, (parent.width - width) / 2)
            y: parent.height - height - page.pad
            spacing: Math.round(page.gap / 2)
            height: Math.round(shellHeight * 0.05)
            visible: page.liveSources.length > 0
                     && online.pager.length > 0

            Repeater {
                model: online.pager

                delegate: Rectangle {
                    id: hop
                    required property var modelData
                    // A hop with no url is the one the site marked disabled - the first
                    // page's 上一页 - and stays visible so the row does not shift.
                    readonly property bool live: modelData.url.length > 0
                                                 && !modelData.active
                    // A page number is one or two glyphs: at the row's own padding it would
                    // come out a stubby 24 x 36, so a chip is never narrower than it is
                    // tall. 上一页 and 2202 are already wider than that and keep their width.
                    width: Math.max(height, hopText.implicitWidth
                                          + Math.round(shellHeight * 0.028))
                    height: parent.height
                    radius: Theme.controlRadius
                    color: hopMouse.containsMouse && hop.live ? "#2A2A2E" : "#1B1B1F"
                    border.width: 1
                    border.color: modelData.active ? Theme.accent : "#3A3A40"

                    Text {
                        id: hopText
                        anchors.centerIn: parent
                        text: modelData.label
                        color: modelData.active ? Theme.accent
                               : (hop.live ? Theme.textPrimary : "#4A4A4E")
                        font.pixelSize: page.smallFs
                        font.family: Theme.uiFont
                    }
                    MouseArea {
                        id: hopMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: hop.live ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: if (hop.live) {
                            page.hopToTop();
                            online.open(modelData.url);
                        }
                    }
                }
            }
        }
    }

    // ---- the magnets behind one poster ----
    //
    // The same shape as the source card: a title, a hairline, then one flat row per seed
    // with its size and the action on the right. The rows used to be bordered plates, which
    // read as buttons stacked inside a dialog that is itself a button-shaped thing.
    Popup {
        id: magPopup
        property var links: []
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.round(page.width * 0.5)
        padding: magPad
        visible: page.detail !== null
        closePolicy: Popup.CloseOnPressOutside

        readonly property real magPad: Math.round(shellHeight * 0.028)
        readonly property real magGap: Math.round(shellHeight * 0.019)
        readonly property real magRow: Math.round(shellHeight * 0.064)
        readonly property real titleH: Math.round(page.titleFs * 1.35)
        readonly property real noteH: (online.busy || links.length === 0)
                                      ? Math.round(page.smallFs * 1.35) + magGap : 0
        // The header is counted arithmetically rather than off the Column's implicitHeight:
        // the list's height comes out of the popup's, so a content-driven popup height would
        // be a ring with no solution.
        readonly property real headH: titleH + noteH + 2 * magGap + 1
        height: Math.min(2 * magPad + headH + Math.max(1, links.length) * magRow,
                         Math.round(page.height * 0.72))

        background: Rectangle {
            radius: 6
            color: "#141418"
            border.color: "#3A3A40"
            border.width: 1
        }

        Column {
            id: magContent
            width: parent.width
            spacing: 0

            Text {
                width: parent.width
                text: page.detail ? page.detail.title : ""
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.pixelSize: page.titleFs
                font.family: Theme.uiFont
            }
            Text {
                width: parent.width
                visible: magPopup.noteH > 0
                text: online.busy ? "正在取磁力…"
                    : (magPopup.links.length === 0 ? "这一页没有可用的磁力链接" : "")
                color: online.busy ? Theme.textSecondary : Theme.accent
                font.pixelSize: page.smallFs
                font.family: Theme.uiFont
            }
            Item { width: 1; height: magPopup.magGap }
            Rectangle { width: parent.width; height: 1; color: "#26262A" }
            Item { width: 1; height: magPopup.magGap }

            ListView {
                id: magList
                width: parent.width
                height: Math.max(magPopup.magRow,
                                 magPopup.height - 2 * magPopup.magPad - magPopup.headH)
                clip: true
                model: magPopup.links
                boundsBehavior: Flickable.StopAtBounds

                delegate: Item {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: magPopup.magRow

                    Rectangle {
                        anchors.top: parent.top
                        width: parent.width
                        height: 1
                        color: "#26262A"
                        visible: index > 0
                    }
                    // Hover is a band behind the row, not a plate around it: the whole row
                    // is the target, so the band is what has to reach the edges.
                    Rectangle {
                        anchors.fill: parent
                        color: mh.containsMouse ? "#1B1B1F" : "transparent"
                    }
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 2
                        anchors.rightMargin: 2
                        spacing: 12
                        Text {
                            width: parent.width - 70 - 12 - 64 - 24
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            elide: Text.ElideMiddle
                            color: Theme.textPrimary
                            font.pixelSize: page.smallFs
                            font.family: Theme.uiFont
                        }
                        Text {
                            width: 70
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.size
                            color: Theme.textSecondary
                            font.pixelSize: page.smallFs
                            font.family: Theme.uiFont
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "加入下载"
                            color: Theme.accent
                            font.pixelSize: page.smallFs
                            font.family: Theme.uiFont
                        }
                    }
                    MouseArea {
                        id: mh
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // The card picture comes along with the seed, so the row is
                            // recognisable before a single byte has landed.
                            downloads.add(modelData.magnet, page.saveDir,
                                          page.detail ? page.detail.poster : "");
                            page.detail = null;
                            page.jobsRequested();
                        }
                    }
                }
            }
        }

        // The list's bar, as a sibling of the Column so the Column does not lay it out
        // as one more row. The Column sits at the popup's origin, so the list's own
        // offsets are already in this coordinate system.
        ThinScrollBar {
            target: magList
            x: magList.width - width + 2
            y: magList.y
            height: magList.height
        }

        Connections {
            target: online
            // The hops answer asynchronously; an empty list would otherwise read as
            // "this poster has nothing", which is not the same as "not yet".
            function onMagnetsReady(url, links) {
                if (page.detail && page.detail.href === url)
                    magPopup.links = links;
            }
        }
    }

}
