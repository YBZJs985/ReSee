import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml.Models

Rectangle {
    id: root
    color: Theme.bg1
    readonly property bool controlsOpen: rateMenu.opened || audioPopup.opened || calendarPopup.opened
    readonly property bool editing: jump.activeFocus
    readonly property var rates: [0.5, 1, 2, 4, 8]

    readonly property string dateText: player.masterTimeMs > 0 ? player.masterDateText : player.dateNav.todayDateStr()

    function cancelEditingOutside(scenePosition) {
        const local = jump.mapFromItem(null, scenePosition)
        if (editing && !jump.contains(local))
            jump.focus = false
    }

    function toggleCalendar() {
        if (calendarPopup.visible)
            calendarPopup.close()
        else
            calendarPopup.open()
    }

    function shiftDay(direction) {
        const date = new Date(dateText + "T12:00:00")
        date.setDate(date.getDate() + direction)
        player.jumpToDateKeepTime(Qt.formatDate(date, "yyyy-MM-dd"))
    }

    function changeRate(direction) {
        const index = rates.indexOf(player.playbackRate)
        player.setPlaybackRate(rates[Math.max(0, Math.min(rates.length - 1, index + direction))])
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 3
        DarkButton {
            text: "−5 秒"
            onClicked: player.stepSeconds(-5)
        }
        IconButton {
            name: "previous"
            label: "上一段录像"
            onClicked: player.prevSegment()
        }
        IconButton {
            name: player.playing ? "pause" : "play"
            label: player.playing ? "暂停" : "播放"
            onClicked: player.togglePlay()
        }
        IconButton {
            name: "next"
            label: "下一段录像"
            onClicked: player.nextSegment()
        }
        DarkButton { text: "+5 秒"; onClicked: player.stepSeconds(5) }
    }

    RowLayout {
        anchors.centerIn: parent
        spacing: 12

        Row {
            Layout.alignment: Qt.AlignVCenter
            spacing: 0

            DarkButton {
                text: "‹"
                padX: 8
                Accessible.name: "前一天"
                onClicked: root.shiftDay(-1)
            }

            DarkButton {
                id: dateButton
                text: root.dateText + "  ▾"
                font.family: Theme.fontMono
                Accessible.name: "选择日期 " + root.dateText
                active: calendarPopup.opened
                onClicked: root.toggleCalendar()

                CalendarJumpPopup {
                    id: calendarPopup
                    x: Math.round((dateButton.width - width) / 2)
                    y: -height - 6
                }
            }

            DarkButton {
                text: "›"
                padX: 8
                Accessible.name: "后一天"
                onClicked: root.shiftDay(1)
            }
        }

        TextField {
            id: jump
            Layout.preferredWidth: 240
            Layout.alignment: Qt.AlignVCenter
            height: 40
            horizontalAlignment: TextInput.AlignHCenter
            font.family: Theme.fontMono
            font.pixelSize: activeFocus ? 22 : 28
            color: Theme.text
            selectionColor: Theme.accent
            selectedTextColor: Theme.ink
            Accessible.name: "播放时间。回车跳转"
            property bool failed: false
            Binding on text {
                when: !jump.activeFocus
                value: player.masterTimeText
                restoreMode: Binding.RestoreNone
            }
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    visible: jump.activeFocus
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: jump.failed ? Theme.error : Theme.accent
                }
            }
            onActiveFocusChanged: {
                failed = false
                if (activeFocus) selectAll()
            }
            onTextEdited: failed = false
            onAccepted: {
                failed = !player.jumpToDateTimeString(text)
                if (!failed) focus = false
            }
            Keys.onEscapePressed: focus = false
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4
        DarkButton {
            id: rateButton
            text: player.playbackRate + "×  ▾"
            Accessible.name: "播放速度 " + player.playbackRate + " 倍"
            onClicked: rateMenu.visible ? rateMenu.close() : rateMenu.open()
            Menu {
                id: rateMenu
                y: -implicitHeight
                palette.window: Theme.bg1
                palette.windowText: Theme.text
                palette.text: Theme.text
                palette.highlight: Theme.raised
                palette.highlightedText: Theme.accent
                Instantiator {
                    model: root.rates
                    delegate: MenuItem {
                        required property real modelData
                        text: modelData + "×"
                        checkable: true
                        checked: player.playbackRate === modelData
                        onTriggered: player.setPlaybackRate(modelData)
                    }
                    onObjectAdded: (index, item) => rateMenu.insertItem(index, item)
                    onObjectRemoved: (index, item) => rateMenu.removeItem(item)
                }
            }
        }
        IconButton {
            id: audio
            name: player.audioMuted ? "mute" : "audio"
            label: "音频"
            active: player.audioStreamIndex >= 0
            onClicked: audioPopup.visible ? audioPopup.close() : audioPopup.open()
            Popup {
                id: audioPopup
                x: audio.width - width
                y: -height - 4
                width: 272
                height: 308
                padding: 8
                focus: true
                background: Rectangle { color: Theme.bg1; border.color: Theme.line }
                contentItem: AudioMonitorPanel {}
            }
        }
    }
}