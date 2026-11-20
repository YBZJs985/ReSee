pragma Singleton
import QtQuick

QtObject {
    readonly property color bg0: "#0E0E0E"
    readonly property color bg1: "#161616"
    readonly property color bg2: "#202020"
    readonly property color raised: "#303030"
    readonly property color line: "#2A2A2A"
    readonly property color text: "#E6E6E6"
    readonly property color textDim: "#A1A1A1"
    readonly property color accent: "#FF6A1A"
    readonly property color warn: "#F5A524"
    readonly property color error: "#E5484D"
    readonly property color ok: "#65A684"
    readonly property color night: "#806EA5"
    readonly property color track: "#5A5A5A"
    readonly property color ink: "#101010"
    readonly property color black: "#080808"
    readonly property var markerColors: [String(accent), String(ok), String(warn), String(error), String(night)]
    readonly property var hueStops: ["#FF0000", "#FFFF00", "#00FF00", "#00FFFF", "#0000FF", "#FF00FF", "#FF0000"]
    readonly property var timelineSwatches: ["#3485EF", "#3B82F6", "#0EA5E9", "#06B6D4", "#10B981", "#22C55E",
                                              "#7975C9", "#6366F1", "#8B5CF6", "#A855F7", "#F59E0B", "#EF4444"]
    readonly property color hud: Qt.alpha(black, 0.78)
    readonly property int radius: 3
    readonly property string fontUi: Qt.fontFamilies().includes("HarmonyOS Sans SC") ? "HarmonyOS Sans SC" : Qt.application.font.family
    readonly property string fontMono: Qt.fontFamilies().includes("JetBrains Mono") ? "JetBrains Mono" : "Consolas"
}