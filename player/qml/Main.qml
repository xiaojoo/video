import QtQuick
import QtQuick.Dialogs
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

    // Which control the open dropdown belongs to, so a second click on the same one is a
    // dismissal instead of a re-open on top of itself. Only read while the picker is
    // visible, so a stale name from a list that has since timed out is harmless.
    property string pickerFor: ""

    // The three cells that hold one of several values share this one dropdown. `at` is
    // "right" when it is opened as a submenu of the right-click menu, which also pulls it
    // in to the parent card's edge instead of the row's.
    function openPicker(name, item, at) {
        if (picker.visible && win.pickerFor === name) {
            picker.hide();
            win.pickerFor = "";
            return;
        }
        tip.hide();
        win.pickerFor = name;
        const inset = at === "right" ? playerMenu.sidePad : 0;
        if (name === "speed") {
            picker.show(item, win.speedSteps.map(v => win.speedLabel(v)),
                        win.nearest(win.speedSteps, playerCore.speed), at, inset);
            win.pickerApply = (i) => playerCore.setSpeed(win.speedSteps[i]);
        } else if (name === "aspect") {
            picker.show(item, win.aspectSteps.map(s => s === "-1" ? "自动" : s), win.aspectIdx,
                        at, inset);
            win.pickerApply = (i) => win.pickAspect(i);
        } else if (name === "rotate") {
            picker.show(item, win.rotateSteps.map(d => d + "°"), win.rotateIdx, at, inset);
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
            picker.show(item, tracks.map(t => t.label), cur, at, inset);
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
        function onPlaylistsChanged() { win.plRev++ }
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
        win.closeSub();
        // Every transport command - the icons, the context menu, the keyboard - comes
        // through here, so this is the one place that can say "the pointer asked for
        // something else" and leave the rename.
        playlist.endEdit();
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
        else if (what === "mute" || what === "volume" || what === "volumeMuted")
            playerCore.cycleProperty("mute");
        else if (what === "playlist") win.setPlaylist(!win.plOpen);
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
    //
    // The name and the key hint are read off the cell itself, not written out again
    // here: the bubble over the icon and the menu row are then the same two strings by
    // construction, including the ones that carry a value (0°, 自动, 1×) and the one
    // that swaps with the state (播放 / 暂停).
    function menuList() {
        const names = win.menuNames;
        const out = [];
        for (let i = 0; i < names.length; ++i) {
            const cell = controls.cells[names[i]];
            out.push({ label: cell ? cell.tipText : names[i],
                       action: names[i],
                       on: win.menuLive(names[i]),
                       icon: cell ? cell.name : "",
                       keys: cell ? cell.tipKeys : "",
                       sub: cell ? cell.hasMenu : false });
        }
        return out;
    }

    function openMenuAt(px, py) {
        tip.hide();
        win.closeSub();
        // The menu is a transport surface, so it brings the bar with it: the dropdowns
        // it hands off to live inside the chrome, and would be invisible at opacity 0.
        win.uiVisible = true;
        playerMenu.showAt(px, py, win.menuList(), null);
        win.touch();
    }

    function menuPick(what) {
        // A value row's command is the list beside it, and hover already opened that -
        // the row does not hide the menu, so there is nothing left for the click to do.
        // Every other row is a command the keys already send.
        if (win.isValueCell(what))
            return;
        win.act(what);
    }

    readonly property var valueCells: ["cc", "audioTrack", "rotate", "aspect", "speed"]
    function isValueCell(what) {
        return win.valueCells.indexOf(what) >= 0;
    }

    // --- the menu's submenus ----------------------------------------------
    // The five rows that hold one of several values open the same list the icon opens,
    // but beside their own row and with the menu left up, so the pair reads as one
    // cascade instead of a jump down to the control row.
    property string subFor: ""
    property Item subRow: null

    function openSub(what, row) {
        win.subFor = what;
        win.subRow = row;
        win.openPicker(what, row, "right");
    }

    function closeSub() {
        win.subFor = "";
        win.subRow = null;
        picker.hide();
    }

    function menuHovered(what, row) {
        subDwell.stop();
        if (what.length === 0) {
            // A plain row under the pointer means the list beside the previous row is no
            // longer the thing being read. Leaving a row altogether does not close it:
            // that would dismiss the pair the moment the pointer starts for the list.
            if (win.subFor.length > 0)
                win.closeSub();
            return;
        }
        if (win.subFor === what)
            return;
        win.closeSub();
        subDwell.what = what;
        subDwell.row = row;
        subDwell.restart();
    }

    // The dwell is what stops a sweep down sixteen rows from popping five boxes. Same
    // family as the 220 ms that closes the dropdown once the pointer has left it.
    Timer {
        id: subDwell
        interval: 200
        property string what: ""
        property Item row: null
        onTriggered: if (row) win.openSub(what, row)
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
        // The film's own column: the full window until the dock opens and takes a
        // strip off the right - and when it does, the window has already grown by the
        // same number, so this is the width the picture had before.
        x: 0
        y: 0
        width: win.stageW
        height: win.height
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

    // ---- the playlist dock -------------------------------------------------
    // Opening it widens the window by exactly the panel's width, so the film keeps the
    // rect it had and every chrome pixel on it keeps its place - which is the whole
    // reason this is a column of the window and not a card over the picture. Where the
    // window cannot grow (the maximised, fullscreen or mini state, or a screen edge
    // closer than the panel is wide) `plExtra` stays 0 and the dock stands over the
    // right of the picture instead; the film never shrinks because the dock opened.
    property bool plOpen: false
    // Which of the dock's two lists is showing. Only the labels inside the dock change
    // it; the cell and `P` just open and close the column, and it reopens on whichever
    // list was last read.
    property string plTab: "files"
    readonly property real plW: Math.round(Theme.playlistWidthRatio * shellHeight)
    // What the last open actually added. A stored number rather than a binding: the
    // room to our right is measured against our own width, and a binding that reads
    // `width` to produce the thing that changes `width` settles one step late.
    property real plExtra: 0
    // The width we added but have not been able to take back yet, because the window was
    // maximised (or fullscreen, or mini) when the dock closed: those states own the rect,
    // and writing it there would fight the shell. Measured before this existed: grow ->
    // maximise -> close the dock -> restore left the window at 1615 px with the dock gone,
    // because the grown width had been baked into the restore rect.
    property real plOwed: 0
    // The film's width, and everything drawn on it. Window chrome (the caption buttons,
    // the resize bands, the top drag strip) follows `width`; the picture's own chrome
    // follows this.
    readonly property real stageW: width - plExtra
    // The folder listing. `libRev` is the white-read dependency: a rename in the media
    // library has to reach the rows too, and a function call in a binding only tracks
    // what it reads.
    readonly property var plItems: { win.libRev; return library.itemsBeside(playerCore.mediaUrl) }

    // ---- 我的播单 -----------------------------------------------------------
    // The lists are read through a counter rather than a plain binding: a function call
    // in a binding only re-runs when a property it *read* changes, and `library.playlists()`
    // reads none - the database did. `libRev` is folded in so a rename in the media
    // library reaches the titles shown here too.
    property int plRev: 0
    readonly property var plLists: { win.plRev; win.libRev; return library.playlists() }
    property int plOpenList: -1
    readonly property var plEntries: {
        win.plRev
        return win.plOpenList >= 0 ? library.playlistItems(win.plOpenList) : []
    }
    // Which list the film on screen came from and which entry of it. -1 / "" means it was
    // opened some other way, and then nothing follows it - the queue is a fact about how
    // this film started, not a mode the user has to remember to leave.
    property int plQueueList: -1
    property string plQueuePath: ""
    // The list the file dialog is about to add to, kept because the dialog's answer
    // arrives a whole event loop after it was opened.
    property int plPickTarget: -1

    function plCreate() {
        const id = library.createPlaylist("")
        if (id >= 0)
            win.plOpenList = id
        win.touch()
    }

    function plToggleList(id) {
        win.plOpenList = win.plOpenList === id ? -1 : id
        playlist.pendingDelete = -1
        playlist.endEdit()
        win.touch()
    }

    function plDeleteList(id) {
        library.deletePlaylist(id)
        if (win.plOpenList === id)
            win.plOpenList = -1
        if (win.plQueueList === id) {
            win.plQueueList = -1
            win.plQueuePath = ""
        }
        playlist.pendingDelete = -1
        playlist.endEdit()
    }

    function plPlayEntry(id, path, url) {
        win.plOpenList = id
        win.plQueueList = id
        win.plQueuePath = path
        playerCore.load(url)
        win.touch()
    }

    // The next entry of the list the current film came from. Read from the database
    // rather than a remembered array, so an entry added, removed or reordered while the
    // film plays is already reflected in what comes next.
    function plAdvance() {
        if (win.plQueueList < 0)
            return
        const e = library.playlistItems(win.plQueueList)
        let i = -1
        for (let k = 0; k < e.length; ++k)
            if (e[k].path === win.plQueuePath)
                i = k
        if (i < 0 || i + 1 >= e.length) {
            win.plQueueList = -1
            win.plQueuePath = ""
            console.log("queue -> end")
            return
        }
        const n = e[i + 1]
        win.plQueuePath = n.path
        console.log("queue -> next", n.name)
        playerCore.load(n.url)
    }

    function plCanGrow() {
        return !videoHost.zoomed && !videoHost.fullscreen && !videoHost.mini
    }

    function setPlaylist(on) {
        if (on === win.plOpen)
            return;
        if (on) {
            // Still carrying the width from a dock that closed while the window was
            // maximised: adopt it instead of growing a second time on top of it.
            if (win.plOwed > 0) {
                win.plExtra = Math.min(win.plW, win.plOwed);
                win.plOwed = 0;
                win.plOpen = true;
                win.touch();
                return;
            }
            // In the states that own the rect the dock simply stands over the right of
            // the picture; `plExtra` stays 0 so the film keeps the whole window.
            win.plExtra = plCanGrow()
                          ? Math.max(0, Math.min(win.plW, videoHost.workArea().x
                                                 + videoHost.workArea().width
                                                 - (win.x + win.width)))
                          : 0;
            win.plOpen = true;
            // Both writes happen in this one turn, so the scene graph sees the pair and
            // not the step in between.
            if (win.plExtra > 0)
                videoHost.setRect(win.x, win.y, win.width + win.plExtra, win.height);
        } else {
            const back = win.plExtra;
            win.plExtra = 0;
            win.plOpen = false;
            // The rename does not come back on its own, and reopening the dock to a row
            // still in its box looks like the panel remembered being clicked.
            playlist.endEdit();
            if (back > 0) {
                if (plCanGrow())
                    videoHost.setRect(win.x, win.y, win.width - back, win.height);
                else
                    win.plOwed = back;
            }
        }
        win.touch();
    }

    // The owed width goes back the moment the window is ours to size again. Triggered on
    // the window's own width rather than `videoHost.resized`: that signal is only emitted
    // by the transitions this app starts itself, and an external restore (the shell's
    // Win+Down, a snap, the task bar) changes the rect without it - measured, the window
    // came back 333 px too wide and stayed that way until the dock was opened and closed
    // again. Decided on the rect rather than on `videoHost.zoomed`, because the WM_SIZE
    // that carries the restore arrives while the zoomed flag is still set.
    onWidthChanged: {
        if (win.plOwed <= 0 || videoHost.fullscreen || videoHost.mini)
            return;
        const wa = videoHost.workArea();
        if (width >= wa.width && height >= wa.height)
            return;
        const back = win.plOwed;
        win.plOwed = 0;
        videoHost.setRect(x, y, width - back, height);
    }

    // The chrome stays up as long as the pointer is anywhere in that band - the
    // bar, a button, or the gap between two buttons - and the countdown only
    // resumes once it has left.
    readonly property bool pointerInBand: bandZone.containsMouse || bar.hovering
                                          || bar.scrubbing || controls.pointerInside
                                          || picker.visible || win.plOpen
                                          || playerMenu.visible
    // The pointer is on the open dropdown, on the row of icons it belongs to, or on the
    // menu row that a submenu hangs off.
    readonly property bool pointerOnMenu: picker.visible
                                          && (picker.pointerOn || controls.pointerInside
                                              || playerMenu.pointerInside)
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

    // The dock does not close when the picture changes shape, the way the old chapter
    // drawer did: it is a column of the window now, so a resize only moves the edge it
    // stops at.
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
            playlist.endEdit();
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
        // A film opened any way other than the queue cancels it: the queue describes how
        // this particular film was reached, and picking from the folder list, the library
        // or the command line is a different answer.
        function onMediaUrlChanged() {
            thumbs.clear()
            if (win.plQueuePath.length > 0 && playerCore.mediaUrl !== win.plQueuePath) {
                win.plQueueList = -1
                win.plQueuePath = ""
            }
        }
        function onEndOfFile() { win.plAdvance() }
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
        width: win.stageW
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
        // The dock takes the window's right strip, so the caption goes back to the
        // film's edge - measured before this, the three tiles landed on top of the
        // dock's own header and its ✕.
        anchors.rightMargin: win.plExtra
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
        // Everything the film carries lives in here, so it all follows the film's
        // column rather than the window's.
        x: 0
        y: 0
        width: win.stageW
        height: win.height
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
            x: win.stageW - win.inset - width
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
            x: Math.max(win.inset, Math.min(win.stageW - win.inset - width,
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

        ControlRow {
            id: controls
            x: 0
            width: win.stageW
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
            muted: playerCore.muted
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

        // Click outside an open dropdown closes it. This sits *under*
        // the chrome (the gesture layer is below it, the controls above): a click on
        // another menu cell has to reach that cell, or switching menus costs two clicks.
        MouseArea {
            anchors.fill: parent
            z: -0.25
            visible: picker.visible
            enabled: picker.visible
            onClicked: picker.hide()
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

    // The one dropdown, at window level: the same list serves the five value icons and
    // the submenu that hangs off a row of the right-click menu, and the menu's own
    // click-outside layer (z 33) would otherwise eat every click meant for it.
    Picker {
        id: picker
        z: 40
        refH: shellHeight
        onPicked: (i) => {
            if (win.pickerApply)
                win.pickerApply(i);
            const fromMenu = win.subFor.length > 0;
            win.closeSub();
            // A value chosen out of a submenu ends the whole interaction, the same way
            // choosing a command from the menu does.
            if (fromMenu)
                playerMenu.hide();
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
        // The dock's column is not the film's: a right-click there must not open the
        // transport menu, so this layer only answers for the picture's own width.
        onClicked: (m) => { if (m.x < win.stageW) win.openMenuAt(m.x, m.y) }
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
        onHovered: (what, row) => win.menuHovered(what, row)
        // The submenu belongs to the menu: when the menu goes, the list beside it goes
        // too - which is also how a click outside the pair clears it. closeSub() hides the
        // dropdown, so this must not be written the other way round or the two would
        // call each other.
        onVisibleChanged: if (!visible) win.closeSub()
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
    Shortcut { sequence: "P"; enabled: win.page === "player"; onActivated: win.act("playlist") }
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
            else if (win.plOpen)
                win.setPlaylist(false);
            else if (win.page === "library")
                win.page = "player";
            else if (videoHost.fullscreen)
                win.toggleFs();
        }
    }

    // The playlist dock: a column of the window rather than of `ui`, because it is
    // opened and closed by hand and must not fade out with the chrome while it is being
    // read. z 30 puts it over the film and under the menu layers, which is the order the
    // right-click gate below assumes.
    Playlist {
        id: playlist
        z: 30
        refH: shellHeight
        mediaPath: playerCore.mediaUrl
        items: win.plItems
        edges: win.edgeTimes
        labels: win.chapterLabels
        chapter: playerCore.chapter
        tab: win.plTab
        lists: win.plLists
        entries: win.plEntries
        openList: win.plOpenList
        queuePath: win.plQueuePath
        visible: win.plOpen && win.page === "player"
        onPicked: (url) => { playerCore.load(url); win.touch() }
        onChapterPicked: (index) => {
            const t = win.edgeTimes[index];
            if (t !== undefined)
                playerCore.seekTo(t + 0.5);
            win.touch();
        }
        onTabPicked: (tab) => { win.plTab = tab; playlist.endEdit(); win.touch() }
        onCreateRequested: win.plCreate()
        onListClicked: (id) => win.plToggleList(id)
        onListRenamed: (id, name) => library.renamePlaylist(id, name)
        onListDeleteRequested: (id) => { playlist.pendingDelete = id; win.touch() }
        onListDeleteConfirmed: (id) => win.plDeleteList(id)
        onListAddRequested: (id) => { win.plPickTarget = id; filePick.open() }
        onEntryClicked: (id, path, url) => win.plPlayEntry(id, path, url)
        onEntryRemoved: (id, path) => library.removeFromPlaylist(id, path)
        onEntryMoved: (id, path, dir) => library.movePlaylistItem(id, path, dir)
        onDismissed: win.setPlaylist(false)
    }

    // Adding from disk goes through the shell's own picker: the point of a playlist is to
    // hold things that are not in the library, and a folder scan would either copy or
    // catalogue them. Neither happens - the dialog's urls are handed to the database as
    // paths, and only paths.
    FileDialog {
        id: filePick
        title: "添加到播单"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["可播放的文件 (*.mp4 *.m4v *.mkv *.webm *.avi *.mov *.flv *.wmv"
                      + " *.ts *.m2ts *.mp3 *.flac *.alac *.m4a *.opus *.ogg *.oga *.wav *.ape)",
                      "所有文件 (*)"]
        onAccepted: {
            if (win.plPickTarget < 0)
                return
            const paths = []
            // `selectedFiles`, not `selectedUrls`: this Qt's FileDialog exports
            // fileMode / selectedFile / selectedFiles / currentFile / currentFiles /
            // currentFolder (read out of QtQuick/Dialogs/plugins.qmltypes), and the name
            // the docs use for the same list elsewhere is not what this type has. With
            // `selectedUrls` the handler threw "ReferenceError: selectedUrls is not
            // defined" on its first line, so a real pick - dialog open, file named,
            // 打开 clicked, dialog gone - added nothing and said nothing.
            for (let i = 0; i < selectedFiles.length; ++i)
                paths.push(String(selectedFiles[i]))
            library.addToPlaylist(win.plPickTarget, paths)
            win.plOpenList = win.plPickTarget
            win.plPickTarget = -1
        }
    }

    // Going to the library page, or into the mini window, takes the room the dock was
    // given away: both surfaces own the whole window, so the dock goes back the way it
    // came and the width it added goes with it.
    Connections {
        target: videoHost
        function onMiniChanged() { if (videoHost.mini && win.plOpen) win.setPlaylist(false) }
    }
    onPageChanged: if (page !== "player" && win.plOpen) win.setPlaylist(false)

    Loader {
        z: 10
        anchors.fill: parent
        active: page === "library"
        // An inactive Loader has no content, but it is still a full-window item at z 10 -
        // and it is the first thing a hit-test meets. Measured while chasing the cursor:
        // the item under the pointer for every point on the player page came back as this
        // Loader, so nothing below it was ever reached.
        visible: page === "library"
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
        width: Math.max(0, win.stageW - 2 * win.cornerGrab - caption.width)
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
