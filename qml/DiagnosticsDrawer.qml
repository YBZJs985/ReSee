import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root
    color: Theme.bg1
    radius: 8
    border.color: Theme.line
    border.width: 1

    signal closeRequested()

    // 0 = 录像健康（默认）, 1 = 同步诊断
    property int activeTab: 0
    Connections {
        target: player
        function onSettingsChanged() {
            if (!player.developerDiagnostics) root.activeTab = 0
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // 顶部页签切换 + 关闭按钮
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                radius: 6
                color: Theme.black
                border.color: Theme.line

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 2
                    spacing: 2

                    DarkButton {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        text: "录像健康"
                        fontSize: 11
                        padX: 6; padY: 2
                        active: root.activeTab === 0
                        onClicked: root.activeTab = 0
                    }
                    DarkButton {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        text: "同步诊断"
                        visible: player.developerDiagnostics
                        fontSize: 11
                        padX: 6; padY: 2
                        active: root.activeTab === 1
                        onClicked: root.activeTab = 1
                    }
                }
            }

            DarkButton {
                text: "✕"
                padX: 7; padY: 2; implicitHeight: 26
                onClicked: root.closeRequested()
            }
        }

        // 双页内容区（不销毁对象、切换零延迟、不影响播放解码）
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.activeTab

            // =====================================================
            // 页 0：录像健康监测（整合自“米家监控存活”）
            // =====================================================
            ColumnLayout {
                id: healthTabLayout
                spacing: 8
                property string hoveredCamName: ""
                property string hoveredStorageText: ""
                property double hoveredStorageRatio: 0.0

                // 健康概览 + 立即检测
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: healthSumCol.implicitHeight + 14
                    radius: 6
                    color: Theme.bg2
                    border.color: !player.health.nasAccessible ? Theme.error : Theme.line

                    ColumnLayout {
                        id: healthSumCol
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 5

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            Text {
                                text: player.health.summaryText
                                color: {
                                    if (!player.health.nasAccessible || player.health.offlineCount > 0) return Theme.error
                                    if (player.health.delayedCount > 0) return Theme.warn
                                    if (player.health.normalCount > 0) return Theme.ok
                                    return Theme.textDim
                                }
                                font.pixelSize: 12
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }

                            DarkButton {
                                text: player.health.checking ? "检测中..." : "立即检测"
                                accent: !player.health.checking
                                enabled: !player.health.checking
                                fontSize: 10
                                padX: 8; padY: 2; implicitHeight: 22
                                onClicked: player.health.checkNow()
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text: "NAS: " + (player.health.nasAccessible ? "可访问" : "不可访问") +
                                      "  |  最近检测: " + player.health.lastCheckText
                                color: Theme.textDim
                                font.pixelSize: 10
                                Layout.fillWidth: true
                            }
                        }
                    }
                }

                // NAS 存储空间条
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: diskCol.implicitHeight + 14
                    radius: 6
                    color: Theme.bg2
                    border.color: Theme.line

                    Column {
                        id: diskCol
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 5

                        RowLayout {
                            width: parent.width
                            Text {
                                text: "NAS 存储空间"
                                color: Theme.text
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                Layout.fillWidth: true
                            }
                            Text {
                                text: player.health.diskOk
                                      ? (player.health.diskLevelText + " (" + player.health.diskFreePctText + " 剩余)")
                                      : "读取失败 / 未知"
                                color: !player.health.diskOk ? Theme.textDim
                                       : (player.health.diskLevelCode === 0 ? Theme.ok
                                          : (player.health.diskLevelCode === 1 ? Theme.warn : Theme.error))
                                font.pixelSize: 10
                            }
                        }

                        // 横向进度条（悬停右侧卡片时高亮该摄像头占比）
                        Rectangle {
                            width: parent.width
                            height: 8
                            radius: 4
                            color: Theme.bg0
                            border.color: Theme.line
                            clip: true

                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: player.health.diskOk ? Math.max(4, parent.width * player.health.diskUsedRatio) : 0
                                radius: 4
                                color: player.health.diskLevelCode === 0 ? Theme.accent
                                       : (player.health.diskLevelCode === 1 ? Theme.warn : Theme.error)
                                opacity: healthTabLayout.hoveredStorageRatio > 0 ? 0.38 : 1.0
                                Behavior on opacity { NumberAnimation { duration: 120 } }
                            }

                            Rectangle {
                                visible: player.health.diskOk && healthTabLayout.hoveredStorageRatio > 0
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: Math.max(6, parent.width * Math.min(1.0, healthTabLayout.hoveredStorageRatio))
                                radius: 4
                                color: Theme.accent
                                border.color: Theme.accent
                                border.width: 1
                                Behavior on width { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                            }
                        }

                        Text {
                            visible: player.health.diskOk
                            text: healthTabLayout.hoveredStorageRatio > 0
                                  ? ("「" + healthTabLayout.hoveredCamName + "」占用 " + healthTabLayout.hoveredStorageText +
                                     "（占总空间 " + (healthTabLayout.hoveredStorageRatio * 100).toFixed(1) + "%）")
                                  : ("已用 " + player.health.diskUsedText + " / 总 " + player.health.diskTotalText +
                                     "  ·  可用 " + player.health.diskFreeText)
                            color: healthTabLayout.hoveredStorageRatio > 0 ? Theme.accent : Theme.textDim
                            font.pixelSize: 10
                            font.family: Theme.fontMono
                            elide: Text.ElideRight
                            width: parent.width
                        }
                    }
                }

                // 五台摄像头健康卡片列表（点击展开明细）
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: player.health
                    spacing: 6
                    clip: true

                    delegate: Rectangle {
                        id: camCard
                        required property string cameraId
                        required property string displayName
                        required property int statusCode
                        required property string statusText
                        required property string lastWriteText
                        required property string ageText
                        required property string latestFileName
                        required property string latestRangeText
                        required property var lastJumpMs
                        required property string storageText
                        required property double storageRatio
                        required property var fileCount
                        required property bool isDual
                        required property var channels
                        required property string errorMessage
                        required property string thumbUrl

                        property bool expanded: false
                        property bool cardHovered: cardHover.hovered

                        width: ListView.view.width
                        height: cardCol.implicitHeight + 14
                        radius: 6
                        color: cardHovered ? Theme.line : (expanded ? Theme.line : Theme.bg2)
                        border.color: cardHovered ? Theme.accent : (expanded ? Theme.accent : Theme.line)

                        Behavior on height { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                        Behavior on border.color { ColorAnimation { duration: 110 } }

                        HoverHandler {
                            id: cardHover
                            onHoveredChanged: {
                                if (hovered) {
                                    healthTabLayout.hoveredCamName = camCard.displayName
                                    healthTabLayout.hoveredStorageText = camCard.storageText
                                    healthTabLayout.hoveredStorageRatio = camCard.storageRatio
                                    player.highlightedCameraId = camCard.cameraId
                                } else if (player.highlightedCameraId === camCard.cameraId) {
                                    healthTabLayout.hoveredCamName = ""
                                    healthTabLayout.hoveredStorageText = ""
                                    healthTabLayout.hoveredStorageRatio = 0.0
                                    player.highlightedCameraId = ""
                                }
                            }
                        }

                        ColumnLayout {
                            id: cardCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 7
                            spacing: 4

                            // 紧凑头部（缩略图 + 名称状态 + 存储占比细条，点击展开/收起）
                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 42

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: camCard.expanded = !camCard.expanded
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    spacing: 8

                                    // 摄像头静帧缩略图
                                    Rectangle {
                                        Layout.preferredWidth: 50
                                        Layout.preferredHeight: 36
                                        Layout.alignment: Qt.AlignVCenter
                                        radius: 5
                                        color: Theme.black
                                        border.color: camCard.cardHovered ? Theme.accent : Theme.line
                                        clip: true

                                        Image {
                                            id: thumbImg
                                            anchors.fill: parent
                                            source: camCard.thumbUrl
                                            fillMode: Image.PreserveAspectCrop
                                            asynchronous: true
                                            cache: true
                                            visible: status === Image.Ready
                                        }

                                        Text {
                                            anchors.centerIn: parent
                                            visible: thumbImg.status !== Image.Ready
                                            text: "无图"
                                            color: Theme.textDim
                                            font.pixelSize: 9
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        spacing: 2

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 5

                                            Rectangle {
                                                width: 7; height: 7; radius: 3.5
                                                color: {
                                                    if (camCard.statusCode === 0) return Theme.ok
                                                    if (camCard.statusCode === 1) return Theme.warn
                                                    if (camCard.statusCode === 2) return Theme.error
                                                    return Theme.textDim
                                                }
                                            }

                                            Text {
                                                text: camCard.displayName
                                                color: Theme.text
                                                font.pixelSize: 11
                                                font.weight: Font.DemiBold
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }

                                            Text {
                                                text: camCard.statusText
                                                color: {
                                                    if (camCard.statusCode === 0) return Theme.ok
                                                    if (camCard.statusCode === 1) return Theme.warn
                                                    if (camCard.statusCode === 2) return Theme.error
                                                    return Theme.textDim
                                                }
                                                font.pixelSize: 10
                                                font.weight: Font.Bold
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                text: camCard.errorMessage !== ""
                                                      ? camCard.errorMessage
                                                      : ("最近写入: " + camCard.ageText + "  ·  " + camCard.storageText)
                                                color: Theme.textDim
                                                font.pixelSize: 10
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Text {
                                                text: camCard.expanded ? "▾" : "▸"
                                                color: Theme.textDim
                                                font.pixelSize: 10
                                            }
                                        }

                                        // 占 NAS 总存储空间比例微缩条
                                        Rectangle {
                                            Layout.fillWidth: true
                                            height: 4
                                            radius: 2
                                            color: Theme.bg0
                                            clip: true

                                            Rectangle {
                                                anchors.left: parent.left
                                                anchors.top: parent.top
                                                anchors.bottom: parent.bottom
                                                width: camCard.storageRatio > 0
                                                       ? Math.max(4, parent.width * Math.min(1.0, camCard.storageRatio))
                                                       : 0
                                                radius: 2
                                                color: camCard.cardHovered ? Theme.accent : Theme.accent
                                                Behavior on color { ColorAnimation { duration: 110 } }
                                            }
                                        }
                                    }
                                }
                            }

                            // 展开详细信息（单摄直接查看最后录像，双摄分别列出 00/10 通道）
                            ColumnLayout {
                                visible: camCard.expanded
                                Layout.fillWidth: true
                                spacing: 5

                                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }

                                Text {
                                    Layout.fillWidth: true
                                    text: "文件数: " + camCard.fileCount + "  ·  占用: " + camCard.storageText +
                                          (camCard.storageRatio > 0 ? (" (" + (camCard.storageRatio * 100).toFixed(1) + "%)") : "")
                                    color: Theme.textDim
                                    font.pixelSize: 10
                                }

                                // 单摄展示
                                ColumnLayout {
                                    visible: !camCard.isDual
                                    Layout.fillWidth: true
                                    spacing: 3

                                    Text {
                                        Layout.fillWidth: true
                                        elide: Text.ElideMiddle
                                        text: "最新文件: " + camCard.latestFileName
                                        color: Theme.text
                                        font.pixelSize: 10
                                        font.family: Theme.fontMono
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: "录像区间: " + camCard.latestRangeText
                                        color: Theme.textDim
                                        font.pixelSize: 10
                                        font.family: Theme.fontMono
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: "落盘时间: " + camCard.lastWriteText
                                        color: Theme.textDim
                                        font.pixelSize: 10
                                        font.family: Theme.fontMono
                                    }
                                    DarkButton {
                                        Layout.fillWidth: true
                                        text: "查看最后录像"
                                        accent: true
                                        fontSize: 10
                                        padX: 8; padY: 3; implicitHeight: 22
                                        enabled: camCard.lastJumpMs > 0
                                        onClicked: player.jumpToHealthLastVideo(camCard.lastJumpMs)
                                    }
                                }

                                // 双摄展示（00 与 10 两路独立写入状态与最后录像跳转）
                                Repeater {
                                    model: camCard.isDual ? camCard.channels : []
                                    delegate: Rectangle {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        implicitHeight: dualChCol.implicitHeight + 10
                                        radius: 5
                                        color: Theme.line
                                        border.color: Theme.line

                                        ColumnLayout {
                                            id: dualChCol
                                            anchors.fill: parent
                                            anchors.margins: 5
                                            spacing: 2

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Text {
                                                    text: modelData.displayName
                                                    color: Theme.text
                                                    font.pixelSize: 10
                                                    font.weight: Font.DemiBold
                                                    Layout.fillWidth: true
                                                }
                                                Text {
                                                    text: modelData.statusText + " (" + modelData.ageText + ")"
                                                    color: {
                                                        if (modelData.statusCode === 0) return Theme.ok
                                                        if (modelData.statusCode === 1) return Theme.warn
                                                        if (modelData.statusCode === 2) return Theme.error
                                                        return Theme.textDim
                                                    }
                                                    font.pixelSize: 10
                                                }
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                elide: Text.ElideMiddle
                                                text: modelData.latestRangeText + " · 落盘 " + modelData.lastWriteText
                                                color: Theme.textDim
                                                font.pixelSize: 9
                                                font.family: Theme.fontMono
                                            }
                                            DarkButton {
                                                Layout.fillWidth: true
                                                text: "查看该路最后录像"
                                                fontSize: 10
                                                padX: 6; padY: 2; implicitHeight: 20
                                                enabled: modelData.lastJumpMs > 0
                                                onClicked: player.jumpToHealthLastVideo(modelData.lastJumpMs)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // =====================================================
            // 页 1：原有同步与性能诊断面板
            // =====================================================
            ColumnLayout {
                spacing: 8

                // 全局摘要卡片
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: sumCol.implicitHeight + 14
                    radius: 6
                    color: Theme.bg2
                    border.color: player.diagnostics.nasDisconnected ? Theme.error : Theme.line

                    Column {
                        id: sumCol
                        anchors.fill: parent
                        anchors.margins: 7
                        spacing: 4

                        RowLayout {
                            width: parent.width
                            spacing: 6

                            Rectangle {
                                height: 18
                                width: gsTxt.implicitWidth + 12
                                radius: 4
                                color: {
                                    const c = player.diagnostics.globalStateCode
                                    if (c === 0) return Theme.line
                                    if (c === 2) return Theme.warn
                                    if (c === 3) return Theme.textDim
                                    if (c === 4) return Theme.warn
                                    if (c === 5) return Theme.error
                                    return Theme.bg2
                                }
                                Text {
                                    id: gsTxt
                                    anchors.centerIn: parent
                                    text: player.diagnostics.globalStateText
                                    color: {
                                        const c = player.diagnostics.globalStateCode
                                        if (c === 0) return Theme.ok
                                        if (c === 2) return Theme.warn
                                        if (c === 3) return Theme.accent
                                        if (c === 4) return Theme.warn
                                        if (c === 5) return Theme.error
                                        return Theme.textDim
                                    }
                                    font.pixelSize: 10
                                    font.weight: Font.Bold
                                }
                            }

                            Text {
                                text: "最大误差: " + (player.diagnostics.hasValidSyncError ? (player.diagnostics.maxSyncErrorMs + " ms") : "—")
                                color: (player.diagnostics.hasValidSyncError && player.diagnostics.maxSyncErrorMs > 900) ? Theme.error : Theme.ok
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                Layout.fillWidth: true
                            }
                        }

                        Text {
                            text: "已同步 " + player.diagnostics.syncedCount + " 路 · 无录像 " +
                                  player.diagnostics.noVideoCount + " 路 · 未就绪 " + player.diagnostics.notReadyCount + " 路"
                            color: Theme.text
                            font.pixelSize: 10
                        }

                        Text {
                            text: "NAS: " + (player.diagnostics.nasDisconnected ? "断开重连中" : "在线") +
                                  " | 缓存命中: " + player.diagnostics.cacheHitRate.toFixed(1) + "% | 延迟: " +
                                  player.diagnostics.avgNasReadMs.toFixed(1) + " ms"
                            color: Theme.accent
                            font.pixelSize: 10
                        }
                        Text {
                            text: "内存: " + player.diagnostics.processMemMb.toFixed(0) + " MB  CPU: " +
                                  player.diagnostics.cpuUsagePct.toFixed(1) + "%  SSD: " +
                                  player.diagnostics.ssdCacheGb.toFixed(2) + "/" +
                                  player.diagnostics.ssdCacheMaxGb.toFixed(0) + " GB"
                            color: Theme.textDim
                            font.pixelSize: 10
                        }
                    }
                }

                // 通道明细列表
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: player.diagnostics
                    spacing: 6
                    clip: true

                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 66
                        radius: 6
                        color: Theme.bg2
                        border.color: Theme.line

                        Column {
                            anchors.fill: parent
                            anchors.margins: 7
                            spacing: 2

                            RowLayout {
                                width: parent.width
                                Text {
                                    text: model.displayName
                                    color: Theme.text
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: model.hwBackend + " " + model.resolution
                                    color: Theme.accent
                                    font.pixelSize: 10
                                    font.family: Theme.fontMono
                                }
                            }

                            Text {
                                text: model.stateText + " | " + model.wallTimeText +
                                      " | 误差 " + model.syncErrorText + " | " + model.fps.toFixed(1) + "fps"
                                color: {
                                    const c = model.stateCode
                                    if (c === 5 || c === 6) return Theme.error
                                    if (c === 1) return Theme.warn
                                    if (c === 7 || c === 8) return Theme.accent
                                    if (c === 4 || c === 0) return Theme.textDim
                                    if (c === 3) return Theme.warn
                                    return (model.hasValidTime && Math.abs(model.syncErrorMs) > 900) ? Theme.error : Theme.ok
                                }
                                font.pixelSize: 10
                                font.family: Theme.fontMono
                            }

                            Text {
                                text: "偏移 " + model.offsetMs + "ms | Seek " + model.seekMs.toFixed(0) +
                                      "ms | 队列 " + model.queueDepth +
                                      (model.calibConfidence > 0 ? (" | 置信 " + model.calibConfidence.toFixed(2)) : "")
                                color: Theme.textDim
                                font.pixelSize: 10
                            }
                        }
                    }
                }
            }
        }
    }
}
