import QtQuick
import QtQuick.Controls

Button {
    id: control

    property bool active: false

    focusPolicy: Qt.NoFocus // keys must keep going to the window's key handler
    implicitHeight: 28
    implicitWidth: Math.max(32, label.implicitWidth + 20)
    font.pixelSize: Theme.fontSize

    contentItem: Text {
        id: label
        text: control.text
        font: control.font
        color: control.enabled ? Theme.text : Theme.textDim
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.down ? Theme.accent
             : control.hovered ? Theme.hover
             : control.active ? Theme.surface : "transparent"
    }
}
