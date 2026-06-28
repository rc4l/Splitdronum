import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// splitdronum launcher window. Form state mirrors play.ps1; Play hands it to the C++ Backend.
// Dark "Clean Console" palette (charcoal + warm amber) to match the in-game Qt overlay.
ApplicationWindow {
    id: win
    visible: true
    width: 720; height: 480
    minimumWidth: 560; minimumHeight: 450
    title: "splitdronum"
    color: theme.bg

    // Hide the launcher while the game runs (backend.busy), reappear when it closes. The backend polls
    // for host.exe and flips busy; we just follow it (and pop back to the front on return).
    Connections {
        target: backend
        function onBusyChanged() {
            if (backend.busy) win.hide()
            else { win.show(); win.raise(); win.requestActivate() }
        }
    }

    // --- shared palette (mirrors overlay/qml/Theme.qml) ---
    QtObject {
        id: theme
        readonly property color bg:       "#16181d"
        readonly property color panel:    "#1e2128"
        readonly property color border:   "#2c3038"
        readonly property color text:     "#e8eaf0"
        readonly property color textDim:  "#8a90a0"
        readonly property color accent:   "#ffb648"
        readonly property color accentDk: "#16181d"
    }

    // Fusion honors the window palette -- set a dark scheme with an amber highlight.
    palette {
        window:          theme.bg
        windowText:      theme.text
        base:            theme.panel
        alternateBase:   theme.border
        text:            theme.text
        button:          theme.panel
        buttonText:      theme.text
        highlight:       theme.accent
        highlightedText: theme.accentDk
        placeholderText: theme.textDim
        mid:             theme.border
    }

    // --- form state (defaults mirror play.ps1) ---
    property string iwad: "freedoom2.wad"
    property string gamePath: ""
    property int    fpsMode: 0          // 0 = monitor refresh, 1 = uncapped, 2 = custom
    property int    fpsCustom: 240
    property bool   autoStartKbm: true  // checked: start as keyboard+mouse. off: wait for any input to claim P1.
    property var    kbmProfiles: []     // saved configs the KB+M player can load
    property string kbmProfile: ""      // the chosen one (auto-picked when 0/1 exist; dropdown when many)

    // Refresh the KB+M profile list + pick a default: none -> fresh gen (""), one -> use it, many -> last chosen.
    function refreshKbm() {
        kbmProfiles = backend.kbmProfiles(gamePath)
        if (kbmProfiles.length === 0)      kbmProfile = ""
        else if (kbmProfiles.length === 1) kbmProfile = kbmProfiles[0]
        else {
            var last = backend.lastKbmProfile()
            kbmProfile = (kbmProfiles.indexOf(last) >= 0) ? last : kbmProfiles[0]
        }
    }
    Component.onCompleted: refreshKbm()
    onGamePathChanged: refreshKbm()

    FileDialog {
        id: gameDialog
        title: "Select zandronum.exe"
        nameFilters: ["Executable (*.exe)", "All files (*)"]
        onAccepted: win.gamePath = selectedFile.toString().replace("file:///", "")
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 22
        spacing: 16

        // --- header: logo + title ---
        RowLayout {
            Layout.fillWidth: true
            spacing: 14
            Image {
                source: "qrc:/logo.png"
                sourceSize.width: 56; sourceSize.height: 56
                Layout.preferredWidth: 56; Layout.preferredHeight: 56
                fillMode: Image.PreserveAspectFit
            }
            ColumnLayout {
                spacing: 2
                Label {
                    text: "splitdronum"
                    color: theme.text
                    font.pixelSize: 26; font.bold: true
                }
                Label {
                    text: "Local-splitscreen co-op for Zandronum  -  one window, N stock clients"
                    color: theme.textDim
                    font.pixelSize: 13
                }
            }
            Item { Layout.fillWidth: true }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: theme.border }

        // --- form ---
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 16
            rowSpacing: 14

            Label { text: "IWAD"; color: theme.text; Layout.alignment: Qt.AlignVCenter }
            TextField {
                Layout.fillWidth: true
                text: win.iwad
                placeholderText: "freedoom2.wad"
                onTextEdited: win.iwad = text
            }

            Label { text: "Game path"; color: theme.text; Layout.alignment: Qt.AlignVCenter }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    Layout.fillWidth: true
                    text: win.gamePath
                    placeholderText: "blank = bundled engine build"
                    onTextEdited: win.gamePath = text
                }
                Button { text: "Browse..."; onClicked: gameDialog.open() }
            }

            Label { text: "FPS cap"; color: theme.text; Layout.alignment: Qt.AlignVCenter }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                ComboBox {
                    id: fpsCombo
                    Layout.preferredWidth: 180
                    model: ["Monitor refresh", "Uncapped", "Custom"]
                    currentIndex: win.fpsMode
                    onActivated: win.fpsMode = currentIndex
                }
                SpinBox {
                    visible: win.fpsMode === 2
                    from: 1; to: 1000; stepSize: 10
                    value: win.fpsCustom
                    onValueModified: win.fpsCustom = value
                }
                Item { Layout.fillWidth: true }
            }

            Label { text: "Auto-start as KB + M"; color: theme.text; Layout.alignment: Qt.AlignVCenter }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Switch {
                    checked: win.autoStartKbm
                    onToggled: win.autoStartKbm = checked
                }
                Label {
                    text: win.autoStartKbm ? "starts immediately with keyboard + mouse as Player 1"
                                           : "waits on a start screen -- first input device becomes Player 1"
                    color: theme.textDim; font.pixelSize: 11
                    Layout.fillWidth: true; wrapMode: Text.WordWrap
                }
            }

            // KB+M profile: only shown when auto-start is on AND there are multiple saved configs to choose from.
            // (none -> fresh gen, one -> used silently -- both need no UI.) The choice is remembered for next time.
            Label {
                text: "KB + M profile"; color: theme.text; Layout.alignment: Qt.AlignVCenter
                visible: win.autoStartKbm && win.kbmProfiles.length > 1
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                visible: win.autoStartKbm && win.kbmProfiles.length > 1
                ComboBox {
                    Layout.preferredWidth: 220
                    model: win.kbmProfiles
                    currentIndex: Math.max(0, win.kbmProfiles.indexOf(win.kbmProfile))
                    onActivated: win.kbmProfile = win.kbmProfiles[currentIndex]
                }
                Item { Layout.fillWidth: true }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: theme.border }

        // --- play + hints ---
        RowLayout {
            Layout.fillWidth: true
            spacing: 16
            Button {
                id: playBtn
                text: "Play"
                Layout.preferredWidth: 140; Layout.preferredHeight: 46
                font.pixelSize: 18; font.bold: true
                background: Rectangle {
                    radius: 8
                    color: playBtn.down ? Qt.darker(theme.accent, 1.2)
                                        : (playBtn.hovered ? Qt.lighter(theme.accent, 1.08) : theme.accent)
                }
                contentItem: Text {
                    text: playBtn.text; color: theme.accentDk
                    font: playBtn.font
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                }
                onClicked: backend.launch(win.iwad, win.gamePath, win.fpsMode, win.fpsCustom, win.autoStartKbm, win.kbmProfile)
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                Label {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap
                    text: "Player 1 is keyboard + mouse. Press Start on a controller in-game to add a player."
                    color: theme.textDim; font.pixelSize: 12
                }
                Label {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap
                    text: "First launch builds the DLL + host (once). Close the game window to stop everything."
                    color: theme.textDim; font.pixelSize: 12
                }
            }
        }

        Item { Layout.fillHeight: true }

        // --- status line from the backend ---
        Label {
            Layout.fillWidth: true
            visible: backend.status.length > 0
            text: backend.status
            color: theme.accent
            wrapMode: Text.WordWrap
            font.pixelSize: 13
        }
    }
}
