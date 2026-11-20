import QtQuick
import QtQuick.Controls

Button {
    id: root
    property bool active: false
    property bool accent: false
    property bool danger: false
    property int fontSize: 12
    property int labelAlignment: Text.AlignHCenter
    property int padX: 10
    property int padY: 4
    property bool enabledBtn: true

    enabled: enabledBtn
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    leftPadding: padX
    rightPadding: padX
    topPadding: padY
    bottomPadding: padY
    implicitHeight: Math.max(26, implicitContentHeight + topPadding + bottomPadding)
    opacity: enabled ? 1 : 0.45
    Accessible.name: text

    contentItem: Text {
        text: root.text
        color: root.accent ? Theme.ink : root.active ? Theme.accent : root.danger ? Theme.error : Theme.text
        font.pixelSize: root.fontSize
        font.weight: root.active || root.accent ? Font.DemiBold : Font.Normal
        horizontalAlignment: root.labelAlignment
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: Theme.radius
        color: root.accent ? Theme.accent
              : root.down ? Theme.raised
              : root.hovered || root.active ? Theme.bg2 : "transparent"
        border.width: root.visualFocus ? 1 : 0
        border.color: root.accent ? Theme.ink : Theme.accent
    }
}