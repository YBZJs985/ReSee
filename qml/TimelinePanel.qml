pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReSee

Rectangle {
    id: root
    color: Theme.bg1

    readonly property bool expanded: player.timelineExpanded
    readonly property real start: player.timeline.viewStartMs
    readonly property real end: player.timeline.viewEndMs
    readonly property real span: Math.max(1, end - start)
    readonly property int streamCount: player.streamConfigs.length
    readonly property real playheadX: labelWidth + Math.max(0, Math.min(1, ratio(player.masterTimeMs))) * tracks.width
    readonly property int labelWidth: expanded ? 120 : 94
    readonly property int scrollHeight: 17
    readonly property int trackCount: expanded ? Math.max(1, streamCount) : 1
    readonly property int visibleTrackCount: Math.min(trackCount, 10)
    readonly property real trackOffset: trackScroll.position * trackCount * 18
    readonly property real dayMs: 86400000
    readonly property real tzMs: player.timezoneHours * 3600000
    readonly property real firstDay: dayStart(player.timeline.totalMinMs)
    readonly property real archiveEnd: dayStart(player.timeline.totalMaxMs - 1) + dayMs
    readonly property real maxScrollStart: Math.max(firstDay, archiveEnd - span)
    readonly property real scrollRange: maxScrollStart - firstDay
    property real selectionStart: -1
    property real selectionEnd: -1

    implicitHeight: 24 + visibleTrackCount * 18 + 6 + scrollHeight

    function dayStart(ms) { return Math.floor((ms + tzMs) / dayMs) * dayMs - tzMs }

    function ratio(ms) { return (ms - start) / span }
    function timeAt(x) { return Math.round(start + Math.max(0, Math.min(1, x / Math.max(1, tracks.width))) * span) }

    WheelHandler {
        acceptedModifiers: Qt.ControlModifier
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (event) => {
            const angle = event.angleDelta.y || event.angleDelta.x
            const pixels = event.pixelDelta.y || event.pixelDelta.x
            const delta = angle || pixels
            if (delta !== 0)
                player.timeline.panByRatio(-0.35 * delta / 120)
        }
    }

    DarkButton {
        y: 2
        text: root.expanded ? "▾ 通道" : "▸ 总览"
        Accessible.name: root.expanded ? "收起通道时间轴" : "展开通道时间轴"
        implicitHeight: 22
        onClicked: player.timelineExpanded = !root.expanded
    }

    Item {
        y: 24
        width: root.labelWidth
        height: root.visibleTrackCount * 18
        clip: true
        Column {
            y: -root.trackOffset
            Repeater {
                model: root.expanded ? root.streamCount : 0
                Label {
                    required property int index
                    width: root.labelWidth
                    height: 18
                    leftPadding: 10
                    rightPadding: trackScroll.visible ? 16 : 7
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    color: Theme.textDim
                    font.pixelSize: 10
                    text: player.streamConfigs[index]?.displayName ?? ""
                }
            }
        }
        ScrollBar {
            id: trackScroll
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            orientation: Qt.Vertical
            visible: root.trackCount > root.visibleTrackCount
            policy: ScrollBar.AlwaysOn
            size: root.visibleTrackCount / root.trackCount
            stepSize: 1 / root.trackCount
            Accessible.name: "滚动录像通道"
            onVisibleChanged: if (!visible) position = 0
        }
    }

    Item {
        id: tracks
        x: root.labelWidth
        width: root.width - x - 10
        height: root.height - root.scrollHeight
        clip: true

        property var ticks: []
        property var bookmarks: []
        function refreshTicks() { ticks = player.timeline.getTicks(Math.max(2, Math.floor(width / 104))) }
        function refreshBookmarks() { bookmarks = player.bookmarks.bookmarksInRange(root.start, root.end) }
        onWidthChanged: refreshTicks()
        Component.onCompleted: { refreshTicks(); refreshBookmarks() }

        Connections {
            target: player.timeline
            function onRangeChanged() { tracks.refreshTicks(); tracks.refreshBookmarks() }
            function onIndexChanged() { tracks.refreshTicks(); tracks.refreshBookmarks() }
        }
        Connections {
            target: player.bookmarks
            function onCountChanged() { tracks.refreshBookmarks() }
        }

        Repeater {
            model: tracks.ticks
            delegate: Item {
                required property var modelData
                x: Math.round(modelData.ratio * tracks.width)
                height: tracks.height
                Rectangle { y: 17; width: 1; height: parent.height - y; color: Theme.line }
                Text {
                    x: 4
                    y: 4
                    text: modelData.label
                    color: Theme.textDim
                    font.family: Theme.fontMono
                    font.pixelSize: 10
                }
            }
        }

        Item {
            y: 24
            width: parent.width
            height: root.visibleTrackCount * 18
            clip: true
            Column {
                y: -root.trackOffset
                width: parent.width
                Repeater {
                    model: root.trackCount
                    delegate: TimelineTrackItem {
                        required property int index
                        width: tracks.width
                        height: 18
                        model: player.timeline
                        stream: root.expanded && root.streamCount > 0 ? index : -1
                        dayColor: player.timelineDayColor
                        nightColor: player.timelineNightColor
                        writingColor: Theme.warn
                        errorColor: Theme.error
                    }
                }
            }
        }

        Repeater {
            model: tracks.bookmarks
            delegate: Rectangle {
                required property var modelData
                x: Math.round(modelData.ratio * tracks.width)
                y: 20
                width: 2
                height: tracks.height - y
                color: modelData.color
                Rectangle { width: 8; height: 5; radius: 1; color: parent.color }
            }
        }

        Rectangle {
            visible: root.selectionStart >= 0
            x: Math.min(root.ratio(root.selectionStart), root.ratio(root.selectionEnd)) * tracks.width
            width: Math.abs(root.selectionEnd - root.selectionStart) / root.span * tracks.width
            y: 18
            height: tracks.height - y
            color: Qt.alpha(Theme.accent, 0.20)
        }

        Rectangle {
            id: playhead
            readonly property real position: root.ratio(player.masterTimeMs)
            visible: position >= 0 && position <= 1
            x: Math.min(tracks.width - width, Math.round(position * tracks.width))
            y: 18
            width: 2
            height: tracks.height - y
            color: Theme.accent
            Rectangle { x: -3; width: 8; height: 4; radius: 1; color: Theme.accent }
        }

        Label {
            visible: playhead.visible && player.scrubbing && !interaction.containsMouse
            x: Math.max(0, Math.min(tracks.width - width, playhead.x - width / 2))
            y: 1
            text: player.masterTimeText
            color: Theme.ink
            font.family: Theme.fontMono
            font.pixelSize: 10
            leftPadding: 4
            rightPadding: 4
            background: Rectangle { color: Theme.accent; radius: Theme.radius }
        }

        Rectangle {
            visible: interaction.containsMouse
            x: Math.min(tracks.width - width, Math.round(interaction.mouseX))
            y: 20
            width: 1
            height: tracks.height - y
            color: Theme.textDim
        }

        Label {
            visible: interaction.containsMouse
            x: Math.max(0, Math.min(tracks.width - width, interaction.mouseX + 8))
            text: player.formatEpochMs(root.timeAt(interaction.mouseX))
            topPadding: 2
            bottomPadding: 2
            leftPadding: 5
            rightPadding: 5
            color: Theme.text
            font.family: Theme.fontMono
            font.pixelSize: 10
            background: Rectangle { color: Theme.raised; radius: Theme.radius }
        }

        MouseArea {
            id: interaction
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
            property real previousX: 0
            property string gesture: ""

            onPressed: (mouse) => {
                previousX = mouse.x
                if (mouse.button !== Qt.LeftButton) {
                    gesture = "pan"
                } else if (mouse.modifiers & Qt.ShiftModifier) {
                    gesture = "select"
                    root.selectionStart = root.timeAt(mouse.x)
                    root.selectionEnd = root.selectionStart
                } else {
                    gesture = "seek"
                    root.selectionStart = -1
                    player.beginTimelineScrub()
                    player.updateTimelineScrub(root.timeAt(mouse.x))
                }
            }
            onPositionChanged: (mouse) => {
                if (gesture === "pan") {
                    player.timeline.panByRatio((previousX - mouse.x) / Math.max(1, width))
                    previousX = mouse.x
                } else if (gesture === "select") {
                    root.selectionEnd = root.timeAt(mouse.x)
                } else if (gesture === "seek") {
                    player.updateTimelineScrub(root.timeAt(mouse.x))
                }
            }
            function finish() {
                if (gesture === "seek") player.endTimelineScrub()
                gesture = ""
            }
            onReleased: finish()
            onCanceled: finish()
            onWheel: (wheel) => {
                if (wheel.modifiers & Qt.ControlModifier) {
                    wheel.accepted = false
                    return
                }
                const dy = wheel.angleDelta.y || wheel.pixelDelta.y
                if (dy === 0) return
                const anchor = Math.max(0, Math.min(1, wheel.x / Math.max(1, width)))
                player.timeline.zoomAt(dy > 0 ? 0.75 : 1 / 0.75, anchor)
                wheel.accepted = true
            }
        }
    }

    Label {
        x: 10
        anchors.bottom: parent.bottom
        height: root.scrollHeight
        verticalAlignment: Text.AlignVCenter
        text: player.formatEpochMs(root.start).slice(0, 10)
        font.family: Theme.fontMono
        font.pixelSize: 10
        color: Theme.textDim
    }

    Slider {
        id: scrollbar
        x: tracks.x
        width: tracks.width
        height: root.scrollHeight
        anchors.bottom: parent.bottom
        padding: 0
        from: 0
        to: 1
        enabled: player.timeline.totalMaxMs > 0 && root.scrollRange > 0
        value: enabled ? Math.max(0, Math.min(1, (root.start - root.firstDay) / root.scrollRange)) : 0
        Accessible.name: "横向浏览录像时间轴"
        Accessible.description: player.formatEpochMs(root.start)
        onMoved: {
            const target = root.firstDay + value * root.scrollRange
            player.timeline.panByRatio((target - root.start) / root.span)
        }

        background: Rectangle {
            x: scrollbar.leftPadding
            y: Math.round((scrollbar.height - height) / 2)
            width: scrollbar.availableWidth
            height: 4
            radius: 2
            color: Theme.line
        }

        handle: Rectangle {
            x: scrollbar.leftPadding + scrollbar.visualPosition * (scrollbar.availableWidth - width)
            y: Math.round((scrollbar.height - height) / 2)
            width: scrollbar.enabled
                ? Math.min(scrollbar.availableWidth, Math.max(34, scrollbar.availableWidth * root.span / Math.max(1, root.archiveEnd - root.firstDay)))
                : scrollbar.availableWidth
            height: scrollbar.hovered || scrollbar.pressed ? 8 : 6
            radius: height / 2
            color: !scrollbar.enabled ? Theme.line
                : scrollbar.visualFocus || scrollbar.pressed ? Theme.textDim : Theme.track
        }

        ToolTip.visible: enabled && (pressed || hovered)
        ToolTip.delay: 350
        ToolTip.text: player.formatEpochMs(root.start) + " – " + player.formatEpochMs(root.end)
    }
}
