import QtQuick
import QtQuick.Controls

CheckBox {
    id: control
    implicitHeight: 28
    implicitWidth: Math.max(indicator.width, indicator.width + spacing + contentItem.implicitWidth)
    spacing: 8
    padding: 0
    hoverEnabled: true

    indicator: Rectangle {
        x: control.leftPadding
        y: (control.height - height) / 2
        width: 17
        height: 17
        radius: 4
        color: control.checked
               ? (control.down ? Theme.accent : (control.hovered ? Theme.accent : Theme.accent))
               : (control.hovered ? Theme.bg2 : Theme.black)
        border.width: 1
        border.color: control.checked
                      ? Theme.accent
                      : (control.hovered ? Theme.textDim : Theme.line)

        Behavior on color { ColorAnimation { duration: 110 } }
        Behavior on border.color { ColorAnimation { duration: 110 } }

        Canvas {
            anchors.fill: parent
            visible: control.checked
            onVisibleChanged: if (visible) requestPaint()
            onPaint: {
                const ctx = getContext("2d");
                ctx.clearRect(0, 0, width, height);
                ctx.strokeStyle = Theme.ink;
                ctx.lineWidth = 2.0;
                ctx.lineCap = "round";
                ctx.lineJoin = "round";
                ctx.beginPath();
                ctx.moveTo(4.2, 8.8);
                ctx.lineTo(7.3, 12.0);
                ctx.lineTo(12.8, 5.2);
                ctx.stroke();
            }
        }
    }

    contentItem: Text {
        text: control.text
        font.pixelSize: 12
        color: control.enabled ? (control.hovered ? Theme.text : Theme.text) : Theme.textDim
        verticalAlignment: Text.AlignVCenter
        leftPadding: control.indicator.width + control.spacing
    }
}
