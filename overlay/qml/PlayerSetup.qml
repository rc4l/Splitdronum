import QtQuick

// "Clean Console" player-setup panel: every option on ONE panel, the focused field highlighted. Pixel font
// (VT323). join = {controller, field, word1, word2, crosshair, motion, taken, pane}. field: 0 word1,
// 1 word2, 2 crosshair, 3 motion. Sizes to content; centered in its pane.
Item {
    id: card
    property var theme
    property var join

    readonly property int  f:     join ? join.field : 0
    readonly property bool taken: !!join && join.taken === true

    readonly property int   seat:       (join && join.seat !== undefined) ? join.seat : 1
    readonly property color accent:     theme.seatColors[(seat >= 0 && seat < 4) ? seat : 1]   // per-seat tint
    readonly property color accentSoft: Qt.rgba(accent.r, accent.g, accent.b, 0.14)             // focused-field wash

    implicitWidth: panel.width
    implicitHeight: panel.height

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: 600
        height: col.implicitHeight + 52
        radius: theme.radius
        color: theme.panel
        border.width: 1; border.color: theme.border

        Column {
            id: col
            width: parent.width - 52
            anchors.centerIn: parent
            spacing: 16

            Row {                                  // header
                spacing: 12
                Rectangle { width: 34; height: 34; radius: 9; color: theme.chip; border.color: accent; border.width: 1.5
                    Text { anchors.centerIn: parent; text: "P" + (join ? join.controller : 1); color: accent; font { family: theme.fontUi; pixelSize: 22 } } }
                Column { anchors.verticalCenter: parent.verticalCenter
                    Text { text: "PLAYER SETUP"; color: theme.text; font { family: theme.fontUi; pixelSize: 28 } }
                    Text { text: "Controller " + (join ? join.controller : 1); color: theme.textDim; font { family: theme.fontUi; pixelSize: 19 } } }
            }

            Column {                               // NAME -- two chips, the focused word shows < >
                spacing: 8
                Text { text: "NAME"; color: theme.textDim; font { family: theme.fontUi; pixelSize: 19; letterSpacing: 2 } }
                Row {
                    spacing: 10
                    Rectangle {
                        width: r1.implicitWidth + 26; height: 46; radius: 9
                        color: (f === 0) ? accentSoft : theme.chip
                        border.width: 1.5; border.color: (f === 0) ? accent : theme.chip
                        Row { id: r1; anchors.centerIn: parent; spacing: 7
                            Text { visible: f === 0; text: "<"; color: accent; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 26 } }
                            Text { text: join ? join.word1 : ""; color: taken ? theme.bad : theme.text; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 32 } }
                            Text { visible: f === 0; text: ">"; color: accent; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 26 } } }
                    }
                    Rectangle {
                        width: r2.implicitWidth + 26; height: 46; radius: 9
                        color: (f === 1) ? accentSoft : theme.chip
                        border.width: 1.5; border.color: (f === 1) ? accent : theme.chip
                        Row { id: r2; anchors.centerIn: parent; spacing: 7
                            Text { visible: f === 1; text: "<"; color: accent; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 26 } }
                            Text { text: join ? join.word2 : ""; color: taken ? theme.bad : theme.text; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 32 } }
                            Text { visible: f === 1; text: ">"; color: accent; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 26 } } }
                    }
                    Text { visible: taken; anchors.verticalCenter: parent.verticalCenter; text: "name taken"; color: theme.bad; font { family: theme.fontUi; pixelSize: 21 } }
                }
            }

            Rectangle {                            // CROSSHAIR row
                width: parent.width; height: 50; radius: 10; color: (f === 2) ? theme.row : "transparent"
                Rectangle { visible: f === 2; width: 3; height: 26; radius: 2; color: accent; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter }
                Text { anchors.left: parent.left; anchors.leftMargin: 16; anchors.verticalCenter: parent.verticalCenter; text: "CROSSHAIR"; color: theme.text; font { family: theme.fontUi; pixelSize: 25 } }
                Row { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; spacing: 14
                    Item { width: 22; height: 22; anchors.verticalCenter: parent.verticalCenter; visible: join && join.crosshair > 0   // live preview
                        Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: 0;  width: 2; height: 8; color: accent }
                        Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: 14; width: 2; height: 8; color: accent }
                        Rectangle { anchors.verticalCenter: parent.verticalCenter; x: 0;  width: 8; height: 2; color: accent }
                        Rectangle { anchors.verticalCenter: parent.verticalCenter; x: 14; width: 8; height: 2; color: accent } }
                    Row { spacing: 9; anchors.verticalCenter: parent.verticalCenter
                        Text { visible: f === 2; text: "<"; color: accent; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 24 } }
                        Text { text: join ? (join.crosshair === 0 ? "off" : join.crosshair) : ""; color: theme.text; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 27 } }
                        Text { visible: f === 2; text: ">"; color: accent; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 24 } } }
                }
            }

            Rectangle {                            // MOTION COMFORT row
                width: parent.width; height: 50; radius: 10; color: (f === 3) ? theme.row : "transparent"
                Rectangle { visible: f === 3; width: 3; height: 26; radius: 2; color: accent; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter }
                Text { anchors.left: parent.left; anchors.leftMargin: 16; anchors.verticalCenter: parent.verticalCenter; text: "MOTION COMFORT"; color: theme.text; font { family: theme.fontUi; pixelSize: 25 } }
                Row { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; spacing: 12
                    Text { visible: f === 3; text: "<"; color: accent; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 24 } }
                    Rectangle { width: 52; height: 28; radius: 14; anchors.verticalCenter: parent.verticalCenter
                        color: (join && join.motion) ? accent : "#3a3f4a"
                        Rectangle { width: 22; height: 22; radius: 11; color: "#16181d"; anchors.verticalCenter: parent.verticalCenter
                            x: (join && join.motion) ? 27 : 3; Behavior on x { NumberAnimation { duration: 90 } } } }
                    Text { visible: f === 3; text: ">"; color: accent; anchors.verticalCenter: parent.verticalCenter; font { family: theme.fontUi; pixelSize: 24 } }
                }
            }

            Text {                                 // footer hints -- auto-shrink to never overrun the panel
                width: parent.width; horizontalAlignment: Text.AlignHCenter
                fontSizeMode: Text.HorizontalFit; minimumPixelSize: 13
                text: taken ? "scroll the name to a free one        A  create + join        B  back"
                            : "up / down  select        left / right  change        A  create + join        B  back"
                color: theme.textDim; font { family: theme.fontUi; pixelSize: 20 }
            }
        }
    }
}
