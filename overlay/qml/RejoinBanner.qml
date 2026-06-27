import QtQuick
import QtQuick.Effects

// "SEAT 0 LEFT — press [ENTER] to rejoin", the keyboard-player rejoin prompt. Pink neon glow + a keycap.
Item {
    id: banner
    property var theme
    implicitWidth: pill.width
    implicitHeight: pill.height

    Rectangle {
        id: pill
        anchors.centerIn: parent
        width: row.implicitWidth + 52
        height: row.implicitHeight + 26
        radius: 10
        color: theme.bannerBg
        border.width: 1.5
        border.color: theme.pink
        layer.enabled: true
        layer.effect: MultiEffect { shadowEnabled: true; shadowColor: theme.pink; shadowBlur: 1.0; shadowOpacity: 0.8; blurMax: 56 }

        Row {
            id: row
            anchors.centerIn: parent
            spacing: 10
            Text {
                anchors.verticalCenter: parent.verticalCenter; text: "SEAT 0 LEFT"
                color: theme.pink; font { family: theme.fontUi; pixelSize: 18; bold: true; letterSpacing: 2 }
            }
            Text { anchors.verticalCenter: parent.verticalCenter; text: "—"; color: theme.textDim; font.pixelSize: 18 }
            Text { anchors.verticalCenter: parent.verticalCenter; text: "press"; color: theme.text; font { family: theme.fontUi; pixelSize: 18 } }
            Rectangle {                                   // ENTER keycap
                anchors.verticalCenter: parent.verticalCenter
                width: key.implicitWidth + 18; height: key.implicitHeight + 8
                radius: 5; color: "transparent"; border.color: theme.cyan; border.width: 1.5
                Text { id: key; anchors.centerIn: parent; text: "ENTER"; color: theme.cyan; font { family: theme.fontMono; pixelSize: 15; bold: true } }
            }
            Text { anchors.verticalCenter: parent.verticalCenter; text: "to rejoin"; color: theme.text; font { family: theme.fontUi; pixelSize: 18 } }
        }
    }
}
