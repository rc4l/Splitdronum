import QtQuick

// Per-seat "controller disconnected" card. The embedder sizes/positions it to span the seat's pane (16px
// padding). Clean Console panel, seat-color text, "Backspace" as a white keycap (matching the rejoin
// banner's ENTER key). seat = the pane index (0-3) -> drives the accent color + the "Player N" label.
Item {
    id: card
    property var theme
    property int seat: 0
    readonly property color accent: theme.seatColors[(seat >= 0 && seat < 4) ? seat : 1]

    Rectangle {
        anchors.fill: parent
        radius: 10
        color: theme.panel
        border.width: 1; border.color: theme.border

        Row {
            anchors.centerIn: parent
            spacing: 7
            Text { anchors.verticalCenter: parent.verticalCenter; color: accent; font { family: theme.fontUi; pixelSize: 16 }
                text: "Player " + (seat + 1) + "  -  controller disconnected   (hold" }
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: kc.implicitWidth + 16; height: kc.implicitHeight + 6; radius: 5; color: "#ffffff"
                Text { id: kc; anchors.centerIn: parent; text: "Backspace"; color: "#16181d"; font { family: theme.fontUi; pixelSize: 15 } } }
            Text { anchors.verticalCenter: parent.verticalCenter; text: "to drop)"; color: accent; font { family: theme.fontUi; pixelSize: 16 } }
        }
    }
}
