import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    color: Theme.bg1
    radius: 8
    border.color: Theme.line
    border.width: 1

    signal closeRequested()
    signal editRequested(var bm)
    signal searchInputFocusedChanged(bool focused)

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: "时间事件标记 (" + player.bookmarks.count + ")"
                color: Theme.text
                font.pixelSize: 13
                font.weight: Font.Bold
                Layout.fillWidth: true
            }
            DarkButton {
                text: "✕"
                padX: 7
                padY: 2
                implicitHeight: 24
                onClicked: root.closeRequested()
            }
        }

        // 搜索与按日筛选
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                radius: 6
                color: Theme.black
                border.color: Theme.line
                TextInput {
                    id: searchBox
                    anchors.fill: parent
                    anchors.margins: 7
                    verticalAlignment: TextInput.AlignVCenter
                    color: Theme.text
                    font.pixelSize: 11
                    onActiveFocusChanged: root.searchInputFocusedChanged(activeFocus || dateBox.activeFocus)
                    onTextChanged: player.bookmarks.searchQuery = text
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: searchBox.text === ""
                        text: "搜索标题或备注..."
                        color: Theme.textDim
                        font.pixelSize: 11
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 96
                Layout.preferredHeight: 28
                radius: 6
                color: Theme.black
                border.color: Theme.line
                TextInput {
                    id: dateBox
                    anchors.fill: parent
                    anchors.margins: 7
                    verticalAlignment: TextInput.AlignVCenter
                    color: Theme.text
                    font.pixelSize: 11
                    font.family: Theme.fontMono
                    onActiveFocusChanged: root.searchInputFocusedChanged(searchBox.activeFocus || activeFocus)
                    onTextChanged: player.bookmarks.dateFilter = text
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: dateBox.text === ""
                        text: "YYYY-MM-DD"
                        color: Theme.textDim
                        font.pixelSize: 10
                    }
                }
            }
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: player.bookmarks
            spacing: 6
            clip: true

            delegate: Rectangle {
                width: ListView.view.width
                height: colBox.implicitHeight + 14
                radius: 6
                color: Theme.bg2
                border.color: bmArea.containsMouse ? Theme.accent : Theme.line

                MouseArea {
                    id: bmArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: player.seekToMs(model.timestampMs)
                }

                ColumnLayout {
                    id: colBox
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: model.color
                        }

                        Text {
                            text: model.title
                            color: Theme.text
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        DarkButton {
                            text: "改"
                            fontSize: 10
                            padX: 6
                            padY: 1
                            implicitHeight: 20
                            onClicked: root.editRequested({
                                id: model.bookmarkId,
                                title: model.title,
                                note: model.note,
                                color: model.color,
                                streamIndex: model.streamIndex
                            })
                        }

                        DarkButton {
                            text: "删"
                            fontSize: 10
                            padX: 6
                            padY: 1
                            implicitHeight: 20
                            danger: true
                            onClicked: player.bookmarks.deleteBookmark(model.bookmarkId)
                        }
                    }

                    Text {
                        text: model.timeText + (model.streamIndex >= 0 ? (" · 通道" + model.streamIndex) : " · 全局")
                        color: Theme.accent
                        font.pixelSize: 10
                        font.family: Theme.fontMono
                    }

                    Text {
                        visible: model.note !== ""
                        text: model.note
                        color: Theme.textDim
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Image {
                        visible: model.screenshotUrl !== ""
                        source: model.screenshotUrl
                        Layout.fillWidth: true
                        Layout.preferredHeight: 84
                        fillMode: Image.PreserveAspectCrop
                        clip: true
                    }
                }
            }
        }
    }
}
