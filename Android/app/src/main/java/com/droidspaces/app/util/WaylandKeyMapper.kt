package com.droidspaces.app.util

import android.view.KeyEvent

// Android KeyEvent.KEYCODE_* to Linux evdev scancode. Gboard and hardware
// keyboards both deliver Android keycodes; this table converts them before
// they reach the Wayland server. Only keycodes that have a sensible evdev
// mapping are included. Returns -1 for unmapped codes.
object WaylandKeyMapper {
    private val map = android.util.SparseIntArray().apply {
        // Letters
        put(KeyEvent.KEYCODE_A, 30); put(KeyEvent.KEYCODE_B, 48)
        put(KeyEvent.KEYCODE_C, 46); put(KeyEvent.KEYCODE_D, 32)
        put(KeyEvent.KEYCODE_E, 18); put(KeyEvent.KEYCODE_F, 33)
        put(KeyEvent.KEYCODE_G, 34); put(KeyEvent.KEYCODE_H, 35)
        put(KeyEvent.KEYCODE_I, 23); put(KeyEvent.KEYCODE_J, 36)
        put(KeyEvent.KEYCODE_K, 37); put(KeyEvent.KEYCODE_L, 38)
        put(KeyEvent.KEYCODE_M, 50); put(KeyEvent.KEYCODE_N, 49)
        put(KeyEvent.KEYCODE_O, 24); put(KeyEvent.KEYCODE_P, 25)
        put(KeyEvent.KEYCODE_Q, 16); put(KeyEvent.KEYCODE_R, 19)
        put(KeyEvent.KEYCODE_S, 31); put(KeyEvent.KEYCODE_T, 20)
        put(KeyEvent.KEYCODE_U, 22); put(KeyEvent.KEYCODE_V, 47)
        put(KeyEvent.KEYCODE_W, 17); put(KeyEvent.KEYCODE_X, 45)
        put(KeyEvent.KEYCODE_Y, 21); put(KeyEvent.KEYCODE_Z, 44)

        // Numbers
        put(KeyEvent.KEYCODE_0, 11); put(KeyEvent.KEYCODE_1, 2)
        put(KeyEvent.KEYCODE_2, 3); put(KeyEvent.KEYCODE_3, 4)
        put(KeyEvent.KEYCODE_4, 5); put(KeyEvent.KEYCODE_5, 6)
        put(KeyEvent.KEYCODE_6, 7); put(KeyEvent.KEYCODE_7, 8)
        put(KeyEvent.KEYCODE_8, 9); put(KeyEvent.KEYCODE_9, 10)

        // Symbols
        put(KeyEvent.KEYCODE_MINUS, 12); put(KeyEvent.KEYCODE_EQUALS, 13)
        put(KeyEvent.KEYCODE_LEFT_BRACKET, 26); put(KeyEvent.KEYCODE_RIGHT_BRACKET, 27)
        put(KeyEvent.KEYCODE_BACKSLASH, 43); put(KeyEvent.KEYCODE_SEMICOLON, 39)
        put(KeyEvent.KEYCODE_APOSTROPHE, 40); put(KeyEvent.KEYCODE_COMMA, 51)
        put(KeyEvent.KEYCODE_PERIOD, 52); put(KeyEvent.KEYCODE_SLASH, 53)
        put(KeyEvent.KEYCODE_GRAVE, 41)

        // Editing and function
        put(KeyEvent.KEYCODE_SPACE, 57); put(KeyEvent.KEYCODE_ENTER, 28)
        put(KeyEvent.KEYCODE_DEL, 14); put(KeyEvent.KEYCODE_FORWARD_DEL, 111)
        put(KeyEvent.KEYCODE_TAB, 15); put(KeyEvent.KEYCODE_ESCAPE, 1)
        put(KeyEvent.KEYCODE_CAPS_LOCK, 58)

        // Modifiers
        put(KeyEvent.KEYCODE_SHIFT_LEFT, 42); put(KeyEvent.KEYCODE_SHIFT_RIGHT, 54)
        put(KeyEvent.KEYCODE_CTRL_LEFT, 29); put(KeyEvent.KEYCODE_CTRL_RIGHT, 97)
        put(KeyEvent.KEYCODE_ALT_LEFT, 56); put(KeyEvent.KEYCODE_ALT_RIGHT, 100)
        put(KeyEvent.KEYCODE_META_LEFT, 125); put(KeyEvent.KEYCODE_META_RIGHT, 126)

        // Arrows and navigation
        put(KeyEvent.KEYCODE_DPAD_UP, 103); put(KeyEvent.KEYCODE_DPAD_DOWN, 108)
        put(KeyEvent.KEYCODE_DPAD_LEFT, 105); put(KeyEvent.KEYCODE_DPAD_RIGHT, 106)
        put(KeyEvent.KEYCODE_MOVE_HOME, 102); put(KeyEvent.KEYCODE_MOVE_END, 107)
        put(KeyEvent.KEYCODE_PAGE_UP, 104); put(KeyEvent.KEYCODE_PAGE_DOWN, 109)
        put(KeyEvent.KEYCODE_INSERT, 110)

        // F-keys
        put(KeyEvent.KEYCODE_F1, 59); put(KeyEvent.KEYCODE_F2, 60)
        put(KeyEvent.KEYCODE_F3, 61); put(KeyEvent.KEYCODE_F4, 62)
        put(KeyEvent.KEYCODE_F5, 63); put(KeyEvent.KEYCODE_F6, 64)
        put(KeyEvent.KEYCODE_F7, 65); put(KeyEvent.KEYCODE_F8, 66)
        put(KeyEvent.KEYCODE_F9, 67); put(KeyEvent.KEYCODE_F10, 68)
        put(KeyEvent.KEYCODE_F11, 87); put(KeyEvent.KEYCODE_F12, 88)

        // Numpad enter
        put(KeyEvent.KEYCODE_NUMPAD_ENTER, 28)
    }

