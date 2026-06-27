import QtQuick

// "Clean Console" palette -- restrained modern dark UI: a solid charcoal panel (reads reliably over busy
// gameplay), one warm amber accent, minimal glow. Hex STRINGS so they bind as colors AND drop into the
// cards' rich-text markup. Instantiated once in Overlay.qml and passed to the cards.
QtObject {
    readonly property string bg:       "#16181d"     // backdrop tint (panels sit on the game)
    readonly property string panel:    "#f01e2128"   // ~94% opaque card surface
    readonly property string row:      "#262a33"     // focused-row fill
    readonly property string chip:     "#2a2e37"     // name chips / value chips
    readonly property string border:   "#2c3038"

    readonly property string text:     "#e8eaf0"
    readonly property string textDim:  "#8a90a0"

    readonly property string accent:   "#ffb648"     // warm amber -- default accent / fallback
    readonly property string accentSoft: "#22ffb648" // amber wash for focused-field fills
    readonly property string good:     "#67d98b"     // motion-comfort ON
    readonly property string bad:      "#ff6b6b"     // name-taken warning

    // per-seat accent colors -- each seat's overlay is tinted its own color so players can tell panes apart.
    readonly property var seatColors: ["#67d98b", "#ffb648", "#46b4ff", "#b46cff"]  // 0 green, 1 amber, 2 blue, 3 purple

    // pixel font (Vonwaon Bitmap 16px, bundled in fonts/) -- loaded async; bindings update once it's ready.
    property FontLoader _pixel: FontLoader { source: Qt.resolvedUrl("fonts/VonwaonBitmap-16px.ttf") }
    readonly property string fontUi:   _pixel.name
    readonly property string fontMono: _pixel.name
    readonly property int    radius:   14
}
