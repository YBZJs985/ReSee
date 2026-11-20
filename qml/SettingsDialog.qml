pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs as Native

Dialog {
    id: root
    anchors.centerIn: Overlay.overlay
    width: Math.min(880, Overlay.overlay.width - 24)
    height: Math.min(600, Overlay.overlay.height - 24)
    modal: true
    font.pixelSize: 13
    padding: 0
    closePolicy: Popup.CloseOnEscape

    property int page: 0
    property var dirty: ({})
    property bool opening: false
    property string shortcutWarning: ""
    readonly property var pageNames: ["机位", "播放与缓存", "健康监测", "时间轴", "快捷键"]

    function openAddCamera() {
        newName.text = ""
        newPath.text = ""
        dualCamera.checked = false
        dualCamera.lastCheckedPath = ""
        addCameraDialog.open()
    }

    function queue(section) {
        if (!opened || opening) return
        dirty[section] = true
        persist.restart()
    }

    function savePending() {
        const pending = dirty
        if (pending.names) {
            const values = []
            for (let i = 0; i < cameraRows.count; ++i)
                values.push(cameraRows.itemAt(i).displayName)
            player.updateStreamNames(values)
        }
        if (pending.offsets) {
            for (let i = 0; i < cameraRows.count; ++i) {
                const text = cameraRows.itemAt(i).offset
                const value = Number(text)
                if (text.trim() !== "" && Number.isInteger(value) && value !== player.streamConfigs[i].offsetMs)
                    player.setStreamOffsetMs(i, value)
            }
        }
        if (pending.cache)
            player.updateCacheAndDecodeSettings(cachePath.text, Number(cacheSize.text) || player.cacheMaxGb, hardware.checked)
        if (pending.audio)
            player.setAudioFollowsFocus(audioFollow.checked)
        if (pending.health)
            player.updateHealthSettings(autoCheck.checked, Number(interval.text) || player.healthCheckIntervalMin,
                                        notifyOffline.checked, notifyRecovered.checked)
        if (pending.location && latitude.acceptableInput && longitude.acceptableInput && timezone.acceptableInput)
            player.updateLocationSettings(locationEnabled.checked, city.text, Number(latitude.text),
                                          Number(longitude.text), Number(timezone.text))
        if (pending.colors)
            player.updateTimelineColors(dayColor.colorValue, nightColor.colorValue)
        if (pending.shortcuts) {
            const values = [playKey.text, markKey.text, backKey.text, forwardKey.text, exitKey.text]
            const normalized = values.map(key => player.normalizedShortcut(key))
            if (new Set(normalized).size !== values.length || normalized.some(key => key === "")) {
                shortcutWarning = "快捷键无效或重复"
            } else {
                shortcutWarning = ""
                player.updateShortcuts(...values)
            }
        }
        dirty = ({})
    }

    onAboutToShow: {
        opening = true
        shortcutWarning = ""
        cachePath.text = player.cacheDir
        cacheSize.text = String(player.cacheMaxGb)
        hardware.checked = player.preferHwDecode
        audioFollow.checked = player.audioFollowsFocus
        autoCheck.checked = player.healthAutoCheck
        interval.text = String(player.healthCheckIntervalMin)
        notifyOffline.checked = player.healthNotifyOffline
        notifyRecovered.checked = player.healthNotifyRecovered
        locationEnabled.checked = player.locationConfigured
        city.text = player.locationCity
        latitude.text = String(player.latitude)
        longitude.text = String(player.longitude)
        timezone.text = String(player.timezoneHours)
        dayColor.colorValue = player.timelineDayColor
        nightColor.colorValue = player.timelineNightColor
        playKey.text = player.shortcutTogglePlay
        markKey.text = player.shortcutAddBookmark
        backKey.text = player.shortcutStepBack
        forwardKey.text = player.shortcutStepForward
        exitKey.text = player.shortcutExitFocus
        for (let i = 0; i < cameraRows.count; ++i)
            cameraRows.itemAt(i).loadFields()
        opening = false
    }
    onAboutToHide: {
        persist.stop()
        savePending()
    }

    component SettingsField: TextField {
        id: field
        font.pixelSize: 13
        leftPadding: 8
        rightPadding: 8
        selectionColor: Theme.accent
        selectedTextColor: Theme.ink
        background: Rectangle {
            implicitHeight: 32
            color: Theme.bg0
            border.width: 1
            border.color: field.activeFocus ? Theme.accent : Theme.line
            radius: Theme.radius
        }
    }

    Timer { id: persist; interval: 500; onTriggered: root.savePending() }
    background: Rectangle { color: Theme.bg1; border.color: Theme.line }

    header: Rectangle {
        height: 48
        color: Theme.bg1
        Label {
            x: 16
            anchors.verticalCenter: parent.verticalCenter
            text: "设置"
            font.pixelSize: 16
            font.weight: Font.DemiBold
            color: Theme.text
        }
        DarkButton {
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: "×"
            Accessible.name: "关闭设置"
            onClicked: root.close()
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
    }

    contentItem: RowLayout {
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 142
            Layout.fillHeight: true
            color: Theme.bg0
            ListView {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 3
                model: root.pageNames
                currentIndex: root.page
                delegate: DarkButton {
                    required property int index
                    required property string modelData
                    width: ListView.view.width
                    height: 36
                    labelAlignment: Text.AlignLeft
                    padX: 13
                    text: modelData
                    active: root.page === index
                    onClicked: root.page = index
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 20
            currentIndex: root.page

            ScrollView {
                id: cameraPage
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: cameraPage.availableWidth
                    spacing: 10
                    RowLayout {
                        Label { text: "机位"; color: Theme.textDim; Layout.preferredWidth: 128 }
                        Label { text: "设备 ID"; color: Theme.textDim; Layout.preferredWidth: 116 }
                        Label { text: "录像目录"; color: Theme.textDim; Layout.fillWidth: true }
                        Label { text: "偏移 (ms)"; color: Theme.textDim; Layout.preferredWidth: 118 }
                        Item { Layout.preferredWidth: 48 }
                    }
                    Repeater {
                        id: cameraRows
                        model: player.streamCount
                        delegate: RowLayout {
                            id: cameraRow
                            required property int index
                            Layout.fillWidth: true
                            readonly property var stream: player.streamConfigs[index] ?? ({})
                            readonly property bool separatePath: index === 0 || stream.cameraId !== player.streamConfigs[index - 1]?.cameraId
                            property alias displayName: streamName.text
                            property alias offset: offsetField.text
                            property alias directory: pathField.text
                            function loadFields() {
                                streamName.text = stream.displayName
                                pathField.text = stream.rootPath
                                offsetField.text = String(stream.offsetMs)
                            }
                            Connections {
                                target: player
                                function onSettingsChanged() {
                                    if (cameraRow.index >= player.streamCount) return
                                    if (!offsetField.activeFocus && !root.dirty.offsets)
                                        offsetField.text = String(cameraRow.stream.offsetMs)
                                }
                            }
                            SettingsField {
                                id: streamName
                                Layout.preferredWidth: 128
                                Accessible.name: "通道 " + (cameraRow.index + 1) + " 名称"
                                onTextEdited: root.queue("names")
                            }
                            Label {
                                text: cameraRow.stream.streamKey
                                color: Theme.textDim
                                font.family: Theme.fontMono
                                font.pixelSize: 11
                                Layout.preferredWidth: 116
                                elide: Text.ElideRight
                            }
                            SettingsField {
                                id: pathField
                                Layout.fillWidth: true
                                readOnly: !cameraRow.separatePath
                                text: cameraRow.stream.rootPath
                                color: readOnly ? Theme.textDim : Theme.text
                                Accessible.name: "通道 " + (cameraRow.index + 1) + " 录像目录"
                            }
                            RowLayout {
                                Layout.preferredWidth: 118
                                spacing: 2
                                DarkButton {
                                    text: "−"
                                    Accessible.name: "通道 " + (cameraRow.index + 1) + " 偏移减 1 秒"
                                    onClicked: {
                                        offsetField.text = String((Number(offsetField.text) || 0) - 1000)
                                        root.queue("offsets")
                                    }
                                }
                                SettingsField {
                                    id: offsetField
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 50
                                    validator: IntValidator {}
                                    Accessible.name: "通道 " + (cameraRow.index + 1) + " 偏移毫秒"
                                    onTextEdited: if (acceptableInput) root.queue("offsets")
                                }
                                DarkButton {
                                    text: "+"
                                    Accessible.name: "通道 " + (cameraRow.index + 1) + " 偏移加 1 秒"
                                    onClicked: {
                                        offsetField.text = String((Number(offsetField.text) || 0) + 1000)
                                        root.queue("offsets")
                                    }
                                }
                            }
                            DarkButton {
                                Layout.preferredWidth: 48
                                visible: cameraRow.separatePath
                                text: "移除"
                                danger: true
                                Accessible.name: "移除 " + cameraRow.stream.displayName
                                onClicked: {
                                    root.savePending()
                                    removeCameraDialog.cameraId = cameraRow.stream.cameraId
                                    removeCameraDialog.cameraName = cameraRow.stream.displayName
                                    removeCameraDialog.open()
                                }
                            }
                            Item { Layout.preferredWidth: 48; visible: !cameraRow.separatePath }
                        }
                    }
                    RowLayout {
                        Layout.topMargin: 10
                        PrimaryButton { text: "添加摄像机"; onClicked: root.openAddCamera() }
                        DarkButton {
                            text: "应用目录并重新扫描"
                            enabled: !player.scanning && player.streamCount > 0
                            onClicked: {
                                root.savePending()
                                const dirs = []
                                for (let i = 0; i < cameraRows.count; ++i) {
                                    const row = cameraRows.itemAt(i)
                                    if (row.separatePath) dirs.push(row.directory)
                                }
                                player.updateCameraDirectories(dirs, true)
                                for (let i = 0; i < cameraRows.count; ++i)
                                    cameraRows.itemAt(i).loadFields()
                            }
                        }
                        DarkButton {
                            text: "当前帧 OCR 校准"
                            enabled: player.streamCount > 0
                            onClicked: player.runAutoOcrCalibration(true)
                        }
                    }
                }
            }

            Item {
                ColumnLayout {
                    width: parent.width
                    spacing: 10
                    DarkCheckBox { id: hardware; text: "启用 GPU 硬件解码"; onClicked: root.queue("cache") }
                    DarkCheckBox { id: audioFollow; text: "放大画面时自动切换音频"; onClicked: root.queue("audio") }
                    RowLayout {
                        Label { text: "SSD 缓存上限 (GB)"; color: Theme.text }
                        SettingsField {
                            id: cacheSize
                            Layout.preferredWidth: 70
                            Accessible.name: "缓存上限 GB"
                            validator: IntValidator { bottom: 1; top: 500 }
                            onTextEdited: if (acceptableInput) root.queue("cache")
                        }
                    }
                    RowLayout {
                        Label { text: "缓存目录"; color: Theme.text }
                        SettingsField {
                            id: cachePath
                            Layout.fillWidth: true
                            Accessible.name: "缓存目录"
                            onEditingFinished: root.queue("cache")
                        }
                        DarkButton { text: "浏览…"; onClicked: cacheFolderDialog.open() }
                    }
                    DarkButton { text: "清空缓存"; danger: true; onClicked: clearConfirmation.open() }
                }
            }

            Item {
                ColumnLayout {
                    width: parent.width
                    spacing: 10
                    DarkCheckBox { id: autoCheck; text: "启用定时健康检测"; onClicked: root.queue("health") }
                    RowLayout {
                        Label { text: "检测间隔 (分钟)"; color: Theme.text }
                        SettingsField {
                            id: interval
                            Layout.preferredWidth: 70
                            Accessible.name: "健康检测间隔"
                            validator: IntValidator { bottom: 1; top: 1440 }
                            onTextEdited: if (acceptableInput) root.queue("health")
                        }
                    }
                    DarkCheckBox { id: notifyOffline; text: "离线时发送通知"; onClicked: root.queue("health") }
                    DarkCheckBox { id: notifyRecovered; text: "恢复正常时发送通知"; onClicked: root.queue("health") }
                    DarkCheckBox { text: "显示详细同步诊断"; checked: player.developerDiagnostics; onClicked: player.developerDiagnostics = checked }
                }
            }

            Item {
                ColumnLayout {
                    width: parent.width
                    spacing: 12
                    DarkCheckBox { id: locationEnabled; text: "按所在地计算日出、日落"; onClicked: root.queue("location") }
                    ComboBox {
                        id: preset
                        Layout.preferredWidth: 180
                        model: player.presetCities
                        textRole: "name"
                        currentIndex: -1
                        displayText: currentIndex < 0 ? "选择城市" : currentText
                        onActivated: (index) => {
                            const place = player.presetCities[index]
                            city.text = place.name
                            latitude.text = String(place.lat)
                            longitude.text = String(place.lon)
                            timezone.text = String(place.tzHours)
                            locationEnabled.checked = true
                            root.queue("location")
                        }
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 10
                        rowSpacing: 6
                        Label { text: "城市"; color: Theme.text }
                        SettingsField { id: city; Accessible.name: "城市"; onTextEdited: root.queue("location") }
                        Label { text: "纬度"; color: Theme.text }
                        SettingsField { id: latitude; Accessible.name: "纬度"; validator: DoubleValidator { bottom: -89.9; top: 89.9; locale: "C" } onTextEdited: if (acceptableInput) root.queue("location") }
                        Label { text: "经度"; color: Theme.text }
                        SettingsField { id: longitude; Accessible.name: "经度"; validator: DoubleValidator { bottom: -180; top: 180; locale: "C" } onTextEdited: if (acceptableInput) root.queue("location") }
                        Label { text: "UTC 时差"; color: Theme.text }
                        SettingsField { id: timezone; Accessible.name: "UTC 时差"; validator: IntValidator { bottom: -12; top: 14 } onTextEdited: if (acceptableInput) root.queue("location") }
                    }
                    RowLayout {
                        spacing: 18
                        DarkColorPicker { id: dayColor; label: "白天"; onColorEdited: root.queue("colors") }
                        DarkColorPicker { id: nightColor; label: "夜间"; onColorEdited: root.queue("colors") }
                        DarkButton {
                            text: "恢复默认颜色"
                            onClicked: {
                                dayColor.colorValue = String(Theme.track)
                                nightColor.colorValue = String(Theme.night)
                                root.queue("colors")
                            }
                        }
                    }
                }
            }

            Item {
                GridLayout {
                    columns: 2
                    rowSpacing: 8
                    columnSpacing: 16
                    Label { text: "播放 / 暂停"; color: Theme.text }
                    KeyRecorder { id: playKey; Accessible.name: "播放暂停快捷键"; onRecorded: root.queue("shortcuts") }
                    Label { text: "添加标记"; color: Theme.text }
                    KeyRecorder { id: markKey; Accessible.name: "添加标记快捷键"; onRecorded: root.queue("shortcuts") }
                    Label { text: "后退"; color: Theme.text }
                    KeyRecorder { id: backKey; Accessible.name: "后退快捷键"; onRecorded: root.queue("shortcuts") }
                    Label { text: "前进"; color: Theme.text }
                    KeyRecorder { id: forwardKey; Accessible.name: "前进快捷键"; onRecorded: root.queue("shortcuts") }
                    Label { text: "还原焦点"; color: Theme.text }
                    KeyRecorder { id: exitKey; Accessible.name: "还原焦点快捷键"; onRecorded: root.queue("shortcuts") }
                    Label { Layout.columnSpan: 2; text: root.shortcutWarning; visible: text !== ""; color: Theme.error }
                }
            }
        }
    }

    Native.FolderDialog {
        id: recordingFolderDialog
        title: "选择录像目录"
        onAccepted: {
            newPath.text = player.localFolderPath(selectedFolder)
            if (newName.text.trim() === "")
                newName.text = newPath.text.split(/[\\/]/).filter(part => part.length).pop() || ""
            dualCamera.lastCheckedPath = newPath.text
            dualCamera.checked = player.detectDualCameraDir(newPath.text)
        }
    }

    Native.FolderDialog {
        id: cacheFolderDialog
        title: "选择缓存目录"
        onAccepted: {
            cachePath.text = player.localFolderPath(selectedFolder)
            root.queue("cache")
        }
    }

    Dialog {
        id: addCameraDialog
        objectName: "addCameraDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(540, root.width - 30)
        modal: true
        title: "添加摄像机"
        background: Rectangle { color: Theme.bg1; border.color: Theme.line }
        contentItem: ColumnLayout {
            spacing: 8
            Label { text: "摄像机名称"; color: Theme.text }
            SettingsField { id: newName; objectName: "newCameraName"; Layout.fillWidth: true; Accessible.name: "摄像机名称" }
            Label { text: "录像目录"; color: Theme.text }
            RowLayout {
                SettingsField {
                    id: newPath
                    objectName: "newCameraPath"
                    Layout.fillWidth: true
                    Accessible.name: "录像目录"
                    onEditingFinished: {
                        if (text !== dualCamera.lastCheckedPath) {
                            dualCamera.lastCheckedPath = text
                            dualCamera.checked = player.detectDualCameraDir(text)
                        }
                    }
                }
                DarkButton { text: "浏览…"; onClicked: recordingFolderDialog.open() }
            }
            DarkCheckBox {
                id: dualCamera
                objectName: "newCameraDual"
                property string lastCheckedPath: ""
                text: "双摄（00 / 10）"
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                DarkButton { text: "取消"; onClicked: addCameraDialog.close() }
                PrimaryButton {
                    objectName: "saveNewCamera"
                    text: "添加"
                    enabled: newPath.text.trim() !== ""
                    onClicked: {
                        root.savePending()
                        if (player.addCamera(newName.text, newPath.text, dualCamera.checked)) {
                            addCameraDialog.close()
                            for (let i = 0; i < cameraRows.count; ++i)
                                cameraRows.itemAt(i).loadFields()
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: removeCameraDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: 350
        property string cameraId
        property string cameraName
        title: "移除摄像机？"
        background: Rectangle { color: Theme.bg1; border.color: Theme.line }
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.text
                text: "将「" + removeCameraDialog.cameraName + "」从回放布局移除。原始录像会保留。"
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                DarkButton { text: "取消"; onClicked: removeCameraDialog.close() }
                DarkButton {
                    text: "移除"
                    danger: true
                    onClicked: {
                        player.removeCamera(removeCameraDialog.cameraId)
                        removeCameraDialog.close()
                        for (let i = 0; i < cameraRows.count; ++i)
                            cameraRows.itemAt(i).loadFields()
                    }
                }
            }
        }
    }

    Dialog {
        id: clearConfirmation
        width: 310
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "清空缓存？"
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label { text: "再次播放时会重新读取录像。"; color: Theme.text; padding: 14 }
        onAccepted: player.clearAllCaches()
    }
}