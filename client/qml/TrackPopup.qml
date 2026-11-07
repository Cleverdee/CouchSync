import QtQuick
import QtQuick.Controls

// Audio / subtitle track picker (plus subtitle file loading and delay for subtitles).
Popup {
    id: popup

    property string kind: "audio" // "audio" | "sub"
    property var player
    signal loadSubtitleRequested()

    readonly property var entries: {
        const list = player.tracks.filter(t => t.type === kind)
        if (kind === "sub")
            return [{ id: 0, label: "Off", selected: !list.some(t => t.selected) }].concat(list)
        return list
    }

    function openFor(k) {
        kind = k
        open()
    }

    parent: Overlay.overlay
    x: parent.width - width - 12
    y: parent.height - 76 - height
    width: 340
    padding: 8
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.panel
        radius: Theme.radius
        border.color: Theme.hover
    }

    contentItem: Column {
        spacing: 2

        Text {
            text: popup.kind === "audio" ? "Audio track" : "Subtitles"
            color: Theme.textDim
            font.pixelSize: 12
            bottomPadding: 4
        }

        Text {
            visible: popup.entries.length === 0
            text: "No tracks"
            color: Theme.textDim
            font.pixelSize: Theme.fontSize
        }

        Repeater {
            model: popup.entries
            delegate: Rectangle {
                required property var modelData
                width: popup.availableWidth
                height: 28
                radius: Theme.radius
                color: ma.containsMouse ? Theme.hover : "transparent"

                Text {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    color: modelData.selected ? Theme.accent : Theme.text
                    font.pixelSize: Theme.fontSize
                    text: (modelData.selected ? "●  " : "  ") + modelData.label
                }

                MouseArea {
                    id: ma
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        if (popup.kind === "audio")
                            popup.player.setAudioTrack(modelData.id)
                        else
                            popup.player.setSubTrack(modelData.id)
                    }
                }
            }
        }

        Column {
            visible: popup.kind === "sub"
            width: popup.availableWidth
            spacing: 4
            topPadding: 6

            Row {
                spacing: 6
                FlatButton {
                    text: "Load subtitle file..."
                    onClicked: { popup.close(); popup.loadSubtitleRequested() }
                }
            }

            Row {
                spacing: 6
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSize
                    text: "Delay"
                }
                FlatButton { text: "−"; onClicked: popup.player.adjustSubDelay(-0.1) }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 64
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.text
                    font.pixelSize: Theme.fontSize
                    text: (popup.player.subDelay > 0 ? "+" : "") + popup.player.subDelay.toFixed(1) + " s"
                }
                FlatButton { text: "+"; onClicked: popup.player.adjustSubDelay(0.1) }
                FlatButton { text: "Reset"; onClicked: popup.player.resetSubDelay() }
            }
        }
    }
}
