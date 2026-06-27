import QtQuick

// Clean rejoin prompt -- a dark pill with an amber ENTER key, no glow. Pixel font.
Item {
    id: banner
    property var theme
    implicitWidth: pill.width
    implicitHeight: pill.height

    Rectangle {
        id: pill
        anchors.centerIn: parent
        width: row.implicitWidth + 48; height: row.implicitHeight + 22
        radius: 12; color: theme.panel
        border.width: 1; border.color: theme.border

        Row {
            id: row
            anchors.centerIn: parent
            spacing: 12
            Text { anchors.verticalCenter: parent.verticalCenter; text: "SEAT 0 LEFT"; color: theme.seatColors[0]; font { family: theme.fontUi; pixelSize: 26 } }
            Text { anchors.verticalCenter: parent.verticalCenter; text: "press"; color: theme.text; font { family: theme.fontUi; pixelSize: 24 } }
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: key.implicitWidth + 18; height: key.implicitHeight + 6; radius: 5; color: "#ffffff"
                Text { id: key; anchors.centerIn: parent; text: "ENTER"; color: "#16181d"; font { family: theme.fontUi; pixelSize: 20 } } }
            Text { anchors.verticalCenter: parent.verticalCenter; text: "to rejoin"; color: theme.text; font { family: theme.fontUi; pixelSize: 24 } }
        }
    }
}
