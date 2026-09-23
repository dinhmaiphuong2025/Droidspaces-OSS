package com.droidspaces.app.util

// One key on the Wayland extra keys bar. Codes are Linux evdev numbers.
// Sticky keys latch down on first tap and release on the second, like the
// Termux extra keys row, so Ctrl/Alt combos work without multi-touch.
data class WaylandExtraKey(
    val label: String,
    val code: Int,
    val sticky: Boolean
)

fun defaultWaylandExtraKeys(): List<WaylandExtraKey> = listOf(
    WaylandExtraKey("Esc", 1, false),
    WaylandExtraKey("Tab", 15, false),
    WaylandExtraKey("Ctrl", 29, true),
    WaylandExtraKey("Alt", 56, true),
    WaylandExtraKey("Left", 105, false),
    WaylandExtraKey("Up", 103, false),
    WaylandExtraKey("Down", 108, false),
    WaylandExtraKey("Right", 106, false),
    WaylandExtraKey("Enter", 28, false),
    WaylandExtraKey("Bksp", 14, false)
)

// Preset picker for the customizer dialog. Codes are Linux evdev numbers.
val waylandExtraKeyPresets: List<WaylandExtraKey> = listOf(
    WaylandExtraKey("Esc", 1, false),
    WaylandExtraKey("Tab", 15, false),
    WaylandExtraKey("Enter", 28, false),
    WaylandExtraKey("Bksp", 14, false),
    WaylandExtraKey("Del", 111, false),
    WaylandExtraKey("Ins", 110, false),
    WaylandExtraKey("Home", 102, false),
    WaylandExtraKey("End", 107, false),
    WaylandExtraKey("PgUp", 104, false),
    WaylandExtraKey("PgDn", 109, false),
    WaylandExtraKey("Up", 103, false),
    WaylandExtraKey("Down", 108, false),
    WaylandExtraKey("Left", 105, false),
    WaylandExtraKey("Right", 106, false),
    WaylandExtraKey("Space", 57, false),
    WaylandExtraKey("Ctrl", 29, true),
    WaylandExtraKey("Shift", 42, true),
    WaylandExtraKey("Alt", 56, true),
    WaylandExtraKey("Super", 125, true),
    WaylandExtraKey("Menu", 139, false),
    WaylandExtraKey("F1", 59, false),
    WaylandExtraKey("F2", 60, false),
    WaylandExtraKey("F3", 61, false),
    WaylandExtraKey("F4", 62, false),
    WaylandExtraKey("F5", 63, false),
    WaylandExtraKey("F6", 64, false),
    WaylandExtraKey("F7", 65, false),
    WaylandExtraKey("F8", 66, false),
    WaylandExtraKey("F9", 67, false),
    WaylandExtraKey("F10", 68, false),
    WaylandExtraKey("F11", 87, false),
    WaylandExtraKey("F12", 88, false)
)
