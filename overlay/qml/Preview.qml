import QtQuick

// Standalone preview of the overlay over a stand-in "game" backdrop, with mock state -- so the UI can be
// designed + screenshotted without the host. (The real host sets Overlay.join / seat0Gone instead.)
Rectangle {
    width: 1280; height: 720
    color: "#101a24"   // stand-in game background (the real overlay is transparent over the seats)

    Overlay {
        anchors.fill: parent
        screenW: 1280; screenH: 720
        seat0Gone: true
        join: ({
            controller: 1, step: 3, word1: "sneaky", word2: "revvy",
            crosshair: 3, motion: true, taken: false,
            pane: { x: 640, y: 0, w: 640, h: 720 }
        })
    }
}
