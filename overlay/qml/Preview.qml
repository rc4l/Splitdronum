import QtQuick

// Standalone preview over a stand-in backdrop, with mock state -- design + screenshot without the host.
Rectangle {
    width: 1280; height: 720
    color: "#101a24"

    Overlay {
        anchors.fill: parent
        screenW: 1280; screenH: 720
        seat0Gone: true
        join: ({
            controller: 1, field: 2, word1: "happy", word2: "imp",
            crosshair: 3, motion: true, taken: false,
            pane: { x: 0, y: 0, w: 1280, h: 720 }
        })
    }
}
