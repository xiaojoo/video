import QtQuick
import QtQuick.Controls
import Playerui

// The playlist dock: two lists in the column the window grows to fit - the files in the
// same folder as what is playing, and the chapters of the file itself. There is no
// chapter drawer any more and no chapter button in the control row: one cell opens this
// column, and the two tabs inside it are the whole surface.
//
// The sizes are ratios of the launch height rather than the pixels the drawer hardcoded,
// because this column's width is a ratio too: at 4K the two must still agree.
Rectangle {
    id: pl

    // The launch height. The dock is the reason the window got wider, so it sizes
    // itself by the number the rest of the pinned chrome uses, never by the width it
    // just changed.
    property real refH: 723
    // What PlayerCore is holding. That is a file system path, not a file:// url -
    // `PlayerCore::load` folds the url down to a path and keys the strip cache, the
    // duration write-back and the catalogue on that one string - so the rows are
    // matched by their path and loaded by their url.
    property string mediaPath: ""
    // Library::itemsBeside, and the chapter edges and labels the header line and the
    // hover bubble already read. All handed over by the caller, so no two surfaces can
    // disagree about what this file has.
    property var items: []
    property var edges: []
    property var labels: []
    property int chapter: -1
    // Which list is showing: "files" or "chapters". The caller owns it, because the cell
    // and the `T` key each ask for one of them.
    property string tab: "files"

    signal picked(string url)
    signal chapterPicked(int index)
    signal dismissed()
    signal tabPicked(string tab)

    readonly property real pad: Math.round(0.0221 * refH)          // 16 px at 723
    readonly property real thumbW: Math.round(0.1328 * refH)        // 96
    readonly property real thumbH: Math.round(thumbW * 9 / 16)      // 54
    readonly property real rowH: thumbH + 2 * Math.round(0.0111 * refH)   // 54 + 16 = 70
    readonly property real chapRowH: Math.round(0.047 * refH)       // 34, the drawer's row
    readonly property real titleFs: Math.round(0.0194 * refH)       // 14
    readonly property real smallFs: Math.round(0.0180 * refH)       // 13
    readonly property real headFs: Math.round(0.0249 * refH)        // 18
    readonly property real tabH: Math.round(0.047 * refH)           // 34
    // Where the text column starts and how wide it is: the cover, then a gap.
    readonly property real textX: pad + thumbW + Math.round(0.0138 * refH)   // 16 + 96 + 10
    readonly property real textW: Math.max(40, width - textX - pad)
    // The chapter rows keep the drawer's column: a fixed time on the right, the name
    // eats the rest.
    readonly property real chapTimeW: Math.round(0.094 * refH)      // 68

    // A file the catalogue never probed has duration 0, and 0:00 would be a claim; the
    // row hides that column instead. Chapter one starts at 0 and has to read 0:00.
    function fmt(t) {
        if (!isFinite(t) || t < 0)
            return "";
        const s = Math.floor(t % 60), m = Math.floor(t / 60) % 60, h = Math.floor(t / 3600);
        const p = (v, n) => String(v).padStart(n, "0");
        return h > 0 ? h + ":" + p(m, 2) + ":" + p(s, 2) : p(m, 1) + ":" + p(s, 2);
    }
    // The library's own name for the file when the user typed one, the file name
    // otherwise - the same rule the player header follows, so the two surfaces cannot
    // call the same file two things.
    function label(it) {
        return it.title.length > 0 ? it.title : it.name;
    }
    function name(i) {
        return labels.length > i ? labels[i] : "";
    }

    width: Math.round(Theme.playlistWidthRatio * refH)
    height: parent ? parent.height : 0
    x: parent ? parent.width - width : 0
    // Opaque, flush, square: this is a column of the shell, not a card floating over
    // the film. The tone is the old drawer's, so the two read as one family.
    // Whether it is on screen at all is the caller's `plOpen`.
    color: "#FF0D0D10"

    Item {
        id: headRow
        x: 0
        y: Math.round(pl.refH * 0.0166)
        width: parent.width
        // A fixed band rather than `Math.max(headText.height, ...)`: a binding on an
        // object that reads its own children declared below it is the shape that has
        // silently never re-evaluated in this Qt.
        height: Math.round(pl.refH * 0.047)

        Text {
            id: headText
            x: pl.pad
            anchors.verticalCenter: parent.verticalCenter
            text: "播放列表"
            color: Theme.textPrimary
            font.pixelSize: pl.headFs
            font.family: Theme.uiFont
        }

        Text {
            id: closeGlyph
            anchors.right: parent.right
            anchors.rightMargin: Math.round(pl.refH * 0.0194)
            anchors.verticalCenter: parent.verticalCenter
            text: "✕"
            color: Theme.textSecondary
            font.pixelSize: Math.round(pl.refH * 0.0221)
            font.family: Theme.uiFont
            MouseArea {
                anchors.fill: parent
                anchors.margins: -6
                cursorShape: Qt.PointingHandCursor
                onClicked: pl.dismissed()
            }
        }
    }

    // Two lists one column apart, so the switch is a tab strip rather than two surfaces
    // stacked over each other: each label centred in its own half, the active one
    // underlined - the shape of the reference.
    Item {
        id: tabs
        x: 0
        y: headRow.y + headRow.height + Math.round(pl.refH * 0.0083)
        width: parent.width
        height: pl.tabH

        Repeater {
            model: [{ key: "files", text: "文件" }, { key: "chapters", text: "章节" }]
            delegate: Item {
                required property var modelData
                required property int index
                readonly property bool active: pl.tab === modelData.key
                width: tabs.width / 2
                height: tabs.height
                x: index * width

                Text {
                    id: tabLabel
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.text
                    color: parent.active ? Theme.textPrimary : Theme.textSecondary
                    font.pixelSize: pl.titleFs
                    font.family: Theme.uiFont
                }
                Rectangle {
                    width: Math.round(tabLabel.implicitWidth * 0.65)
                    height: Math.max(2, Math.round(pl.refH * 0.0028))
                    radius: height / 2
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    color: Theme.accent
                    visible: parent.active
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: pl.tabPicked(modelData.key)
                }
            }
        }
    }

    // ---- the folder's files ----
    ListView {
        id: fileList
        x: 0
        y: tabs.y + tabs.height
        width: parent.width
        height: parent.height - y
        clip: true
        visible: pl.tab === "files"
        enabled: visible
        boundsBehavior: Flickable.StopAtBounds
        model: pl.items
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        // Both empty cases say what is missing rather than showing a blank column: a
        // file that is not from a folder (a disc), and a folder with nothing else.
        Text {
            visible: pl.items.length === 0
            x: pl.pad
            y: Math.round(pl.refH * 0.0249)
            width: fileList.width - 2 * pl.pad
            text: pl.mediaPath.length === 0 ? "没有正在播放的文件" : "这个文件夹里没有其他可播放的文件"
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            font.pixelSize: pl.smallFs
            font.family: Theme.uiFont
        }

        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property bool playing: modelData.path === pl.mediaPath
            width: fileList.width
            height: pl.rowH

            Rectangle {
                anchors.fill: parent
                color: rowHit.containsMouse ? "#22222A" : "transparent"
            }

            Rectangle {
                id: art
                x: pl.pad
                anchors.verticalCenter: parent.verticalCenter
                width: pl.thumbW
                height: pl.thumbH
                radius: 4
                color: "#17171B"
                // Two pixels inside: at one, the image's square corner still sticks out
                // of the 4 px arc, and a plain quad is beyond the reach of any radius.
                Image {
                    anchors.fill: parent
                    anchors.margins: 2
                    source: row.modelData.poster
                    fillMode: Image.PreserveAspectCrop
                    visible: status === Image.Ready
                }
            }

            Text {
                id: title
                x: pl.textX
                y: Math.round(pl.refH * 0.0111)
                width: pl.textW
                text: pl.label(row.modelData)
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                color: row.playing ? Theme.accent : Theme.textPrimary
                font.pixelSize: pl.titleFs
                font.family: Theme.uiFont
            }

            Text {
                id: nowPlaying
                visible: row.playing
                x: pl.textX
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Math.round(pl.refH * 0.0111)
                text: "正在播放"
                color: Theme.accent
                font.pixelSize: pl.smallFs
                font.family: Theme.uiFont
            }

            Text {
                x: nowPlaying.visible ? nowPlaying.x + nowPlaying.width
                                         + Math.round(pl.refH * 0.0111) : pl.textX
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Math.round(pl.refH * 0.0111)
                visible: row.modelData.duration > 0
                text: pl.fmt(row.modelData.duration)
                color: Theme.textSecondary
                font.pixelSize: pl.smallFs
                font.family: Theme.uiFont
            }

            MouseArea {
                id: rowHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: pl.picked(row.modelData.url)
            }
        }
    }

    // ---- this file's chapters ----
    ListView {
        id: chapList
        x: 0
        y: tabs.y + tabs.height
        width: parent.width
        height: parent.height - y
        clip: true
        visible: pl.tab === "chapters"
        enabled: visible
        boundsBehavior: Flickable.StopAtBounds
        model: pl.edges
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Text {
            visible: pl.edges.length === 0
            x: pl.pad
            y: Math.round(pl.refH * 0.0249)
            width: chapList.width - 2 * pl.pad
            text: "此文件没有章节"
            color: Theme.textSecondary
            font.pixelSize: pl.smallFs
            font.family: Theme.uiFont
        }

        delegate: Item {
            required property var modelData
            required property int index
            width: chapList.width
            height: pl.chapRowH

            Rectangle {
                anchors.fill: parent
                color: chapHit.containsMouse ? "#22222A" : "transparent"
            }
            Text {
                x: pl.pad
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 2 * pl.pad - pl.chapTimeW
                text: (index + 1) + ". " + pl.name(index)
                elide: Text.ElideRight
                color: pl.chapter === index ? Theme.accent : Theme.textPrimary
                font.pixelSize: pl.titleFs
                font.family: Theme.uiFont
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: pl.pad
                anchors.verticalCenter: parent.verticalCenter
                text: pl.fmt(modelData)
                color: Theme.textSecondary
                font.pixelSize: pl.smallFs
                font.family: Theme.uiFont
            }
            MouseArea {
                id: chapHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: pl.chapterPicked(index)
            }
        }
    }

    // Right-clicks that land here must not fall through to the film's context menu,
    // which is a window-wide layer under this one. Declared last with a negative z so
    // the rows keep their own clicks; this only catches what they do not take.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        z: -1
    }
}
