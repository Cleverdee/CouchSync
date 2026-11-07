import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import CouchSync

ApplicationWindow {
    id: root

    property string startupFile: ""
    // Command-line room automation (see main.cpp).
    property string cliServer: ""
    property string cliCreateName: ""
    property string cliJoinCode: ""
    property string cliJoinName: ""
    property string cliPassword: ""
    property bool logSync: false

    property bool panelOpen: false
    property bool showSync: false

    readonly property bool fullscreen: visibility === Window.FullScreen
    readonly property var subtitleExts: ["srt", "ass", "ssa", "vtt", "sub"]

    // Controls/cursor auto-hide: shown on mouse movement, hidden after 2 s while playing.
    property bool controlsShown: true
    readonly property bool canHide: mpv.playing && !trackPopup.opened && !joinDialog.opened && !bar.hovered
                                    && !seekSlider.pressed && !volumeSlider.pressed
    readonly property bool barVisible: controlsShown || !canHide

    width: 1280
    height: 720
    minimumWidth: 640
    minimumHeight: 360
    visible: true
    color: "black"
    title: (mpv.mediaTitle !== "" ? mpv.mediaTitle + " - " : "")
           + (roomClient.connected ? "Room " + roomClient.roomCode + " - " : "") + "CouchSync"

    palette {
        window: Theme.bg
        windowText: Theme.text
        base: Theme.surface
        text: Theme.text
        button: Theme.surface
        buttonText: Theme.text
        highlight: Theme.accent
        highlightedText: "white"
        mid: Theme.hover
        dark: Theme.hover
        light: Theme.hover
        toolTipBase: Theme.surface
        toolTipText: Theme.text
    }

    function fmt(sec) {
        const s = Math.max(0, Math.floor(sec))
        const h = Math.floor(s / 3600)
        const m = Math.floor((s % 3600) / 60)
        const ss = s % 60
        const pad = n => (n < 10 ? "0" : "") + n
        return (h > 0 ? h + ":" + pad(m) : m) + ":" + pad(ss)
    }

    function toggleFullscreen() {
        visibility = fullscreen ? Window.Windowed : Window.FullScreen
        poke()
    }

    function poke() {
        controlsShown = true
        hideTimer.restart()
    }

    function isSubtitle(url) {
        const ext = url.toString().split(".").pop().toLowerCase()
        return subtitleExts.indexOf(ext) >= 0
    }

    function handleDrop(urls) {
        const videos = urls.filter(u => !isSubtitle(u))
        if (videos.length > 0) {
            mpv.loadFile(videos[0].toString())
            return
        }
        // Only subtitle files were dropped: attach them to the current file.
        for (const u of urls)
            mpv.addSubtitle(u.toString())
    }

    onCanHideChanged: if (canHide) poke()
    onClosing: {
        mpv.saveWatchLater()
        roomClient.leave()
    }

    function showToast(text, isError) {
        toastHost.show(text, isError)
    }

    Component.onCompleted: {
        const server = cliServer !== "" ? cliServer : roomClient.savedServer
        if (cliCreateName !== "")
            roomClient.createRoom(server, cliCreateName, cliPassword)
        else if (cliJoinCode !== "")
            roomClient.joinRoom(server, cliJoinName !== "" ? cliJoinName : "Guest", cliJoinCode, cliPassword)
    }

    RoomClient { id: roomClient }

    SyncController {
        id: syncCtl
        player: mpv
        room: roomClient
        logSync: root.logSync
        onToast: text => root.showToast(text, false)
    }

    Connections {
        target: roomClient
        function onJoined() {
            joinDialog.close()
            root.panelOpen = true
            roomPanel.clearChat()
        }
        function onLeft() {
            root.panelOpen = false
            roomPanel.clearChat()
        }
        function onErrorOccurred(code, message) {
            joinDialog.errorText = message
            root.showToast(message, true)
        }
        function onChatReceived(name, text, mine) {
            roomPanel.addChat(name, text, mine)
        }
    }

    Timer {
        id: hideTimer
        interval: 2000
        running: true
        onTriggered: root.controlsShown = false
    }

    FileDialog {
        id: openDialog
        title: "Open media file"
        nameFilters: ["Media files (*.mkv *.mp4 *.m4v *.avi *.webm *.mov *.wmv *.flv *.ts *.m2ts *.mpg *.mpeg *.ogv *.mp3 *.flac *.m4a *.ogg *.opus *.wav)",
                      "All files (*)"]
        onAccepted: mpv.loadFile(selectedFile.toString())
    }

    FileDialog {
        id: subDialog
        title: "Load subtitle file"
        nameFilters: ["Subtitles (*.srt *.ass *.ssa *.vtt *.sub)", "All files (*)"]
        onAccepted: mpv.addSubtitle(selectedFile.toString())
    }

    // Key handling: everything goes to mpv's own bindings except fullscreen/open.
    Item {
        id: keyRoot
        anchors.fill: parent
        focus: true

        Keys.onPressed: event => {
            // Only forward keys when the video area itself has the focus; the chat input
            // and dialogs keep their own keys.
            if (!keyRoot.activeFocus)
                return
            if (trackPopup.opened || openDialog.visible || subDialog.visible || joinDialog.opened)
                return
            const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
            const alt = (event.modifiers & Qt.AltModifier) !== 0
            root.poke()
            if (event.key === Qt.Key_D && ctrl) {
                event.accepted = true // sync readout toggle (Shortcut below); never forwarded to mpv
            } else if (event.key === Qt.Key_O && ctrl) {
                openDialog.open()
                event.accepted = true
            } else if (event.key === Qt.Key_F && !ctrl && !alt) {
                root.toggleFullscreen()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape && root.fullscreen) {
                root.toggleFullscreen()
                event.accepted = true
            } else if (mpv.sendKey(event.key, event.modifiers, event.text)) {
                event.accepted = true
            }
        }

        Shortcut {
            sequence: "Ctrl+D"
            onActivated: root.showSync = !root.showSync
        }

        Item {
            id: videoArea
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.rightMargin: roomPanel.width // the panel is a sibling of keyRoot, so it can't be an anchor target

        MpvItem {
            id: mpv
            anchors.fill: parent

            onShutdown: root.close()

            Component.onCompleted: {
                if (root.startupFile !== "")
                    loadFile(root.startupFile)
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: root.barVisible ? Qt.ArrowCursor : Qt.BlankCursor
            onPressed: keyRoot.forceActiveFocus()
            onPositionChanged: root.poke()
            onClicked: clickTimer.restart()
            onDoubleClicked: {
                clickTimer.stop()
                root.toggleFullscreen()
            }

            WheelHandler {
                onWheel: event => {
                    mpv.sendKeyName(event.angleDelta.y > 0 ? "WHEEL_UP" : "WHEEL_DOWN")
                    root.poke()
                }
            }
        }

        // Single click pauses; delayed so a double click (fullscreen) doesn't also toggle pause.
        Timer {
            id: clickTimer
            interval: 250
            onTriggered: mpv.togglePause()
        }

        DropArea {
            anchors.fill: parent
            onDropped: drop => root.handleDrop(drop.urls)
        }

        Text {
            anchors.centerIn: parent
            visible: !mpv.hasMedia
            color: Theme.textDim
            font.pixelSize: 18
            text: "Drop a video here, or press Ctrl+O"
        }

        // ---- controls bar -----------------------------------------------------
        Rectangle {
            id: bar

            readonly property bool hovered: barHover.hovered

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 72
            color: Theme.panel
            opacity: root.barVisible ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 150 } }

            HoverHandler { id: barHover }

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                anchors.topMargin: 6
                anchors.bottomMargin: 6
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: root.fmt(mpv.position)
                        color: Theme.text
                        font.pixelSize: 12
                        Layout.preferredWidth: 52
                        horizontalAlignment: Text.AlignRight
                    }

                    FlatSlider {
                        id: seekSlider
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, mpv.duration)
                        enabled: mpv.hasMedia && mpv.duration > 0
                        onMoved: mpv.seek(value)

                        Binding {
                            target: seekSlider
                            property: "value"
                            value: mpv.position
                            when: !seekSlider.pressed
                        }
                    }

                    Text {
                        text: root.fmt(mpv.duration)
                        color: Theme.text
                        font.pixelSize: 12
                        Layout.preferredWidth: 52
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    FlatButton { text: "Open"; onClicked: openDialog.open() }
                    FlatButton {
                        text: mpv.paused || !mpv.hasMedia ? "▶" : "❚❚"
                        onClicked: mpv.togglePause()
                    }
                    FlatButton {
                        visible: !roomClient.connected
                        text: "Watch together"
                        onClicked: joinDialog.open()
                    }
                    FlatButton {
                        visible: roomClient.connected
                        text: "Room"
                        active: root.panelOpen
                        onClicked: root.panelOpen = !root.panelOpen
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        visible: mpv.subDelay !== 0
                        text: "Sub delay " + (mpv.subDelay > 0 ? "+" : "") + mpv.subDelay.toFixed(1) + " s"
                        color: Theme.textDim
                        font.pixelSize: 12
                        Layout.rightMargin: 6
                    }

                    FlatButton {
                        text: "Audio"
                        active: trackPopup.opened && trackPopup.kind === "audio"
                        onClicked: trackPopup.openFor("audio")
                    }
                    FlatButton {
                        text: "Subs"
                        active: trackPopup.opened && trackPopup.kind === "sub"
                        onClicked: trackPopup.openFor("sub")
                    }

                    FlatButton {
                        text: mpv.muted ? "Muted" : "Vol"
                        active: mpv.muted
                        onClicked: mpv.toggleMute()
                    }
                    FlatSlider {
                        id: volumeSlider
                        Layout.preferredWidth: 96
                        from: 0
                        to: 130
                        onMoved: mpv.setVolume(value)

                        Binding {
                            target: volumeSlider
                            property: "value"
                            value: mpv.volume
                            when: !volumeSlider.pressed
                        }
                    }

                    FlatButton {
                        text: root.fullscreen ? "Exit full" : "Full"
                        onClicked: root.toggleFullscreen()
                    }
                }
            }
        }

        // ---- overlays ---------------------------------------------------------
        ToastHost {
            id: toastHost
            anchors.top: parent.top
            anchors.topMargin: 16
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Text {
            anchors.centerIn: parent
            visible: syncCtl.countdown > 0 && roomClient.connected
            text: syncCtl.countdown
            color: "white"
            font.pixelSize: 140
            font.bold: true
            style: Text.Outline
            styleColor: "#aa000000"
        }

        Column {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 12
            spacing: 6

            Rectangle {
                visible: syncCtl.mediaWarning !== ""
                width: warnText.implicitWidth + 24
                height: 30
                radius: Theme.radius
                color: "#e6a67a1e"
                Text {
                    id: warnText
                    anchors.centerIn: parent
                    text: "⚠ " + syncCtl.mediaWarning
                    color: "white"
                    font.pixelSize: Theme.fontSize
                }
            }

            Rectangle {
                visible: root.showSync
                width: syncText.implicitWidth + 20
                height: 26
                radius: Theme.radius
                color: Theme.panel
                Text {
                    id: syncText
                    anchors.centerIn: parent
                    color: Theme.textDim
                    font.pixelSize: 12
                    font.family: "Consolas"
                    text: roomClient.connected
                          ? "drift " + (syncCtl.driftMs > 0 ? "+" : "") + syncCtl.driftMs.toFixed(0) + " ms"
                            + "  RTT " + roomClient.rttMs.toFixed(0) + " ms"
                            + "  offset " + roomClient.offsetMs.toFixed(0) + " ms"
                            + (syncCtl.ready ? "" : "  (waiting)")
                          : "not in a room"
                }
            }
        }
        } // videoArea
    }

    RoomPanel {
        id: roomPanel
        room: roomClient
        sync: syncCtl
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.panelOpen && roomClient.connected ? 300 : 0
        visible: width > 0
        onLeaveRequested: roomClient.leave()
        onCollapseRequested: { root.panelOpen = false; keyRoot.forceActiveFocus() }
        onReleaseFocus: keyRoot.forceActiveFocus()
        onNotify: text => root.showToast(text, false)
    }

    JoinDialog {
        id: joinDialog
        client: roomClient
        onClosed: keyRoot.forceActiveFocus()
    }

    TrackPopup {
        id: trackPopup
        player: mpv
        onLoadSubtitleRequested: subDialog.open()
        onClosed: keyRoot.forceActiveFocus()
    }

    // Never leave the window without a key target (e.g. after a dialog closes).
    onActiveFocusItemChanged: if (!activeFocusItem) keyRoot.forceActiveFocus()
}
