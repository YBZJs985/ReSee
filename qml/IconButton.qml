import QtQuick
import QtQuick.Controls

DarkButton {
    id: root
    required property string name
    required property string label
    implicitWidth: 30
    implicitHeight: 28
    Accessible.name: label
    ToolTip.text: label
    ToolTip.visible: hovered
    ToolTip.delay: 400

    contentItem: Item {
        implicitWidth: 16
        implicitHeight: 16
        Image {
            anchors.centerIn: parent
            width: 16
            height: 16
            sourceSize.width: 24
            sourceSize.height: 24
            source: "qrc:/ui/resources/toolbar/" + root.name + ".svg"
        }
    }
}