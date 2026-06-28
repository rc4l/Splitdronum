import QtQuick

// Root of the splitdronum overlay. The host sets `join` / `seat0Gone` / `screenW` / `screenH`; everything
// else is reactive. Transparent background -- the host composites this over the seats. Components live in
// the same directory, so they resolve without an explicit import.
Item {
    id: root
    // size is set by the embedder (Preview.qml anchors.fill; the host/rctest set width/height explicitly),
    // so children can lay out against root's size in both the preview and offscreen render-control paths.

    property var  join: null          // {controller, seat, field, word1, word2, crosshair, motion, taken, hold, pane}
    property bool seat0Gone: false
    property var  disconnects: []     // [{seat, pane:{x,y,w,h}}, ...] -- controller-unplugged seats this frame
    property int  screenW: 1920       // backbuffer size, so pane px map into this item
    property int  screenH: 1080
    property bool waitStart: false    // auto-start off + nothing spawned: show the "hold Enter / press Start" prompt
    property real kbHold: 0           // keyboard Enter hold-to-join ring progress (0..1)
    property var  browse: null        // saved-config browser: { index, items:[{name,taken}] } (null = closed)

    Theme { id: appTheme }

    onKbHoldChanged: kbRing.requestPaint()

    // Keyboard hold-to-join: a centered ring that fills while Enter is held (wait-screen start AND seat-0
    // rejoin), plus the wait-screen prompt. Mirrors the controller's hold-START ring so neither can join by
    // accident. Uses seat 0's accent (green) since this is the keyboard+mouse player.
    Column {
        anchors.centerIn: parent
        spacing: 18
        visible: (root.waitStart && !root.join) || root.kbHold > 0
        Item {
            width: 92; height: 92; anchors.horizontalCenter: parent.horizontalCenter
            Canvas {
                id: kbRing; anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d"); ctx.reset();
                    var cx = width/2, cy = height/2, r = width/2 - 7;
                    ctx.lineWidth = 8; ctx.lineCap = "round";
                    ctx.strokeStyle = Qt.rgba(1,1,1,0.13);
                    ctx.beginPath(); ctx.arc(cx,cy,r,0,2*Math.PI); ctx.stroke();
                    if (root.kbHold > 0) {
                        ctx.strokeStyle = appTheme.seatColors[0];
                        ctx.beginPath(); ctx.arc(cx,cy,r,-Math.PI/2,-Math.PI/2 + 2*Math.PI*Math.min(1,root.kbHold)); ctx.stroke();
                    }
                }
            }
            Text { anchors.centerIn: parent; text: root.kbHold >= 1 ? "GO" : "Enter"
                color: root.kbHold > 0 ? appTheme.seatColors[0] : appTheme.textDim
                font { family: appTheme.fontUi; pixelSize: 16 } }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.waitStart && !root.join
            text: "Hold Enter to start  -  or press Start on a controller to join"
            color: appTheme.text; font { family: appTheme.fontUi; pixelSize: 18 }
        }
    }

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
        browse: root.browse
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
