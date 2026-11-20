import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    color: Theme.bg1

    signal openBookmarkDialog()
    signal toggleBookmarks()
    signal openSettings()
    signal toggleDiagnostics()
    property int healthLevel: 0

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 5

        SegmentSwitch {}
        Item { Layout.fillWidth: true }
        Text {
            visible: player.scanning
            text: player.scanStatusText
            color: Theme.textDim
            font.pixelSize: 11
            elide: Text.ElideRight
            Layout.maximumWidth: 220
        }
        DarkButton { text: "+ 标记"; onClicked: root.openBookmarkDialog() }
        DarkButton { text: "标记列表"; onClicked: root.toggleBookmarks() }
        IconButton { name: "settings"; label: "设置"; onClicked: root.openSettings() }
        Rectangle { width: 1; height: 16; color: Theme.line }
        DarkButton {
            text: "状态"
            leftPadding: 24
            Accessible.name: "设备状态 " + player.health.summaryText
            onClicked: root.toggleDiagnostics()
            Rectangle {
                width: 7
                height: 7
                radius: width / 2
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                color: root.healthLevel === 2 ? Theme.error : root.healthLevel === 1 ? Theme.warn : Theme.textDim
            }
        }
    }
}