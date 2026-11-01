import QtQuick
import QtQuick.Controls
import Playerui

// The download jobs, as a panel that can be hosted anywhere - today on the 下载 page,
// which is its own tab beside 本地媒体库 and 在线媒体.
//
// Rows are two lines in the same rhythm the playlist dock uses, and every number on
// them comes from the engine child process's own /status. The engine's English
// sentence about resume is not shown; the numbers it was built from are.
Item {
    id: panel

    // The engine's rows, as a plain array for the toolbar's own arithmetic - and as a
    // ListModel for the list itself. The list used to hang off the array directly, and
    // every 1 s status tick replaced it, which a ListView reads as a brand-new model:
    // measured at 12 delegate rebuilds in 7 s, i.e. every row destroyed and rebuilt once
    // a second. Each of those took the cover image back through its async load (a blank
    // frame), dropped the hover state, and could swallow a click that landed mid-rebuild -
    // which is what read as 卡顿 on the start/pause mark. `syncRows` moves the values into
    // the rows that are already there instead.
    // The whole-list mark is a reading of the list, not a memory of the last press: it
    // says 暂停 only while every row it can act on is pulling, and 开始 otherwise. One
    // row starting or stopping is not a statement about the rest, so the mark turns only
    // when the list as a whole has. A finished row is skipped - start() has nothing to
    // fetch for it and stop() would only take its status away.
    readonly property bool allRunning: {
        let n = 0
        for (let i = 0; i < jobs.length; ++i) {
            if (jobs[i].jobComplete)
                continue
            if (!jobs[i].jobRunning)
                return false
            ++n
        }
        return n > 0
    }
    // The mark keeps its place and its plate when there is nothing to act on - it goes
    // dim, not away, so the toolbar does not reshuffle itself as jobs come and go.

    property var jobs: []
    onJobsChanged: syncRows()

    function indexOfRow(id) {
        for (let i = 0; i < jobRows.count; ++i)
            if (jobRows.get(i).jobId === id)
                return i;
        return -1;
    }

    function syncRows() {
        const seen = []
        for (let i = 0; i < jobs.length; ++i) {
            const m = jobs[i];
            seen.push(m.jobId)
            const at = indexOfRow(m.jobId)
            if (at < 0)
                jobRows.insert(i, m)
            else {
                if (at !== i)
                    jobRows.move(at, i, 1)
                jobRows.set(i, m)
            }
        }
        // From the back, so removing one does not shift the indices of another.
        for (let i = jobRows.count - 1; i >= 0; --i)
            if (seen.indexOf(jobRows.get(i).jobId) < 0)
                jobRows.remove(i)
    }

    // Where new jobs land, and where the row says they will land.
    property string saveDir: ""
    // Which row is asking "delete this?" - one id, so only one row can ask at a time.
    property int pendingRemove: -1

    // The line the list starts under, the same one the tab row leaves above the toolbar:
    // the page's own gutter. At 0 the first card sat directly on the toolbar's bottom
    // edge while the row above it had 13 px, so the whole column read as one block.
    readonly property real gapY: Math.round(Theme.refW * Theme.sideInsetWRatio)
    // Sizes follow the window's launch height the way the rest of the app's chrome does.
    // Off the panel's own height they came out two thirds of everything around them - 11 px
    // text in a page whose tabs read 13 - and grew whenever the page grew.
    readonly property real rowH: Math.round(0.094 * Theme.refH)
    readonly property real titleFs: Math.round(0.0194 * Theme.refH)
    readonly property real smallFs: Math.round(0.0180 * Theme.refH)
    readonly property real hitS: Math.round(0.0304 * Theme.refH)
    // The same row height as the two menu rows and the pager above it.
    readonly property real barH: Math.round(0.05 * Theme.refH)
    // The picture on the left of a job row: 16:9, the height the card has left after its
    // own inset.
    readonly property real thumbH: Math.round(0.072 * Theme.refH)

    // The badge on the picture. The name is all the row has to say what kind of file this
    // is, and the reference puts that on the picture rather than in the sentence under it.
    function extOf(name) {
        const i = name.lastIndexOf(".");
        if (i < 0 || name.length - i > 5)
            return "";
        return name.substring(i + 1).toUpperCase();
    }

    signal playRequested(int jobId, bool running)
    // The row's one transport mark: pull, or hold the pull. Watching the file is the card
    // picture's own click, not this mark's - two glyphs on one row that both looked like
    // "play" was the thing being asked for here.
    signal toggleRequested(int jobId, bool run)
    signal removeRequested(int jobId)
    signal wantRequested(int jobId, string mode)
    signal addRequested()
    // The toolbar's four marks. They are one line of the panel's own hierarchy rather
    // than four more items in the host: the panel knows which rows are finished and
    // which are pulling, so it is the one that can grey a mark out.
    signal openDirRequested()
    signal clearAllRequested()
    signal allRequested(bool run)

    // The one bubble the app owns lives up in Main.qml; these two hand it the mark under
    // the pointer. Qt's own ToolTip is a native popup that cannot be styled with the chrome.
    signal tipFor(var item, string text, string keys)
    signal tipOff(var item)

    function sizeText(n) {
        if (n >= 1073741824)
            return (n / 1073741824).toFixed(2) + " GiB"
        if (n >= 1048576)
            return (n / 1048576).toFixed(1) + " MiB"
        return Math.round(n / 1024) + " KiB"
    }

    Column {
        anchors.fill: parent
        spacing: panel.gapY

        // The toolbar: four square marks and the one primary action. It used to be two
        // wide text pills - 添加磁力链接 and 存到 <path>. The path lives in the add dialog
        // and in 设置 now, and 新建 carries the way in, so what is left here is what the
        // page is for: the folder, the finished rows, the whole list, and the settings.
        //
        // Two ends, not one run: the marks stay on the gutter and 新建 sits on the right
        // edge, the way the reference puts its primary action out of the icons' way.
        Item {
            id: bar
            width: parent.width
            height: panel.barH

            Row {
                id: marks
                spacing: Math.round(Theme.refH * 0.0111)
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter

                Repeater {
                    model: [
                        { key: "dir", icon: "folder", tip: "打开下载目录", dead: false },
                        { key: "clear", icon: "trash", tip: "清除全部列表", dead: panel.jobs.length === 0 },
                        // One mark for both directions of the whole list: an arrow into
                        // the tray while something is stopped, a pause bar while it runs.
                        { key: "all", icon: panel.allRunning ? "pause" : "downloadAll",
                          tip: panel.allRunning ? "全部暂停" : "全部开始",
                          dead: panel.jobs.length === 0 },
                    ]
                    delegate: Rectangle {
                        id: mark
                        required property var modelData
                        width: panel.barH
                        height: panel.barH
                        radius: Theme.controlRadius
                        // A mark with nothing to act on keeps its place and its plate; only
                        // the glyph greys, at the value the control row already uses for a
                        // dead cell - dropping the whole plate's opacity turned the fill to
                        // #101013, which read as a hole in the toolbar rather than a control
                        // that cannot be pressed. Its bubble still says what it would do.
                        color: markHit.containsMouse && !modelData.dead ? "#2A2A31" : "#1B1B1F"
                        border.width: 1
                        border.color: "#3A3A40"

                        Icon {
                            anchors.centerIn: parent
                            width: Math.round(panel.barH * 0.5)
                            height: width
                            name: mark.modelData.icon
                            color: mark.modelData.dead ? "#4A4A4E" : Theme.glyph
                        }
                        MouseArea {
                            id: markHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: mark.modelData.dead ? Qt.ArrowCursor : Qt.PointingHandCursor
                            onEntered: panel.tipFor(mark, mark.modelData.tip, "")
                            onExited: panel.tipOff(mark)
                            onClicked: {
                                if (mark.modelData.dead)
                                    return;
                                if (mark.modelData.key === "dir")
                                    panel.openDirRequested();
                                else if (mark.modelData.key === "clear")
                                    panel.clearAllRequested();
                                else if (mark.modelData.key === "all")
                                    panel.allRequested(!panel.allRunning)
                            }
                        }
                    }
                }
            }

            // The way in, in the accent rather than as one more outlined mark: it is the
            // only thing on this line that starts something.
            Rectangle {
                id: newBtn
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: newRow.implicitWidth + Math.round(Theme.refH * 0.039)
                height: panel.barH
                radius: Theme.controlRadius
                color: newHit.containsMouse ? "#FFA733" : Theme.accent

                Row {
                    id: newRow
                    anchors.centerIn: parent
                    spacing: Math.round(Theme.refH * 0.008)
                    Icon {
                        width: Math.round(panel.smallFs * 1.35)
                        height: width
                        name: "plus"
                        color: "#15151A"
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: "新建"
                        color: "#15151A"
                        font.pixelSize: panel.smallFs
                        font.family: Theme.uiFont
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                MouseArea {
                    id: newHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.addRequested()
                }
            }
        }

        ListModel { id: jobRows }

        ListView {
            id: list
            width: parent.width
            height: parent.height - bar.height - panel.gapY
            clip: true
            spacing: 9
            boundsBehavior: Flickable.StopAtBounds
            model: jobRows

            Text {
                visible: panel.jobs.length === 0
                // The same box the toolbar's plates and the job rows sit in. The extra
                // gutter here pushed the sentence 13 px right of both: measured, its left
                // edge was at 30 against the row of marks' 13.
                x: 0
                y: Math.round(Theme.refH * 0.03)
                width: list.width
                text: "还没有任务。点上面的「新建」加一条链接，或到 在线媒体 里挑一部。"
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.pixelSize: panel.smallFs
                font.family: Theme.uiFont
            }

            delegate: Item {
                id: row
                required property var modelData
                width: list.width
                height: panel.rowH
                readonly property bool pending: panel.pendingRemove === modelData.jobId
                // The card runs the full width of the panel, so its left edge is the page's
                // gutter - the same line the tabs above start on. The content is inset
                // inside the card instead.
                readonly property real inset: Math.round(Theme.refH * 0.0166)
                readonly property real thumbW: Math.round(panel.thumbH * 16 / 9)
                readonly property real textX: inset + thumbW + 14
                // The gap between two neighbours on the right, and the same gap between the
                // text block and the first mark.
                readonly property real markGap: Math.round(panel.hitS * 0.45)
                // A property, not a function: a binding that *calls* a function does not
                // subscribe to what the function reads, so `reservedW()` kept the answer it
                // got before the marks had laid themselves out and the progress bar ran out
                // under them.
                readonly property real textW: row.width - textX - inset - marks.width - markGap

                function statusLine() {
                    if (pending)
                        return "再点一次叉确认；点别处取消"
                    if (modelData.jobError.length > 0)
                        return modelData.jobError
                    const got = panel.sizeText(modelData.jobCompleted)
                    const all = modelData.jobTotal > 0 ? " / " + panel.sizeText(modelData.jobTotal) : ""
                    if (modelData.jobComplete)
                        return got + all + " · 已完成"
                    if (!modelData.jobRunning)
                        return got + all + " · 已停止"
                    if (modelData.jobTotal <= 0)
                        return modelData.jobKind === "http"
                            ? "正在探测地址…"
                            : "正在取种子信息 · " + modelData.jobPeers + " 个对端"
                    let t = modelData.jobPct.toFixed(1) + "% · " + got + all
                          + " · " + panel.sizeText(Math.round(modelData.jobRate)) + "/s"
                          + (modelData.jobKind === "http"
                                ? " · 连接 " + modelData.jobPeers
                                : " · 做种 " + modelData.jobSeeders + "/连接 " + modelData.jobPeers)
                    if (modelData.jobResumeMs > 0)
                        t += " · 接上进度 " + modelData.jobResumeMs + " ms"
                    return t
                }

                Rectangle {
                    anchors.fill: parent
                    color: row.pending ? "#22222A" : "transparent"
                }

                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    color: "#141418"
                    border.width: 1
                    border.color: row.pending ? Theme.accent : "#26262A"
                }

                // The picture is the way in: it is the biggest thing on the row and it is
                // what you are deciding about. The badge says what kind of file it is.
                Rectangle {
                    id: thumb
                    x: row.inset
                    y: Math.round((row.height - panel.thumbH) / 2)
                    width: row.thumbW
                    height: panel.thumbH
                    radius: 4
                    color: thumbHit.containsMouse ? "#26262E" : "#1F1F25"

                    // The site's card picture when the job came off the wall, the file's
                    // own first frame when it did not. No sourceSize: that belongs to the
                    // layout and re-decodes every time the window is dragged.
                    Image {
                        id: thumbArt
                        anchors.fill: parent
                        anchors.margins: 1
                        source: modelData.jobPoster
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        visible: status === Image.Ready
                    }

                    // The mark sits on a plate, or it disappears on a bright frame.
                    Rectangle {
                        anchors.centerIn: parent
                        width: Math.round(panel.thumbH * 0.56)
                        height: width
                        radius: width / 2
                        color: "#B30A0A0C"
                        Icon {
                            anchors.centerIn: parent
                            width: Math.round(parent.width * 0.62)
                            height: width
                            name: "watchNow"
                            color: Theme.glyph
                        }
                    }
                    Rectangle {
                        x: 5
                        y: parent.height - 19
                        width: badge.implicitWidth + 8
                        height: 15
                        radius: 3
                        color: "#0A0A0C"
                        Text {
                            id: badge
                            anchors.centerIn: parent
                            text: panel.extOf(modelData.jobName)
                            color: Theme.textSecondary
                            font.pixelSize: Math.round(panel.smallFs * 0.72)
                            font.family: Theme.uiFont
                        }
                    }
                    MouseArea {
                        id: thumbHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        // The row says whether the bytes on disk could be demuxed yet, because
                        // for a plain mp4 the answer is "not until the last box lands" - its
                        // index sits at the tail. Offering a play that cannot start is what
                        // read as a dead button and a player page full of the last film.
                        onEntered: panel.tipFor(thumb, modelData.jobPlayable
                                                ? "播放视频" : "暂不能播放", "")
                        onExited: panel.tipOff(thumb)
                        onClicked: if (modelData.jobPlayable)
                                       panel.playRequested(modelData.jobId, modelData.jobRunning)
                    }
                }

                Text {
                    id: nameText
                    x: row.textX
                    y: row.inset
                    width: row.textW
                    text: row.pending ? "删掉这个任务？已下到的文件不动" : modelData.jobName
                    elide: Text.ElideRight
                    color: row.pending ? Theme.accent : Theme.textPrimary
                    font.pixelSize: panel.titleFs
                    font.family: Theme.uiFont
                }

                Rectangle {
                    id: prog
                    x: row.textX
                    y: nameText.y + Math.round(panel.titleFs * 1.5)
                    width: row.textW
                    height: 3
                    radius: 1.5
                    color: Theme.track
                    visible: modelData.jobTotal > 0
                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        radius: parent.radius
                        width: Math.round(parent.width * modelData.jobPct / 100)
                        color: Theme.accent
                    }
                }

                Text {
                    x: row.textX
                    y: prog.y + prog.height + 7
                    width: row.textW
                    text: row.statusLine()
                    elide: Text.ElideRight
                    color: modelData.jobError.length > 0 && !row.pending
                           ? Theme.accent : Theme.textSecondary
                    font.pixelSize: panel.smallFs
                    font.family: Theme.uiFont
                }

                // The four marks, all drawn on every row. The pause used to appear only
                // while the job was pulling, so pressing it made it vanish - and it stopped
                // the download too, which is not what a mark beside 播放 should mean.
                Row {
                    id: marks
                    anchors.right: parent.right
                    anchors.rightMargin: row.inset
                    anchors.verticalCenter: parent.verticalCenter
                    // Wide enough to read as two different things: 2 px said "one control".
                    // The same gap sits between the text block and this row.
                    spacing: row.markGap

                    Rectangle {
                        id: modeChip
                        // Which half of the job this row is pulling. Kept in place when the
                        // file is complete - a mark that leaves would slide the other three.
                        readonly property bool live: !modelData.jobComplete
                        // A url job has no 边看/全下 to choose between; the mark would be a
                        // switch that changes nothing.
                        visible: modelData.jobKind !== "http"
                        width: Math.round(panel.hitS * 1.5)
                        height: panel.hitS
                        radius: 4
                        opacity: live ? 1 : 0.35
                        color: modeHit.containsMouse && live ? "#2A2A31" : "#1F1F25"
                        border.width: 1
                        border.color: modelData.jobWant === "all" ? Theme.accent : "#3A3A40"

                        Icon {
                            anchors.centerIn: parent
                            width: Math.round(panel.hitS * 0.62)
                            height: width
                            name: modelData.jobWant === "all" ? "downloadAll" : "watchNow"
                            color: modelData.jobWant === "all" ? Theme.accent : Theme.glyph
                        }
                        MouseArea {
                            id: modeHit
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: modeChip.live
                            cursorShape: Qt.PointingHandCursor
                            onEntered: panel.tipFor(modeChip,
                                modelData.jobWant === "all" ? "全下：整片拉完" : "边看：只拉要看的",
                                "")
                            onExited: panel.tipOff(modeChip)
                            onClicked: panel.wantRequested(modelData.jobId,
                                modelData.jobWant === "all" ? "stream" : "all")
                        }
                    }

                    Rectangle {
                        id: pullBox
                        width: panel.hitS
                        height: panel.hitS
                        radius: 4
                        color: pullHit.containsMouse ? "#2A2A31" : "#1F1F25"

                        Icon {
                            anchors.centerIn: parent
                            width: Math.round(panel.hitS * 0.55)
                            height: width
                            name: modelData.jobRunning ? "pause" : "play"
                            color: Theme.glyph
                        }
                        MouseArea {
                            id: pullHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            // On a finished row this mark used to say 开始下载 and do
                            // nothing: start() returns early once a job is complete, so the
                            // only thing a press here could do was nothing. The row is done,
                            // so the mark now means what its glyph already looked like.
                            onEntered: panel.tipFor(pullBox, modelData.jobComplete
                                                    ? "播放视频"
                                                    : (modelData.jobRunning ? "暂停下载"
                                                                            : "开始下载"), "")
                            onExited: panel.tipOff(pullBox)
                            onClicked: {
                                if (modelData.jobComplete)
                                    panel.playRequested(modelData.jobId, false)
                                else
                                    panel.toggleRequested(modelData.jobId, !modelData.jobRunning)
                            }
                        }
                    }

                    Rectangle {
                        id: removeBox
                        width: panel.hitS
                        height: panel.hitS
                        radius: 4
                        color: removeHit.containsMouse ? "#2A2A31" : "#1F1F25"

                        Icon {
                            anchors.centerIn: parent
                            width: Math.round(panel.hitS * 0.5)
                            height: width
                            name: "close"
                            color: row.pending ? Theme.accent : Theme.glyph
                        }
                        MouseArea {
                            id: removeHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onEntered: panel.tipFor(removeBox, row.pending ? "再点一次确认"
                                                                          : "删掉任务", "")
                            onExited: panel.tipOff(removeBox)
                            onClicked: {
                                if (row.pending) {
                                    panel.removeRequested(modelData.jobId)
                                    panel.pendingRemove = -1
                                } else {
                                    panel.pendingRemove = modelData.jobId
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // The list's bar, outside the Column so the Column does not treat it as a row.
    ThinScrollBar {
        target: list
        x: panel.width - width
        y: bar.height + panel.gapY
        height: panel.height - bar.height - panel.gapY
    }

    // A press that is not on a mark cancels a pending delete, so the question in the
    // row always has a way out.
    MouseArea {
        anchors.fill: parent
        z: -1
        onPressed: panel.pendingRemove = -1
    }
}
