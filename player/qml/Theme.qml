pragma Singleton
import QtQuick

QtObject {
    // Colours sampled from the reference: accent read at the play head,
    // track read at y=649 over the scrim.
    readonly property color accent: "#FF8B00"
    readonly property color track: "#463D2E"
    readonly property color textPrimary: "#FFFFFF"
    readonly property color textSecondary: "#B8B8B8"
    readonly property color glyph: "#E6E6E6"

    // One token for the whole chrome: the reference is set in SF Pro, which
    // cannot be shipped. Segoe UI Variable Display is the closest family this
    // OS actually has; swap this one line if a font file ever gets bundled.
    readonly property string uiFont: "Segoe UI Variable Display"

    // Ratios of window height, measured on the 1282x723 reference.
    readonly property real barCenterRatio: 0.898
    readonly property real controlCenterRatio: 0.957
    readonly property real controlRowHeightRatio: 0.084
    readonly property real tickInnerRatio: 0.0055
    readonly property real tickOuterRatio: 0.0104
    readonly property real lineThicknessRatio: 0.0058
    readonly property real knobDiameterRatio: 0.0194
    readonly property real tickHalfBaseRatio: 0.00215
    readonly property real titleFontRatio: 0.0295
    readonly property real subtitleFontRatio: 0.0205
    readonly property real titleTopRatio: 0.8110
    readonly property real titleLineGapRatio: 0.0097
    readonly property real iconRatio: 0.0345
    readonly property real thumbWidthRatio: 0.127
    readonly property real thumbHeightRatio: 0.093
    // Shortened from the reference's 0.45 so the band mpv draws subtitles in
    // can sit above it: under --wid the subs are painted into the host window
    // and our scrim would otherwise flatten them to 38/255. The matching lift is
    // mpv's sub-margin-y=185, set once in PlayerCore.
    readonly property real scrimHeightRatio: 0.24

    // Ratios of window width.
    readonly property real sideInsetWRatio: 0.0203
    readonly property real leftGroupPitchWRatio: 0.0352
    readonly property real centerPitchWRatio: 0.0507
}
