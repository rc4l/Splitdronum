import QtQuick

// Root of the splitdronum overlay. The host sets `join` / `seat0Gone` / `screenW` / `screenH`; everything
// else is reactive. Transparent background -- the host composites this over the seats. Components live in
// the same directory, so they resolve without an explicit import.
Item {
    id: root
    // size is set by the embedder (Preview.qml anchors.fill; the host/rctest set width/height explicitly),
    // so children can lay out against root's size in both the preview and offscreen render-control paths.

    property var  join: null          // {controller, seat, field, word1, word2, crosshair, motion, taken, pane:{x,y,w,h}}
    property bool seat0Gone: false
    property var  disconnects: []     // [{seat, pane:{x,y,w,h}}, ...] -- controller-unplugged seats this frame
    property int  screenW: 1920       // backbuffer size, so pane px map into this item
    property int  screenH: 1080

    Theme { id: appTheme }

    Repeater {                        // one card per disconnected seat, spanning its pane (16px padding)
        model: root.disconnects
        DisconnectCard {
            theme: appTheme
            seat:   modelData.seat
            x:      (modelData.pane.x + 16) / root.screenW * root.width
            width:  (modelData.pane.w - 32) / root.screenW * root.width
            height: 52 / root.screenH * root.height
            y:      (modelData.pane.y + (modelData.pane.h - 52) / 2) / root.screenH * root.height
        }
    }

    PlayerSetup {                     // single panel, centered in its reserved pane
        theme: appTheme
        join: root.join
        visible: !!root.join
        x:      root.join ? root.join.pane.x / root.screenW * root.width  : 0
        y:      root.join ? root.join.pane.y / root.screenH * root.height : 0
        width:  root.join ? root.join.pane.w / root.screenW * root.width  : 0
        height: root.join ? root.join.pane.h / root.screenH * root.height : 0
    }

    RejoinBanner {                    // top-center
        theme: appTheme
        visible: root.seat0Gone
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.04
    }
}
