import QtQuick
import Playerui

Window {
    id: win
    visible: true
    // One window now: the picture is a layer in here, so the window carries the
    // native caption itself and no longer needs to be frameless or transparent.
    width: shellWidth
    height: shellHeight
    x: shellX
    y: shellY
    color: "#000000"
    title: playerCore.mediaTitle.length ? playerCore.mediaTitle : "Playerui"
    readonly property real vw: width
    readonly property real vh: height
    readonly property real inset: Theme.sideInsetWRatio * vw
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

    // The bar and the transport buttons must agree on where the edges are, so
    // both read this list: real chapters when mpv has them, mock ticks scaled
    // onto the duration otherwise.
    function edgeList() {
        const d = playerCore.duration;
        if (d <= 0)
            return [];
        if (playerCore.chapterStarts.length > 1)
            return playerCore.chapterStarts;
        return win.mockTicks.map(f => f * d);
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

    readonly property var mockTicks: [
        0.0951, 0.1537, 0.2016, 0.2390, 0.2919, 0.3325, 0.3927, 0.4309,
        0.4659, 0.5309, 0.5691, 0.6138, 0.6634, 0.7114, 0.7553, 0.8374,
        0.9228, 0.9585
    ]

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
    // "the area with the bar and the buttons in it" is one number, not two.
    readonly property real bandTop: win.vh * (1 - Theme.scrimHeightRatio)

    // The chrome stays up as long as the pointer is anywhere in that band - the
    // bar, a button, or the gap between two buttons - and the countdown only
    // resumes once it has left.
    readonly property bool pointerInBand: bandZone.containsMouse || bar.hovering
                                          || bar.scrubbing || controls.pointerInside
                                          || picker.visible
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
        y: win.vh * (1 - Theme.scrimHeightRatio)
        width: win.vw
        height: win.vh * Theme.scrimHeightRatio
        opacity: win.uiVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        gradient: Gradient {
            GradientStop { position: 0.00; color: "#00000000" }
            GradientStop { position: 0.18; color: "#8C000000" }
            GradientStop { position: 0.45; color: "#D8000000" }
            GradientStop { position: 1.00; color: "#F7000000" }
        }
    }

    Item {
        id: ui
        visible: page !== "library" && !videoHost.transitioning
        anchors.fill: parent
        opacity: win.uiVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 180 } }

        CaptionButtons {
            id: caption
            anchors.top: parent.top
            anchors.right: parent.right
            // Windowed and maximised, the system caption owns the top-right corner
            // and these three would sit over its buttons and eat the clicks.
            visible: videoHost.fullscreen
            fullscreen: videoHost.fullscreen
            onPicked: (what) => {
                if (what === "close") win.close();
                else if (what === "max") win.toggleFs();
                else videoHost.minimize();   // the host owns the taskbar entry
            }
        }

        Column {
            x: caption.gap
            // Centred on the bump row he asked the title to match: the group sits
            // gap px below the window edge, so the row's middle is gap + tileH/2.
            // The 2.5 px is measured: at this size the ink band sits that far below
            // the line box's centre, so centring the box leaves the words low.
            readonly property real rowMid: caption.gap + caption.tileH / 2 - 2.5
            // Out of fullscreen the film name has moved up into the system caption,
            // and the chapter line takes the seat it vacated.
            y: Math.max(0, rowMid - (videoHost.fullscreen ? mediaTitle.height
                                                          : chapterLine.height) / 2)
            spacing: Theme.titleLineGapRatio * win.vh

            Text {
                id: mediaTitle
                text: playerCore.mediaTitle.length ? playerCore.mediaTitle : "Oppenheimer (2023)"
                visible: videoHost.fullscreen
                // Column stacks by height and ignores visibility, so an invisible
                // title would still push the chapter line down.
                height: visible ? implicitHeight : 0
                color: Theme.textPrimary
                font.pixelSize: win.headerFont
                font.family: Theme.uiFont
                font.weight: Font.Normal
            }
            Text {
                id: chapterLine
                text: playerCore.chapter >= 0 ? "Chapter " + (playerCore.chapter + 1) : "Chapter 2"
                color: Theme.textSecondary
                // The house factor for secondary text (Tip and the dropdown use it
                // too); otherwise this line ends up taller than the title above it.
                font.pixelSize: win.headerFont * 0.85
                font.family: Theme.uiFont
            }
        }

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
            winW: win.vw
            winH: win.vh
            x: Math.max(win.inset, Math.min(win.vw - win.inset - width,
                                            bar.x + bar.width * bar.hoverPos - width / 2))
            y: bar.y - height - win.vh * 0.020
            visible: bar.hovering && bar.hoverPos >= 0
            chapterLabel: "Chapter " + (win.chapterAt(bar.hoverPos) + 1)
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
            onPicked: (index) => {
                const t = playerCore.chapterStarts[index];
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
            playing: !playerCore.paused
            volume: playerCore.volume
            rotateDeg: win.rotateSteps[win.rotateIdx]
            aspectLabel: win.aspectIdx === 0 ? "自动" : win.aspectSteps[win.aspectIdx]
            speedLabel: win.speedLabel(playerCore.speed)
            audioCount: playerCore.audioTracks.length
            subCount: playerCore.subTracks.length
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
            winW: win.vw
            winH: win.vh
        }

        // Click outside an open dropdown closes it. This sits *under* the chrome
        // (the gesture layer is below it, the controls above): a click on another
        // menu cell has to reach that cell, or switching menus costs two clicks.
        MouseArea {
            anchors.fill: parent
            z: -0.25
            visible: picker.visible
            enabled: picker.visible
            onClicked: picker.hide()
        }

        Picker {
            id: picker
            z: 31
            space: ui
            winH: win.vh
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
    // These were the only border the user had while the host was a bare popup:
    // the top strip moved the window, the edges and corners resized it. The
    // windowed state now carries a real Windows caption and resize frame, which
    // also does snapping, Win+arrows and the restore-on-double-click, so the
    // bands are switched off rather than deleted - the one state left without a
    // frame is fullscreen, where a resize grab is not wanted either.
    //
    // Positions were rebuilt from the window's own screen origin, not from the
    // item-local delta: the window moves under the grab, so a local delta would
    // read zero on the very next event and the drag would stall.
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
        // Off, not deleted - see the note above the grab metrics. Left on, these
        // bands would sit 8 px inside the picture (the chrome no longer covers the
        // frame) and steal the edge clicks from the video.
        enabled: false
        hoverEnabled: true
        onPressed: (m) => win.grabBegin(band, m)
        onPositionChanged: { if (pressed) win.grabResize(band, edges, mouse); }
    }

    // cursorShape is set at each use site: MouseArea applies it on enter and
    // restores it on leave by itself. Routing it through a property needs a
    // Qt.CursorShape-typed property, and QML rejects that type outright
    // ("Unable to assign int to [unknown property type]").
    Band { edges: 1;  cursorShape: Qt.SizeHorCursor;  x: 0; y: 0; width: win.edgeGrab; height: win.height }
    Band { edges: 2;  cursorShape: Qt.SizeHorCursor;  x: win.width - win.edgeGrab; y: 0; width: win.edgeGrab; height: win.height }
    Band { edges: 4;  cursorShape: Qt.SizeVerCursor;  x: 0; y: 0; width: win.width; height: win.edgeGrab }
    Band { edges: 8;  cursorShape: Qt.SizeVerCursor;  x: 0; y: win.height - win.edgeGrab; width: win.width; height: win.edgeGrab }
    Band { edges: 5;  cursorShape: Qt.SizeFDiagCursor; x: 0; y: 0; width: win.cornerGrab; height: win.cornerGrab }
    Band { edges: 10; cursorShape: Qt.SizeFDiagCursor; x: win.width - win.cornerGrab; y: win.height - win.cornerGrab; width: win.cornerGrab; height: win.cornerGrab }
    Band { edges: 6;  cursorShape: Qt.SizeBDiagCursor; x: win.width - win.cornerGrab; y: 0; width: win.cornerGrab; height: win.cornerGrab }
    Band { edges: 9;  cursorShape: Qt.SizeBDiagCursor; x: 0; y: win.height - win.cornerGrab; width: win.cornerGrab; height: win.cornerGrab }

    MouseArea {
        id: topStrip
        z: 21
        x: win.cornerGrab
        y: 0
        width: Math.max(0, win.width - 2 * win.cornerGrab - caption.width)
        height: Math.max(16, caption.height)
        // The native caption is the move handle now.
        enabled: false
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
