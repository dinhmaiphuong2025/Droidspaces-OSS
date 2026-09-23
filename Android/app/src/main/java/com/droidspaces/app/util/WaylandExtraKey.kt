package com.droidspaces.app.util

// One key on the Wayland extra keys bar. Codes are Linux evdev scancodes.
//
// Types mirror Anland's ExtraKeysBar/HudAction: key sends down+up, modifier
// latches on tap and locks on long-press, combo presses several codes at once,
// text injects a UTF-8 string, system fires an app command.
data class WaylandExtraKey(
    val label: String,
    val code: Int = 0,
    val sticky: Boolean = false,
    val type: String = TYPE_KEY,
    val repeat: Boolean = false,
    val comboKeys: List<Int> = emptyList(),
    val text: String = "",
    val systemCommand: String = ""
) {
    companion object {
        const val TYPE_KEY = "key"
        const val TYPE_MODIFIER = "modifier"
        const val TYPE_COMBO = "combo"
        const val TYPE_TEXT = "text"
        const val TYPE_SYSTEM = "system"
    }
}

fun defaultWaylandExtraKeys(): List<List<WaylandExtraKey>> = listOf(
    listOf(
        WaylandExtraKey("Esc", 1),
        WaylandExtraKey("Tab", 15),
        WaylandExtraKey("Ctrl", 29, type = WaylandExtraKey.TYPE_MODIFIER),
        WaylandExtraKey("Alt", 56, type = WaylandExtraKey.TYPE_MODIFIER),
        WaylandExtraKey("Super", 125, type = WaylandExtraKey.TYPE_MODIFIER),
        WaylandExtraKey("⌨", type = WaylandExtraKey.TYPE_SYSTEM, systemCommand = "toggle_ime")
    ),
    listOf(
        WaylandExtraKey("←", 105, repeat = true),
        WaylandExtraKey("↑", 103, repeat = true),
        WaylandExtraKey("↓", 108, repeat = true),
        WaylandExtraKey("→", 106, repeat = true),
        WaylandExtraKey("Enter", 28),
        WaylandExtraKey("Bksp", 14, repeat = true)
    )
)

// Flat list of presets for the key picker grid, grouped by category.
val waylandExtraKeyPresets: List<WaylandExtraKey> = listOf(
    // Modifiers
    WaylandExtraKey("Ctrl", 29, type = WaylandExtraKey.TYPE_MODIFIER),
    WaylandExtraKey("Shift", 42, type = WaylandExtraKey.TYPE_MODIFIER),
    WaylandExtraKey("Alt", 56, type = WaylandExtraKey.TYPE_MODIFIER),
    WaylandExtraKey("Super", 125, type = WaylandExtraKey.TYPE_MODIFIER),
    // Editing
    WaylandExtraKey("Esc", 1),
    WaylandExtraKey("Tab", 15),
    WaylandExtraKey("Enter", 28),
    WaylandExtraKey("Bksp", 14, repeat = true),
    WaylandExtraKey("Del", 111, repeat = true),
    WaylandExtraKey("Space", 57),
    // Navigation
    WaylandExtraKey("↑", 103, repeat = true),
    WaylandExtraKey("↓", 108, repeat = true),
    WaylandExtraKey("←", 105, repeat = true),
    WaylandExtraKey("→", 106, repeat = true),
    WaylandExtraKey("Home", 102, repeat = true),
    WaylandExtraKey("End", 107, repeat = true),
    WaylandExtraKey("PgUp", 104, repeat = true),
    WaylandExtraKey("PgDn", 109, repeat = true),
    // Text shortcuts
    WaylandExtraKey("/", type = WaylandExtraKey.TYPE_TEXT, text = "/"),
    WaylandExtraKey("-", type = WaylandExtraKey.TYPE_TEXT, text = "-"),
    WaylandExtraKey("|", type = WaylandExtraKey.TYPE_TEXT, text = "|"),
    WaylandExtraKey("~", type = WaylandExtraKey.TYPE_TEXT, text = "~"),
    // F-keys
    WaylandExtraKey("F1", 59), WaylandExtraKey("F2", 60),
    WaylandExtraKey("F3", 61), WaylandExtraKey("F4", 62),
    WaylandExtraKey("F5", 63), WaylandExtraKey("F6", 64),
    WaylandExtraKey("F7", 65), WaylandExtraKey("F8", 66),
    WaylandExtraKey("F9", 67), WaylandExtraKey("F10", 68),
    WaylandExtraKey("F11", 87), WaylandExtraKey("F12", 88),
    // System
    WaylandExtraKey("⌨", type = WaylandExtraKey.TYPE_SYSTEM, systemCommand = "toggle_ime"),
    WaylandExtraKey("⚙", type = WaylandExtraKey.TYPE_SYSTEM, systemCommand = "open_settings")
)

// Evdev label for display in the editor
fun evdevLabel(code: Int): String = when (code) {
    1 -> "ESC"; 14 -> "BKSP"; 15 -> "TAB"; 28 -> "ENTER"; 29 -> "CTRL"
    42 -> "SHIFT"; 56 -> "ALT"; 57 -> "SPACE"; 59 -> "F1"; 60 -> "F2"
    61 -> "F3"; 62 -> "F4"; 63 -> "F5"; 64 -> "F6"; 65 -> "F7"
    66 -> "F8"; 67 -> "F9"; 68 -> "F10"; 87 -> "F11"; 88 -> "F12"
    102 -> "HOME"; 103 -> "UP"; 104 -> "PGUP"; 105 -> "LEFT"
    106 -> "RIGHT"; 107 -> "END"; 108 -> "DOWN"; 109 -> "PGDN"
    110 -> "INS"; 111 -> "DEL"; 125 -> "SUPER"; 139 -> "MENU"
    else -> if (code in 2..11) "${(code + 47) % 10}" // 0-9
           else if (code in 16..25 || code in 30..38 || code in 44..50)
               "Key $code"
           else "Key $code"
}
