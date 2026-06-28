import QtQuick

// Preview the load-existing browser overlay (mode 2). Render with: overlay_preview SceneBrowse.qml
Item {
    width: 820; height: 620
    Image { anchors.fill: parent; source: "preview_bg.png"; fillMode: Image.PreserveAspectCrop }
    Theme { id: t }
    PlayerSetup {
        anchors.fill: parent
        theme: t
        join: ({ controller: 1, seat: 1, field: 0, word1: "angry", word2: "baron",
                 crosshair: 3, motion: false, taken: false, known: false, hold: 0.4, mode: 2, variant: "" })
        browse: ({ index: 1, items: [
            { name: "angry_baron", taken: true  },
            { name: "happy_imp",   taken: false },
            { name: "feral_demon", taken: false },
            { name: "grim_caco",   taken: true  },
            { name: "swift_vile",  taken: false } ] })
    }
}
