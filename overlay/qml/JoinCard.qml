import QtQuick
import QtQuick.Effects

// The create-and-join wizard card. `join` = {controller, step, word1, word2, crosshair, motion, taken}.
// step: 0 prompt, 1 word1, 2 word2, 3 crosshair, 4 motion-comp. Sizes to its content; centered in its pane.
Item {
    id: card
    property var theme
    property var join

    readonly property bool   taken: !!join && join.taken === true
    readonly property string glow:  taken ? theme.pink : theme.cyan

    implicitWidth: panel.width
    implicitHeight: panel.height

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: col.implicitWidth + 72
        height: col.implicitHeight + 50
        radius: theme.radius
        color: theme.panel
        border.width: 1.5
        border.color: card.glow

        layer.enabled: true
        layer.effect: MultiEffect {        // neon glow (a blurred, zero-offset drop shadow)
            shadowEnabled: true
            shadowColor: card.glow
            shadowBlur: 1.0
            shadowOpacity: 0.85
            blurMax: 64
        }

        Column {
            id: col
            anchors.centerIn: parent
            spacing: 13

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "CONTROLLER " + (join ? join.controller : 1)
                color: theme.textDim
                font { family: theme.fontUi; pixelSize: 15; bold: true; letterSpacing: 3 }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                textFormat: Text.RichText
                text: card.fieldHtml()
                color: theme.text
                font { family: theme.fontMono; pixelSize: 30; bold: true }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                textFormat: Text.RichText
                text: card.hintHtml()
                color: theme.textDim
                font { family: theme.fontUi; pixelSize: 15 }
            }
        }
    }

    function span(c, s) { return '<font color="' + c + '">' + s + '</font>' }
    function arr(s, c)   { return span(theme.textDim, '‹') + ' <b>' + span(c, s) + '</b> ' + span(theme.textDim, '›') }
    function nm()        { return span(theme.cyan, (join.word1 || '') + (join.word2 || '')) }

    function fieldHtml() {
        if (!join) return ''
        switch (join.step) {
        case 0:  return 'press <b>' + span(theme.text, 'A') + '</b> to make a player'
        case 1:  return arr(join.word1, theme.text) + span(theme.cyan, join.word2)
        case 2:  return span(theme.cyan, join.word1) + arr(join.word2, theme.text)
        case 3:  return nm() + '&nbsp;&nbsp;&nbsp;crosshair ' + arr(join.crosshair, theme.text)
        default: return nm() + '&nbsp;&nbsp;&nbsp;motion comp ' + arr(join.motion ? 'ON' : 'off', join.motion ? theme.green : theme.textDim)
        }
    }
    function hintHtml() {
        if (!join) return ''
        if (join.step === 0) return 'B cancel'
        if (join.step === 4) return taken ? span(theme.pink, 'name taken — scroll to another') + '&nbsp;&nbsp;&nbsp;B back'
                                          : 'A create + join&nbsp;&nbsp;&nbsp;B back'
        return 'dpad change&nbsp;&nbsp;&nbsp;A next&nbsp;&nbsp;&nbsp;B back'
    }
}
