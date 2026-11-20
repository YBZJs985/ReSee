import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    parent: Overlay.overlay
    x: editMode ? (parent.width - width) / 2 : Math.max(8, Math.min(parent.width - width - 8, anchorPoint.x - width / 2))
    y: editMode ? (parent.height - height) / 2 : Math.max(8, anchorPoint.y - height - 8)
    width: 506
    modal: true
    padding: 16
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property point anchorPoint: Qt.point(0, 0)
    property bool editMode: false
    property var editingId: 0
    property string selectedColor: String(Theme.accent)
    property int selectedStream: -1

    function openForCreate(point) {
        anchorPoint = point
        editMode = false
        editingId = 0
        titleField.text = "关键事件"
        noteField.text = ""
        selectedColor = String(Theme.accent)
        selectedStream = (player.focusedStream >= 0) ? player.focusedStream : -1
        open()
    }

    onOpened: { titleField.forceActiveFocus(); titleField.selectAll() }

    function submit() {
        if (titleField.text.trim() === "") return
        if (editMode) {
            player.bookmarks.updateBookmark(editingId, titleField.text, noteField.text, selectedColor, selectedStream)
        } else {
            player.addBookmarkWithCapture(titleField.text, noteField.text, selectedColor, selectedStream)
        }
        close()
    }

    function openForEdit(bm) {
        editMode = true
        editingId = bm.id
        titleField.text = bm.title
        noteField.text = bm.note
        selectedColor = bm.color
        selectedStream = bm.streamIndex
        open()
    }

    background: Rectangle {
        color: Theme.bg1
        radius: 10
        border.color: Theme.line
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: 10

        Text {
            text: root.editMode ? "编辑事件标记" : ("添加时间标记 · " + player.masterFullText)
            color: Theme.text
            font.pixelSize: 14
            font.weight: Font.Bold
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            radius: 6
            color: Theme.black
            border.color: titleField.activeFocus ? Theme.accent : Theme.line
            TextInput {
                id: titleField
                Accessible.name: "标记标题"
                onAccepted: root.submit()
                anchors.fill: parent
                anchors.margins: 8
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.text
                font.pixelSize: 12
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 68
            radius: 6
            color: Theme.black
            border.color: noteField.activeFocus ? Theme.accent : Theme.line
            TextEdit {
                id: noteField
                anchors.fill: parent
                anchors.margins: 8
                color: Theme.text
                font.pixelSize: 12
                wrapMode: TextEdit.Wrap
                Text {
                    visible: noteField.text === ""
                    text: "备注..."
                    color: Theme.textDim
                    font.pixelSize: 12
                }
            }
        }

        // 颜色与关联通道选择
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Text { text: "色标:"; color: Theme.textDim; font.pixelSize: 11 }
            Repeater {
                model: Theme.markerColors
                delegate: Rectangle {
                    required property string modelData
                    width: 20
                    height: 20
                    radius: 10
                    color: modelData
                    border.color: root.selectedColor === modelData ? Theme.text : "transparent"
                    border.width: 2
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.selectedColor = modelData
                    }
                }
            }
            Item { Layout.fillWidth: true }
        }

        Flow {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            spacing: 3
            Text { text: "视角:"; color: Theme.textDim; font.pixelSize: 11 }
            DarkButton {
                text: "全局"
                fontSize: 10; padX: 7; padY: 2; implicitHeight: 22
                active: root.selectedStream === -1
                onClicked: root.selectedStream = -1
            }
            Repeater {
                model: player.streamConfigs.length
                delegate: DarkButton {
                    required property int index
                    text: player.streamConfigs[index].displayName
                    fontSize: 10; padX: 7; padY: 2; implicitHeight: 22
                    active: root.selectedStream === index
                    onClicked: root.selectedStream = index
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            DarkButton {
                text: "取消"
                onClicked: root.close()
            }
            PrimaryButton {
                text: root.editMode ? "保存" : "创建标记"
                onClicked: root.submit()
            }
        }
    }
}
