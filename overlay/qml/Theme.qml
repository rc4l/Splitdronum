import QtQuick

// Cyberpunk-dark palette (reference: the "cyberpunk-dark" VS Code theme -- near-black base, neon pink +
// cyan accents, code-editor mono type). Stored as hex STRINGS so they bind as colors AND drop into the
// cards' rich-text markup. Instantiated once in Overlay.qml and passed to the cards.
QtObject {
    readonly property string bg:       "#0a0a0f"     // near-black base
    readonly property string panel:    "#ee12121c"   // ~93% opaque card surface
    readonly property string bannerBg: "#ee1a0e14"
    readonly property string border:   "#2a2a3e"

    readonly property string text:     "#e6e6f2"
    readonly property string textDim:  "#6e6e86"

    readonly property string pink:     "#ff2a6d"   // primary neon (canonical cyberpunk-dark)
    readonly property string cyan:     "#00f0ff"   // secondary neon
    readonly property string yellow:   "#fcee0a"
    readonly property string green:    "#00ff9f"
    readonly property string purple:   "#bd00ff"
    readonly property string red:      "#ff0055"

    readonly property string fontUi:   "Segoe UI"
    readonly property string fontMono: "Consolas"   // the "code" feel for names/values
    readonly property int    radius:   14
}
