import QtQuick

// Preview scene: all four customization panels (one per seat color) over a 2x2 game backdrop.
// Render with:  overlay_preview ScenePanels.qml
Item {
    id: root
    width: 1600; height: 900

    Grid {                                   // 2x2 Doom backdrop, like a 4-player split
        anchors.fill: parent; columns: 2; rows: 2
        Repeater { model: 4
            Image { width: root.width / 2; height: root.height / 2; source: "preview_bg.png"; fillMode: Image.PreserveAspectCrop } }
    }

    Theme { id: appTheme }

    Repeater {
        model: 4
        PlayerSetup {
            theme: appTheme
            width: root.width / 2; height: root.height / 2
            x: (index % 2) * (root.width / 2)
            y: Math.floor(index / 2) * (root.height / 2)
            join: ({ controller: index, seat: index, field: index,
                     word1: ["happy", "sneaky", "angry", "quick"][index],
                     word2: ["imp", "revvy", "baron", "fiend"][index],
                     crosshair: index === 3 ? 0 : 3, motion: index % 2 === 0, taken: false })
        }
    }
}
