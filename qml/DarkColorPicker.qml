import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property string label: ""
    property string colorValue: String(Theme.track)
    property var presetSwatches: Theme.timelineSwatches

    signal colorEdited(string hexColor)

    implicitWidth: triggerRow.implicitWidth
    implicitHeight: 28

    property real hue: 0.6
    property real sat: 0.78
    property real val: 0.94
    property bool internalUpdating: false

    function syncFromHex(hexStr) {
        if (internalUpdating) return
        const c = Qt.color(hexStr)
        if (!c || c.a === undefined) return
        const r = c.r, g = c.g, b = c.b
        const maxC = Math.max(r, g, b)
        const minC = Math.min(r, g, b)
        const d = maxC - minC
        let h = 0
        if (d > 0.0001) {
            if (maxC === r) h = ((g - b) / d + (g < b ? 6 : 0)) / 6
            else if (maxC === g) h = ((b - r) / d + 2) / 6
            else h = ((r - g) / d + 4) / 6
        }
        const s = maxC > 0.0001 ? (d / maxC) : 0
        hue = h
        sat = s
        val = maxC
        svCanvas.requestPaint()
    }

    function commitHsv() {
        internalUpdating = true
        const c = Qt.hsva(hue, sat, val, 1.0)
        const toHex = (v) => {
            const n = Math.max(0, Math.min(255, Math.round(v * 255)))
            return (n < 16 ? "0" : "") + n.toString(16).toUpperCase()
        }
        const hex = "#" + toHex(c.r) + toHex(c.g) + toHex(c.b)
        colorValue = hex
        hexInput.text = hex
        internalUpdating = false
        colorEdited(hex)
    }

    onColorValueChanged: {
        syncFromHex(colorValue)
        if (hexInput && !hexInput.activeFocus) {
            hexInput.text = colorValue.toUpperCase()
        }
    }

    Component.onCompleted: syncFromHex(colorValue)

    // BoardUI ColorWell 紧凑触发样式
    RowLayout {
        id: triggerRow
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        Text {
            visible: root.label !== ""
            text: root.label
            color: Theme.textDim
            font.pixelSize: 11
        }

        Rectangle {
            id: wellBtn
            implicitWidth: wellRow.implicitWidth + 14
            height: 26
            radius: 5
            color: wellMouse.containsMouse ? Theme.bg2 : Theme.black
            border.color: pickerPopup.opened ? Theme.accent : (wellMouse.containsMouse ? Theme.textDim : Theme.line)
            border.width: 1

            Row {
                id: wellRow
                anchors.centerIn: parent
                spacing: 7

                Rectangle {
                    width: 14
                    height: 14
                    radius: 3.5
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.colorValue
                    border.color: Qt.alpha(Theme.text, 0.267)
                    border.width: 1
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.colorValue.toUpperCase()
                    color: Theme.text
                    font.pixelSize: 11
                    font.family: Theme.fontMono
                }
            }

            MouseArea {
                id: wellMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (pickerPopup.opened) pickerPopup.close()
                    else pickerPopup.open()
                }
            }
        }
    }

    // BoardUI 风格调色浮层
    Popup {
        id: pickerPopup
        y: wellBtn.height + 6
        x: Math.max(-140, wellBtn.x - 20)
        width: 236
        padding: 10
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        background: Rectangle {
            color: Theme.bg1
            radius: 8
            border.color: Theme.line
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: 8

            // SV 二维饱和度 / 明度拾色板
            Item {
                Layout.fillWidth: true
                height: 120

                Canvas {
                    id: svCanvas
                    anchors.fill: parent
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        const hueCol = Qt.hsva(root.hue, 1.0, 1.0, 1.0)
                        ctx.fillStyle = hueCol.toString()
                        ctx.fillRect(0, 0, width, height)

                        const wg = ctx.createLinearGradient(0, 0, width, 0)
                        wg.addColorStop(0, "rgba(255,255,255,1)")
                        wg.addColorStop(1, "rgba(255,255,255,0)")
                        ctx.fillStyle = wg
                        ctx.fillRect(0, 0, width, height)

                        const bg = ctx.createLinearGradient(0, 0, 0, height)
                        bg.addColorStop(0, "rgba(0,0,0,0)")
                        bg.addColorStop(1, "rgba(0,0,0,1)")
                        ctx.fillStyle = bg
                        ctx.fillRect(0, 0, width, height)
                    }
                }

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.color: Theme.line
                    radius: 4
                }

                Rectangle {
                    width: 12; height: 12; radius: 6
                    x: Math.max(0, Math.min(parent.width, root.sat * parent.width)) - 6
                    y: Math.max(0, Math.min(parent.height, (1.0 - root.val) * parent.height)) - 6
                    color: "transparent"
                    border.color: Theme.text
                    border.width: 2
                    Rectangle {
                        anchors.fill: parent; anchors.margins: 1; radius: 5
                        color: "transparent"; border.color: Theme.black; border.width: 1
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.CrossCursor
                    function pick(mx, my) {
                        root.sat = Math.max(0, Math.min(1, mx / width))
                        root.val = Math.max(0, Math.min(1, 1.0 - (my / height)))
                        root.commitHsv()
                    }
                    onPressed: (m) => pick(m.x, m.y)
                    onPositionChanged: (m) => { if (pressed) pick(m.x, m.y) }
                }
            }

            // 色相滑轨 (Hue Rail)
            Item {
                Layout.fillWidth: true
                height: 14

                Canvas {
                    anchors.fill: parent
                    onPaint: {
                        const ctx = getContext("2d")
                        const g = ctx.createLinearGradient(0, 0, width, 0)
                        const stops = Theme.hueStops
                        for (let i = 0; i < stops.length; ++i) {
                            g.addColorStop(i / (stops.length - 1), stops[i])
                        }
                        ctx.fillStyle = g
                        ctx.fillRect(0, 2, width, height - 4)
                    }
                }

                Rectangle {
                    width: 8; height: 14; radius: 3
                    x: Math.max(0, Math.min(parent.width - width, root.hue * (parent.width - width)))
                    color: Theme.text
                    border.color: Theme.line
                    border.width: 1
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    function pickHue(mx) {
                        root.hue = Math.max(0, Math.min(1, mx / width))
                        svCanvas.requestPaint()
                        root.commitHsv()
                    }
                    onPressed: (m) => pickHue(m.x)
                    onPositionChanged: (m) => { if (pressed) pickHue(m.x) }
                }
            }

            // 推荐预设色板 (Swatches)
            GridLayout {
                Layout.fillWidth: true
                columns: 6
                columnSpacing: 6
                rowSpacing: 6

                Repeater {
                    model: root.presetSwatches
                    delegate: Rectangle {
                        required property string modelData
                        width: 28; height: 18; radius: 4
                        color: modelData
                        border.width: root.colorValue.toUpperCase() === modelData ? 2 : 1
                        border.color: root.colorValue.toUpperCase() === modelData ? Theme.text : Theme.line

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.colorValue = modelData
                                root.syncFromHex(modelData)
                                hexInput.text = modelData
                                root.colorEdited(modelData)
                            }
                        }
                    }
                }
            }

            // HEX 输入框
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Text { text: "HEX:"; color: Theme.textDim; font.pixelSize: 10 }
                Rectangle {
                    Layout.fillWidth: true
                    height: 24
                    radius: 4
                    color: Theme.black
                    border.color: Theme.line

                    TextInput {
                        id: hexInput
                        anchors.fill: parent
                        anchors.margins: 5
                        verticalAlignment: TextInput.AlignVCenter
                        color: Theme.text
                        font.pixelSize: 11
                        font.family: Theme.fontMono
                        text: root.colorValue.toUpperCase()
                        onTextEdited: {
                            const s = text.trim()
                            if (/^#[0-9A-Fa-f]{6}$/.test(s)) {
                                root.colorValue = s.toUpperCase()
                                root.syncFromHex(s)
                                root.colorEdited(s.toUpperCase())
                            }
                        }
                    }
                }
                DarkButton {
                    text: "完成"
                    accent: true
                    fontSize: 10
                    padX: 10
                    padY: 2
                    implicitHeight: 24
                    onClicked: pickerPopup.close()
                }
            }
        }
    }
}
