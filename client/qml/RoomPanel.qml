import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Collapsible right-hand room panel: code, members, host settings, chat, leave.
Rectangle {
    id: panel

    property var room   // RoomClient
    property var sync   // SyncController
    signal leaveRequested()
    signal collapseRequested()
    signal releaseFocus()
    signal notify(string text)

    function addChat(name, text, mine) {
        chatModel.append({ who: name, body: text, mine: mine })
        if (chatModel.count > 200)
            chatModel.remove(0)
        chatList.positionViewAtEnd()
    }
    function clearChat() { chatModel.clear() }
    function focusChat() { chatInput.forceActiveFocus() }

    color: Theme.bg

    Rectangle {
        width: 1
        height: parent.height
        color: Theme.hover
    }

    ListModel { id: chatModel }

    function fmtDrift(ms) {
        const r = Math.round(ms)
        return (r > 0 ? "+" : "") + r + " ms"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Text { text: "Room"; color: Theme.textDim; font.pixelSize: 12; Layout.fillWidth: true }
            FlatButton { text: "›"; onClicked: panel.collapseRequested() }
        }

        Text {
            text: panel.room.roomCode
            color: Theme.text
            font.pixelSize: 26
            font.family: "Consolas"
            font.letterSpacing: 3
        }

        RowLayout {
            spacing: 4
            FlatButton {
                text: "Copy code"
                onClicked: { panel.room.copyText(panel.room.roomCode); panel.notify("Room code copied") }
            }
            FlatButton {
                text: "Copy invite link"
                onClicked: { panel.room.copyText(panel.room.inviteLink); panel.notify("Invite link copied") }
            }
        }

        Text { text: "Members"; color: Theme.textDim; font.pixelSize: 12; topPadding: 4 }

        ListView {
            id: memberList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 196)
            clip: true
            interactive: contentHeight > height
            spacing: 2
            model: panel.room.members

            delegate: Item {
                id: member
                required property var modelData
                width: ListView.view.width
                height: 26

                RowLayout {
                    anchors.fill: parent
                    spacing: 6

                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: member.modelData.state === "ready" ? "#4cc38a" : "#e0b340"
                    }
                    Text {
                        text: member.modelData.name + (member.modelData.isMe ? " (you)" : "")
                        color: Theme.text
                        font.pixelSize: Theme.fontSize
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        visible: member.modelData.host
                        width: hostLabel.implicitWidth + 10
                        height: 16
                        radius: 3
                        color: Theme.accent
                        Text {
                            id: hostLabel
                            anchors.centerIn: parent
                            text: "HOST"
                            color: "white"
                            font.pixelSize: 9
                            font.bold: true
                        }
                    }
                    Text {
                        text: member.modelData.state === "ready" ? panel.fmtDrift(member.modelData.driftMs) : "waiting"
                        color: Theme.textDim
                        font.pixelSize: 11
                        Layout.preferredWidth: 58
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }
        }

        // Host settings.
        ColumnLayout {
            visible: panel.room.isHost
            Layout.fillWidth: true
            spacing: 4
            Text { text: "Host settings"; color: Theme.textDim; font.pixelSize: 12; topPadding: 4 }
            FlatCheck {
                text: "Only host controls playback"
                checked: panel.room.hostOnly
                onToggled: panel.room.setHostOnly(!panel.room.hostOnly)
            }
            FlatCheck {
                text: "Wait for everyone"
                checked: panel.room.waitForAll
                onToggled: panel.room.setWaitForAll(!panel.room.waitForAll)
            }
        }
        Text {
            visible: !panel.room.isHost && panel.room.hostOnly
            text: "Only the host controls playback"
            color: Theme.textDim
            font.pixelSize: 12
        }

        Text { text: "Chat"; color: Theme.textDim; font.pixelSize: 12; topPadding: 4 }

        ListView {
            id: chatList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: chatModel
            ScrollBar.vertical: ScrollBar { }

            delegate: Column {
                id: msg
                required property string who
                required property string body
                required property bool mine
                width: ListView.view.width - 10
                spacing: 0
                Text {
                    text: msg.who
                    color: msg.mine ? Theme.accent : Theme.textDim
                    font.pixelSize: 11
                    font.bold: true
                }
                Text {
                    width: parent.width
                    text: msg.body
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.text
                    font.pixelSize: Theme.fontSize
                }
            }
        }

        FlatField {
            id: chatInput
            Layout.fillWidth: true
            placeholderText: "Message"
            maximumLength: 500
            onAccepted: {
                if (text !== "")
                    panel.room.sendChat(text)
                text = ""
            }
            Keys.onEscapePressed: panel.releaseFocus()
        }

        FlatButton {
            text: "Leave room"
            Layout.alignment: Qt.AlignRight
            onClicked: panel.leaveRequested()
        }
    }
}
