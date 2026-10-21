pragma Singleton
import QtQuick

QtObject {
    // Colours sampled from the reference: accent read at the play head,
    // track read at y=649 over the scrim.
    readonly property color accent: "#FF8B00"
    readonly property color track: "#463D2E"
    // The unplayed part of the scrub bar, sampled off the reference strip he sent
    // (124,124,127 at the middle of the grey run).
    readonly property color trackFlat: "#7C7C7F"
    readonly property color textPrimary: "#FFFFFF"
    readonly property color textSecondary: "#B8B8B8"
    readonly property color glyph: "#E6E6E6"

    // One token for the whole chrome: the reference is set in SF Pro, which
    // cannot be shipped. Segoe UI Variable Display is the closest family this
    // OS actually has; swap this one line if a font file ever gets bundled.
    readonly property string uiFont: "Segoe UI Variable Display"

    // Ratios of window height, measured on the 1282x723 reference.
    readonly property real controlRowHeightRatio: 0.084
    readonly property real controlCenterRatio: 0.957
    readonly property real barCenterRatio: 0.898
    // 6 px on the reference strip, which is 0.0079 of the 762 px window it was
    // measured against. The old 0.0058 gave 4.4 px.
    readonly property real barLineRatio: 0.0079
    readonly property real knobDiameterRatio: 0.0194
    readonly property real titleFontRatio: 0.0295
    readonly property real subtitleFontRatio: 0.0205
    readonly property real titleLineGapRatio: 0.0097
    readonly property real iconRatio: 0.0345
    readonly property real thumbWidthRatio: 0.127
    readonly property real thumbHeightRatio: 0.093
    // Shortened from the reference's 0.45 so the band mpv draws subtitles in
    // can sit above it: under --wid the subs are painted into the host window
    // and our scrim would otherwise flatten them to 38/255. The matching lift is
    // mpv's sub-margin-y=185, set once in PlayerCore.
    //
    // 0.24 of the *launch* height (723 px) is 173 px; asked to make it smaller
    // without a number, so the coefficient is stated: 0.19, which is 0.79x of what
    // it was = 137 px. A ratio of shellHeight rather than the live height is the
    // point - the chrome under it is pinned, so scaling this with the window only
    // made the black band grow on the way to fullscreen.
    readonly property real scrimHeightRatio: 0.19

    // Ratios of window width. Halved from 0.0203 at his ask ("左右间隙调小一点");
    // no number given, so the coefficient is 0.5. Kept as a ratio of the *launch*
    // width (see `inset` in Main.qml): at 0.0102 of the live width it was 13 px on a
    // 1282 window and 39 px on a 3840 one, which is the gap he asked to hold still.
    readonly property real sideInsetWRatio: 0.0102
    // The gaps *between* the icons used to be ratios of the window width, so on a
    // 3840 monitor they came out 2.96x wider than at 1298. Written against the window
    // height instead - which the chrome no longer scales - each one keeps the pixel
    // value it had at the launch size: 0.0352 x 1298 = 45.7 px = 0.0599 x 762, and
    // 0.0507 x 1298 = 65.8 px = 0.0863 x 762.
    readonly property real leftGroupPitchHRatio: 0.0599
    readonly property real centerPitchHRatio: 0.0863
    readonly property real volWidthHRatio: 0.1124
    readonly property real volGapHRatio: 0.0205
    // The gap between the time at the end of the bar and the bar itself: 28 px on the
    // reference strip = 0.0368 of its height.
    readonly property real timeGapHRatio: 0.0368
    // The chapter drawer, as a ratio of the launch width: the card it replaces measured
    // 0.24 of the *live* width, which is how it turned into a 900 px column at 3840.
    readonly property real drawerWidthRatio: 0.24
    // The playlist dock on the right. Read off the reference he sent: that panel is
    // 330 px on a 677 px tall window = 0.49 of its height. 0.46 is the same column
    // narrowed by 7% so a row keeps a 200 px text column next to its 96 px cover at
    // the launch size. A ratio of the launch height, like everything else that must
    // not grow because the window did.
    readonly property real playlistWidthRatio: 0.46
}
