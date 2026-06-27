import QtQuick

// Preview scene: all four seats showing the controller-disconnected card over a 2x2 game backdrop.
// Render with:  overlay_preview SceneDisconnect.qml
Item {
    id: root
    width: 1280; height: 720

    Grid {
        anchors.fill: parent; columns: 2; rows: 2
        Repeater { model: 4
            Image { width: root.width / 2; height: root.height / 2; source: "preview_bg.png"; fillMode: Image.PreserveAspectCrop } }
    }

    Overlay {                                // the real overlay, driven with a 4-disconnect state
        anchors.fill: parent
        screenW: root.width; screenH: root.height
        disconnects: [
            { seat: 0, pane: { x: 0,   y: 0,   w: 640, h: 360 } },
            { seat: 1, pane: { x: 640, y: 0,   w: 640, h: 360 } },
            { seat: 2, pane: { x: 0,   y: 360, w: 640, h: 360 } },
            { seat: 3, pane: { x: 640, y: 360, w: 640, h: 360 } }
        ]
    }
}
