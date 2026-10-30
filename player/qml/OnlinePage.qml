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

    // ---- one screen page, however many posters that turns out to be ----
    // The site hands out a fixed 20 per page. A page here is instead however many posters
    // the wall holds - columns across times the rows that fit down to the pager, and never
    // fewer than 12 - and the site's pages are only how they get fetched: a screen page
    // starts wherever the last one ended, which is usually mid-site-page, and may run over
    // its end. Rows that do not fit are not a void: they stay on the page and the wall
    // scrolls to them, because a half row of posters is still content. Only the end of the
    // source's list may leave space, and `total` is what says so.
    readonly property int minPer: 12
    // ceil, not floor: the row that only half fits is still half a row of posters that
    // exist, and leaving 158 px of page background under it (measured at 2400 x 1350) is
    // the void he asked against. It is shown and reached with the bar instead.
    readonly property int per: Math.max(page.minPer, wall.columns
                                        * Math.max(1, Math.ceil(wall.height / wall.cellHeight)))
    readonly property int total: page.siteSize * page.siteLast
    readonly property int pages: Math.max(1, Math.ceil(page.total / page.per))
    // The poster the wall starts on, as an absolute index into the source's list. Keeping
    // the index rather than the page number is what lets a resize keep the same poster on
    // screen instead of jumping off to wherever the new page 3 begins.
    property int first: 0
    readonly property int cur: page.per > 0 ? Math.floor(page.first / page.per) : 0

    property var sitePages: ({})   // site page number -> the items it brought
    property int siteSize: 0       // the site's own page size, read off the first page
    property int siteLast: 0       // the last site page its own paginator admits to
    property string rootUrl: ""    // the url of site page 1 for this list
    property string pageTpl: ""    // a site page url with \u0001 where its number goes
    // Every page number the site itself published a link for, kept across landings: the
    // paginator around page 7 tells us the url of 6 and 8, and those are the site's own
    // words rather than something guessed from a pattern.
    property var numUrl: ({})
    property var shown: []         // the posters of the current screen page
    // Which url was asked for on behalf of which site page. Keying the answer by the url,
    // not by "the last thing I asked for", is what keeps two fetches in flight from filing
    // one page's posters under another's number.
    property var reqUrl: ({})

    // What the paginator of the page that just landed is telling us: which numbers have
    // which urls, and how far the list goes.
    function notePager() {
        for (const p of online.pager) {
            if (p.url.length === 0 || !/^[0-9]+$/.test(p.label))
                continue;
            page.numUrl[p.label] = p.url;
            page.siteLast = Math.max(page.siteLast, parseInt(p.label));
        }
        page.buildTpl();
    }

    // The number's place in a url, found by comparing the site's own links for two
    // different numbers: what differs between those two is the number, so everything
    // before the first difference and after the last one is template. Deriving it beats
    // assuming `?page=`.
    //
    // Any two numbers will do. Requiring specifically "2" and "3" was the bug: the page you
    // are standing on is drawn as the active span with no link at all, so the landing on
    // site page 2 has no "2" to read - and used to write an empty template over the good
    // one page 1 had just handed over. From then on every screen page that needed a third
    // site page had no way to ask for one, which is the empty bottom of a big window.
    function buildTpl() {
        if (page.pageTpl.length > 0)
            return;
        const nums = Object.keys(page.numUrl).map(Number).sort((a, b) => a - b);
        for (let i = 1; i < nums.length && page.pageTpl.length === 0; ++i) {
            const t = page.splitTpl(nums[i - 1], page.numUrl[String(nums[i - 1])],
                                   nums[i], page.numUrl[String(nums[i])]);
            if (t.length > 0)
                page.pageTpl = t;
        }
    }

    function splitTpl(n1, u1, n2, u2) {
        let a = 0;
        while (a < u1.length && a < u2.length && u1[a] === u2[a])
            ++a;
        let b = 0;
        while (b < u1.length - a && b < u2.length - a
               && u1[u1.length - 1 - b] === u2[u2.length - 1 - b])
            ++b;
        const t = u1.slice(0, a) + "\u0001" + u1.slice(u1.length - b);
        // Only a template that puts the printed url back together when the number is
        // substituted counts as one. 2 and 2204 differ by three characters, and the scan
        // above finds no gap between them at all; without this check that partial
        // difference would be kept as the pattern and every page built from it would miss.
        return t.replace("\u0001", String(n1)) === u1 ? t : "";
    }

    function urlFor(n) {
        if (n <= 1)
            return page.rootUrl;
        const known = page.numUrl[String(n)];
        if (known !== undefined)
            return known;
        return page.pageTpl.length > 0 ? page.pageTpl.replace("\u0001", String(n)) : "";
    }

    // A pill, a tab or a source row is a new list: the cache and the position go with the
    // one it replaces.
    function goto(url) {
        page.rootUrl = url;
        page.sitePages = {};
        page.siteSize = 0;
        page.siteLast = 0;
        page.pageTpl = "";
        page.numUrl = {};
        page.shown = [];
        page.first = 0;
        page.reqUrl = {};
        page.reqUrl[url] = 1;
        page.hopToTop();
        online.open(url);
    }

    // Assemble the screen page from the cached site pages, fetching whichever is missing.
    // One fetch at a time: the model answers with a single `items` list, so two in flight
    // could not be told apart. When a page lands, this runs again and either finishes or
    // asks for the next one.
    function fill() {
        if (page.siteSize <= 0)
            return;
        const per = page.per;
        const lastIdx = Math.min(page.first + per, page.total) - 1;
        const out = [];
        let idx = page.first;
        // Both halves matter: the page is full when `out` reaches `per`, and `lastIdx` is
        // what stops a short tail (the last screen page) from running off the list.
        while (idx <= lastIdx && out.length < per) {
            const sp = Math.floor(idx / page.siteSize) + 1;
            const got = page.sitePages[sp];
            if (got === undefined) {
                const u = page.urlFor(sp);
                if (u.length === 0 || page.reqUrl[u] !== undefined)
                    break;          // no way to ask, or it is already on its way
                page.reqUrl[u] = sp;
                online.open(u);
                return;             // fill() runs again when the answer lands
            }
            let j = idx - (sp - 1) * page.siteSize;
            while (j < got.length && out.length < per && idx <= lastIdx) {
                out.push(got[j]);
                ++j;
                ++idx;
            }
            if (j >= got.length && idx <= lastIdx)
                idx = (sp) * page.siteSize;   // step to the first poster of the next site page
        }
        page.shown = out;
    }

    function jumpTo(k) {
        page.first = Math.max(0, Math.min(k, page.pages - 1)) * page.per;
        page.hopToTop();
        page.fill();
    }

    // The chips: the two ends, then the pages that matter - the first, the last, and the
    // current one's neighbours. The shape the site's own paginator uses.
    readonly property var chips: {
        const n = page.pages, c = page.cur + 1;
        const nums = [];
        const add = v => { if (v >= 1 && v <= n && nums.indexOf(v) < 0) nums.push(v); };
        add(1); add(c - 1); add(c); add(c + 1); add(n);
        nums.sort((a, b) => a - b);
        const out = [{ kind: "prev", label: "上一页", live: c > 1, on: false, to: c - 2 }];
        for (const v of nums)
            out.push({ kind: "page", label: String(v), live: v !== c, on: v === c, to: v - 1 });
        out.push({ kind: "next", label: "下一页", live: c < n, on: false, to: c });
        return out;
    }

    // A resize changes how many posters fit, so the page boundary moves under the wall.
    // Snap back to the start of the page the old index fell in - the same posters stay
    // one page apart, rather than the first row being a slice of two.
    onPerChanged: {
        page.first = Math.floor(page.first / page.per) * page.per;
        page.fill();
    }

    Component.onCompleted: {
        online.loadSources();
        // Read off the engine rather than off `liveSources`: this runs in the same tick as
        // the load, and a binding asked for before the list exists keeps that answer.
        const live = online.sources.filter(s => s.enabled);
        if (live.length > 0)
            page.goto(live[0].url);
    }

    Connections {
        target: online
        // `listReady`, not `itemsChanged`: open() also emits itemsChanged - before its
        // request goes out, to clear the category row of the site being left - so the only
        // moment that reliably means "the posters for this url are in hand" is this one.
        function onListReady(url, ok) {
            const sp = page.reqUrl[url];
            if (sp === undefined)
                return;
            delete page.reqUrl[url];
            if (!ok)
                return;   // see the note in fill(): a failed page is retried by the next
                          // click or resize, not by a loop against a site that is refusing
            // A detached copy, not the property read. `online.items` is a live view of the
            // model's own list: caching it meant every later fetch rewrote *every* cached
            // page to the posters that had just arrived. Measured on a 2400 x 1350 window -
            // site page 1 answered with 130096 first and page 2 with 126424, and both cache
            // slots ended up holding 126424, so 13 of the 33 posters on the wall were
            // repeats and the site's order was gone.
            const items = [];
            for (const o of online.items)
                items.push({ title: o.title, poster: o.poster, href: o.href });
            if (items.length === 0)
                return;
            page.sitePages[sp] = items;
            if (page.siteSize === 0) {
                page.siteSize = items.length;
                page.rootUrl = url;
            }
            page.notePager();
            page.fill();
        }
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
                for (const live of online.sources)
                    if (live.enabled) {
                        page.goto(live.url);
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
                        onClicked: page.goto(modelData.url)
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
                    onClicked: page.goto(modelData.url)
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
            // The screen page, not the site's: `page.shown` holds exactly `per` posters.
            model: page.shown

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
                        magPopup.note = "";
                        magPopup.waiting = true;
                        page.detail = modelData;
                        magPopup.open();
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

        // ---- the page hop: this wall's own pages, along the bottom ----
        // The chips are the same shape the site's paginator was drawn with; what changed is
        // what a page is. Its numbers count screen pages (`page.per` posters each), so the
        // row appears only when the list is longer than one screen.
        Row {
            id: pager
            x: Math.max(page.pad, (parent.width - width) / 2)
            y: parent.height - height - page.pad
            spacing: Math.round(page.gap / 2)
            height: Math.round(shellHeight * 0.05)
            visible: page.liveSources.length > 0 && page.pages > 1

            Repeater {
                model: page.chips

                delegate: Rectangle {
                    id: hop
                    required property var modelData
                    // 上一页 on the first page is greyed rather than gone, so the row does
                    // not shift when it is clicked.
                    readonly property bool live: modelData.live === true
                    // A page number is one or two glyphs: at the row's own padding it would
                    // come out a stubby 24 x 36, so a chip is never narrower than it is
                    // tall. 上一页 and 2202 are already wider than that and keep their width.
                    width: Math.max(height, hopText.implicitWidth
                                          + Math.round(shellHeight * 0.028))
                    height: parent.height
                    radius: Theme.controlRadius
                    color: hopMouse.containsMouse && hop.live ? "#2A2A2E" : "#1B1B1F"
                    border.width: 1
                    border.color: modelData.on ? Theme.accent : "#3A3A40"

                    Text {
                        id: hopText
                        anchors.centerIn: parent
                        text: modelData.label
                        color: modelData.on ? Theme.accent
                               : (hop.live ? Theme.textPrimary : "#4A4A4E")
                        font.pixelSize: page.smallFs
                        font.family: Theme.uiFont
                    }
                    MouseArea {
                        id: hopMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: hop.live ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: if (hop.live)
                            page.jumpTo(modelData.to)
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
        // Why the list is empty, in the dialog's own words. It used to be written into
        // online.error, which is the posters' line: the 429 showed up across the page
        // while this dialog was already listing seeds.
        property string note: ""
        // The dialog's own wait: the wall's busy flag is not set by a magnet fetch
        // anymore, so nothing else would say "not yet, hold on".
        property bool waiting: false
        modal: true
        // No dim: the posters behind are the context of the choice, and the wall should
        // not go grey every time one of them is clicked.
        Overlay.modal: Item { }
        // Whole pixels, not `anchors.centerIn`: the overlay is 723 px tall and this card
        // comes out an even height, so centring lands the whole layer on y=159.5. Measured
        // on his own title: the 1 px border was drawn as two columns of (39,39,44) instead
        // of one column of (58,58,64) like the settings card, and every glyph in the list
        // was resampled at that half offset - which is the text shimmering while the bar
        // is dragged, the bar itself having been fixed already.
        readonly property Item ovl: Overlay.overlay
        // The top edge sits where the tallest this panel can get would be centred, and the
        // card only ever grows downward from there. Centring on the *current* height made
        // the whole card jump upward the moment the links landed - measured on 坠落2:
        // waiting 132 px tall with its top at y=342, nine rows 485 px tall with its top at
        // y=165, i.e. 177 px of lurch on every first open. That was the "首次抖", not the
        // text.
        readonly property real maxH: Math.round(page.height * 0.72)
        x: ovl ? Math.round((ovl.width - magPopup.width) / 2) : 0
        y: ovl ? Math.round((ovl.height - maxH) / 2) : 0
        width: Math.round(page.width * 0.5)
        padding: magPad
        // Opened by the poster's click rather than bound to `page.detail`. The binding
        // survived the dismiss - `close()` is a C++ write, which does not break a QML
        // binding - but it only re-evaluates when detail's *value* changes. Clicking the
        // same poster hands back the very same model object, so nothing recomputed and the
        // card stayed shut; a different poster changed the value and opened it again. That
        // is the "must click another card first" he reported. Reproduced: dismiss, click
        // the same poster, no card.
        closePolicy: Popup.CloseOnPressOutside
        onClosed: page.detail = null

        readonly property real magPad: Math.round(shellHeight * 0.028)
        readonly property real magGap: Math.round(shellHeight * 0.019)
        readonly property real magRow: Math.round(shellHeight * 0.064)
        readonly property real titleH: Math.round(page.titleFs * 1.35)
        // The header is the title and the rule, nothing else. The one line that answers
        // the click ("正在取磁力…", "站点回了 429…") used to sit here, which made it look
        // like part of the heading; it belongs to the list under the rule, because that
        // empty area is what it is talking about. headH is counted arithmetically rather
        // than off the Column's implicitHeight: the list's height comes out of the
        // popup's, so a content-driven popup height would be a ring with no solution.
        readonly property real headH: titleH + 2 * magGap + 1
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
                    id: row
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
                    // Each line is placed on a whole pixel rather than centred: the row is
                    // 46 px and a 13 px line box is 19 px tall, so centring leaves every
                    // glyph at y+13.5 and the text is resampled half a pixel off its own
                    // raster - which is what shimmers when the list moves. The three
                    // columns are laid out by hand for the same reason: a Row centres its
                    // children and would put the half pixel straight back.
                    readonly property real nameW: width - 4 - 48 - 56 - 24
                    Text {
                        x: 2
                        width: row.nameW
                        y: Math.round((parent.height - implicitHeight) / 2)
                        text: modelData.label
                        elide: Text.ElideMiddle
                        color: Theme.textPrimary
                        font.pixelSize: page.smallFs
                        font.family: Theme.uiFont
                    }
                    Text {
                        x: 2 + row.nameW + 12
                        width: 48
                        y: Math.round((parent.height - implicitHeight) / 2)
                        text: modelData.size
                        color: Theme.textSecondary
                        font.pixelSize: page.smallFs
                        font.family: Theme.uiFont
                    }
                    Text {
                        x: 2 + row.nameW + 12 + 48 + 12
                        width: 56
                        y: Math.round((parent.height - implicitHeight) / 2)
                        text: "加入下载"
                        color: Theme.accent
                        font.pixelSize: page.smallFs
                        font.family: Theme.uiFont
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

        // The answer to the click, in the list's own area and on the title's left edge:
        // it speaks for the empty space under the rule, not for the heading above it.
        // A sibling of the Column rather than a row of it, so the list keeps its height.
        Text {
            x: 0
            y: magList.y + magPopup.magGap
            width: magList.width
            visible: magPopup.waiting || magPopup.links.length === 0
            elide: Text.ElideRight
            text: magPopup.waiting ? "正在取磁力…"
                : (magPopup.note.length > 0 ? magPopup.note : "这一页没有可用的磁力链接")
            color: magPopup.waiting ? Theme.textSecondary : Theme.accent
            font.pixelSize: page.smallFs
            font.family: Theme.uiFont
        }

        // The list's bar, as a sibling of the Column so the Column does not lay it out
        // as one more row. The Column sits at the popup's origin, so the list's own
        // offsets are already in this coordinate system.
        //
        // Out in the padding, not over the rows: its right edge is 2 px off the card's
        // own border, and the 14 px it needs comes out of the gutter the list never used.
        ThinScrollBar {
            target: magList
            x: magPopup.width - magPopup.magPad - 2 - width
            y: magList.y
            height: magList.height
        }

        Connections {
            target: online
            // The hops answer asynchronously; an empty list would otherwise read as
            // "this poster has nothing", which is not the same as "not yet". A reply for
            // a poster that is no longer the one open is dropped - the guard is the href,
            // because clicking another card while this one is in flight is normal.
            function onMagnetsReady(url, links, note) {
                if (!page.detail || page.detail.href !== url)
                    return;
                magPopup.waiting = false;
                magPopup.links = links;
                magPopup.note = note;
            }
        }
    }

}
