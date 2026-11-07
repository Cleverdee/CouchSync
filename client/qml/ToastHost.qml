import QtQuick

// Stack of short-lived messages ("Ali paused", "Waiting for Sara…", errors).
Column {
    id: host

    function show(text, isError) {
        if (toasts.count >= 5)
            toasts.remove(0)
        toasts.append({ msg: text, isError: !!isError })
    }

    spacing: 6

    ListModel { id: toasts }

    Repeater {
        model: toasts

        delegate: Rectangle {
            id: toast
            required property int index
            required property string msg
            required property bool isError

            anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
            width: Math.min(label.implicitWidth + 28, 480)
            height: 32
            radius: Theme.radius
            color: isError ? "#e6a83232" : Theme.panel
            border.width: 1
            border.color: isError ? "#ff7070" : Theme.hover

            Text {
                id: label
                anchors.centerIn: parent
                width: parent.width - 20
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: toast.msg
                color: Theme.text
                font.pixelSize: Theme.fontSize
            }

            Timer {
                interval: toast.isError ? 6000 : 3500
                running: true
                onTriggered: toasts.remove(toast.index)
            }
        }
    }
}
