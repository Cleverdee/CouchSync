import QtQuick
import QtQuick.Controls

// Dark text input matching the rest of the UI.
TextField {
    id: control

    color: Theme.text
    placeholderTextColor: Theme.textDim
    selectionColor: Theme.accent
    selectedTextColor: "white"
    font.pixelSize: Theme.fontSize
    leftPadding: 8
    rightPadding: 8
    implicitHeight: 30
    selectByMouse: true

    background: Rectangle {
        radius: Theme.radius
        color: Theme.surface
        border.width: 1
        border.color: control.activeFocus ? Theme.accent : "transparent"
    }
}
