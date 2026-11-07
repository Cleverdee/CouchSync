import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// "Watch together": create a room or join one by code / invite link.
Popup {
    id: dlg

    property var client            // RoomClient
    property bool joining: false   // false = create, true = join
    property string errorText: ""

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: 400
    padding: 16
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function go() {
        const name = nameField.text.trim()
        if (name === "") {
            errorText = "Enter your name"
            nameField.forceActiveFocus()
            return
        }
        if (joining && codeField.text.trim() === "") {
            errorText = "Enter the room code"
            codeField.forceActiveFocus()
            return
        }
        errorText = ""
        client.savedName = name
        client.savedServer = serverField.text.trim()
        if (joining)
            client.joinRoom(serverField.text, name, codeField.text, passField.text)
        else
            client.createRoom(serverField.text, name, passField.text)
    }

    onAboutToShow: {
        errorText = ""
        nameField.text = client.savedName
        serverField.text = client.savedServer
        passField.text = ""
    }
    onOpened: (nameField.text === "" ? nameField : (joining ? codeField : nameField)).forceActiveFocus()

    background: Rectangle {
        color: Theme.bg
        radius: Theme.radius
        border.color: Theme.hover
    }

    contentItem: ColumnLayout {
        spacing: 10

        Text {
            text: "Watch together"
            color: Theme.text
            font.pixelSize: 16
            font.bold: true
        }

        Text { text: "Your name"; color: Theme.textDim; font.pixelSize: 12 }
        FlatField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: "Name"
            maximumLength: 32
            onAccepted: dlg.go()
        }

        Text { text: "Server"; color: Theme.textDim; font.pixelSize: 12 }
        FlatField {
            id: serverField
            Layout.fillWidth: true
            placeholderText: "ws://localhost:8787/ws"
            onAccepted: dlg.go()
        }

        RowLayout {
            spacing: 4
            FlatButton {
                text: "Create room"
                active: !dlg.joining
                onClicked: dlg.joining = false
            }
            FlatButton {
                text: "Join room"
                active: dlg.joining
                onClicked: { dlg.joining = true; codeField.forceActiveFocus() }
            }
        }

        FlatField {
            id: codeField
            visible: dlg.joining
            Layout.fillWidth: true
            placeholderText: "Room code or couchsync:// invite link"
            onAccepted: dlg.go()
            onTextChanged: {
                const r = dlg.client.parseInvite(text)
                if (r.code) {
                    serverField.text = r.server
                    text = r.code
                }
            }
        }

        FlatField {
            id: passField
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: dlg.joining ? "Password (if the room has one)" : "Password (optional)"
            onAccepted: dlg.go()
        }

        Text {
            visible: dlg.errorText !== ""
            Layout.fillWidth: true
            text: dlg.errorText
            color: "#ff8a8a"
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            FlatButton { text: "Cancel"; onClicked: dlg.close() }
            FlatButton {
                text: dlg.client.connecting ? "Connecting…" : (dlg.joining ? "Join" : "Create")
                enabled: !dlg.client.connecting
                active: true
                onClicked: dlg.go()
            }
        }
    }
}
