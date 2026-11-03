import QtQuick
import QtQuick.Controls
import QtQuick.Window
import Playerui

// 在线媒体: a magnet-index site as a poster wall. The download jobs used to share this
// page and have a tab of their own beside it now; what is left here is the wall.
//
// Like the library card menu, this page talks to the context object (online) directly;
// the alternative was a signal per action and a handler in Main.qml that only forwarded it.
Item {
    id: page

    property string saveDir: ""
    // Which surface the magnet board uses: the centred card over the wall, or the column
    // the window grows to fit. The setting owns it; the board forks on it.
    property bool magnetPanel: false
    // Which side the column stands on, when there is one.
    property bool magnetRight: false
    // The column is the reason the window is 333 px wider, so opening and closing it is an
    // ask the host answers with the rect.
    signal magnetDockAsked(bool on)
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

    // Leaving the page drops the caret too: coming back to a field still drawn in the
    // accent says the box is waiting for keys the user has long since stopped typing.
    onVisibleChanged: if (!visible) searchBox.releaseFocus()

    // The page's gutter. The media library hands over the same value its own header uses,
    // so the posters start on the pills' line; on its own it reads off the player control
    // row's gap, which is what that alignment is defined by.
    property real pad: Math.round(Theme.refW * Theme.sideInsetWRatio)
    // The distance between two neighbours - pills, chips, posters. Kept apart from `pad` for
    // the same reason the player's row keeps them apart: one is the margin to the window,
    // the other is the gap between things.
    property real gap: Math.round(Theme.refH * Theme.controlGapRatio)
    // Half of the gutter per side: two neighbours sit one gutter apart and the outermost
    // posters still land on the gutter.
    readonly property real inset: Math.round(gap / 2)
    // The block under each poster that holds one or two lines of title, with the same
    // inset above and below it as between the posters side to side.
    readonly property real lineH: Math.round(smallFs * 1.35)
    readonly property real labelBlock: 2 * inset + 2 * lineH
    // How wide a poster is *at least*: 194 px of art, measured off the reference window,
    // where six of them filled a 1282 px page. The window decides how many fit; when what is
    // left over at the end of the row is not enough for one more card, that surplus is eaten
    // by the cards instead of standing as an empty band - the row is divided by the column
    // count the 194 px minimum produced. The surplus is bounded by one card, so a six-column
    // row grows each poster by at most 212/6 = 35 px (194 -> 229 at the worst width), and it
    // falls back to exactly 194 the moment the seventh column fits. At the reference 1282 the
    // division is exact - 1274 / 6 = 212 - so nothing moves there.
    readonly property real artW: 194
    // The search box, as wide as the caption's four tiles: 4 x 40 + the four 6 px gaps
    // around the hairline + the 1 px rule. Both right edges already stand on the page's
    // 13 px inset, so the same width puts this box's left edge on the gear's - the two
    // are the only things on that line, and he asked for them to line up.
    readonly property real searchW: 4 * 40 + 4 * 6 + 1
    readonly property real smallFs: Math.round(0.0180 * Theme.refH)
    readonly property real titleFs: Math.round(0.0194 * Theme.refH)
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

    // A category is still the one being looked at while its own pages are walked: only the
    // page number of a url says "which page", and the two sites hide it in opposite places
    // - one puts it in the query (`.../movies/?page=2`) and keeps the category in the path,
    // the other puts it in the path (`.../dianying/page/2`) and keeps the category in the
    // query (`?tags1=languang`). So the number comes out of both ends and nothing else
    // does. Dropping the whole query - the first version - made every category of one path
    // read as the same list, so all four chips lit at once; dropping only the query made a
    // path number read as a different list, so the first page hop greyed out the very chip
    // being walked.
    function sameList(a, b) {
        if (a.length === 0 || b.length === 0)
            return false;
        return page.listKey(a) === page.listKey(b);
    }

    function listKey(u) {
        const q = u.indexOf("?");
        const path = (q < 0 ? u : u.slice(0, q))
                         .replace(/\/page\/[0-9]+(?=\/|$)/, "")
                         .replace(/\/+$/, "");
        if (q < 0)
            return path;
        const rest = u.slice(q + 1).split("&")
                      .filter(kv => !/^(page|paged)=[0-9]+$/.test(kv));
        return path + (rest.length > 0 ? "?" + rest.join("&") : "");
    }

    // The wall's rows are GridView delegates, and a call made from the click handler there
    // does not resolve anything outside the row: measured, `online.magnets(...)`,
    // `page.askMagnets(...)` and even `host.askMagnets(...)` on a property that `typeof`
    // had just reported as "object" each threw "… is not defined" - so the request never
    // left and the column sat on 正在取链接…. The `page.detail` write on the line above
    // does land (the column draws the clicked film), so the click only writes `detail` and
    // the request is made here, where the page's own scope is not in question.
    onDetailChanged: {
        if (!detail || !detail.href)
            return;
        online.magnets(detail.href);
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
    // `siteSize * siteLast` is what the site's paginator claims, and the claim is a few
    // posters too many: measured on 电影, the paginator's last number is 1414, pages 1..1413
    // carry 20 each and page 1414 carries 11 - 28,271 real posters against 28,280 claimed. At
    // 12 to a screen page that is one page too many, and the last chip then led to a page with
    // nothing in it. Once the tail has been fetched its own length is the number.
    readonly property int total: page.lastLen > 0
            ? (page.siteLast - 1) * page.siteSize + page.lastLen
            : page.siteSize * page.siteLast
    readonly property int pages: Math.max(1, Math.ceil(page.total / page.per))
    // The poster the wall starts on, as an absolute index into the source's list. Keeping
    // the index rather than the page number is what lets a resize keep the same poster on
    // screen instead of jumping off to wherever the new page 3 begins.
    property int first: 0
    readonly property int cur: page.per > 0 ? Math.floor(page.first / page.per) : 0

    property var sitePages: ({})   // site page number -> the items it brought
    property int siteSize: 0       // the site's own page size, read off the first page
    property int siteLast: 0       // the last site page its own paginator admits to
    property int lastLen: 0        // how many posters that last page really carries
    property string rootUrl: ""    // the url of site page 1 for this list
    // What the search box holds, kept next to the box's own text because the box is a
    // control and this is a fact about the list on screen: it is what turns "no posters
    // parsed" into "nothing matched that word".
    property string word: ""
    property string emptyNote: ""
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
            if (parseInt(p.label) > page.siteLast) {
                // A longer list than the one already measured: `lastLen` was the tail of the
                // shorter claim, and keeping it would cut this one short by the same gap.
                page.siteLast = parseInt(p.label);
                page.lastLen = 0;
            }
        }
        // A page with no linked number at all is a list of one: a search answer prints its
        // single page as the active span with no href, and a short category can do the same.
        // Left at zero, `total` came out zero and the wall stayed empty with the posters in
        // hand.
        if (page.siteLast === 0)
            page.siteLast = 1;
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
        page.emptyNote = "";
        page.sitePages = {};
        page.siteSize = 0;
        page.siteLast = 0;
        page.lastLen = 0;
        page.pageTpl = "";
        page.numUrl = {};
        page.shown = [];
        page.first = 0;
        page.reqUrl = {};
        page.reqUrl[url] = 1;
        page.hopToTop();
        online.open(url);
    }

    // A name search on whichever site is open. The origin, not the source's stored url: a
    // search started from a category page asks the same site the category belongs to.
    function search(q) {
        const t = q.trim();
        if (t.length === 0)
            return;
        const m = /^(https?:\/\/[^/]+)/.exec(page.rootUrl);
        if (m === null)
            return;
        page.word = t;
        // Two search shapes: the index site answers on /s/<word>/, the WordPress one on
        // its query string. Both print the same cards, so the wall is no different.
        page.goto(/xbgame\.net/.test(m[1]) ? m[1] + "/?s=" + encodeURIComponent(t)
                                           : m[1] + "/s/" + encodeURIComponent(t) + "/");
    }

    // A pill or a category is a different list, so the word that produced the one being
    // left stops applying: a name still typed in the box that is not what the wall shows
    // reads as a filter that silently did nothing.
    function leaveSearch() {
        page.word = "";
        searchBox.text = "";
        searchBox.releaseFocus();
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
                // Nothing assembled yet and the list has to be asked for: leaving the previous
                // page on the wall is what made a page click read as "the pager moved, the
                // posters did not". The wall's own skeleton covers exactly this gap.
                if (out.length === 0 && page.shown.length > 0)
                    page.shown = [];
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

    // The chips: the two ends as arrows, then the pages that matter - the first, the
    // current one and the two it leads, the last - with a dot chip wherever a jump of more
    // than one page was left out. On the first page of 3674 that reads 1 2 3 … 3674, which
    // is the shape the reference shows; the old row printed the last number bare, so
    // "2 3674" read as two neighbours.
    readonly property var chips: {
        const n = page.pages, c = page.cur + 1;
        const nums = [];
        const add = v => { if (v >= 1 && v <= n && nums.indexOf(v) < 0) nums.push(v); };
        add(1); add(c - 1); add(c); add(c + 1); add(c + 2); add(n);
        nums.sort((a, b) => a - b);
        const out = [{ kind: "prev", icon: "chevronLeft", live: c > 1, on: false, to: c - 2 }];
        let last = 0;
        for (const v of nums) {
            if (last > 0 && v - last > 1)
                out.push({ kind: "gap", label: "…", live: false, on: false, to: 0 });
            out.push({ kind: "page", label: String(v), live: v !== c, on: v === c, to: v - 1 });
            last = v;
        }
        out.push({ kind: "next", icon: "chevronRight", live: c < n, on: false, to: c });
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
            // The site answers a no-hit search with the same page shape and zero rows, so
            // silence here would be a blank wall that reads as a broken fetch.
            if (items.length === 0) {
                if (page.word.length > 0)
                    page.emptyNote = "站点没有搜到「" + page.word + "」，换个词试试。";
                return;
            }
            page.sitePages[sp] = items;
            if (page.siteSize === 0) {
                page.siteSize = items.length;
                page.rootUrl = url;
            }
            // The tail has its own length, and it is usually short: 电影's page 1414 carries 11
            // of the 20 the paginator's arithmetic assumed.
            if (sp === page.siteLast)
                page.lastLen = items.length;
            page.notePager();
            // Ask for the tail as soon as a page says how long the list is - after notePager,
            // which is what fills siteLast in. Left to itself the last chip is arithmetic over
            // a page that may hold 11 posters instead of 20, so the number under the wall
            // changes the moment someone clicks it. One extra request per list, before the click.
            if (sp === 1 && page.siteLast > 1 && page.lastLen === 0) {
                const tu = page.urlFor(page.siteLast);
                if (tu.length > 0 && page.reqUrl[tu] === undefined
                        && page.sitePages[page.siteLast] === undefined) {
                    page.reqUrl[tu] = page.siteLast;
                    online.open(tu);
                }
            }
            if (page.first >= page.total) {
                // The page he asked for turned out to be past the end of the list. Land on the
                // real last one rather than on a wall of nothing.
                page.first = Math.max(0, page.pages - 1) * page.per;
                page.hopToTop();
            }
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
        id: plate
        anchors.fill: parent
        color: "#0A0A0C"

        // Anywhere on the page that is not one of the controls below means "done with the
        // field": the caret's frame is the only thing on screen saying the search box is
        // live, and it must go out when the eye has moved on. A click on an item that does
        // not ask for focus leaves QML's focus alone, so the page asks for it here.
        MouseArea {
            anchors.fill: parent
            onClicked: searchBox.releaseFocus()
        }
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
            height: Math.round(Theme.refH * 0.05)

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
                    width: t.implicitWidth + Math.round(Theme.refH * 0.028)
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
                        onClicked: { page.leaveSearch(); page.goto(modelData.url) }
                    }
                }
            }

        }

        // ---- 搜索 by name, at the right end of the sources' line ----
        // The site's own search box runs `window.open("/s/" + q + "/")` - read off the
        // inline script every page it serves carries - so a search is just one more list
        // url and `goto` does everything else: same wall, same posters, same magnet dialog.
        // What a search answer does not carry is anywhere to go next: its 下一页 is printed
        // `disabled` with no href, and `/s/<q>/page/2/` answers 404. One page, at most 20
        // titles, which is why `notePager` falls back to a single page rather than leaving
        // the total at zero.
        MyField {
            id: searchBox
            readonly property real markS: 10       // the clear mark's own box
            readonly property real markGap: 8      // and its gap to the field's right edge
            x: parent.width - page.pad - width
            y: tabs.y
            width: page.searchW
            height: tabs.height
            fontSize: page.smallFs
            framed: true
            rightInset: markS + 2 * markGap
            placeholder: "搜索片名"
            onCommitted: page.search(text)

            Icon {
                width: searchBox.markS
                height: searchBox.markS
                x: parent.width - searchBox.markGap - width
                anchors.verticalCenter: parent.verticalCenter
                name: "close"
                weight: 1.5
                color: Theme.textSecondary
                // Only while there is something to clear.
                visible: searchBox.text.length > 0
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: searchBox.text = ""
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
            // Up to the search box rather than to the page's gutter: the two share this
            // line, and a long category run has to stop short of it, not run under it.
            width: Math.max(0, searchBox.x - page.gap - x)
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
            // style would put a white groove at its end. A tap on the empty part of the row
            // is a click somewhere else all the same - the ListView would otherwise eat it.
            TapHandler {
                onTapped: searchBox.releaseFocus()
            }

            delegate: Rectangle {
                required property var modelData
                readonly property bool cur: page.sameList(online.currentUrl, modelData.url)
                width: ct.implicitWidth + Math.round(Theme.refH * 0.028)
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
                    onClicked: { page.leaveSearch(); page.goto(modelData.url) }
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
                // A search that came back empty is an answer, not a failure, so it reads
                // before anything the model has to say.
                if (page.emptyNote.length > 0)
                    return page.emptyNote;
                // Only what the wall itself cannot show reaches this any more - a
                // throttled or broken transfer is said on the detail board, in C++.
                if (online.error.length > 0)
                    return online.error;
                if (page.liveSources.length === 0)
                    return "上面还没有可用的在线源，点窗口右上角的齿轮加一个。";
                return "";
            }
        
        }

        GridView {
            id: wall
            // A poster's art is `page.inset` inside its cell on each side, and it is the
            // art's edges that should meet the page's gutter - so the grid starts one inset
            // to the left of it.
            x: page.pad - page.inset
            y: page.contentTop
            width: parent.width - 2 * (page.pad - page.inset)
            // The whole height is the wall's: the page hop is no longer a strip held under
            // it, it is the last thing in the list (see `footer`), so nothing is reserved.
            height: parent.height - y - page.pad
            clip: true
            // The grid stays up whenever it has a source to read from. It used to hide
            // itself for the duration of *any* page-level request, which is what the posters
            // flickered on: a screen page is 12 posters and the site hands out 20, so every
            // hop asks for a second site page, and each of those asks blanked the twelve
            // posters already drawn for as long as the site took to answer (measured on
            // xbgame: 2.07-2.14 s for the 230 KB list page). The skeleton is the right thing
            // to show when there is nothing to show; over a wall that is already drawn it is
            // the flicker.
            visible: page.liveSources.length > 0
            enabled: visible
            // How many cards fit *whole*: the row is divided by this count, so a column is
            // only counted when its own trailing inset is paid for. The old rule gave the
            // last cell 2 * inset of borrowed width, which a fixed card could hide but an
            // elastic one turned into a card of 191 px (below the 194 minimum) and, at the
            // widths in between, five pixels of the last poster clipped by the grid.
            readonly property int columns: Math.max(1, Math.floor(width
                                                                  / (page.artW + 2 * page.inset)))
            cellWidth: Math.floor(width / wall.columns)
            // The poster keeps its own 1.72 ratio and the title block is added below it,
            // rather than the cell being 1.72 with the label squeezed into a leftover 17%
            // - that leftover made the art-to-art distance 46 px while the side-to-side
            // gap was 18.
            cellHeight: Math.round((cellWidth - 2 * page.inset) * 1.72) + page.labelBlock
            boundsBehavior: Flickable.StopAtBounds
            // The screen page, not the site's: `page.shown` holds exactly `per` posters.
            model: page.shown

            // ---- the page hop: this wall's own pages, at the end of the list ----
            // It used to be a strip pinned to the bottom of the page, which cost the wall a
            // row of posters and put the chips over the last row's labels. As the grid's
            // footer it is part of the scrolled content, so it only shows once the list has
            // been run to its end.
            footer: Item {
                id: pagerFooter
                // The shelf the chips sit on. Measured off his second picture, where the two
                // ends keep going: over a 41 px chip the flat top runs 60 px past the group,
                // an S of 32 px drops 34 px, and the low run then runs on to the page edge.
                // Scaled to this row's 36 px chip: 10 above, 53 past, 28 of curve, 30 down.
                readonly property real lineRoom: Math.round(Theme.refH * 0.0138)
                readonly property real overhang: Math.round(Theme.refH * 0.0733)
                readonly property real run: Math.round(Theme.refH * 0.0387)
                readonly property real drop: Math.round(Theme.refH * 0.0415)
                width: wall.width
                height: pager.visible ? pager.height + 2 * page.inset + pagerFooter.lineRoom : 0

                // The wall scrolls in sub-pixel steps, so while it moves this row sits
                // between two device rows. The chips' 1 px borders take that as a spread
                // over both rows, and the line does too - but the line is a Canvas, so it
                // reaches the screen as a texture, and the second row it grows is a dimmer
                // copy (measured while scrolling: the line's own row stayed at 38, and the
                // row under it took 15..21 out of the 10-value page behind it, changing
                // every frame). The chips are geometry and settle back to one row at rest;
                // the line's extra row is what reads as the line shivering under them.
                // Moving both by the same sub-pixel remainder cancels it, so the texture
                // is blitted row for row and the line is drawn the way it is at rest.
                readonly property real snapY: {
                    const dpr = Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1;
                    const abs = wall.y + pagerFooter.y - wall.contentY;
                    return Math.round(abs * dpr) / dpr - abs;
                }

                // ---- the line: flat over the chips, falling away and running out ----
                Canvas {
                    id: shelf
                    x: 0
                    y: page.inset + pagerFooter.snapY
                    width: pagerFooter.width
                    height: pagerFooter.drop
                    visible: pager.visible
                    antialiasing: true

                    // The path is laid out around the chip group, and a page-count change
                    // moves that group without moving this canvas: the canvas is the wall's
                    // whole width and only repaints on its own size. Reading the group's
                    // width through a property is what makes the change a repaint trigger -
                    // without it the drops stayed where the *previous* group ended, which
                    // put them under the arrows.
                    property real groupW: pager.width
                    property real groupX: pager.x
                    onGroupWChanged: requestPaint()
                    onGroupXChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const c = getContext("2d")
                        c.reset()
                        // The two ends stop where the poster art stops. This canvas lives in
                        // the wall's coordinates, and the wall already sits `pad - inset` off
                        // the page, so the card gutter here is the cell's own inset.
                        const gap = page.inset
                        const r = pagerFooter.run
                        const gl = groupX
                        const gr = groupX + groupW
                        // Where the flat top ends and the drop begins, on each side.
                        const fl = Math.max(gap + r + 1, gl - pagerFooter.overhang)
                        const fr = Math.min(width - gap - r - 1, gr + pagerFooter.overhang)
                        const yTop = 0.5
                        const yLow = height - 0.5
                        c.lineWidth = 1
                        c.lineCap = "butt"
                        c.strokeStyle = "#26262A"
                        c.beginPath()
                        c.moveTo(gap, yLow)
                        c.lineTo(fl - r, yLow)
                        c.bezierCurveTo(fl - r * 0.45, yLow, fl - r * 0.45, yTop, fl, yTop)
                        c.lineTo(fr, yTop)
                        c.bezierCurveTo(fr + r * 0.45, yTop, fr + r * 0.45, yLow, fr + r, yLow)
                        c.lineTo(width - gap, yLow)
                        c.stroke()
                        // The two low runs and their drops carry the accent: dim at the page
                        // edge, strongest where the drop meets the flat, and gone by the top
                        // of the drop (measured there: (56,39,32) at the edge, (138,90,2) at
                        // the junction, (31,31,36) - grey - 2 px above it).
                        for (const side of [0, 1]) {
                            const from = side ? width - gap : gap
                            const to = side ? fr + r : fl - r
                            const g = c.createLinearGradient(from, 0, to, 0)
                            g.addColorStop(0, Qt.rgba(1, 0.545, 0, 0.16))
                            g.addColorStop(0.72, Theme.accent)
                            g.addColorStop(1, Qt.rgba(1, 0.545, 0, 0))
                            c.strokeStyle = g
                            c.beginPath()
                            if (side === 0) {
                                c.moveTo(gap, yLow)
                                c.lineTo(fl - r, yLow)
                                c.bezierCurveTo(fl - r * 0.45, yLow, fl - r * 0.45, yTop, fl, yTop)
                            } else {
                                c.moveTo(width - gap, yLow)
                                c.lineTo(fr + r, yLow)
                                c.bezierCurveTo(fr + r * 0.45, yLow, fr + r * 0.45, yTop, fr, yTop)
                            }
                            c.stroke()
                        }
                    }
                }

                Row {
                    id: pager
                    x: Math.max(0, (parent.width - width) / 2)
                    y: page.inset + pagerFooter.lineRoom + pagerFooter.snapY
                    // The reference's gap is 0.39 of a chip's height; at this row's 36 px
                    // that is 14, not the page's 18 px control gap.
                    spacing: Math.round(height * 0.39)
                    height: Math.round(Theme.refH * 0.05)
                    visible: page.liveSources.length > 0 && page.pages > 1

                    Repeater {
                        model: page.chips

                        delegate: Rectangle {
                            id: hop
                            required property var modelData
                            // The left arrow on the first page is greyed rather than gone, so
                            // the row does not shift when a page is clicked.
                            readonly property bool live: modelData.live === true
                            readonly property bool arrow: modelData.icon !== undefined
                            // A square, as the reference's are (44 x 41): the number, the arrow
                            // and the dots all get the same plate, so the row reads as a strip of
                            // chips rather than as words of different lengths. Four digits
                            // (3674) is 28 px of ink in a 36 px plate.
                            width: height
                            height: parent.height
                            radius: Math.round(height * 0.2)
                            // The current page keeps the amber plate, so hovering it must not
                            // paint the grey back over it.
                            color: modelData.on ? "#3A2A0E"
                                   : (hopMouse.containsMouse && hop.live ? "#2A2A2E" : "#1B1B1F")
                            border.width: 1
                            border.color: modelData.on ? Theme.accent : "#3A3A40"

                            Icon {
                                visible: hop.arrow
                                anchors.centerIn: parent
                                // 0.56 of the plate: the glyph's own ink then stands as tall as
                                // the digits beside it (9 px at this row's 13 px text).
                                width: Math.round(hop.height * 0.56)
                                height: width
                                name: modelData.icon || ""
                                color: hop.live ? Theme.textPrimary : "#4A4A4E"
                            }
                            Text {
                                visible: !hop.arrow
                                anchors.centerIn: parent
                                // Arrow chips carry an icon and no label; binding the
                                // missing key straight to text warns on every rebuild.
                                text: modelData.label || ""
                                color: modelData.on ? Theme.accent
                                       : (modelData.kind === "gap" ? Theme.textSecondary
                                          : (hop.live ? Theme.textPrimary : "#4A4A4E"))
                                font.pixelSize: page.smallFs
                                font.family: Theme.uiFont
                            }
                            MouseArea {
                                id: hopMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: hop.live ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: {
                                    searchBox.releaseFocus();
                                    if (hop.live)
                                        page.jumpTo(modelData.to);
                                }
                            }
                        }
                    }
                }
            }

            // The grid is a Flickable, so a click on the empty part of it is consumed here
            // and never reaches the page behind; a tap still means the same thing.
            TapHandler {
                onTapped: searchBox.releaseFocus()
            }

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
                    // One click lists the links behind the poster. Nothing is downloaded
                    // until one of them is chosen.
                    onClicked: {
                        searchBox.releaseFocus();
                        // The column no longer closes on a press elsewhere, so a second press
                        // on the poster it is already showing has nothing to swap. Writing the
                        // same model object would not fire onDetailChanged either, and the
                        // column would sit waiting on a request that never goes out.
                        if (magPopup.visible && page.detail === modelData)
                            return;
                        magPopup.note = "";
                        magPopup.shareNote = "";
                        magPopup.info = {};
                        magPopup.tab = "seed";
                        magPopup.waiting = true;
                        // The verdicts belong to the rows on screen, and the wall has already
                        // dropped the dead ones out of those rows. Carrying them into the next
                        // poster would subtract the same rows from its counts a second time.
                        magPopup.deadHops = [];
                        magPopup.pendingHops = ({});
                        magPopup.hopByUrl = ({});
                        magPopup.pendingHop = "";
                        magPopup.queuedHops = ([]);
                        magPopup.sweeping = false;
                        page.detail = modelData;
                        magPopup.open();
                    }
                }
            }
        }

        // ---- the wait, made visible: the wall's own shape, empty ----
        // The same grid, the same cells, the same gaps - so the page does not move when
        // the posters arrive. The wave runs across the columns so it reads as "fetching"
        // rather than as a picture that failed to load.
        // Only over an empty wall: the posters already on screen are the answer to a request
        // for more of them, and covering them is the flicker, not the feedback. The three
        // dots below stay up for either case.
        Grid {
            id: skeleton
            x: wall.x
            y: wall.y
            columns: wall.columns
            spacing: 0
            visible: online.busy && page.shown.length === 0

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
    }

    // ---- the magnets behind one poster ----
    //
    // One board, two surfaces. The setting picks which: a centred card over the wall, or a
    // 333 px column that the window grows to fit - the same trade the playlist dock makes,
    // so the wall keeps every poster it had and the board takes a strip of its own.
    //
    // The board is shaped like the site's own film page either way: the cover and its info
    // rows first, the download tabs under them, and only then the links. It has one size
    // from the first frame - it used to grow by 177 px when the list landed, and the cover
    // is what holds that space while the hops are still in flight.
    Popup {
        id: magPopup
        // The film's own facts as the detail page printed them: title, poster, the
        // label/value rows, the download tabs with their counts, and the rows behind
        // each tab.
        property var info: ({})
        // Which of the site's tabs the list is showing. The rows of every tab arrive with
        // the page, so moving this is what a tab click does - no second request.
        property string tab: "seed"
        // Why the last netdisk row did not open. It belongs to the tab row's own line, not
        // to the list's empty state: the rows are there, the click is what failed.
        property string shareNote: ""
        // The row whose hop is being followed: the answer arrives on a signal, and by
        // then the click that asked for it is nowhere to be found.
        property string pendingName: ""
        readonly property var lists: info.lists || ({})
        // Rows the drive has said are gone. The list is filtered rather than edited:
        // the page's own answer stays whole, so a row comes back the moment we stop
        // holding the verdict.
        property var deadHops: []
        readonly property var links: (lists[tab] || []).filter(function (r) {
            return magPopup.deadHops.indexOf(r.hop) < 0;
        })
        // The sweep. Every row of this film is asked in the background, one at a time,
        // from the moment the page lands. Nothing is pressed and nothing is said: a row the
        // drive no longer knows simply stops being in the list, and the ledger is what keeps
        // it out next time the poster is opened.
        property bool sweeping: false
        property var queuedHops: ([])     // hops still to ask, in tab order
        property var pendingHops: ({})    // hop: "wait" (hop out) | "ask" (drive out)
        property var hopByUrl: ({})       // the drive's share url -> the row it came from
        property var verifiedHops: ({})   // hops answered alive this run: never asked twice
        property int pendingSince: 0      // when the one row in flight was asked
        property int sweepPauseUntil: 0   // the site said "too often": nothing is asked before this
        property string pendingHop: ""    // the row whose hop is being resolved

        // What a tab has on screen. The site's own number counts rows that are not there.
        function shownCount(key) {
            let n = 0;
            for (const r of (lists[key] || []))
                if (deadHops.indexOf(r.hop) < 0)
                    ++n;
            return n;
        }

        // The sweep covers the tab he is looking at, and starts again when he moves to
        // another one. Asking all five would be 546 hops for a single film - a background
        // pass that spends the site's patience on rows nobody has opened yet, and the rows
        // he does click are verified by the click itself.
        function startSweep() {
            const q = [];
            for (const r of (lists[tab] || [])) {
                if (!r.hop || deadHops.indexOf(r.hop) >= 0)
                    continue;
                if (verifiedHops[r.hop] || pendingHops[r.hop])
                    continue;
                // Asked once, answered reachable: that is written down, so the row never
                // costs the site a hop again.
                if (netdisk.knownAlive(r.hop))
                    continue;
                q.push(r.hop);
            }
            queuedHops = q;
            sweeping = q.length > 0;
        }

        // One row per tick, because each one costs a hop to the site and a call to the
        // drive, and the site paces anyone who asks for a page - measured, the same press
        // that was refused went through on its own a minute later. A row that never answers
        // is let go after half a minute rather than holding the queue open.
        function sweepOne() {
            if (!visible) {
                sweeping = false;
                queuedHops = ([]);
                return;
            }
            if (Date.now() < sweepPauseUntil)
                return;
            const p = pendingHops;
            const k = Object.keys(p);
            if (k.length > 0) {
                if (Date.now() - pendingSince < 30000)
                    return;
                delete p[k[0]];
                pendingHops = p;
            }
            const q = queuedHops.slice();
            if (!q.length) {
                sweeping = false;
                return;
            }
            const hop = q.shift();
            queuedHops = q;
            p[hop] = "wait";
            pendingHops = p;
            pendingSince = Date.now();
            online.shareUrl(hop);
        }

        Timer {
            id: sweepStep
            interval: 1500
            repeat: true
            running: magPopup.sweeping
            onTriggered: magPopup.sweepOne()
        }
        // Why the list is empty, in the dialog's own words. It used to be written into
        // online.error, which is the posters' line: the 429 showed up across the page
        // while this dialog was already listing seeds.
        property string note: ""
        // The dialog's own wait: the wall's busy flag is not set by a magnet fetch
        // anymore, so nothing else would say "not yet, hold on".
        property bool waiting: false

        // ---- the two surfaces ----
        // `dock` is the setting; everything below is a fork on it. The column is the
        // playlist's width so the two side panels agree at any launch size.
        readonly property bool dock: page.magnetPanel
        readonly property bool right: page.magnetRight
        readonly property real dockW: Math.round(Theme.playlistWidthRatio * Theme.refH)
        readonly property Item ovl: Overlay.overlay
        modal: !dock
        // No dim in card mode: the posters behind are the context of the choice, and the
        // wall should not go grey every time one of them is clicked.
        Overlay.modal: Item { }
        // The card is centred off the overlay, on whole pixels, not by `anchors.centerIn`:
        // the overlay is 723 px tall and the card an even height, so centring lands the
        // whole layer on y=159.5 - measured on his own title, the 1 px border came out as
        // two columns of (39,39,44) instead of one of (58,58,64) and every glyph in the
        // list was resampled at that half offset.
        parent: ovl
        width: dock ? dockW : Math.round(Theme.refW * 0.82 / 2) * 2
        height: dock ? Math.min(ovl ? ovl.height : 0, magPopup.fitH)
                     : Math.round(Theme.refH * 0.88 / 2) * 2
        x: dock ? (right ? (ovl ? ovl.width - width : 0) : 0)
                    : (ovl ? Math.round((ovl.width - width) / 2) : 0)
        y: dock ? 0 : (ovl ? Math.round((ovl.height - height) / 2) : 0)
        padding: magPad
        // Opened by the poster's click rather than bound to `page.detail`. The binding
        // survived the dismiss - `close()` is a C++ write, which does not break a QML
        // binding - but it only re-evaluates when detail's *value* changes. Clicking the
        // same poster hands back the very same model object, so nothing recomputed and the
        // card stayed shut; a different poster changed the value and opened it again. That
        // is the "must click another card first" he reported. Reproduced: dismiss, click
        // the same poster, no card.
        // In the card, a press anywhere else is how it is dismissed. In the column it is
        // not: the column is 333 px of the window and the wall behind it stays clickable,
        // so a press on another poster must only replace what the column shows - closing
        // on it was what made the whole window shrink and grow again for nothing. The way
        // out is the cross in the column's own bar.
        closePolicy: dock ? Popup.NoAutoClose : Popup.CloseOnPressOutside
        // The column is the reason the window is 333 px wider, so opening and closing it
        // is what asks the window to grow and give the room back.
        onAboutToShow: if (dock)
                page.magnetDockAsked(true)
        // The room is wanted by being on screen *and* being a column, so choosing a side -
        // or 弹框 - while the board is up asks again. Without this, switching 弹框 to a column
        // left the card drawn over the wall: nothing had told the window to grow.
        onDockChanged: page.magnetDockAsked(visible && dock)
        onClosed: {
            if (dock)
                page.magnetDockAsked(false);
            page.detail = null;
        }

        readonly property real magPad: dock ? Math.round(Theme.refH * 0.0221)
                                            : Math.round(Theme.refH * 0.03)
        // The header's lift out of the card's own 16 px padding, so its top gap becomes the
        // page's gutter (13 px at this size) - the number the tab row is paid with above and
        // below itself.
        readonly property real topLift: dock ? Math.round(Theme.refW * Theme.sideInsetWRatio)
                                               - magPad : 0
        readonly property real barH: dock ? Math.round(Theme.refH * 0.047) : 0
        // The measure behind the label column: the same font the rows draw their label in.
        Text {
            id: metaInk
            visible: false
            text: (magPopup.metaLongest || "") + ":"
            font.pixelSize: page.smallFs
            font.family: Theme.uiFont
        }
        // The cross's own square: the same 34 px the column's bar gives it, so the two
        // surfaces dismiss the same way and take the same room.
        readonly property real crossBox: Math.round(Theme.refH * 0.047)
        // The glyph inside that box, and how far the box has to reach into the padding so
        // the cross's own ink ends on the same inset the text starts on (16 in the column,
        // 22 in the card) instead of 11 px further in. The hit box keeps its 34 px.
        readonly property real crossInk: Math.round(Theme.refH * 0.0249)
        readonly property real crossShift: Math.round(crossBox / 2 - crossInk / 2
                                                      + crossInk * 0.24)
        // Before the detail page lands only the card's own two facts exist: the cover is
        // what holds the block open so nothing moves under the pointer when the rest of
        // the film's rows arrive.
        readonly property var film: info.title ? info : (page.detail || {})
        readonly property var filmMeta: info.meta || []
        readonly property var filmTabs: info.tabs || []
        // The column is a third of the card's width, so the cover and the label gutter
        // both come down; the rows go two lines tall instead of one.
        readonly property real posterW: dock ? 96 : Math.round(Theme.refH * 0.2326)
        readonly property real posterH: dock ? Math.round(96 * 1.72)
                                             : Math.round(Theme.refH * 0.3264)
        readonly property string metaLongest: (filmMeta || []).reduce(function (a, r) {
            return (r.k || "").length > a.length ? (r.k || "") : a;
        }, "")
        readonly property real labelW: dock ? Math.round(metaInk.implicitWidth + 15)
                                            : Math.round(Theme.refH * 0.144)
        readonly property real filmTitleFs: Math.round(Theme.refH * 0.0221)
        readonly property real chipFs: Math.round(Theme.refH * 0.0152)
        readonly property real headGap: Math.round(Theme.refH * 0.019)
        readonly property real tabH: Math.round(Theme.refH * 0.0415)
        readonly property real rowH: dock ? Math.round(Theme.refH * 0.0719)
                                          : Math.round(Theme.refH * 0.0553)
        // The list gets whatever the column has left under it, read off the Column's own
        // layout rather than a second arithmetic of the blocks above it. That arithmetic
        // subtracted a hint line the Column never spends - its Text is invisible while
        // there is nothing to say - so the box stopped 18 px short of the bottom padding
        // and the band under the last row read as 62 px against the 16 px the sides have.
        // `magList.y` comes from the blocks above, none of which read the list's height, so
        // this is not the ring the old comment was afraid of.
        readonly property real listTop: magContent.y + magList.y
        readonly property real aboveH: 2 * magPad + listTop
        readonly property real listAvail: height - aboveH
        // The rows the column has room for: the ones that landed, or the one line the empty
        // state speaks from. While the hops are in flight it reserves what fits down to the
        // window's own bottom, so the wait looks the way it always did and the card only
        // ever pulls its bottom edge up when the rows land - it never grows after them.
        readonly property real fullRows: Math.max(1, Math.ceil(((ovl ? ovl.height : 0)
                                                                - aboveH) / rowH))
        readonly property real shownRows: waiting ? fullRows : Math.max(1, links.length)
        // The column's card is as tall as what it has to show. A four row list used to sit
        // in a box sized for eight and the panel ran the full height of the window, so the
        // band under the last row was 226 px against the 16 px the sides have; now the card
        // ends 16 px under its last row and the strip below it is the window's own black.
        // The top edge does not move, so the rows land where the skeleton's bars were.
        readonly property real fitH: aboveH + shownRows * rowH
        // In the card, whole rows only: the leftover 294 px showed an eighth row as two
        // pixels of chip border under the last full row. In the column the box reaches the
        // padding line, and the bar on the right is what says there is more.
        readonly property real listH: dock ? Math.max(0, Math.min(listAvail, shownRows * rowH))
                                           : Math.floor(listAvail / rowH) * rowH
        // One row of tabs in the card, two in the column: the flow's own height, held to
        // at least one tab line so the underline still has something to sit on.
        // While the links are still coming the tabs are not there to give the row its
        // height, and in the column six of them never fit on one line - so the row is held
        // open at the two lines it comes back to. Without this the list sits 48 px higher
        // while waiting than the rows it is waiting for land at.
        readonly property real tabFlowH: dock && waiting
                                         ? Math.round(2 * tabH + tabRow.spacing)
                                         : Math.max(tabH, tabRow.implicitHeight)

        background: Rectangle {
            radius: magPopup.dock ? 0 : 6
            // Black, the same plate the player and the window's own margin are on: the card
            // was a step lighter than the page behind it, which read as a second surface
            // rather than as the film's detail.
            color: "#000000"
            border.color: "#3A3A40"
            // The column is not a card floating on the page, it is a strip of the window,
            // so it stops drawing an outline: the hairline down its right edge and the one
            // under its last row only said "here is a box" where there is none.
            border.width: magPopup.dock ? 0 : 1
        }

        Column {
            id: magContent
            width: parent.width
            spacing: 0
            // The column's top gap is the page's own gutter: the same 13 px that puts the
            // 本地媒体库 tab row 13 px under the window's top and 13 px above the row under
            // it. The card's padding is 16 on all four sides, so the header is lifted by the
            // difference and the two titles land on one line - measured before this, the
            // column's title ink started 3 px below the tab row's.
            y: magPopup.topLift

            // ---- the column's own head: the title and its way out ----
            // The card needs neither: it is dismissed by clicking anywhere on the wall,
            // and the title is already the biggest thing in its info block.
            Item {
                id: dockBar
                width: parent.width
                height: magPopup.barH
                visible: magPopup.dock

                Text {
                    id: dockTitle
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - height - magPopup.magPad
                    text: magPopup.film.title || ""
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.pixelSize: magPopup.filmTitleFs
                    font.family: Theme.uiFont
                }

                Rectangle {
                    id: dockClose
                    anchors.right: parent.right
                    anchors.rightMargin: -magPopup.crossShift
                    anchors.verticalCenter: parent.verticalCenter
                    width: dockBar.height
                    height: width
                    radius: Theme.controlRadius
                    color: dockCloseArea.containsMouse ? "#2A2A2E" : "transparent"

                    // The same cross the playlist dock's header uses, drawn rather than
                    // typed: a glyph would sit on its baseline and not on the box's centre.
                    // The same drawn cross the card uses - one shape, so the two surfaces
                    // need one shift to land on their text's inset.
                    Icon {
                        anchors.centerIn: parent
                        width: magPopup.crossInk
                        height: width
                        name: "close"
                        color: Theme.textSecondary
                    }
                    MouseArea {
                        id: dockCloseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: magPopup.close()
                    }
                }
                // The 4 px the ✕ reads as "not touching the title" - and the column's own
                // gutter, which is what the title starts on.
                readonly property real tail: magPopup.magPad
            }
            Item { width: 1; height: magPopup.dock ? 10 : 0 }

            // ---- the film: cover on the left, the site's info rows on the right ----
            Item {
                id: head
                width: parent.width
                height: Math.max(magPopup.posterH, infoCol.implicitHeight)

                Rectangle {
                    id: poster
                    width: magPopup.posterW
                    height: magPopup.posterH
                    radius: Theme.controlRadius
                    color: "#0E0E11"
                    clip: true

                    Image {
                        id: posterImage
                        anchors.fill: parent
                        // The wall's own art is the fallback: not every detail page carries a
                        // usable cover (one site prints a hotlink-protected CDN wrapper whose
                        // origin is gone), and the poster that was just clicked is the same
                        // film.
                        source: magPopup.film.poster
                                || (page.detail ? page.detail.poster : "")
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        // `status` alone resolves to the page's own status line, which sits
                        // in this file as an id - so the image would never be shown.
                        visible: posterImage.status === Image.Ready
                    }
                }

                Column {
                    id: infoCol
                    x: poster.width + (magPopup.dock ? 12 : Math.round(Theme.refH * 0.025))
                    // The card's cross lives in this block's top-right corner, so the rows
                    // stop short of it; the column has its own header and needs nothing.
                    width: head.width - x - (magPopup.dock ? 0 : magPopup.crossBox + 8)
                    spacing: Math.round(Theme.refH * 0.0045)

                    Text {
                        width: parent.width
                        // In the column the title is already in its header, and repeating
                        // it would cost the block two of its eight rows.
                        visible: !magPopup.dock
                        text: magPopup.film.title || ""
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.pixelSize: magPopup.filmTitleFs
                        font.family: Theme.uiFont
                    }

                    Repeater {
                        model: magPopup.filmMeta

                        delegate: Item {
                            id: metaRow
                            required property var modelData
                            width: infoCol.width
                            height: Math.max(metaKey.implicitHeight, metaVal.implicitHeight)

                            Text {
                                id: metaKey
                                width: magPopup.labelW
                                text: metaRow.modelData.k + ":"
                                color: Theme.textSecondary
                                font.pixelSize: page.smallFs
                                font.family: Theme.uiFont
                            }
                            Text {
                                id: metaVal
                                x: magPopup.labelW
                                width: metaRow.width - magPopup.labelW
                                text: metaRow.modelData.v
                                // 主演 is the one row that runs long; the site itself cuts
                                // the cast off behind a link, so two lines is the most the
                                // block gives any single row.
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                color: Theme.textPrimary
                                font.pixelSize: page.smallFs
                                font.family: Theme.uiFont
                            }
                        }
                    }
                }

                // ---- the same block, while its rows are still in flight ----
                // The list has drawn a skeleton since the board grew one; the film's own
                // block was the hole that stayed empty until the page landed. The bars use
                // the real block's measures - the cover's square, the label column's x, one
                // line of small text plus the column's spacing per bar - and breathe on the
                // same clock as the list's, so the two halves of the wait look like one.
                Item {
                    id: headSkel
                    anchors.fill: parent
                    visible: magPopup.waiting
                    readonly property real colX: infoCol.x
                    readonly property real pitch: Math.round(Theme.refH * 0.018 * 1.35)
                                                  + Math.round(Theme.refH * 0.0045)
                    readonly property real labelW: Math.round(page.smallFs * 2)
                    readonly property real valueX: Math.round(page.smallFs * 3.4)

                    Rectangle {
                        width: magPopup.posterW
                        height: magPopup.posterH
                        radius: Theme.controlRadius
                        color: "#141418"
                        SequentialAnimation on opacity {
                            running: headSkel.visible
                            loops: Animation.Infinite
                            NumberAnimation { from: 0.45; to: 1; duration: 420
                                                easing.type: Easing.InOutQuad }
                            NumberAnimation { from: 1; to: 0.45; duration: 420
                                              easing.type: Easing.InOutQuad }
                        }
                    }

                    Repeater {
                        model: Math.floor(magPopup.posterH / headSkel.pitch)

                        delegate: Item {
                            id: hsRow
                            required property int index
                            x: headSkel.colX
                            y: index * headSkel.pitch
                            width: head.width - x
                            height: headSkel.pitch
                            readonly property real phase: index * 90

                            SequentialAnimation on opacity {
                                running: headSkel.visible
                                loops: Animation.Infinite
                                PauseAnimation { duration: hsRow.phase }
                                NumberAnimation { from: 0.45; to: 1; duration: 420
                                                    easing.type: Easing.InOutQuad }
                                NumberAnimation { from: 1; to: 0.45; duration: 420
                                                  easing.type: Easing.InOutQuad }
                            }

                            Rectangle {
                                width: headSkel.labelW
                                height: page.smallFs
                                radius: 2
                                color: "#141418"
                            }
                            Rectangle {
                                x: headSkel.valueX
                                width: Math.round(parent.width * 0.55)
                                height: page.smallFs
                                radius: 2
                                color: "#141418"
                                opacity: 0.75
                            }
                        }
                    }
                }
            }

            Item { width: 1; height: magPopup.headGap }
            Rectangle { width: parent.width; height: 1; color: "#26262A" }
            Item { width: 1; height: 6 }

            // ---- the site's own download tabs, counts and all ----
            Item {
                id: tabBar
                width: parent.width
                height: magPopup.tabFlowH

                // A flow, not a row: six of these labels do not fit in 301 px, and the
                // column has to show all six. In the card the same flow never wraps, so
                // the line it draws is the line it always drew.
                Flow {
                    id: tabRow
                    width: parent.width
                    spacing: magPopup.dock ? Math.round(Theme.refH * 0.0249)
                                           : Math.round(Theme.refH * 0.036)

                    Repeater {
                        // A tab with 0 behind it has no rows to show. It used to sit there
                        // greyed out, which was one more thing to look past.
                        model: (magPopup.filmTabs || []).filter(function (t) {
                            return magPopup.shownCount(t.key) > 0;
                        })

                        delegate: Item {
                            id: tabItem
                            required property var modelData
                            readonly property bool active: modelData.key === magPopup.tab
                            // A tab whose count came out 0 has no rows behind it, and the
                            // site greys those for the same reason.
                            readonly property bool live: magPopup.shownCount(modelData.key) > 0
                            width: tabText.implicitWidth
                            height: magPopup.tabH

                            Text {
                                id: tabText
                                anchors.verticalCenter: parent.verticalCenter
                                text: tabItem.modelData.name + "("
                                      + magPopup.shownCount(tabItem.modelData.key) + ")"
                                color: tabItem.active ? Theme.textPrimary
                                       : (tabItem.live ? Theme.textSecondary : "#55555C")
                                font.pixelSize: page.smallFs
                                font.family: Theme.uiFont
                            }
                            Rectangle {
                                visible: tabItem.active
                                anchors.bottom: parent.bottom
                                width: tabText.implicitWidth
                                height: 2
                                radius: 1
                                color: Theme.accent
                            }
                            // The label is only a line of glyphs wide, so the hit area takes
                            // the gap between neighbours as its own: half of the row's
                            // spacing on each side, which is exactly where the next tab's
                            // half begins.
                            MouseArea {
                                anchors.fill: parent
                                anchors.leftMargin: -Math.round(Theme.refH * 0.018)
                                anchors.rightMargin: -Math.round(Theme.refH * 0.018)
                                hoverEnabled: true
                                enabled: tabItem.live
                                cursorShape: tabItem.live ? Qt.PointingHandCursor
                                                         : Qt.ArrowCursor
                                onClicked: {
                                    magPopup.tab = tabItem.modelData.key;
                                    magPopup.shareNote = "";
                                    magPopup.startSweep();
                                }
                            }
                        }
                    }

                }

                // One line for the whole list instead of a button on every row. It says
                // what the tab you are looking at does: a 磁力 row starts a download, a
                // netdisk row has nothing to download until its share page is opened.
                // The column gives it a line of its own below, so it is not drawn twice.
                Text {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    // Only the hop page's own note. The sentence said what the row already
                    // says and was the last piece of copy on the panel.
                    visible: !magPopup.dock && magPopup.shareNote.length > 0
                    text: magPopup.shareNote
                    color: Theme.accent
                    // The tabs take the left of this row, so the sentence gets what is left
                    // and cuts in the middle rather than running under the list's scrollbar.
                    width: Math.min(implicitWidth, tabBar.width - tabRow.implicitWidth - 12)
                    elide: Text.ElideMiddle
                    font.pixelSize: page.smallFs
                    font.family: Theme.uiFont
                }
            }

            Item { width: 1; height: 6 }

            // The column's copy of the note, under the tabs.
            Text {
                id: magHint
                width: parent.width
                visible: magPopup.dock && (magPopup.waiting || magPopup.shareNote.length > 0)
                elide: Text.ElideRight
                // A space while waiting keeps the line the height it will have once the
                // rows land, so the list does not jump under them.
                text: magPopup.waiting ? " " : magPopup.shareNote
                color: Theme.accent
                font.pixelSize: page.smallFs
                font.family: Theme.uiFont
            }
            Item {
                width: 1
                height: magHint.visible ? 4 : 0
            }

            ListView {
                id: magList
                width: parent.width
                height: magPopup.listH
                clip: true
                model: magPopup.links
                boundsBehavior: Flickable.StopAtBounds

                delegate: Item {
                    id: row
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: magPopup.rowH

                    // Hover is a band behind the row, not a plate around it: the whole row
                    // is the target, so the band is what has to reach the edges.
                    Rectangle {
                        anchors.fill: parent
                        color: mh.containsMouse ? "#1B1B1F" : "transparent"
                    }
                    // The size is the site's, the badges and the age are the same row's
                    // facts, and the columns are laid out by hand so every line sits on a
                    // whole pixel. In the column a row that has badges or an age gives the
                    // name the whole first line and moves the facts under it; a row with
                    // neither - the netdisk ones - stays one line, name then size.
                    readonly property bool narrow: magPopup.dock
                    readonly property real sizeW: 52
                    readonly property real featW: narrow ? width - 16 - sizeW - 8 - timeW
                                                         : 128
                    readonly property real timeW: 56
                    readonly property real nameW: width - 16 - 8 - sizeW - 8 - featW - 8 - timeW
                    readonly property bool hasTime: (modelData.time || "").length > 0
                    readonly property bool hasFeats: (modelData.features || []).length > 0
                    readonly property bool hasSize: (modelData.size || "").length > 0
                    readonly property bool twoLine: narrow && (hasFeats || hasTime)
                    // What the name line gives up to the columns to its right - only the ones
                    // this row actually carries. A netdisk row has no size and no date, and
                    // the fixed 52 + 64 it used to hand back is why its name turned into "…"
                    // two characters early with the right edge still black.
                    readonly property real tailW: (hasSize ? 8 + sizeW : 0)
                                                  + (hasTime ? 8 + timeW : 0)
                    readonly property real line1: !twoLine
                                                  ? Math.round((height - page.smallFs * 1.35) / 2)
                                                  : (narrow ? Math.round(Theme.refH * 0.0083)
                                                            : Math.round((height - page.smallFs * 1.35) / 2))
                    readonly property real line2: Math.round(height - page.smallFs * 1.35 - 4)
                    // An id is not a property of the row, so the measure is handed up as
                    // one before the name's width and the size's x read it back.
                    readonly property real nameInk: nameText.implicitWidth
                    // What the name line actually gets, after the row's own arithmetic.
                    readonly property real nameShown: Math.min(nameInk, width - tailW)
                    // What the name actually measures, so a one-line row can put the size
                    // right after the glyphs instead of a fixed column out where the name
                    // may never reach.
                    Text {
                        id: nameText
                        visible: false
                        text: modelData.title || modelData.label
                        font.pixelSize: page.smallFs
                        font.family: Theme.uiFont
                    }
                    Text {
                        // Flush with the tab row above: the panel's own 16 px is already
                        // in the content area, so the row adds nothing of its own.
                        x: 0
                        // `width` alone here is this Text's own width, not the row's: the
                        // binding then reads itself and the label comes out empty. Row's.
                        width: !narrow ? row.nameW
                             : (row.twoLine ? row.width
                                            : row.nameShown)
                        y: row.line1
                        text: modelData.title || modelData.label
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.pixelSize: page.smallFs
                        font.family: Theme.uiFont
                    }
                    Text {
                        x: (!narrow ? row.nameW + 8
                                    : (row.twoLine ? 0 : row.nameShown + 8))
                        y: row.twoLine ? row.line2 : row.line1
                        width: row.sizeW
                        text: modelData.size || ""
                        color: Theme.textSecondary
                        font.pixelSize: page.smallFs
                        font.family: Theme.uiFont
                    }
                    Row {
                        id: feats
                        x: (narrow ? 0 : row.nameW + 8) + row.sizeW + 8
                        y: row.twoLine ? row.line2 - 2 : row.line1
                        width: row.featW
                        height: narrow ? Math.round(magPopup.chipFs * 1.5) : row.height
                        spacing: 4

                        Repeater {
                            model: row.modelData.features
                                   ? row.modelData.features.slice(0, 3) : []

                            delegate: Rectangle {
                                required property var modelData
                                y: Math.round((feats.height - height) / 2)
                                width: chipText.implicitWidth + Math.round(magPopup.chipFs * 0.55)
                                height: Math.round(magPopup.chipFs * 1.5)
                                radius: 2
                                color: "transparent"
                                border.width: 1
                                border.color: "#3F6FA8"

                                Text {
                                    id: chipText
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: Theme.cool
                                    font.pixelSize: magPopup.chipFs
                                    font.family: Theme.uiFont
                                }
                            }
                        }
                    }
                    Text {
                        anchors.right: parent.right
                        width: row.timeW
                        y: row.twoLine ? row.line2 : row.line1
                        text: modelData.time || ""
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideLeft
                        color: "#8A8A90"
                        font.pixelSize: page.smallFs
                        font.family: Theme.uiFont
                    }
                    MouseArea {
                        id: mh
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (modelData.magnet) {
                                // The card picture comes along with the seed, so the row is
                                // recognisable before a single byte has landed.
                                downloads.add(modelData.magnet, page.saveDir,
                                              page.detail ? page.detail.poster : "");
                                page.detail = null;
                                page.jobsRequested();
                            } else if (modelData.kind === "quark"
                                       || modelData.kind === "baidu"
                                       || modelData.kind === "xunlei"
                                       || modelData.kind === "uc") {
                                // All three end in an address the app can use on its own: 夸克
                                // through the cookies of the browser we drive, 百度 through the
                                // share page's own tab for the one call its edge refuses to
                                // answer us, 迅雷 by
                                // walking the share here and letting its page ask for the address
                                // (the captcha token is signed per action - measured). The rest
                                // still just open a browser.
                                magPopup.shareNote = modelData.kind === "quark"
                                        ? "正在问夸克要地址…"
                                        : (modelData.kind === "baidu" ? "正在问百度要地址…"
                                                                      : (modelData.kind === "uc"
                                                                         ? "正在问 UC 要地址…"
                                                                         : "正在问迅雷要地址…"));
                                magPopup.pendingName = modelData.title || modelData.label;
                                // The row's own id, kept so the verdict can be written under
                                // the key the wall filters on, not only the share's.
                                magPopup.pendingHop = modelData.hop;
                                online.shareUrl(modelData.hop);
                            } else {
                                // A netdisk row: its share address only exists on the hop
                                // page, so this asks for it and the answer opens a browser.
                                magPopup.shareNote = "";
                                online.openShare(modelData.hop);
                            }
                        }
                    }
                }
            }
        }

        // ---- the card's own way out ----
        // The column has had a cross in its bar; the card answered only to a press outside
        // it, which is not something the screen says anywhere. Its right and top gaps are
        // the card's own padding, the same 22 px the text starts on.
        Rectangle {
            id: cardClose
            visible: !magPopup.dock && magPopup.opened
            anchors.right: magContent.right
            anchors.rightMargin: -magPopup.crossShift
            anchors.top: magContent.top
            width: magPopup.crossBox
            height: width
            radius: Theme.controlRadius
            color: cardCloseHit.containsMouse ? "#2A2A2E" : "transparent"

            Icon {
                anchors.centerIn: parent
                width: magPopup.crossInk
                height: width
                name: "close"
                color: Theme.textSecondary
            }
            MouseArea {
                id: cardCloseHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: magPopup.close()
            }
        }

        // The answer to the click, in the list's own area: it speaks for the empty space
        // under the tabs, not for the film above them. A sibling of the Column rather than
        // a row of it, so the list keeps its height.
        Text {
            x: 0
            y: magPopup.listTop + 8
            width: magList.width
            visible: !magPopup.waiting
                     && (magPopup.links.length === 0 || magPopup.note.length > 0)
            elide: Text.ElideRight
            text: magPopup.note.length > 0 ? magPopup.note
                  : (magPopup.tab === "seed" ? "这一页没有可用的磁力链接"
                                             : "这个来源没有链接")
            color: Theme.accent
            font.pixelSize: page.smallFs
            font.family: Theme.uiFont
        }

        // ---- the wait, in the shape of what is coming ----
        // Not a sentence: the rows have a shape, and drawing it means the answers land into
        // placeholders that were already the size of a row, so nothing under the pointer
        // moves when they do. Every measure here is the real row's own - the bullet at x=2,
        // the name line on `line1`, the size line on its second line, one `rowH` apart - and
        // it breathes the way the wall's skeleton does (0.45 -> 1 -> 0.45 over 420 ms, one
        // phase step per row).
        Item {
            id: magSkel
            x: magList.x
            y: magPopup.listTop
            width: magList.width
            height: magPopup.listH
            visible: magPopup.waiting
            // What the name line has left of the row once the columns it does not carry are
            // paid for - the same arithmetic the row itself uses.
            readonly property real nameW: magPopup.dock
                                          ? width - 16 - 8 - 56
                                          : width - 16 - 8 - 52 - 8 - 128 - 8 - 56

            Repeater {
                model: Math.floor(magSkel.height / magPopup.rowH)

                delegate: Item {
                    id: skRow
                    required property int index
                    y: index * magPopup.rowH
                    width: magSkel.width
                    height: magPopup.rowH
                    readonly property bool narrow: magPopup.dock
                    readonly property real line1: narrow
                                                  ? Math.round(Theme.refH * 0.0083)
                                                  : Math.round((height - page.smallFs * 1.35) / 2)
                    readonly property real phase: index * 90

                    SequentialAnimation on opacity {
                        running: magSkel.visible
                        loops: Animation.Infinite
                        PauseAnimation { duration: skRow.phase }
                        NumberAnimation { from: 0.45; to: 1; duration: 420
                                            easing.type: Easing.InOutQuad }
                        NumberAnimation { from: 1; to: 0.45; duration: 420
                                          easing.type: Easing.InOutQuad }
                    }

                    Rectangle {
                        x: 0
                        y: skRow.line1
                        width: Math.round(magPopup.dock ? magSkel.width : magSkel.nameW)
                        height: page.smallFs
                        radius: 2
                        color: "#141418"
                    }
                    Rectangle {
                        visible: skRow.narrow
                        x: 0
                        y: Math.round(skRow.height - page.smallFs * 1.35 - 4)
                        width: 52
                        height: page.smallFs
                        radius: 2
                        color: "#141418"
                        opacity: 0.75
                    }
                }
            }
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
            y: magPopup.listTop
            height: magList.height
        }

        Connections {
            target: online
            // The hops answer asynchronously; an empty list would otherwise read as
            // "this poster has nothing", which is not the same as "not yet". A reply for
            // a poster that is no longer the one open is dropped - the guard is the href,
            // because clicking another card while this one is in flight is normal.
            function onMagnetsReady(url, links, note, detail) {
                if (!page.detail || page.detail.href !== url)
                    return;
                magPopup.waiting = false;
                magPopup.note = note;
                magPopup.info = detail;
                // A film with no magnets and 74 夸克 rows should open on the tab that has
                // something rather than on an empty 磁力 list.
                magPopup.startSweep();
                if (!magPopup.links.length) {
                    const ls = detail.lists || {};
                    for (const t of (detail.tabs || []))
                        if ((ls[t.key] || []).length > 0) {
                            magPopup.tab = t.key;
                            break;
                        }
                }
            }
            function onShareFailed(note) {
                // While the sweep has a row in flight, a refusal is the site pacing a
                // background ask - not something he did, and not a line to show him. The row
                // stays, and the sweep sits out a minute so the site's patience is not spent
                // on rows nobody is looking at.
                const k = Object.keys(magPopup.pendingHops);
                if (k.length > 0) {
                    const p = magPopup.pendingHops;
                    delete p[k[0]];
                    magPopup.pendingHops = p;
                    magPopup.sweepPauseUntil = Date.now() + 60000;
                    return;
                }
                magPopup.shareNote = note;
            }
            function onShareUrlReady(hop, url) {
                if (magPopup.pendingHops[hop] === "wait") {
                    const hb = magPopup.hopByUrl;
                    hb[url] = hop;
                    magPopup.hopByUrl = hb;
                    const p = magPopup.pendingHops;
                    p[hop] = "ask";
                    magPopup.pendingHops = p;
                    netdisk.check(url);
                    return;
                }
                // The poster's own title, not the download row's label: a site row is named
                // like a release notice - title, then 【WEB-4K】, then the subtitle and
                // watermark notes - and only the first part is the film.
                const hb = magPopup.hopByUrl;
                hb[url] = hop;
                magPopup.hopByUrl = hb;
                netdisk.download(url, magPopup.film.title || magPopup.pendingName,
                                 page.detail ? page.detail.poster : "");
            }
        }

        // The ladder's three states, said on the row he clicked. 转存 is the slow part - a big
        // copy is not readable until the drive has materialised it - and until the url is in
        // hand there is nothing in the download list to point at, so silence here reads as a
        // dead button.
        Connections {
            target: netdisk
            function onQueued(key, name, poster) {
                // 转存 is only part of the 夸克 chain. 百度 is read in place - nothing is
                // copied to the drive - so the same words would promise a copy that never comes.
                // 迅雷 walks its share and then asks its own page for the address, so it is
                // "问地址" too - slower than 百度 by one page visit per folder.
                magPopup.shareNote = key.indexOf("pan.baidu.com") >= 0
                        ? "正在问百度要地址…"
                        : (key.indexOf("pan.xunlei.com") >= 0 ? "正在问迅雷要地址…"
                                                              : "正在转存到网盘…");
            }
            function onReady(key, url, headers, name, poster) {
                // Stay on the board: a 网盘 share is the same film in a dozen encodes, and
                // the pull now runs N ways, so queueing a second row is the normal next click.
                // This is also what a magnet row does when it is added.
                magPopup.shareNote = "";
                page.jobsRequested();
            }
            function onChecked(url, dead, code) {
                const hop = magPopup.hopByUrl[url] || "";
                const p = magPopup.pendingHops;
                if (hop)
                    delete p[hop];
                magPopup.pendingHops = p;
                if (!hop)
                    return;
                if (!dead) {
                    // Reachable is remembered in the ledger, not just for this run: the row
                    // has been asked, the answer is in the book, and the next opening of this
                    // film asks nothing at all. If it dies later, the click that finds out
                    // writes the other verdict.
                    netdisk.markAlive(hop, url);
                    const v = magPopup.verifiedHops;
                    v[hop] = true;
                    magPopup.verifiedHops = v;
                }
            }

            // The ledger gained a row. This is the one door every drive's "gone" answer
            // comes through - the sweep's and the ladder a click started - so a share he
            // clicked and found missing goes the same silent way, and does not come back.
            function onDead(url, code) {
                const hop = magPopup.hopByUrl[url] || "";
                if (!hop || magPopup.deadHops.indexOf(hop) >= 0)
                    return;
                netdisk.markDead(hop, code);
                magPopup.deadHops = magPopup.deadHops.concat([hop]);
            }
            function onFailed(key, note) {
                // The row is not marked from here: when the drive says there is no such
                // share, noteDead() has already put it in the ledger and said onDead().
                // Matching the wording of four drives' failure sentences was the old way,
                // and it only ever worked for the ones whose sentence had been read.
                magPopup.shareNote = note;
            }
        }
    }

}
