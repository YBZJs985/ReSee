import QtQuick
import QtQuick.Controls
import ReSee

Item {
    id: root
    required property int streamIndex
    required property string cameraId
    required property string title
    property bool isFocused: false
    property bool compactMode: false
    property bool groupAvailable: false
    property bool groupFocused: false
    readonly property bool highlighted: player.highlightedCameraId !== "" && player.highlightedCameraId === cameraId
    readonly property bool missing: renderItem.decodeStateCode === 4
    readonly property bool failed: renderItem.decodeStateCode === 5 || renderItem.decodeStateCode === 6
    readonly property bool waitingForFrame: renderItem.isLocating || renderItem.isBuffering
    readonly property bool showWaitOverlay: renderItem.hasVideo && waitingForFrame && !player.scrubbing && waitDelayElapsed
    property bool waitDelayElapsed: false
    onWaitingForFrameChanged: if (!waitingForFrame) waitDelayElapsed = false
    readonly property bool activeAudio: player.audioStreamIndex === streamIndex

    signal clickedTile(int streamIndex)
    signal wheelTimeScrub(bool towardPast, int steps)
    signal toggleGroup()

    clip: true
    property var neighbors: ({})
    function refreshNeighbors() {
        if (missing)
            neighbors = player.queryStreamNoVideoNeighbors(streamIndex)
    }

    onMissingChanged: refreshNeighbors()
    Connections {
        target: player
        enabled: root.missing
        function onMasterTimeChanged() { root.refreshNeighbors() }
    }

    Rectangle { anchors.fill: parent; color: Theme.black }

    Canvas {
        anchors.fill: parent
        visible: !renderItem.hasVideo
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.lineWidth = 1
            ctx.strokeStyle = Qt.alpha(Theme.raised, 0.4)
            ctx.beginPath()
            for (let x = -height; x < width; x += 12) {
                ctx.moveTo(x, height)
                ctx.lineTo(x + height, 0)
            }
            ctx.stroke()
        }
    }

    VideoRenderItem {
        id: renderItem
        anchors.fill: parent
        streamIndex: root.streamIndex
        Component.onCompleted: player.registerVideoItem(root.streamIndex, renderItem)
        Component.onDestruction: player.unregisterVideoItem(root.streamIndex, renderItem)
    }

    Timer {
        interval: 180
        running: root.waitingForFrame && !player.scrubbing && !root.waitDelayElapsed
        onTriggered: root.waitDelayElapsed = true
    }

    Rectangle {
        anchors.fill: parent
        visible: root.showWaitOverlay
        color: Qt.alpha(Theme.black, 0.3)

        Column {
            anchors.centerIn: parent
            spacing: 4

            Canvas {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 22
                height: 22
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    ctx.lineWidth = 2
                    ctx.strokeStyle = Theme.warn
                    ctx.beginPath()
                    ctx.arc(11, 11, 8, -Math.PI / 2, 0.75 * Math.PI)
                    ctx.stroke()
                }

                RotationAnimator on rotation {
                    running: root.showWaitOverlay
                    from: 0
                    to: 360
                    duration: 850
                    loops: Animation.Infinite
                }
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                padding: 4
                text: (renderItem.isLocating ? "定位中" : "缓冲中")
                      + (renderItem.waitDurationMs >= 1000 ? "  " + (renderItem.waitDurationMs / 1000).toFixed(1) + " 秒" : "")
                color: Theme.warn
                font.pixelSize: root.compactMode ? 11 : 12
                background: Rectangle { color: Theme.hud; radius: Theme.radius }
            }
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        property real lastX: 0
        property real lastY: 0
        property bool dragged: false

        onPressed: (event) => {
            lastX = event.x
            lastY = event.y
            dragged = false
        }
        onPositionChanged: (event) => {
            if (!pressed || renderItem.zoomScale <= 1.02) return
            const dx = event.x - lastX
            const dy = event.y - lastY
            if (!dragged && Math.abs(dx) < 2 && Math.abs(dy) < 2) return
            dragged = true
            renderItem.panBy(dx, dy)
            lastX = event.x
            lastY = event.y
        }
        onDoubleClicked: {
            if (renderItem.zoomScale > 1.02) renderItem.resetZoom()
            else if (!dragged) root.clickedTile(root.streamIndex)
        }
        onWheel: (wheel) => {
            const dy = wheel.angleDelta.y || wheel.pixelDelta.y
            if (dy === 0) return
            if (wheel.modifiers & Qt.ControlModifier) {
                renderItem.zoomAt(dy > 0 ? 1.18 : 1 / 1.18,
                                  wheel.x / Math.max(1, width), wheel.y / Math.max(1, height))
            } else {
                root.wheelTimeScrub(dy > 0, Math.max(1, Math.round(Math.abs(dy) / 120)))
            }
            wheel.accepted = true
        }
    }

    HoverHandler { id: hover }

    Column {
        anchors.centerIn: parent
        spacing: 3
        visible: !renderItem.hasVideo
        width: Math.min(parent.width - 12, 330)

        Text {
            width: parent.width
            text: root.missing ? "该时刻无录像" : renderItem.decodeStateText
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            color: root.failed ? Theme.error : root.waitingForFrame ? Theme.warn : Theme.textDim
            font.pixelSize: root.compactMode ? 11 : 13
        }
        Row {
            visible: root.missing && !root.compactMode && (hover.hovered || nextButton.activeFocus || prevButton.activeFocus || checkButton.activeFocus)
            anchors.horizontalCenter: parent.horizontalCenter
            DarkButton {
                id: prevButton
                text: "‹ 上一段"
                fontSize: 10
                enabled: !!root.neighbors.hasPrev
                onClicked: player.jumpStreamPrevSegment(root.streamIndex)
            }
            DarkButton {
                id: nextButton
                text: root.neighbors.hasNext ? "下一段 " + root.neighbors.nextStartText.slice(-8) + " ›" : "下一段 ›"
                fontSize: 10
                enabled: !!root.neighbors.hasNext
                onClicked: player.jumpStreamNextSegment(root.streamIndex)
            }
            DarkButton {
                id: checkButton
                text: "重查"
                fontSize: 10
                onClicked: player.recheckStreamAtCurrentTime(root.streamIndex)
            }
        }
    }

    Row {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 4

        Rectangle {
            visible: root.activeAudio && !player.audioMuted
            width: 24
            height: 24
            radius: Theme.radius
            color: Theme.hud
            Accessible.role: Accessible.Graphic
            Accessible.name: "当前音源 " + root.title

            Image {
                anchors.centerIn: parent
                width: 16
                height: 16
                sourceSize.width: 24
                sourceSize.height: 24
                source: "qrc:/ui/resources/toolbar/audio-active.svg"
            }
        }

        Label {
            visible: root.failed
            anchors.verticalCenter: parent.verticalCenter
            text: "⚠"
            color: Theme.warn
            font.pixelSize: 16
            Accessible.name: renderItem.decodeStateText
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 32
        opacity: hover.hovered || root.highlighted || root.isFocused
            || audioButton.activeFocus || focusButton.activeFocus || groupButton.activeFocus || zoomButton.activeFocus ? 1 : 0
        visible: opacity > 0 || activeFocus
        color: Theme.hud
        Behavior on opacity { NumberAnimation { duration: 120 } }

        HudLabel {
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.right: actions.left
            anchors.verticalCenter: parent.verticalCenter
            text: (!root.compactMode ? (root.streamIndex + 1) + "  " : "") + root.title
            color: Theme.text
            elide: Text.ElideRight
            font.pixelSize: 12
        }

        Row {
            id: actions
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            DarkButton {
                id: zoomButton
                visible: renderItem.zoomScale > 1.02
                text: renderItem.zoomScale.toFixed(1) + "×"
                Accessible.name: "重置局部放大"
                onClicked: renderItem.resetZoom()
            }
            DarkButton {
                id: groupButton
                visible: root.groupAvailable
                text: root.groupFocused ? "还原双摄" : "整组放大"
                onClicked: root.toggleGroup()
            }
            IconButton {
                id: audioButton
                visible: !root.compactMode
                name: root.activeAudio ? "audio" : "mute"
                active: root.activeAudio
                label: root.activeAudio ? "关闭此路音频" : "监听此路音频"
                onClicked: player.toggleAudioStream(root.streamIndex)
            }
            IconButton {
                id: focusButton
                name: root.isFocused ? "restore" : "focus"
                label: root.isFocused ? "还原画面布局" : "放大此路画面"
                onClicked: root.clickedTile(root.streamIndex)
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        visible: root.highlighted
        border.width: 2
        border.color: Theme.accent
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 2
        visible: renderItem.isPreview || renderItem.isLocating || (renderItem.isBuffering && root.waitDelayElapsed)
        color: Theme.warn
    }
}