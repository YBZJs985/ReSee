pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    readonly property bool expanded: player.dateNavVisible
    implicitWidth: expanded ? 160 : 32
    color: Theme.bg1
    clip: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 2

        RowLayout {
            Layout.fillWidth: true
            Layout.minimumHeight: 32
            Layout.preferredHeight: 32
            Layout.maximumHeight: 32
            spacing: 0

            Label {
                visible: root.expanded
                Layout.fillWidth: true
                leftPadding: 10
                text: "日期"
                color: Theme.textDim
                font.pixelSize: 12
            }

            DarkButton {
                Layout.preferredWidth: 32
                Layout.preferredHeight: 32
                text: root.expanded ? "‹" : "›"
                Accessible.name: root.expanded ? "折叠日期侧栏" : "展开日期侧栏"
                ToolTip.text: Accessible.name + " (Ctrl+B)"
                ToolTip.visible: hovered
                onClicked: player.dateNavVisible = !root.expanded
            }
        }

        GridLayout {
            visible: root.expanded
            Layout.fillWidth: true
            Layout.leftMargin: 5
            Layout.rightMargin: 5
            columns: 2
            columnSpacing: 2
            rowSpacing: 0

            DarkButton { Layout.fillWidth: true; text: "今天"; onClicked: player.jumpToToday() }
            DarkButton { Layout.fillWidth: true; text: "昨天"; onClicked: player.jumpToYesterday() }
            DarkButton {
                Layout.fillWidth: true
                text: "近 7 天"
                active: player.dateNav.recent7DaysOnly
                onClicked: player.dateNav.recent7DaysOnly = !player.dateNav.recent7DaysOnly
            }
            DarkButton {
                Layout.fillWidth: true
                text: "有录像"
                active: player.dateNav.onlyWithVideo
                onClicked: player.dateNav.onlyWithVideo = !player.dateNav.onlyWithVideo
            }
        }

        ListView {
            id: dateList
            visible: root.expanded
            Layout.fillHeight: true
            Layout.fillWidth: true
            model: player.dateNav
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            readonly property int selectedIndex: player.dateNav.selectedRowIndex
            function revealSelection() {
                if (visible && selectedIndex >= 0 && selectedIndex < count)
                    positionViewAtIndex(selectedIndex, ListView.Contain)
            }
            onSelectedIndexChanged: revealSelection()
            onVisibleChanged: if (visible) Qt.callLater(revealSelection)
            onCountChanged: Qt.callLater(revealSelection)

            delegate: Rectangle {
                id: row
                required property int index
                required property string dateStr
                required property string monthDayText
                required property string yearText
                required property string weekdayText
                required property bool hasVideo
                required property bool isSelected
                required property var coverageSlots
                required property real sunriseFrac
                required property real sunsetFrac

                width: dateList.width
                height: 36
                color: row.isSelected ? Theme.raised : dateButton.hovered || coverageInput.containsMouse ? Theme.bg2 : "transparent"

                DarkButton {
                    id: dateButton
                    width: parent.width
                    height: 24
                    text: row.monthDayText + "  " + row.weekdayText
                    font.family: Theme.fontMono
                    labelAlignment: Text.AlignLeft
                    Accessible.name: row.dateStr + " " + row.weekdayText + (row.hasVideo ? "" : "，无录像")
                    onClicked: player.jumpToDateKeepTime(row.dateStr)

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: (mouse) => player.jumpToDateKeepTime(row.dateStr, (mouse.modifiers & Qt.ControlModifier) !== 0)
                    }

                    Label {
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.yearText
                        color: Theme.textDim
                        font.family: Theme.fontMono
                        font.pixelSize: 10
                    }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    y: 25
                    height: 5
                    color: Theme.line

                    Canvas {
                        anchors.fill: parent
                        readonly property var slots: row.coverageSlots
                        readonly property real sunrise: row.sunriseFrac
                        readonly property real sunset: row.sunsetFrac
                        readonly property color dayColor: player.timelineDayColor
                        readonly property color nightColor: player.timelineNightColor

                        onSlotsChanged: requestPaint()
                        onSunriseChanged: requestPaint()
                        onSunsetChanged: requestPaint()
                        onDayColorChanged: requestPaint()
                        onNightColorChanged: requestPaint()
                        onWidthChanged: requestPaint()

                        function fillRange(ctx, from, to, color) {
                            if (to <= from) return
                            const left = Math.round(from * width)
                            ctx.fillStyle = color
                            ctx.fillRect(left, 0, Math.round(to * width) - left, height)
                        }

                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            if (!slots || slots.length === 0) return

                            let start = -1
                            for (let i = 0; i <= slots.length; ++i) {
                                if (i < slots.length && slots[i] > 0) {
                                    if (start < 0) start = i
                                } else if (start >= 0) {
                                    const from = start / slots.length
                                    const to = i / slots.length
                                    fillRange(ctx, from, Math.min(sunrise, to), nightColor)
                                    fillRange(ctx, Math.max(sunrise, from), Math.min(sunset, to), dayColor)
                                    fillRange(ctx, Math.max(sunset, from), to, nightColor)
                                    start = -1
                                }
                            }
                        }
                    }

                    Rectangle {
                        visible: row.isSelected
                        x: Math.min(parent.width - width, Math.max(0, player.dateNav.currentDayFraction * parent.width))
                        width: 1
                        height: parent.height
                        color: Theme.textDim
                    }
                }

                MouseArea {
                    id: coverageInput
                    y: 24
                    width: parent.width
                    height: 12
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor

                    function fractionAt(x) {
                        return Math.max(0, Math.min(1, (x - 10) / Math.max(1, width - 20)))
                    }

                    ToolTip.visible: containsMouse
                    ToolTip.delay: 350
                    ToolTip.text: containsMouse ? player.dateNav.hoverInfoForSlot(row.index, fractionAt(mouseX)) : ""
                    onClicked: (mouse) => player.jumpToDateAtFraction(row.dateStr, fractionAt(mouse.x))
                }
            }
        }

        Item {
            visible: !root.expanded
            Layout.fillHeight: true
        }
    }
}
