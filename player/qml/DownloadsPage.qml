import QtQuick
import QtQuick.Controls
import Playerui

// 下载: the job list, as a page of its own beside 本地媒体库 and 在线媒体. It used to be a
// second view inside the online page - one pill in that page's source row - and the way in
// is now the window's own tab row, so a job stays reachable whatever the wall is showing.
//
// Like the online page, this one talks to the context objects (downloads, thumbs)
// directly; the alternative was a signal per action and a handler in Main.qml that only
// forwarded it.
Item {
    id: page

    property var jobs: []
    // Where new jobs land, and where the row says they will land.
    property string saveDir: ""

    signal dirRequested()
    // Playing a job leaves this page for the player. The window owns `page`, and this is
    // a component: the ask travels up rather than reaching into an id it cannot see.
    signal playerRequested()
    // The shared callout lives in Main.qml; every mark that wants one hands its item up
    // through here and the library page.
    signal tipFor(var item, string text, string keys)
    signal tipOff(var item)

    // The page's gutter, the same one the library's cards and the wall's posters keep.
    readonly property real pad: Math.round(shellWidth * Theme.sideInsetWRatio)
    readonly property real smallFs: Math.round(0.0180 * shellHeight)
    readonly property real titleFs: Math.round(0.0194 * shellHeight)

    function refreshJobs() {
        const list = downloads.jobs();
        for (const m of list) {
            if (m.jobPoster.length === 0) {
                // Reads the cache only; asking for a still to be made is pumpStills().
                const p = downloads.filePath(m.jobId);
                if (p.length > 0)
                    m.jobPoster = thumbs.coverFor(p);
            }
        }
        jobs = list;
    }

    // A row with no card picture gets the file's first frame, which is the same still the
    // library's cards are built from - so a movie already in the catalogue never costs a
    // second load, and the grabber drops paths it cannot open instead of failing here.
    function pumpStills() {
        const need = [];
        for (const j of jobs) {
            if (j.jobPoster.length > 0)
                continue;
            const p = downloads.filePath(j.jobId);
            if (p.length > 0)
                need.push(p);
        }
        if (need.length > 0)
            thumbs.covers(need);
    }


    // The folder the jobs land in, opened in Explorer. A press here used to open the
    // folder picker instead - that decision lives in 设置 now, and this mark does the
    // thing a folder mark is expected to do.
    function openDir() {
        if (saveDir.length === 0)
            return;
        Qt.openUrlExternally("file:///" + saveDir.replace(/\\/g, "/"));
    }

    Component.onCompleted: {
        refreshJobs();
        pumpStills();
    }

    Connections {
        target: downloads
        function onRowChanged() { page.refreshJobs(); }
        function onCountChanged() {
            page.refreshJobs();
            page.pumpStills();
        }
    }

    Connections {
        target: thumbs
        // A first frame just landed in the cache: read the rows again so the card that
        // was waiting on it picks the picture up.
        function onCoverReady(path, file) {
            page.refreshJobs();
            page.pumpStills();
        }
    }

    DownloadsPanel {
        x: page.pad
        y: 0
        width: parent.width - 2 * page.pad
        height: parent.height
        jobs: page.jobs
        saveDir: page.saveDir
        onPlayRequested: (jobId, running) => {
            if (downloads.isComplete(jobId)) {
                playerCore.load(downloads.filePath(jobId));
                page.playerRequested();
                return;
            }
            if (!running)
                downloads.start(jobId);
            openWait.job = jobId;
            openWait.tries = 0;
            openWait.restart();
        }
        // The row's transport mark, and it stays on this page: the child is started or
        // stopped, nothing is loaded into the player.
        onToggleRequested: (jobId, run) => {
            if (run)
                downloads.start(jobId);
            else
                downloads.stop(jobId);
        }
        onRemoveRequested: (jobId) => downloads.remove(jobId)
        onWantRequested: (jobId, mode) => downloads.setWant(jobId, mode)
        onAddRequested: {
            addSheet.reset();
            addSheet.open();
        }
        onOpenDirRequested: page.openDir()
        // Nothing happens here but the ask. remove() stops the child and takes the state
        // folder with the row, so a job that was still filling loses its partial
        // progress - a single press on a toolbar mark should not say that on its own.
        onClearAllRequested: clearAsk.open()
        // Both directions of the whole list. A finished row is left alone: start() on it
        // has nothing to fetch, and stop() would only take its status away.
        onAllRequested: (run) => {
            for (const j of page.jobs) {
                if (j.jobComplete)
                    continue;
                if (run && !j.jobRunning)
                    downloads.start(j.jobId);
                else if (!run && j.jobRunning)
                    downloads.stop(j.jobId);
            }
        }
        onTipFor: (item, text, keys) => page.tipFor(item, text, keys)
        onTipOff: (item) => page.tipOff(item)
    }

    // The play mark on a stopped job has to wait for its child to report a port
    // before there is a url to load.
    Timer {
        id: openWait
        property int job: -1
        property int tries: 0
        interval: 250
        repeat: true
        onTriggered: {
            const u = downloads.playUrl(job);
            if (u.length > 0) {
                stop();
                playerCore.loadTitled(u, downloads.playTitle(job));
                page.playerRequested();
            } else if (++tries > 60) {
                stop();
            }
        }
    }

    // ---- 新建: paste one or more links, choose where they land, start them ----
    AddLinksDialog {
        id: addSheet
        refWidth: page.width
        saveDir: page.saveDir
        onDirRequested: page.dirRequested()
        // One row per non-empty line. The engine takes them one at a time and answers -1
        // for a line that is neither a magnet nor a .torrent file; those go back into the
        // box with a sentence instead of being dropped on the floor. Only a clean paste
        // closes the dialog.
        onSubmitted: (links) => {
            const bad = [];
            for (const l of links)
                if (downloads.add(l, page.saveDir) < 0)
                    bad.push(l);
            page.refreshJobs();
            if (bad.length > 0) {
                addSheet.rejectSome(bad, links.length - bad.length);
                return;
            }
            addSheet.close();
        }
    }

    // ---- 清空全部列表: the ask, in a card of our own ----
    // Not a system message box: this is the same charcoal the 新建 box and the settings
    // sheet are made of, and the same two answers the source row's 保存/取消 line uses.
    // It says the number, and it says what is lost - the rows, and with a row that was
    // still filling its partial progress - because that is what the press does.
    Popup {
        id: clearAsk
        modal: true
        // No dim behind it, as everywhere else in this app: the list it covers is the
        // thing being talked about.
        Overlay.modal: Item { }
        padding: 0
        // Whole pixels, and the overlay read through a property: it is null while the
        // popup is still being built, and reading it inline threw there.
        readonly property Item ovl: Overlay.overlay
        // On the overlay rather than in this page, so x/y are the window's: as the page's
        // child the card sat 59 px low (the page starts under the tab row), which is the
        // same offset the 新建 box had.
        parent: Overlay.overlay
        x: ovl ? Math.round((ovl.width - clearAsk.width) / 2) : 0
        y: ovl ? Math.round((ovl.height - clearAsk.height) / 2) : 0
        readonly property real pad: Math.round(shellHeight * 0.028)
        readonly property real btnW: Math.round(shellHeight * 0.086)
        readonly property real btnH: Math.round(shellHeight * 0.05)
        width: Math.round(page.width * 0.34)
        height: askBody.implicitHeight + 2 * clearAsk.pad
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: 6
            color: "#141418"
            border.width: 1
            border.color: "#3A3A40"
        }

        Column {
            id: askBody
            x: clearAsk.pad
            y: clearAsk.pad
            width: clearAsk.width - 2 * clearAsk.pad
            spacing: 0

            Text {
                text: "清空全部列表？"
                color: Theme.textPrimary
                font.pixelSize: page.titleFs
                font.family: Theme.uiFont
            }
            Item { width: 1; height: Math.round(page.smallFs * 0.9) }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: page.jobs.length + " 条会从列表里全部去掉。正在下的会停掉，没下完的那部分进度一起丢；"
                      + "磁盘上已经下好的文件不动。"
                color: Theme.textSecondary
                font.pixelSize: page.smallFs
                font.family: Theme.uiFont
            }
            Item { width: 1; height: Math.round(page.smallFs * 1.2) }
            // Bottom right, the way an answer sits under a question.
            Row {
                x: askBody.width - width
                spacing: 8
                Repeater {
                    model: [
                        { key: "go", text: "清空" },
                        { key: "cancel", text: "取消" }
                    ]
                    delegate: Rectangle {
                        id: ans
                        required property var modelData
                        width: clearAsk.btnW
                        height: clearAsk.btnH
                        radius: 3
                        color: ansHit.containsMouse ? "#2A2A2E" : "transparent"
                        border.width: 1
                        border.color: modelData.key === "go" ? Theme.accent : "#3A3A40"
                        Text {
                            anchors.centerIn: parent
                            text: ans.modelData.text
                            color: modelData.key === "go" ? Theme.accent : Theme.textSecondary
                            font.pixelSize: page.smallFs
                            font.family: Theme.uiFont
                        }
                        MouseArea {
                            id: ansHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (ans.modelData.key === "cancel") {
                                    clearAsk.close();
                                    return;
                                }
                                for (const j of page.jobs)
                                    downloads.remove(j.jobId);
                                page.refreshJobs();
                                clearAsk.close();
                            }
                        }
                    }
                }
            }
        }
    }
}
