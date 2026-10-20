import QtQuick
import Playerui

Window {
    id: win
    visible: true
    // One window now: the picture is a layer in here. Qt is told it is frameless so it
    // stops reserving a caption of its own - measured, with the frame left to Qt the
    // client still started 31 px below the window top because Qt compensates for margins
    // it cached at creation. The frame bits go back on the Win32 side in adopt(), and
    // WM_NCCALCSIZE there puts the client over them.
    flags: Qt.Window | Qt.FramelessWindowHint
    width: shellWidth
    height: shellHeight
    x: shellX
    y: shellY
    color: "#000000"
    title: playerCore.mediaTitle.length ? playerCore.mediaTitle : "Playerui"
    readonly property real vw: width
    readonly property real vh: height
    // The margin in from the window's left and right edges. Measured against the width
    // the window *started* at, not the one it has now: the chrome under it (the bar, the
    // glyphs, the time) is pinned to the launch size too, and a 13 px margin that became
    // 39 px on the way to fullscreen was the only part of that row still scaling.
    readonly property real inset: Theme.sideInsetWRatio * shellWidth
    property bool uiVisible: true
    property string page: shellMode
    property int rotateIdx: 0
    property int aspectIdx: 0
    readonly property var rotateSteps: [0, 90, 180, 270]
    readonly property var aspectSteps: ["-1", "16:9", "4:3", "2.35:1", "2.40:1"]
    readonly property var speedSteps: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0]
    // Which closure the open dropdown hands its index to. The picker itself only
    // knows about rows, not about what a row means.
    property var pickerApply: null

    // The header line's type size. He asked for "a bit bigger" than the bump box
    // without a number, so it is one stated factor off it: 1.25 x 11 = 13.75 px.
    readonly property real headerFont: caption.artSize * 1.25

    function nearest(list, v) {
        for (let k = 0; k < list.length; ++k) {
            if (Math.abs(list[k] - v) < 0.01)
                return k;
        }
        return -1;
    }

    // "1.00" reads as a decimal place nobody asked for; "1" is the rate.
    function speedLabel(v) {
        const s = (Math.round(v * 100) / 100).toFixed(2)
        const t = s.indexOf(".") < 0 ? s : s.replace(/0+$/, "").replace(/\.$/, "")
        return t + "×"
    }

    function pickRotate(i) {
        win.rotateIdx = i;
        playerCore.setPropertyInt("video-rotate", win.rotateSteps[i]);
    }

    function pickAspect(i) {
        win.aspectIdx = i;
        playerCore.setPropertyString("video-aspect-override", win.aspectSteps[i]);
    }

    // The three cells that hold one of several values share this one dropdown.
    function openPicker(name, item) {
        tip.hide();
        if (name === "speed") {
            picker.show(item, win.speedSteps.map(v => win.speedLabel(v)),
                        win.nearest(win.speedSteps, playerCore.speed));
            win.pickerApply = (i) => playerCore.setSpeed(win.speedSteps[i]);
        } else if (name === "aspect") {
            picker.show(item, win.aspectSteps.map(s => s === "-1" ? "自动" : s), win.aspectIdx);
            win.pickerApply = (i) => win.pickAspect(i);
        } else if (name === "rotate") {
            picker.show(item, win.rotateSteps.map(d => d + "°"), win.rotateIdx);
            win.pickerApply = (i) => win.pickRotate(i);
        } else if (name === "cc" || name === "audioTrack") {
            const tracks = name === "cc" ? playerCore.subTracks : playerCore.audioTracks;
            if (tracks.length === 0)
                return;
            let cur = -1;
            for (let i = 0; i < tracks.length; ++i) {
                if (tracks[i].selected)
                    cur = i;
            }
            picker.show(item, tracks.map(t => t.label), cur);
            win.pickerApply = (i) => playerCore.selectTrack(name === "cc" ? "sub" : "audio",
                                                            tracks[i].id);
        }
    }
    readonly property bool keepUi: shellKeepUi === true

    function fmt(t) {
        if (!isFinite(t) || t < 0)
            t = 0;
        const s = Math.floor(t % 60), m = Math.floor(t / 60) % 60, h = Math.floor(t / 3600);
        const p = (v, n) => String(v).padStart(n, "0");
        return h > 0 ? h + ":" + p(m, 2) + ":" + p(s, 2) : p(m, 1) + ":" + p(s, 2);
    }

    // Where the chapter edges are. Real file data only: the bar's ticks, the two chapter
    // buttons and the drawer all read this, and it used to fall back to a hardcoded table
    // of eighteen fractions when the file had no chapters - which put a "Chapter 9" and a
    // highlighted drawer row on screen for a file that has none. A chapter-less file now
    // shows no chapters; the two buttons go grey where that leaves nothing to jump to.
    readonly property var edgeTimes: playerCore.chapterStarts

    // The names, resolved once: what the file calls each chapter, or its position in the
    // file's own list when it names nothing. One place computes it, the header line, the
    // hover bubble and the drawer all read the same array.
    readonly property var chapterLabels: {
        const out = [];
        for (let i = 0; i < playerCore.chapterTitles.length; ++i) {
            const t = playerCore.chapterTitles[i];
            out.push(t.length ? t : "章节 " + (i + 1));
        }
        return out;
    }
    function chapterName(i) {
        return win.chapterLabels.length > i ? win.chapterLabels[i] : "";
    }
    // mpv reports -1 when the file has no chapter under the play head.
    readonly property bool hasChapters: playerCore.chapterStarts.length > 0
    // What the library calls the file that is loaded right now - non-empty only where a
    // card was renamed. The player's own title comes from the file name, so without this
    // a renamed card would still play under its release name.
    //
    // `libRev` is the dependency that makes it re-read: the card can be renamed while
    // that same film is already loaded, and 返回播放 comes back with an unchanged
    // mediaUrl - a binding that only watches the url would keep the old name on screen.
    property int libRev: 0
    Connections {
        target: library
        function onRowsChanged() { win.libRev++ }
    }
    readonly property string libraryTitle: {
        win.libRev;                   // read for the dependency, not for the value
        return library.titleForPath(playerCore.mediaUrl);
    }
    function edgeList() {
        return win.edgeTimes;
    }

    // An edge within two seconds is treated as "already there", so the button
    // carries on to the following one instead of nudging back and forth.
    function edgeAt(pos, dir) {
        const e = win.edgeList();
        if (e.length === 0)
            return pos;
        if (dir > 0) {
            for (let i = 0; i < e.length; ++i)
                if (e[i] > pos + 2)
                    return e[i];
            return playerCore.duration;
        }
        for (let i = e.length - 1; i >= 0; --i)
            if (e[i] < pos - 2)
                return e[i];
        return 0;
    }

    function chapterAt(frac) {
        const e = win.edgeList();
        const t = frac * playerCore.duration;
        let i = 0;
        for (let k = 0; k < e.length; ++k) {
            if (t >= e[k])
                i = k;
        }
        return i;
    }

    function setVol(v) {
        playerCore.setVolume(Math.min(130, Math.max(0, v)));
    }

    // Single dispatch for every transport command: the control row's cells and
    // the keyboard shortcuts both land here, so the two cannot drift apart.
    function act(what) {
        console.log("action ->", what);
        picker.hide();
        const d = playerCore.duration;
        if (what === "play" || what === "pause") playerCore.togglePause();
        else if (what === "back10") playerCore.seekTo(Math.max(0, playerCore.position - 10));
        else if (what === "fwd10") playerCore.seekTo(Math.min(d, playerCore.position + 10));
        else if (what === "prevChapter") playerCore.seekTo(win.edgeAt(playerCore.position, -1));
        else if (what === "nextChapter") playerCore.seekTo(win.edgeAt(playerCore.position, +1));
        else if (what === "collapse") win.toggleFs();
        else if (what === "pip") videoHost.toggleMini();
        else if (what === "menu") win.page = "library";
        else if (what === "cc") playerCore.cycleProperty("sid");
        else if (what === "audioTrack") playerCore.cycleProperty("aid");
        else if (what === "mute" || what === "volume") playerCore.cycleProperty("mute");
        else if (what === "chapters") chapters.open = !chapters.open;
        win.touch();
    }

    // --- right-click menu -------------------------------------------------
    // The row of icons under the bar, offered as a menu on the picture. The rows come
    // off the row's own tier orders instead of a second written-out list, so the menu
    // can never promise a command the bar has dropped for this window size - and the
    // mini window, which carries seven icons, gets seven rows rather than sixteen that
    // would hang off its 270 px.
    readonly property var menuNames: controls.leftOrder[controls.tier]
            .concat(controls.centerOrder[controls.tier],
                    controls.rightOrder[controls.tier])

    function menuLabel(name) {
        if (name === "play")
            return playerCore.paused ? "播放" : "暂停";
        const t = {
            menu: "打开媒体库", cast: "投屏", cc: "字幕", volume: "静音",
            prevChapter: "上一个章节", nextChapter: "下一个章节",
            back10: "后退 10 秒", fwd10: "前进 10 秒", audioTrack: "音轨",
            chapters: "章节列表", rotate: "画面旋转", aspect: "画面比例",
            speed: "倍速播放", pip: "小窗播放", collapse: "全屏"
        };
        return t[name];
    }

    // The icon's own `live` rule: grey, still listed, still readable - only the click
    // is withheld, because there is nothing for it to do.
    function menuLive(name) {
        if (name === "cast")
            return false;                       // 投屏 is not wired up
        if (name === "cc")
            return playerCore.subTracks.length > 0;
        if (name === "audioTrack")
            return playerCore.audioTracks.length > 0;
        if (name === "prevChapter" || name === "nextChapter")
            return win.hasChapters;
        return true;
    }

    // Built when the menu opens, from the values live at that moment. Not a binding:
    // a binding whose rows come out of function calls captures the dependencies of
    // whatever it read the first time, and a paused film would keep "暂停".
    function menuList() {
        const names = win.menuNames;
        const out = [];
        for (let i = 0; i < names.length; ++i)
            out.push({ label: win.menuLabel(names[i]), action: names[i],
                       on: win.menuLive(names[i]) });
        return out;
    }

    function openMenuAt(px, py) {
        tip.hide();
        picker.hide();
        chapters.open = false;
        // The menu is a transport surface, so it brings the bar with it: the dropdowns
        // it hands off to live inside the chrome, and would be invisible at opacity 0.
        win.uiVisible = true;
        playerMenu.showAt(px, py, win.menuList(), null);
        win.touch();
    }

    function menuPick(what) {
        // The five cells that hold one of several values open the same dropdown the icon
        // opens, from the same glyph, so the menu cannot offer a different set of choices
        // than the bar does. Every other row is a command the keys already send.
        if (what === "cc" || what === "audioTrack" || what === "rotate"
                || what === "aspect" || what === "speed") {
            const glyph = controls.cells[what];
            if (glyph)
                win.openPicker(what, glyph);
            return;
        }
        win.act(what);
    }

    // The "big view" for F: true fullscreen in one step, under a cover that is a copy
    // of the screen taken before the move and comes off once the geometry has settled.
    // Every entry and exit goes through here so both directions get the same cover.
    function toggleFs() {
        if (win.snapping || win.snapPending)
            return;                     // no second swap on top of a half-drawn one
        picker.hide();
        win.snapPending = true;
        videoHost.beginSwap();          // answers with pictureReady()
        snapWait.restart();             // a texture that never arrives must not stall F
    }

    function startSwap() {
        if (!win.snapPending)
            return;
        win.snapPending = false;
        snapWait.stop();
        win.snapping = true;
        // Straight in: VideoHost puts the cover up and flushes it to the compositor
        // before it touches the rect, so there is nothing left to wait a present for.
        videoHost.toggleFullscreen();
    }

    property bool snapping: false
    property bool snapPending: false

    // The grab's own fallback: a capture that never answers must not stall F.
    Timer {
        id: snapWait
        interval: 200
        onTriggered: win.startSwap()
    }

    Connections {
        target: videoHost
        function onPictureReady() { win.startSwap() }
        function onSettled() { win.snapping = false }
    }

    // The picture. libmpv draws into a texture on this window's own device and the
    // scene graph samples it here, so the scrim, the bar and the buttons are drawn
    // over it in the same pass - which is why there is no second window any more.
    VideoItem {
        id: video
        anchors.fill: parent
        core: playerCore
        // Nothing can be loaded before the render context exists; PlayerCore
        // holds the request until this fires.
        onReady: playerCore.markRenderReady()
        // The host holds the cover over the swap until two frames have gone over at a
        // size other than the one before the geometry write, so the freeze is only as
        // long as the surface re-allocation and the compositor catching up with it.
        onPresented: (w, h, n) => videoHost.notePresented(w, h, n)
    }

    Image {
        anchors.fill: parent
        z: -3
        source: shellBg
        fillMode: Image.PreserveAspectCrop
        cache: false
        visible: shellBg.toString().length > 0
    }

    // Where the control band starts: the same edge the scrim is drawn from, so
    // "the area with the bar and the buttons in it" is one number, not two. The band
    // keeps the height it has at the launch size and rides the bottom edge, exactly
    // like the bar and the row inside it.
    readonly property real scrimH: shellHeight * Theme.scrimHeightRatio
    readonly property real bandTop: win.vh - win.scrimH

    // The chrome stays up as long as the pointer is anywhere in that band - the
    // bar, a button, or the gap between two buttons - and the countdown only
    // resumes once it has left.
    readonly property bool pointerInBand: bandZone.containsMouse || bar.hovering
                                          || bar.scrubbing || controls.pointerInside
                                          || picker.visible || chapters.open
                                          || playerMenu.visible
    // The pointer is on the open dropdown, or on the row of icons it belongs to.
    readonly property bool pointerOnMenu: picker.visible
                                          && (picker.pointerOn || controls.pointerInside)
    onPointerOnMenuChanged: {
        if (!picker.visible)
            return;
        if (pointerOnMenu)
            pickerLeave.stop();
        else
            pickerLeave.restart();
    }
    // Every "the user is still here" signal goes through this. The countdown may
    // not be re-armed while the pointer is inside the control band: when the
    // cursor crosses in, Qt hands that first move to the item it was hovering
    // before (the gesture layer), which would restart the timer right after the
    // band said stop.
    function touch() {
        if (!win.pointerInBand)
            poke.restart();
    }

    onPointerInBandChanged: {
        if (win.pointerInBand) {
            win.uiVisible = true;
            poke.stop();
        } else {
            poke.restart();
        }
    }

    // A bubble that outlives the chrome it belongs to would hang over a bare
    // picture, so the countdown takes it with the rest of the UI.
    onUiVisibleChanged: if (!win.uiVisible) {
        tip.hide()
        picker.hide()
    }

    // The drawer is a reading surface over the picture, so any change to the picture's
    // shape takes it away: our own maximise and fullscreen writes, a drag on the edge,
    // and a resize the shell does to us (Win+Up, snap assist) all move vw/vh.
    onVwChanged: chapters.open = false
    onVhChanged: chapters.open = false

    // Gesture layer, below the controls: empty-area click toggles the chrome,
    // wheel sets volume, double-click toggles fullscreen on the mpv host.
    // It stops at the top of the control band on purpose - a click between two
    // buttons is a miss, not a request to hide everything.
    MouseArea {
        x: 0
        y: 0
        width: win.width
        height: win.bandTop
        z: -1
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        onClicked: (m) => {
            win.uiVisible = !win.uiVisible;
            console.log("uiVisible ->", win.uiVisible);
            win.touch();
        }
        // Double-click on the picture is play/pause now, not a window state.
        onDoubleClicked: playerCore.togglePause()
        onPositionChanged: win.touch()
        onWheel: (w) => win.setVol(playerCore.volume + (w.angleDelta.y > 0 ? 5 : -5))
    }

    // Hover tracker for the control band. Qt hands a mouse-over to exactly one
    // item - the topmost one that accepts hover - so this sits *under* the bar
    // and the buttons and covers the gaps between them; the three together
    // (zone, bar, buttons) are "the pointer is in the band".
    MouseArea {
        id: bandZone
        x: 0
        y: win.bandTop
        width: win.width
        height: win.height - win.bandTop
        z: -0.5
        acceptedButtons: Qt.NoButton     // never swallows a click
        hoverEnabled: true
        onWheel: (w) => win.setVol(playerCore.volume + (w.angleDelta.y > 0 ? 5 : -5))
    }

    Timer {
        id: poke
        interval: 3000
        // While the film is stopped there is nothing for the chrome to get out of
        // the way of, so the bar and the menus stay up until it runs again.
        onTriggered: if (!win.keepUi && !playerCore.paused) win.uiVisible = false
    }

    // The scrub preview is built once the length is known: the strip is spread
    // over the duration, and the cache is keyed on the file.
    Connections {
        target: playerCore
        function onMediaUrlChanged() { thumbs.clear(); }
        // Pausing brings the chrome up - there is nothing to hide it for - and
        // playing again re-arms the countdown, which nothing else would do.
        function onPausedChanged() {
            if (playerCore.paused)
                win.uiVisible = true;
            else
                win.touch();
        }
        function onDurationChanged() {
            if (playerCore.duration > 0 && playerCore.mediaUrl.length > 0) {
                thumbs.build(playerCore.mediaUrl, playerCore.duration, 10);
                library.noteDuration(playerCore.mediaUrl, playerCore.duration);
            }
        }
    }

    // A second launch of the exe does not open a second window: it hands us its
    // file (or its page) through the local socket and quits.
    Connections {
        target: single
        function onRequested(path, page) {
            if (path.length > 0)
                playerCore.load(path);
            win.page = page === "library" ? "library" : "player";
            videoHost.activate();
        }
    }

    // Start the countdown with the UI, otherwise the chrome stays up until the
    // first mouse event and the hidden/shown state is not reproducible.
    Component.onCompleted: if (!win.keepUi) poke.start()

    // The scrim belongs to the picture, not to the chrome. It used to live inside
    // `ui`, so hiding the controls for a transition also lifted the gradient off the
    // bottom of the film - measured +26/255 over those rows, +2.8 averaged across the
    // film - and that brighten-and-return is the pulse he kept calling a flash. It
    // stays up through the transition and only follows the chrome's own fade.
    Rectangle {
        visible: page !== "library"
        x: 0
        y: win.vh - win.scrimH
        width: win.vw
        height: win.scrimH
        opacity: win.uiVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        gradient: Gradient {
            GradientStop { position: 0.00; color: "#00000000" }
            GradientStop { position: 0.18; color: "#8C000000" }
            GradientStop { position: 0.45; color: "#D8000000" }
            GradientStop { position: 1.00; color: "#F7000000" }
        }
    }

    // The window's own top row. It used to live inside `ui` and only appear in
    // fullscreen, because out of fullscreen the native caption carried the film name and
    // the min/max/close glyphs. WM_NCCALCSIZE puts the client over that caption in every
    // state now, so this row is the only caption there is - and it has to survive the
    // library page, where `ui` is hidden, or the window would have no close button left.
    Item {
        id: topRow
        visible: !videoHost.transitioning
        opacity: (page === "library" || win.uiVisible) ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: caption.height
        // Above `ui`: the click-outside layer that dismisses a dropdown or the drawer
        // covers the whole window, and the window's own close/min/max must not be eaten
        // by it - measured, a click on the maximise tile with the drawer out only closed
        // the drawer.
        z: 1

        CaptionButtons {
            id: caption
            anchors.top: parent.top
            anchors.right: parent.right
            fullscreen: videoHost.fullscreen
            onPicked: (what) => {
                if (what === "close") win.close();
                else if (what === "max") videoHost.toggleMaximized();
                else videoHost.minimize();   // the host owns the taskbar entry
            }
        }

        Column {
            id: titleCol
            x: caption.gap
            visible: page !== "library"
            // Stops where the three glyphs start, with the same gap on its right as on
            // its left, so a long film name runs out of room against the buttons instead
            // of under them.
            readonly property real room: Math.max(0, topRow.width - 2 * caption.gap - caption.width)
            width: room
            // Centred on the *name line*, not on the two-line block. Measured off the
            // screen: the three glyphs' ink lies on row 18.5 of this row (the tiles run
            // 6..34, so the ink sits 1.5 px above the tile centre), and the name's ink band
            // sits 2 px below the middle of its own line box - so the box goes in at
            // tile centre -3.5 and the two ink bands meet. Centring the block instead left
            // the name 8.5 px above the glyphs, and because the line gap is a ratio of the
            // window height it also slid the name up and down as the window changed shape.
            readonly property real rowMid: caption.gap + caption.tileH / 2 - 3.5
            y: Math.max(0, rowMid - mediaTitle.height / 2)
            spacing: Theme.titleLineGapRatio * win.vh

            Text {
                id: mediaTitle
                width: titleCol.room
                elide: Text.ElideRight
                text: win.libraryTitle.length ? win.libraryTitle
                      : (playerCore.mediaTitle.length ? playerCore.mediaTitle
                                                      : "Oppenheimer (2023)")
                color: Theme.textPrimary
                font.pixelSize: win.headerFont
                font.family: Theme.uiFont
                font.weight: Font.Normal
            }

            Text {
                id: chapterLine
                width: titleCol.room
                elide: Text.ElideRight
                text: playerCore.chapter >= 0
                      ? (playerCore.chapterTitle.length ? playerCore.chapterTitle
                                                        : win.chapterName(playerCore.chapter))
                      : ""
                // No file, or a file with no chapters: the line is gone rather than
                // holding a made-up "Chapter 2" under the name.
                visible: text.length > 0
                color: Theme.textSecondary
                // The house factor for secondary text (Tip and the dropdown use it
                // too); otherwise this line ends up taller than the title above it.
                font.pixelSize: win.headerFont * 0.85
                font.family: Theme.uiFont
            }
        }
    }

    Item {
        id: ui
        visible: page !== "library" && !videoHost.transitioning
        anchors.fill: parent
        opacity: win.uiVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 180 } }

        // The timecode now sits at the two ends of the bar: where we are on the left,
        // how long the film is on the right. Same type as the old combined readout.
        Text {
            id: posText
            x: win.inset
            anchors.verticalCenter: bar.verticalCenter
            text: win.fmt(playerCore.position)
            color: Theme.glyph
            font.pixelSize: Theme.subtitleFontRatio * shellHeight
            font.family: Theme.uiFont
        }

        Text {
            id: durText
            x: win.vw - win.inset - width
            anchors.verticalCenter: bar.verticalCenter
            text: win.fmt(playerCore.duration)
            color: Theme.glyph
            font.pixelSize: Theme.subtitleFontRatio * shellHeight
            font.family: Theme.uiFont
        }

        ScrubBar {
            id: bar
            x: posText.x + posText.width + Theme.timeGapHRatio * shellHeight
            width: durText.x - Theme.timeGapHRatio * shellHeight - x
            // Fixed at the launch size, then shifted with the bottom edge, so the bar
            // keeps exactly the height it has where the window started and the same
            // gap to the buttons under it at any window size.
            height: shellHeight * 0.030
            y: Theme.barCenterRatio * shellHeight - height / 2 + (win.vh - shellHeight)
            progress: playerCore.duration > 0 ? playerCore.position / playerCore.duration : 0.0431
            onScrubbed: (frac) => {
                if (playerCore.duration > 0)
                    playerCore.seekTo(frac * playerCore.duration);
            }
        }

        HoverCard {
            id: card
            refW: shellWidth
            refH: shellHeight
            x: Math.max(win.inset, Math.min(win.vw - win.inset - width,
                                            bar.x + bar.width * bar.hoverPos - width / 2))
            y: bar.y - height - shellHeight * 0.020
            visible: bar.hovering && bar.hoverPos >= 0
            chapterLabel: win.hasChapters ? win.chapterName(win.chapterAt(bar.hoverPos)) : ""
            timeLabel: win.fmt(bar.hoverPos * playerCore.duration)
            thumb: {
                const n = thumbs.frames.length
                if (n === 0)
                    return ""
                // hoverPos is -1 until the pointer is on the bar, and the strip
                // can be short of a frame, so the index is clamped both ways.
                const i = Math.max(0, Math.min(n - 1, Math.floor(bar.hoverPos * n)))
                return thumbs.frames[i]
            }
        }



        ChapterList {
            id: chapters
            refW: shellWidth
            // The same gap above as below: the bottom one is the control band.
            gap: win.scrimH
            edges: win.edgeTimes
            labels: win.chapterLabels
            onPicked: (index) => {
                const t = win.edgeTimes[index];
                if (t !== undefined)
                    playerCore.seekTo(t + 0.5);
                chapters.open = false;
            }
            onDismissed: chapters.open = false
        }

        ControlRow {
            id: controls
            x: 0
            width: win.vw
            // Fixed at the height the row has where the window was launched, so
            // maximising or going fullscreen no longer scales the bar, the glyphs and
            // the time text with it. Everything derived from it inside ControlRow
            // (icon, scrub line, knob) is fixed by the same move; the horizontal pitch
            // still follows the width, so the row keeps spanning the window.
            height: shellHeight * Theme.controlRowHeightRatio
            y: Theme.controlCenterRatio * shellHeight - height / 2 + (win.vh - shellHeight)
            // Mini is the narrowest layout there is, so it pins the tier rather than
            // competing with the width thresholds.
            miniTier: videoHost.mini ? 0 : -1
            playing: !playerCore.paused
            volume: playerCore.volume
            rotateDeg: win.rotateSteps[win.rotateIdx]
            aspectLabel: win.aspectIdx === 0 ? "自动" : win.aspectSteps[win.aspectIdx]
            speedLabel: win.speedLabel(playerCore.speed)
            audioCount: playerCore.audioTracks.length
            subCount: playerCore.subTracks.length
            hasChapters: win.hasChapters
            onAction: (what) => win.act(what)
            onVolumePicked: (pct) => playerCore.setVolume(pct)
            onPickFor: (name, item) => win.openPicker(name, item)
            onTipFor: (item, text, keys) => tip.show(item, text, keys, "above")
            onTipOff: (item) => tip.hideFor(item)
        }

        // One bubble for the whole chrome: the transport cells and the caption
        // buttons both hand it their item, and it places itself above or below
        // them without ever leaving the window.
        Tip {
            id: tip
            z: 30
            refH: shellHeight
        }

        // Click outside an open dropdown or an open drawer closes it. This sits *under*
        // the chrome (the gesture layer is below it, the controls above): a click on
        // another menu cell has to reach that cell, or switching menus costs two clicks.
        MouseArea {
            anchors.fill: parent
            z: -0.25
            visible: picker.visible || chapters.open
            enabled: picker.visible || chapters.open
            onClicked: {
                picker.hide()
                chapters.open = false
            }
        }

        Picker {
            id: picker
            z: 31
            space: ui
            refH: shellHeight
            onPicked: (i) => {
                if (win.pickerApply)
                    win.pickerApply(i);
                picker.hide();
            }
        }

        // The dropdown also closes when the pointer leaves it. The delay is what
        // lets the cursor cross the 8 px gap between the icon and the card without
        // being counted as gone.
        Timer {
            id: pickerLeave
            interval: 220
            onTriggered: picker.hide()
        }
    }

    // The picture's right button. Window level, not inside the chrome, so it answers
    // with the bar faded out; and it takes the right button only, which is how a
    // right-click on an icon reaches it - every cell under the bar accepts the left
    // button alone, so the right one falls through the whole stack to here. Under the
    // menu and over everything that would otherwise eat the press.
    MouseArea {
        z: 32
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        enabled: win.page === "player" && !videoHost.transitioning
        onClicked: (m) => win.openMenuAt(m.x, m.y)
    }

    // With the menu up, a left click anywhere is a dismissal - not a request to toggle
    // the chrome or to seek on the bar underneath. Right button is deliberately not
    // taken here: it falls through to the layer below so a second right-click moves the
    // menu instead of only closing it.
    MouseArea {
        z: 33
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        visible: playerMenu.visible
        enabled: playerMenu.visible
        onClicked: playerMenu.hide()
    }

    ContextMenu {
        id: playerMenu
        z: 34
        refH: shellHeight
        onPicked: (action, target) => win.menuPick(action)
    }

    // Keyboard. Shortcut is scoped to this window and fires without waiting for
    // an item to hold focus, which is what the caption-less fullscreen needs.
    Shortcut { sequence: "Space"; enabled: win.page === "player"; onActivated: win.act("play") }
    Shortcut { sequence: "Left"; enabled: win.page === "player"; onActivated: win.act("back10") }
    Shortcut { sequence: "Right"; enabled: win.page === "player"; onActivated: win.act("fwd10") }
    Shortcut { sequence: "Up"; enabled: win.page === "player"; onActivated: win.setVol(playerCore.volume + 5) }
    Shortcut { sequence: "Down"; enabled: win.page === "player"; onActivated: win.setVol(playerCore.volume - 5) }
    Shortcut { sequence: "J"; enabled: win.page === "player"; onActivated: win.act("prevChapter") }
    Shortcut { sequence: "L"; enabled: win.page === "player"; onActivated: win.act("nextChapter") }
    Shortcut { sequence: "S"; enabled: win.page === "player"; onActivated: win.act("cc") }
    Shortcut { sequence: "A"; enabled: win.page === "player"; onActivated: win.act("audioTrack") }
    Shortcut { sequence: "T"; enabled: win.page === "player"; onActivated: win.act("chapters") }
    Shortcut { sequence: "M"; enabled: win.page === "player"; onActivated: win.act("mute") }
    // F is the only entry left that goes to true fullscreen (over the task bar);
    // the double-click and the expand cell both hand the window to Windows' own
    // maximise so all of them land on the same rect.
    Shortcut { sequence: "F"; enabled: win.page === "player"; onActivated: win.toggleFs() }
    Shortcut {
        sequence: "Escape"
        onActivated: {
            if (picker.visible)
                picker.hide();
            else if (chapters.open)
                chapters.open = false;
            else if (win.page === "library")
                win.page = "player";
            else if (videoHost.fullscreen)
                win.toggleFs();
        }
    }

    Loader {
        z: 10
        anchors.fill: parent
        active: page === "library"
        sourceComponent: LibraryPage { }
    }

    // --- frame gestures ---------------------------------------------------
    // The window carries no system resize border: WM_NCCALCSIZE in videohost.cpp hands
    // the client the whole window, so the system hit-test reports HTCLIENT everywhere and
    // these bands are the only thing that resizes it. Measured on the build where they
    // were all off: a drag 60 px outward at all eight edges and corners moved nothing at
    // all except the top strip, which slid the window 60 px up.
    //
    // Only the three edges whose grab leaves the window's own origin alone are live. The
    // left, top and the three corners that touch them are gone, not disabled: a grab
    // there moves win.x/win.y under the cursor, and the cursor position is read back from
    // that origin, so the delta feeds itself - that is the violent flicker they showed.
    //
    // Positions are rebuilt from the window's own screen origin, not from the item-local
    // delta: the window moves under the grab, so a local delta would read zero on the very
    // next event and the drag would stall.
    readonly property int edgeGrab: 8
    readonly property int cornerGrab: 20
    property point pressGlobal
    property rect pressHost

    function grabAt(item, m) {
        return Qt.point(win.x + item.x + m.x, win.y + item.y + m.y);
    }
    function grabBegin(item, m) {
        win.pressGlobal = win.grabAt(item, m);
        win.pressHost = videoHost.rect;
    }
    // bits: 1 left, 2 right, 4 top, 8 bottom
    function grabResize(item, edges, m) {
        const d = Qt.point(win.grabAt(item, m).x - win.pressGlobal.x,
                           win.grabAt(item, m).y - win.pressGlobal.y);
        const r = win.pressHost;
        let l = r.x, t = r.y, rt = r.x + r.width, b = r.y + r.height;
        if (edges & 1)
            l += d.x;
        if (edges & 2)
            rt += d.x;
        if (edges & 4)
            t += d.y;
        if (edges & 8)
            b += d.y;
        if (rt - l < 400) {
            if (edges & 1)
                l = rt - 400;
            else
                rt = l + 400;
        }
        if (b - t < 225) {
            if (edges & 4)
                t = b - 225;
            else
                b = t + 225;
        }
        videoHost.setRect(l, t, rt - l, b - t);
    }
    function grabMove(item, m) {
        const d = Qt.point(win.grabAt(item, m).x - win.pressGlobal.x,
                           win.grabAt(item, m).y - win.pressGlobal.y);
        videoHost.setRect(win.pressHost.x + d.x, win.pressHost.y + d.y,
                          win.pressHost.width, win.pressHost.height);
    }

    component Band: MouseArea {
        id: band
        required property int edges
        z: 20
        // Fullscreen and the maximised state have no edge to grab: the first is the whole
        // screen and the second is the shell's own size.
        enabled: !videoHost.fullscreen && !videoHost.zoomed
        hoverEnabled: true
        onPressed: (m) => win.grabBegin(band, m)
        onPositionChanged: { if (pressed) win.grabResize(band, edges, mouse); }
    }

    // cursorShape is set at each use site: MouseArea applies it on enter and
    // restores it on leave by itself. Routing it through a property needs a
    // Qt.CursorShape-typed property, and QML rejects that type outright
    // ("Unable to assign int to [unknown property type]").
    Band { edges: 2;  cursorShape: Qt.SizeHorCursor;  x: win.width - win.edgeGrab; y: 0; width: win.edgeGrab; height: win.height }
    Band { edges: 8;  cursorShape: Qt.SizeVerCursor;  x: 0; y: win.height - win.edgeGrab; width: win.width; height: win.edgeGrab }
    Band { edges: 10; cursorShape: Qt.SizeFDiagCursor; x: win.width - win.cornerGrab; y: win.height - win.cornerGrab; width: win.cornerGrab; height: win.cornerGrab }

    MouseArea {
        id: topStrip
        z: 21
        x: win.cornerGrab
        y: 0
        width: Math.max(0, win.width - 2 * win.cornerGrab - caption.width)
        height: Math.max(16, caption.height)
        // With no native caption left to grab, this strip is the only move handle the
        // window has. Dragging a zoomed window stays disabled: what the system does
        // there is snap it back to a normal rect under the cursor, and sliding the
        // maximised rect around instead would read as broken.
        enabled: !videoHost.fullscreen && !videoHost.zoomed
        hoverEnabled: true
        cursorShape: Qt.SizeAllCursor
        onPressed: (m) => win.grabBegin(topStrip, m)
        onPositionChanged: (m) => { if (pressed) win.grabMove(topStrip, m); }
    }

    Text {
        visible: shellDiag
        x: win.inset
        y: 8
        color: "#80FF80"
        font.pixelSize: 12
        font.family: "Consolas"
        text: "hwdec=" + playerCore.hwdecCurrent + "  codec=" + playerCore.videoCodec
              + "  chapters=" + playerCore.chapterStarts.length
              + "  vol=" + Math.round(playerCore.volume) + "  fs=" + playerCore.fullscreen + " fsActive=" + playerCore.fullscreenActive
    }
}