    // ASCII character to the evdev key tap(s) that produce it on a US layout.
    // Returns a pair: (evdev scancode, needsShift). Returns null for
    // characters outside the US keyboard.
    private val asciiMap = HashMap<Char, Pair<Int, Boolean>>().apply {
        for (c in 'a'..'z') put(c, Pair(map.get(KeyEvent.KEYCODE_A + (c - 'a'), -1), false))
        for (c in 'A'..'Z') put(c, Pair(map.get(KeyEvent.KEYCODE_A + (c - 'A'), -1), true))
        for (c in '0'..'9') put(c, Pair(map.get(KeyEvent.KEYCODE_0 + (c - '0'), -1), false))
        // Shifted number row
        put('!', Pair(2, true));  put('@', Pair(3, true))
        put('#', Pair(4, true));  put('$', Pair(5, true))
        put('%', Pair(6, true));  put('^', Pair(7, true))
        put('&', Pair(8, true));  put('*', Pair(9, true))
        put('(', Pair(10, true)); put(')', Pair(11, true))
        // Symbols unshifted
        put('-', Pair(12, false)); put('=', Pair(13, false))
        put('[', Pair(26, false)); put(']', Pair(27, false))
        put('\\', Pair(43, false)); put(';', Pair(39, false))
        put('\'', Pair(40, false)); put(',', Pair(51, false))
        put('.', Pair(52, false)); put('/', Pair(53, false))
        put('`', Pair(41, false)); put(' ', Pair(57, false))
        // Symbols shifted
        put('_', Pair(12, true));  put('+', Pair(13, true))
        put('{', Pair(26, true));  put('}', Pair(27, true))
        put('|', Pair(43, true));  put(':', Pair(39, true))
        put('"', Pair(40, true));  put('<', Pair(51, true))
        put('>', Pair(52, true));  put('?', Pair(53, true))
        put('~', Pair(41, true))
        // Whitespace
        put('\t', Pair(15, false)); put('\n', Pair(28, false))
    }

    fun toEvdev(androidKeyCode: Int): Int = map.get(androidKeyCode, -1)

    fun asciiToEvdev(c: Char): Pair<Int, Boolean>? = asciiMap[c]
}
