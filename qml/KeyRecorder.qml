import QtQuick
import QtQuick.Controls

TextField {
    id: root
    readOnly: true
    selectByMouse: false
    implicitWidth: 126
    font.family: Theme.fontMono
    Accessible.description: "聚焦后按下新的快捷键"
    signal recorded()
    Keys.priority: Keys.BeforeItem
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
            return
        const modifiers = [Qt.Key_Control, Qt.Key_Shift, Qt.Key_Alt, Qt.Key_Meta]
        if (modifiers.includes(event.key) || event.isAutoRepeat) {
            event.accepted = true
            return
        }
        const key = event.key
        const named = { [Qt.Key_Space]: "Space", [Qt.Key_Escape]: "Esc",
            [Qt.Key_Left]: "Left", [Qt.Key_Right]: "Right", [Qt.Key_Up]: "Up", [Qt.Key_Down]: "Down",
            [Qt.Key_Return]: "Return", [Qt.Key_Enter]: "Enter", [Qt.Key_Delete]: "Del",
            [Qt.Key_Backspace]: "Backspace", [Qt.Key_Home]: "Home", [Qt.Key_End]: "End",
            [Qt.Key_PageUp]: "PgUp", [Qt.Key_PageDown]: "PgDown" }
        let name = named[key]
        if (!name && key >= Qt.Key_F1 && key <= Qt.Key_F35) name = "F" + (key - Qt.Key_F1 + 1)
        if (!name && key >= 33 && key <= 126) name = String.fromCharCode(key)
        if (!name) return
        const parts = []
        if (event.modifiers & Qt.ControlModifier) parts.push("Ctrl")
        if (event.modifiers & Qt.AltModifier) parts.push("Alt")
        if (event.modifiers & Qt.ShiftModifier) parts.push("Shift")
        if (event.modifiers & Qt.MetaModifier) parts.push("Meta")
        parts.push(name)
        root.text = parts.join("+")
        root.recorded()
        event.accepted = true
    }
}