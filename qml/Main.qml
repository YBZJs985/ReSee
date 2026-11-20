pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReSee

ApplicationWindow {
    id: win
    width: 1520
    height: 920
    minimumWidth: 940
    minimumHeight: 600
    visible: true
    color: Theme.bg0
    title: "ReSee"
    font.family: Theme.fontUi

    palette.window: Theme.bg1
    palette.base: Theme.bg0
    palette.button: Theme.bg2
    palette.mid: Theme.line
    palette.dark: Theme.bg0
    palette.text: Theme.text
    palette.windowText: Theme.text
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.ink

    readonly property bool shortcutsActive: !transport.editing && !transport.controlsOpen && !settingsDialog.opened
        && !bookmarkDialog.opened && !win.bookmarkInputFocused
    property bool bookmarkInputFocused: false
    property bool diskWarningDismissed: false
    readonly property int healthLevel: player.health.offlineCount > 0 || player.diagnostics.nasDisconnected
        ? 2 : player.health.delayedCount > 0 || player.health.unknownCount > 0 || player.health.diskLevelCode > 0 ? 1 : 0

    function syncDiagnostics() {
        if (player.diagnosticsVisible && !diagnosticsPopup.visible)
            diagnosticsPopup.open()
        else if (!player.diagnosticsVisible && diagnosticsPopup.visible)
            diagnosticsPopup.close()
    }
    Component.onCompleted: syncDiagnostics()

    function openBookmark() {
        bookmarkDialog.openForCreate(timeline.mapToItem(Overlay.overlay, timeline.playheadX, 0))
    }

    function focusCamera(index) {
        player.focusedStream = player.focusedStream === index ? -1 : index
    }

    readonly property var configuredKeys: [
        player.shortcutTogglePlay, player.shortcutAddBookmark, player.shortcutStepBack,
        player.shortcutStepForward, player.shortcutExitFocus
    ].map(key => player.normalizedShortcut(key))

    function quickShortcutEnabled(sequence) {
        return shortcutsActive && !configuredKeys.includes(player.normalizedShortcut(sequence))
    }

    Shortcut { sequence: player.shortcutTogglePlay; enabled: win.shortcutsActive; onActivated: player.togglePlay() }
    Shortcut { sequence: player.shortcutAddBookmark; enabled: win.shortcutsActive; onActivated: win.openBookmark() }
    Shortcut { sequence: player.shortcutStepBack; enabled: win.shortcutsActive; onActivated: player.stepSeconds(-5) }
    Shortcut { sequence: player.shortcutStepForward; enabled: win.shortcutsActive; onActivated: player.stepSeconds(5) }
    Shortcut { sequence: player.shortcutExitFocus; enabled: win.shortcutsActive && !bookmarksPopup.opened && !diagnosticsPopup.opened; onActivated: player.focusedStream = -1 }
    Shortcut { sequence: "G"; enabled: win.quickShortcutEnabled(sequence); onActivated: transport.toggleCalendar() }
    Shortcut { sequence: "Ctrl+K"; enabled: win.quickShortcutEnabled(sequence); onActivated: transport.toggleCalendar() }
    Shortcut { sequence: "Ctrl+Left"; enabled: win.quickShortcutEnabled(sequence); onActivated: transport.shiftDay(-1) }
    Shortcut { sequence: "Ctrl+Right"; enabled: win.quickShortcutEnabled(sequence); onActivated: transport.shiftDay(1) }
    Shortcut { sequence: "Ctrl+B"; enabled: win.quickShortcutEnabled(sequence); onActivated: player.dateNavVisible = !player.dateNavVisible }
    Shortcut { sequence: "["; enabled: win.quickShortcutEnabled(sequence); onActivated: transport.changeRate(-1) }
    Shortcut { sequence: "]"; enabled: win.quickShortcutEnabled(sequence); onActivated: transport.changeRate(1) }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TapHandler {
            enabled: transport.editing
            acceptedButtons: Qt.LeftButton
            gesturePolicy: TapHandler.DragThreshold
            onTapped: (eventPoint) => transport.cancelEditingOutside(eventPoint.scenePosition)
        }

        ControlBar {
            id: topBar
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            healthLevel: win.healthLevel
            onOpenBookmarkDialog: win.openBookmark()
            onToggleBookmarks: bookmarksPopup.visible ? bookmarksPopup.close() : bookmarksPopup.open()
            onOpenSettings: settingsDialog.open()
            onToggleDiagnostics: player.diagnosticsVisible = !player.diagnosticsVisible
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            color: Theme.warn
            visible: player.health.diskOk && player.health.diskUsedRatio >= 0.90 && !win.diskWarningDismissed
            Text {
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "NAS 可用空间 " + player.health.diskFreeText
                color: Theme.ink
                font.pixelSize: 12
            }
            DarkButton {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                implicitHeight: 24
                accent: true
                text: "×"
                Accessible.name: "关闭空间提醒"
                onClicked: win.diskWarningDismissed = true
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            DateNavSidebar {
                Layout.preferredWidth: implicitWidth
                Layout.minimumWidth: implicitWidth
                Layout.maximumWidth: implicitWidth
                Layout.fillHeight: true
            }

            Item {
                id: wall
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                readonly property int count: player.streamCount
                readonly property int focusIndex: player.focusedStream
                readonly property bool grouped: player.groupedLayoutAvailable && player.layoutMode === "grouped"
                readonly property int columns: Math.max(1, Math.min(count, Math.ceil(Math.sqrt(count * 1.5))))
                readonly property int rows: Math.max(1, Math.ceil(count / columns))
                readonly property int gap: 2
                readonly property real cellWidth: (width - (columns - 1) * gap) / columns
                readonly property real cellHeight: (height - (rows - 1) * gap) / rows
                readonly property real focusWidth: Math.floor(width * 0.76)
                readonly property real sideWidth: width - focusWidth - gap
                readonly property real groupWidth: Math.floor(width * 0.70)
                readonly property real halfGroupWidth: (groupWidth - gap) / 2

                Column {
                    anchors.centerIn: parent
                    spacing: 12
                    visible: wall.count === 0
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "暂无摄像机"
                        color: Theme.textDim
                    }
                    PrimaryButton {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "添加摄像机"
                        onClicked: {
                            settingsDialog.page = 0
                            settingsDialog.open()
                            settingsDialog.openAddCamera()
                        }
                    }
                }

                Repeater {
                    model: wall.count
                    delegate: VideoTile {
                        required property int index
                        readonly property var streamConfig: player.streamConfigs[index]
                        streamIndex: index
                        title: streamConfig?.displayName ?? ""
                        cameraId: streamConfig?.cameraId ?? ""
                        isFocused: wall.focusIndex === index
                        compactMode: wall.focusIndex >= 0 ? !isFocused : wall.focusIndex === -2 && index >= 2
                        groupAvailable: index === 0 && (wall.focusIndex === -2 || (wall.focusIndex === -1 && wall.grouped))
                        groupFocused: wall.focusIndex === -2

                        readonly property int sideCount: Math.max(1, wall.count - (wall.focusIndex === -2 ? 2 : 1))
                        readonly property int sideIndex: wall.focusIndex === -2 ? index - 2 : index - (index > wall.focusIndex ? 1 : 0)
                        readonly property real sideHeight: (wall.height - (sideCount - 1) * wall.gap) / sideCount
                        readonly property bool groupedTop: wall.grouped && index < 3

                        x: {
                            if (wall.focusIndex !== -1)
                                return compactMode ? wall.focusWidth + wall.gap : 0
                            if (groupedTop && index > 0)
                                return index === 1 ? wall.halfGroupWidth + wall.gap : wall.groupWidth + wall.gap
                            return (index % wall.columns) * (wall.cellWidth + wall.gap)
                        }
                        y: {
                            if (compactMode) return sideIndex * (sideHeight + wall.gap)
                            if (wall.focusIndex === -2) return index * ((wall.height + wall.gap) / 2)
                            if (isFocused) return 0
                            return Math.floor(index / wall.columns) * (wall.cellHeight + wall.gap)
                        }
                        width: {
                            if (compactMode) return wall.sideWidth
                            if (wall.focusIndex !== -1) return wall.count === 1 ? wall.width : wall.focusWidth
                            if (groupedTop) return index < 2 ? wall.halfGroupWidth : wall.width - wall.groupWidth - wall.gap
                            return wall.cellWidth
                        }
                        height: compactMode ? sideHeight : isFocused ? wall.height
                            : wall.focusIndex === -2 ? (wall.height - wall.gap) / 2 : wall.cellHeight
                        z: isFocused ? 2 : 1
                        Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                        Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                        Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                        Behavior on height { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

                        onClickedTile: (stream) => win.focusCamera(stream)
                        onToggleGroup: player.focusedStream = wall.focusIndex === -2 ? -1 : -2
                        onWheelTimeScrub: (past, steps) => player.handleWheelScrub(past, steps)
                        Shortcut {
                            sequence: index < 9 ? String(index + 1) : ""
                            enabled: index < 9 && win.quickShortcutEnabled(sequence)
                            onActivated: win.focusCamera(index)
                        }
                    }
                }
            }
        }

        TransportBar { id: transport; Layout.fillWidth: true; Layout.preferredHeight: 44 }
        TimelinePanel { id: timeline; Layout.fillWidth: true }
    }

    BookmarkDialog { id: bookmarkDialog }
    SettingsDialog { id: settingsDialog }

    Popup {
        id: bookmarksPopup
        x: win.width - width - 8
        y: topBar.height + 4
        width: 334
        height: Math.min(620, wall.height - 12)
        padding: 0
        focus: true
        contentItem: BookmarkDrawer {
            onCloseRequested: bookmarksPopup.close()
            onEditRequested: (bm) => bookmarkDialog.openForEdit(bm)
            onSearchInputFocusedChanged: (focused) => win.bookmarkInputFocused = focused
        }
        onClosed: win.bookmarkInputFocused = false
    }

    Popup {
        id: diagnosticsPopup
        x: win.width - width - 8
        y: topBar.height + 4
        width: 360
        height: Math.min(680, wall.height - 12)
        padding: 0
        focus: true
        contentItem: DiagnosticsDrawer {
            onCloseRequested: player.diagnosticsVisible = false
        }
        onClosed: player.diagnosticsVisible = false
    }

    Connections {
        target: player
        function onLayoutChanged() { win.syncDiagnostics() }
        function onToastMessage(text, isError) {
            toast.text = text
            toast.color = isError ? Theme.error : Theme.text
            toast.visible = true
            toastTimer.restart()
        }
    }
    Connections {
        target: player.health
        function onSummaryChanged() {
            if (player.health.diskOk && player.health.diskUsedRatio < 0.90)
                win.diskWarningDismissed = false
        }
    }

    Label {
        id: toast
        visible: false
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: timeline.height + 52
        padding: 9
        background: Rectangle { radius: Theme.radius; color: Theme.hud }
        Timer { id: toastTimer; interval: 2800; onTriggered: toast.visible = false }
    }
}