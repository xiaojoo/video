import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import Playerui

Item {
    id: page

    readonly property real colW: Math.max(120, page.width * 0.13)
    // 横屏: a card is a frame off the film, so the box keeps the frame's own 16:9
    // and the still fills it instead of being sliced into a poster.
    readonly property real artH: colW * 9 / 16

    function fmtDuration(sec) {
        const s = Math.round(sec)
        const pad = (n) => String(n).padStart(2, "0")
        const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), ss = s % 60
        return h > 0 ? h + ":" + pad(m) + ":" + pad(ss) : m + ":" + pad(ss)
    }

    Rectangle {
        anchors.fill: parent
        color: "#0A0A0C"
    }

    // A cell is the art plus the text under it, and that text height is measured
    // from two hidden items carrying the delegate's own font rather than guessed
    // from a window ratio: the ratio that fits at 723 px clips the year line at 225.
    Text {
        id: titleMetric
        visible: false
        text: "M\nM"
        font.pixelSize: page.height * 0.021
        font.family: Theme.uiFont
    }
    Text {
        id: yearMetric
        visible: false
        text: "M"
        font.pixelSize: page.height * 0.019
        font.family: Theme.uiFont
    }
    readonly property real textBlock: 9 + titleMetric.height + yearMetric.height + 6

    Column {
        id: header
        x: page.width * 0.02
        y: page.height * 0.035
        spacing: page.height * 0.006

        Text {
            text: "媒体库"
            color: Theme.textPrimary
            font.pixelSize: Theme.titleFontRatio * page.height
            font.family: Theme.uiFont
        }
        Text {
            text: library.mediaCount + " 部 · " + library.fileCount + " 个文件 · 时长 "
                  + library.durationCount + "/" + library.fileCount + " · 未解析 "
                  + library.unparsedCount + " · 上次扫描 " + library.lastScanMs + " ms"
            color: Theme.textSecondary
            font.pixelSize: Theme.subtitleFontRatio * page.height
            font.family: Theme.uiFont
        }
    }

    Rectangle {
        id: rescan
        anchors.right: parent.right
        anchors.rightMargin: page.width * 0.02
        y: header.y + header.height * 0.15
        width: rescanText.implicitWidth + page.width * 0.02
        height: rescanText.height + page.height * 0.018
        radius: height / 2
        color: over.containsMouse ? "#2A2A2E" : "#1B1B1F"
        border.color: "#3A3A40"
        border.width: 1

        Text {
            id: rescanText
            anchors.centerIn: parent
            text: "重新扫描"
            color: Theme.textPrimary
            font.pixelSize: Theme.subtitleFontRatio * page.height
            font.family: Theme.uiFont
        }
        MouseArea {
            id: over
            anchors.fill: parent
            hoverEnabled: true
            onClicked: library.scan()
        }
    }

    Component.onCompleted: {
        if (shellAutoPick)
            folderPick.open();
        // Opening the page from the player never scanned, so a catalogue that was
        // already in the database showed up as an empty grid.
        else if (library.lastScanMs === 0)
            library.scan();
    }

    FolderDialog {
        id: folderPick
        title: "选择媒体目录"
        onAccepted: {
            library.addRoot(folderPick.selectedFolder)
            library.scan()
        }
    }

    Rectangle {
        id: pick
        anchors.right: rescan.left
        anchors.rightMargin: page.width * 0.012
        y: rescan.y
        width: pickText.implicitWidth + page.width * 0.02
        height: rescan.height
        radius: height / 2
        color: pickOver.containsMouse ? "#2A2A2E" : "#1B1B1F"
        border.color: "#3A3A40"
        border.width: 1

        Text {
            id: pickText
            anchors.centerIn: parent
            text: "选择文件夹"
            color: Theme.textPrimary
            font.pixelSize: Theme.subtitleFontRatio * page.height
            font.family: Theme.uiFont
        }
        MouseArea {
            id: pickOver
            anchors.fill: parent
            hoverEnabled: true
            onClicked: folderPick.open()
        }
    }

    // The library covers the whole window and hides the transport row, so this
    // page owns its own exit — an empty library has no card to click.
    Rectangle {
        id: backToPlayer
        anchors.right: pick.left
        anchors.rightMargin: page.width * 0.012
        y: pick.y
        width: backText.implicitWidth + page.width * 0.02
        height: pick.height
        radius: height / 2
        color: backOver.containsMouse ? "#2A2A2E" : "#1B1B1F"
        border.color: "#3A3A40"
        border.width: 1

        Text {
            id: backText
            anchors.centerIn: parent
            text: "返回播放"
            color: Theme.textPrimary
            font.pixelSize: Theme.subtitleFontRatio * page.height
            font.family: Theme.uiFont
        }
        MouseArea {
            id: backOver
            anchors.fill: parent
            hoverEnabled: true
            onClicked: win.page = "player"
        }
    }

    GridView {
        id: grid
        x: page.width * 0.02
        y: header.y + header.height + page.height * 0.03
        width: page.width - 2 * page.width * 0.02
        height: page.height - y - page.height * 0.03
        clip: true
        cellWidth: page.colW
        cellHeight: page.artH + page.textBlock
        boundsBehavior: Flickable.StopAtBounds
        model: libraryModel
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        delegate: Item {
            required property string title
            required property int year
            required property string path
            required property bool parsed
            required property string kind
            required property double duration
            required property string poster
            width: grid.cellWidth
            height: grid.cellHeight

            Rectangle {
                id: art
                x: 4
                y: 4
                width: parent.width - 8
                height: page.artH
                radius: 4
                color: "#17171B"
                border.width: mouse.containsMouse ? 2 : 1
                border.color: mouse.containsMouse ? Theme.accent : "#2C2C31"

                // The first frame, cropped to the card by the box it fills. Two
                // pixels inside the card: at one pixel the image's square corner
                // still sticks 1 px outside the card's 4 px arc, and an image is
                // a plain quad that no radius reaches.
                Image {
                    id: cover
                    anchors.fill: parent
                    anchors.margins: 2
                    source: poster
                    fillMode: Image.PreserveAspectCrop
                    visible: status === Image.Ready
                }

                // The badge and the 未识别 tag read off this, not off the still:
                // a white opening frame drops #B8B8B8 text to 1.9:1 on its own.
                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: parent.height * 0.30
                    visible: poster.length > 0
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#000A0A0C" }
                        GradientStop { position: 1.0; color: "#E60A0A0C" }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    text: kind === "audio" ? "♪" : ""
                    color: "#3A3A40"
                    font.pixelSize: parent.height * 0.3
                    visible: text.length > 0
                }
                Text {
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 6
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: parsed ? "" : "未识别"
                    color: Theme.accent
                    font.pixelSize: page.height * 0.018
                    font.family: Theme.uiFont
                }
                // Corner badge: the approved title/year rows keep their rhythm.
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    text: duration > 0 ? page.fmtDuration(duration) : ""
                    color: Theme.textSecondary
                    font.pixelSize: page.height * 0.016
                    font.family: Theme.uiFont
                }
            }

            Text {
                id: nameText
                x: 4
                y: art.y + art.height + 4
                width: parent.width - 8
                padding: 0
                text: title
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.WordWrap
                color: Theme.textPrimary
                font.pixelSize: page.height * 0.021
                font.family: Theme.uiFont
            }
            // Year gets its own line: sharing one item let a long title push it out.
            Text {
                x: 4
                y: nameText.y + nameText.height + 1
                text: year > 0 ? String(year) : " "
                color: Theme.textSecondary
                font.pixelSize: page.height * 0.019
                font.family: Theme.uiFont
            }

            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: { playerCore.load(path); win.page = "player"; }
            }
        }
    }
}
