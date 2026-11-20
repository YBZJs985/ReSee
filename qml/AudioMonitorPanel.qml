pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    spacing: 6

    Label {
        text: "监听音源"
        color: Theme.text
        font.weight: Font.DemiBold
    }

    ListView {
        id: sources
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: player.audioSourceList
        ScrollBar.vertical: ScrollBar {}
        delegate: DarkButton {
            required property var modelData
            width: sources.width
            implicitHeight: 26
            labelAlignment: Text.AlignLeft
            text: modelData.displayName + (modelData.hasAudio ? "" : modelData.hasVideo ? "  ·  无音轨" : "  ·  无录像")
            active: modelData.selected
            onClicked: player.selectManualAudioStream(modelData.streamIndex)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Row {
            Layout.fillWidth: true
            spacing: 3
            Column {
                spacing: 3
                Repeater {
                    model: 2
                    Rectangle {
                        required property int index
                        width: 146
                        height: 5
                        color: Theme.raised
                        visible: index === 0 || player.audioMeterChannels > 1
                        readonly property real level: index === 0 ? player.audioRmsNormL : player.audioRmsNormR
                        readonly property real peak: index === 0 ? player.audioPeakHoldL : player.audioPeakHoldR
                        Rectangle {
                            height: parent.height
                            width: parent.width * Math.min(1, Math.max(0, parent.level))
                            color: parent.level > 0.85 ? Theme.error : parent.level > 0.65 ? Theme.warn : Theme.ok
                        }
                        Rectangle {
                            visible: parent.peak > 0
                            x: Math.min(parent.width - 1, parent.width * parent.peak)
                            height: parent.height
                            width: 1
                            color: Theme.text
                        }
                    }
                }
            }
        }
        Label {
            text: player.audioPeakDbText
            color: Theme.textDim
            font.family: Theme.fontMono
            font.pixelSize: 10
        }
    }

    RowLayout {
        Layout.fillWidth: true
        DarkButton {
            text: player.audioMuted ? "取消静音" : "静音"
            active: player.audioMuted
            onClicked: player.toggleAudioMute()
        }
        Slider {
            id: volumeSlider
            Layout.fillWidth: true
            from: 0
            to: 1
            value: player.audioVolume
            Accessible.name: "音量"
            onMoved: player.setAudioVolume(value)

            background: Rectangle {
                x: volumeSlider.leftPadding
                y: volumeSlider.topPadding + (volumeSlider.availableHeight - height) / 2
                implicitWidth: 200
                implicitHeight: 6
                width: volumeSlider.availableWidth
                height: implicitHeight
                radius: height / 2
                color: "black"
                scale: volumeSlider.mirrored ? -1 : 1

                Rectangle {
                    width: parent.width * volumeSlider.position
                    height: parent.height
                    radius: Math.min(width, height) / 2
                    color: "white"
                }
            }
        }
        DarkButton {
            text: Math.round(player.audioVolume * 100) + "%"
            Accessible.name: "将音量恢复到 100%"
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            onClicked: player.resetAudioVolume100()
        }
    }
}