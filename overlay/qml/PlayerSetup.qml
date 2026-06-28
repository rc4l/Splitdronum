import QtQuick

// Player-setup panel -- a compact settings list (label left, value right; the focused row gets a seat-accent
// bar + soft fill). Left/Right TRAVERSE the rows (name word 1, word 2, crosshair, motion); Up/Down CHANGE the
// focused value (shown by the small ^ v indicator); HOLD Start to commit -- the ring fills so you can't join
// before finalizing. join = {controller, seat, field, word1, word2, crosshair, motion, taken, known, hold}.
// field: 0 word1, 1 word2, 2 crosshair, 3 motion (0 and 1 both sit on the Name row).
Item {
    id: card
    property var theme
    property var join
    property var browse: null   // { index, items:[{name,taken}] } while mode === JM_BROWSE

    readonly property int   f:          join ? join.field : 0
    readonly property bool  taken:       !!join && join.taken === true
    readonly property bool  known:       !!join && join.known === true
    readonly property int   seat:        (join && join.seat !== undefined) ? join.seat : 1
    readonly property real  hold:        (join && join.hold !== undefined) ? join.hold : 0
    readonly property int   mode:        (join && join.mode !== undefined) ? join.mode : 0   // 0 edit, 1 variant
    readonly property string variant:    (join && join.variant !== undefined) ? join.variant : ""
    readonly property color accent:      theme.seatColors[(seat >= 0 && seat < 4) ? seat : 1]
    readonly property color accentSoft:  Qt.rgba(accent.r, accent.g, accent.b, 0.13)

    onHoldChanged: ringCanvas.requestPaint()
    onAccentChanged: ringCanvas.requestPaint()

    implicitWidth: panel.width
    implicitHeight: panel.height

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: 430
        height: col.implicitHeight + 34
        radius: theme.radius
        color: theme.panel
        border.width: 1; border.color: theme.border

        Column {
            id: col
            width: parent.width - 32
            anchors.centerIn: parent
            spacing: 3

            Item {                                     // header: seat dot + name, controller on the right
                width: parent.width; height: 32
                Row { spacing: 9; anchors.verticalCenter: parent.verticalCenter
                    Rectangle { width: 10; height: 10; radius: 5; color: accent; anchors.verticalCenter: parent.verticalCenter }
                    Text { anchors.verticalCenter: parent.verticalCenter; text: "Player " + (seat + 1); color: theme.text; font { family: theme.fontUi; pixelSize: 17 } } }
                Row { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; spacing: 8
                    Rectangle { visible: known; anchors.verticalCenter: parent.verticalCenter; width: sv.implicitWidth + 12; height: 17; radius: 4; color: accentSoft; border.color: accent; border.width: 1
                        Text { id: sv; anchors.centerIn: parent; text: "saved"; color: accent; font { family: theme.fontUi; pixelSize: 11 } } }
                    Text { anchors.verticalCenter: parent.verticalCenter; text: "Controller " + (join ? join.controller : 1); color: theme.textDim; font { family: theme.fontUi; pixelSize: 13 } }
                }
            }
            Rectangle { width: parent.width; height: 1; color: theme.border }
            Item { width: 1; height: 3 }

            Rectangle {                                // Name -- two words; the focused one shows the ^ v changer
                width: parent.width; height: 40; radius: 8; color: (f <= 1) ? accentSoft : "transparent"
                Rectangle { visible: f <= 1; width: 3; height: 22; radius: 2; color: accent; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter }
                Text { anchors.left: parent.left; anchors.leftMargin: 16; anchors.verticalCenter: parent.verticalCenter; text: "Name"; color: theme.text; font { family: theme.fontUi; pixelSize: 15 } }
                Row { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; spacing: 6
                    Text { text: join ? join.word1 : ""; color: taken ? theme.bad : (f === 0 ? theme.text : theme.textDim); anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 17 } }
                    Column { visible: f === 0; anchors.verticalCenter: parent.verticalCenter; spacing: -4   // word 1 changer
                        Text { text: "^"; color: accent; font { family: theme.fontUi; pixelSize: 13 } }
                        Text { text: "v"; color: accent; font { family: theme.fontUi; pixelSize: 13 } } }
                    Text { text: join ? join.word2 : ""; color: taken ? theme.bad : (f === 1 ? theme.text : theme.textDim); anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 17 } }
                    Column { visible: f === 1; anchors.verticalCenter: parent.verticalCenter; spacing: -4   // word 2 changer
                        Text { text: "^"; color: accent; font { family: theme.fontUi; pixelSize: 13 } }
                        Text { text: "v"; color: accent; font { family: theme.fontUi; pixelSize: 13 } } }
                }
            }

            Rectangle {                                // Crosshair -- preview + value (^ v to change)
                width: parent.width; height: 40; radius: 8; color: (f === 2) ? accentSoft : "transparent"
                Rectangle { visible: f === 2; width: 3; height: 22; radius: 2; color: accent; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter }
                Text { anchors.left: parent.left; anchors.leftMargin: 16; anchors.verticalCenter: parent.verticalCenter; text: "Crosshair"; color: theme.text; font { family: theme.fontUi; pixelSize: 15 } }
                Row { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; spacing: 9
                    Item { width: 16; height: 16; anchors.verticalCenter: parent.verticalCenter; visible: join && join.crosshair > 0
                        Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: 0;  width: 2; height: 5; color: accent }
                        Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: 11; width: 2; height: 5; color: accent }
                        Rectangle { anchors.verticalCenter: parent.verticalCenter; x: 0;  width: 5; height: 2; color: accent }
                        Rectangle { anchors.verticalCenter: parent.verticalCenter; x: 11; width: 5; height: 2; color: accent } }
                    Row { spacing: 6; anchors.verticalCenter: parent.verticalCenter
                        Text { text: join ? (join.crosshair === 0 ? "off" : join.crosshair) : ""; color: theme.text; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 17 } }
                        Column { visible: f === 2; anchors.verticalCenter: parent.verticalCenter; spacing: -4
                            Text { text: "^"; color: accent; font { family: theme.fontUi; pixelSize: 13 } }
                            Text { text: "v"; color: accent; font { family: theme.fontUi; pixelSize: 13 } } } }
                }
            }

            Rectangle {                                // Motion comfort -- toggle (^ v to change)
                width: parent.width; height: 40; radius: 8; color: (f === 3) ? accentSoft : "transparent"
                Rectangle { visible: f === 3; width: 3; height: 22; radius: 2; color: accent; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter }
                Text { anchors.left: parent.left; anchors.leftMargin: 16; anchors.verticalCenter: parent.verticalCenter; text: "Motion comfort"; color: theme.text; font { family: theme.fontUi; pixelSize: 15 } }
                Row { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; spacing: 9
                    Rectangle { width: 40; height: 22; radius: 11; anchors.verticalCenter: parent.verticalCenter; color: (join && join.motion) ? accent : "#3a3f4a"
                        Rectangle { width: 16; height: 16; radius: 8; color: "#16181d"; anchors.verticalCenter: parent.verticalCenter
                            x: (join && join.motion) ? 21 : 3; Behavior on x { NumberAnimation { duration: 90 } } } }
                    Column { visible: f === 3; anchors.verticalCenter: parent.verticalCenter; spacing: -4
                        Text { text: "^"; color: accent; font { family: theme.fontUi; pixelSize: 13 } }
                        Text { text: "v"; color: accent; font { family: theme.fontUi; pixelSize: 13 } } }
                }
            }

            Item { width: 1; height: 4 }
            Rectangle { width: parent.width; height: 1; color: theme.border }
            Item { width: 1; height: 4 }

            // --- hold-to-join ring: fills as Start is held; a full ring commits the join ---
            Item {
                width: parent.width; height: 58
                Item {
                    id: ringBox; width: 50; height: 50; anchors.horizontalCenter: parent.horizontalCenter; anchors.top: parent.top
                    Canvas {
                        id: ringCanvas; anchors.fill: parent
                        onPaint: {
                            var ctx = getContext("2d"); ctx.reset();
                            var cx = width/2, cy = height/2, r = width/2 - 4;
                            ctx.lineWidth = 5; ctx.lineCap = "round";
                            ctx.strokeStyle = Qt.rgba(1,1,1,0.13);                  // track
                            ctx.beginPath(); ctx.arc(cx,cy,r,0,2*Math.PI); ctx.stroke();
                            if (card.hold > 0) {                                    // progress arc, from 12 o'clock CW
                                ctx.strokeStyle = card.accent;
                                ctx.beginPath(); ctx.arc(cx,cy,r,-Math.PI/2,-Math.PI/2 + 2*Math.PI*Math.min(1,card.hold)); ctx.stroke();
                            }
                        }
                    }
                    Text { anchors.centerIn: parent; text: card.hold >= 1 ? "GO" : "hold"
                        color: card.hold > 0 ? accent : theme.textDim; font { family: theme.fontUi; pixelSize: 12 } }
                }
            }

            Text {                                     // footer hints -- auto-shrink so they never overrun
                width: parent.width; horizontalAlignment: Text.AlignHCenter
                fontSizeMode: Text.HorizontalFit; minimumPixelSize: 10
                text: taken ? "name taken -- up/down to a free one      hold Start to join      Y load      B back"
                            : "left/right select   up/down change   hold Start join   Y load existing   B back"
                color: theme.textDim; font { family: theme.fontUi; pixelSize: 13 }
            }
        }

        // --- load-existing browser (overlays the panel while mode === JM_BROWSE) ---
        Rectangle {
            anchors.fill: parent; radius: theme.radius
            color: Qt.rgba(0.06, 0.07, 0.09, 0.95)
            border.width: 1; border.color: accent
            visible: card.mode === 2
            Column {
                anchors.fill: parent; anchors.margins: 16; spacing: 8
                Text { text: "Load existing config"; color: theme.text; font { family: theme.fontUi; pixelSize: 16 } }
                Rectangle { width: parent.width; height: 1; color: theme.border }
                ListView {
                    id: blist
                    width: parent.width; height: parent.height - 86; clip: true
                    model: card.browse ? card.browse.items : []
                    currentIndex: card.browse ? card.browse.index : 0
                    onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
                    delegate: Rectangle {
                        width: blist.width; height: 30; radius: 6
                        color: index === blist.currentIndex ? accentSoft : "transparent"
                        Rectangle { visible: index === blist.currentIndex; width: 3; height: 18; radius: 2; color: accent
                            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter }
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 14; anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name.replace(/_/g, " ") + (modelData.taken ? "   (in use)" : "")
                            color: modelData.taken ? theme.textDim
                                                   : (index === blist.currentIndex ? accent : theme.text)
                            font { family: theme.fontUi; pixelSize: 15 }
                        }
                    }
                }
                Text { width: parent.width; horizontalAlignment: Text.AlignHCenter
                    fontSizeMode: Text.HorizontalFit; minimumPixelSize: 10
                    text: "up / down  scroll      hold Start  load      B  back"
                    color: theme.textDim; font { family: theme.fontUi; pixelSize: 13 } }
            }
        }

        // --- "<name> is in use -- load as <variant>?" confirm (overlays the panel) ---
        Rectangle {
            anchors.fill: parent; radius: theme.radius
            color: Qt.rgba(0.06, 0.07, 0.09, 0.92)
            border.width: 1; border.color: accent
            visible: card.mode === 1
            Column {
                anchors.centerIn: parent; width: parent.width - 44; spacing: 14
                Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                    text: (join ? (join.word1 + " " + join.word2) : "") + "  is already in use"
                    color: theme.bad; font { family: theme.fontUi; pixelSize: 16 } }
                Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                    text: "Load as  \"" + card.variant.replace(/_/g, " ") + "\" ?"
                    color: theme.text; font { family: theme.fontUi; pixelSize: 20 } }
                Row { anchors.horizontalCenter: parent.horizontalCenter; spacing: 28
                    Text { text: "A  Yes"; color: accent;        font { family: theme.fontUi; pixelSize: 16 } }
                    Text { text: "B  No";  color: theme.textDim; font { family: theme.fontUi; pixelSize: 16 } } }
            }
        }
    }
}
