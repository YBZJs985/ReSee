pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root
    width: 324
    height: 342
    padding: 12
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    property int displayYear: 0
    property int displayMonth: 1
    property int coverageRevision: 0
    property string comparisonDate: ""

    function openDate(date, exact) {
        player.jumpToDateKeepTime(date, exact)
        root.close()
    }

    function moveMonth(delta) {
        const next = new Date(displayYear, displayMonth - 1 + delta, 1)
        displayYear = next.getFullYear()
        displayMonth = next.getMonth() + 1
    }

    function buildMonthDays(year, month) {
        const first = new Date(year, month - 1, 1)
        const leading = (first.getDay() + 6) % 7
        const days = new Date(year, month, 0).getDate()
        return Array.from({length: 42}, (_, idx) => {
            const day = idx - leading + 1
            return day >= 1 && day <= days ? Qt.formatDate(new Date(year, month - 1, day), "yyyy-MM-dd") : ""
        })
    }

    onAboutToShow: {
        const date = new Date((player.masterTimeMs > 0 ? player.masterDateText : player.dateNav.todayDateStr()) + "T12:00:00")
        displayYear = date.getFullYear()
        displayMonth = date.getMonth() + 1
    }
    onOpened: previousMonth.forceActiveFocus()

    Connections {
        target: player.dateNav
        function onListChanged() { root.coverageRevision++ }
    }

    background: Rectangle { color: Theme.bg1; border.color: Theme.line; radius: Theme.radius }

    contentItem: ColumnLayout {
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            DarkButton { id: previousMonth; text: "‹"; Accessible.name: "上个月"; onClicked: root.moveMonth(-1) }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: root.displayYear + " 年 " + root.displayMonth + " 月"
                color: Theme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            DarkButton { text: "›"; Accessible.name: "下个月"; onClicked: root.moveMonth(1) }
        }
        Grid {
            columns: 7
            spacing: 3
            Layout.alignment: Qt.AlignHCenter
            Repeater {
                model: ["一", "二", "三", "四", "五", "六", "日"]
                Label {
                    required property string modelData
                    width: 39
                    height: 18
                    text: modelData
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.textDim
                    font.pixelSize: 11
                }
            }
            Repeater {
                model: root.buildMonthDays(root.displayYear, root.displayMonth)
                delegate: DarkButton {
                    id: dayButton
                    required property string modelData
                    width: 39
                    height: 32
                    enabled: modelData.length > 0
                    active: modelData === player.masterDateText
                    text: modelData.length ? Number(modelData.slice(-2)).toString() : ""
                    Accessible.name: modelData + (fraction > 0 ? " 有录像" : " 无录像")
                    readonly property real fraction: {
                        root.coverageRevision
                        return modelData.length ? player.dateNav.coverage(modelData) : 0
                    }
                    readonly property bool gap: {
                        root.coverageRevision
                        return modelData.length && player.dateNav.hasGap(modelData)
                    }
                    background: Rectangle {
                        color: dayButton.active ? Theme.bg2
                              : dayButton.hovered ? Theme.raised
                              : dayButton.fraction > 0 ? Qt.alpha(Theme.track, 0.2 + dayButton.fraction * 0.8)
                              : "transparent"
                        radius: Theme.radius
                        border.width: dayButton.active || dayButton.visualFocus ? 1 : 0
                        border.color: Theme.accent
                    }
                    Rectangle {
                        visible: dayButton.gap
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 3
                        width: 3
                        height: 3
                        radius: 2
                        color: Theme.error
                    }
                    onClicked: root.openDate(modelData, false)
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: {
                            root.comparisonDate = dayButton.modelData
                            comparisonMenu.popup(dayButton, 0, dayButton.height)
                        }
                    }
                    Keys.onMenuPressed: {
                        root.comparisonDate = modelData
                        comparisonMenu.popup(dayButton, 0, dayButton.height)
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            DarkButton { text: "昨天"; onClicked: { player.jumpToYesterday(); root.close() } }
            DarkButton { text: "今天"; onClicked: { player.jumpToToday(); root.close() } }
            Item { Layout.fillWidth: true }
            Label { text: "• 缺口"; color: Theme.textDim; font.pixelSize: 11 }
        }
    }

    Menu {
        id: comparisonMenu
        MenuItem { text: "对比同刻"; onTriggered: root.openDate(root.comparisonDate, true) }
    }
}