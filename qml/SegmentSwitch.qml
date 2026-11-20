import QtQuick

Row {
    DarkButton {
        visible: player.groupedLayoutAvailable
        text: "5 组"
        active: player.focusedStream === -1 && player.layoutMode === "grouped"
        onClicked: {
            player.focusedStream = -1
            player.layoutMode = "grouped"
        }
    }
    DarkButton {
        visible: player.streamCount > 1
        text: player.streamCount === 6 ? "6 宫格" : "宫格"
        active: player.focusedStream === -1 && (player.layoutMode === "grid6" || !player.groupedLayoutAvailable)
        onClicked: {
            player.focusedStream = -1
            player.layoutMode = "grid6"
        }
    }
    DarkButton {
        text: "还原"
        visible: player.focusedStream !== -1
        onClicked: player.focusedStream = -1
    }
}