import QtQuick

// Root of the splitdronum overlay. The host sets `join` / `seat0Gone` / `screenW` / `screenH`; everything
// else is reactive. Transparent background -- the host composites this over the seats. Components live in
// the same directory, so they resolve without an explicit import.
Item {
    id: root
    // size is set by the embedder (Preview.qml anchors.fill; the host/rctest set width/height explicitly),
    // so children can lay out against root's size in both the preview and offscreen render-control paths.

    property var  join: null          // {controller, step, word1, word2, crosshair, motion, taken, pane:{x,y,w,h}}
    property bool seat0Gone: false
    property int  screenW: 1920       // backbuffer size, so pane px map into this item
    property int  screenH: 1080

    Theme { id: theme }

    JoinCard {                        // centered in its reserved pane
        theme: theme
        join: root.join
        visible: !!root.join
        x:      root.join ? root.join.pane.x / root.screenW * root.width  : 0
        y:      root.join ? root.join.pane.y / root.screenH * root.height : 0
        width:  root.join ? root.join.pane.w / root.screenW * root.width  : 0
        height: root.join ? root.join.pane.h / root.screenH * root.height : 0
    }

    RejoinBanner {                    // top-center
        theme: theme
        visible: root.seat0Gone
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.04
    }
}
