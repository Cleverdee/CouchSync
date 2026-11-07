import QtQuick

// Checkbox row. `checked` is driven from outside (no internal toggling, so bindings
// to server-side state stay intact); clicking only emits toggled().
Item {
    id: control

    property bool checked: false
    property string text: ""
    signal toggled()

    implicitHeight: 24
    implicitWidth: row.implicitWidth

    Row {
        id: row
        spacing: 8
        anchors.verticalCenter: parent.verticalCenter

        Rectangle {
            width: 16
            height: 16
            radius: 3
            anchors.verticalCenter: parent.verticalCenter
            color: control.checked ? Theme.accent : Theme.surface
            border.width: 1
            border.color: control.checked ? Theme.accent : Theme.hover

            Text {
                anchors.centerIn: parent
                visible: control.checked
                text: "✓"
                color: "white"
                font.pixelSize: 12
            }
        }

        Text {
            text: control.text
            color: Theme.text
            font.pixelSize: Theme.fontSize
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: control.toggled()
    }
}
