package com.droidspaces.app.ui.screen

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.os.Build
import android.os.SystemClock
import android.text.InputType
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewConfiguration
import android.view.WindowManager
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.isImeVisible
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Keyboard
import androidx.compose.material.icons.filled.Mouse
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material.icons.filled.TouchApp
import androidx.compose.material.icons.filled.Widgets
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import kotlinx.coroutines.delay
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.droidspaces.app.ui.wayland.WaylandNative
import com.droidspaces.app.util.ValidationUtils
import com.droidspaces.app.util.WaylandExtraKey
import com.droidspaces.app.util.WaylandKeyMapper
import kotlin.math.abs
import kotlin.math.hypot

enum class InputMode {
    TOUCHPAD,
    DIRECT_TOUCH
}

private fun isModifierEvdev(code: Int): Boolean =
    code in setOf(29, 97, 56, 100, 42, 54, 125, 126)

private class WaylandSurfaceView(context: Context) : SurfaceView(context) {
    var onKeyInput: ((Int, Int) -> Unit)? = null
    var onTextInput: ((String) -> Unit)? = null

    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_ACTION_NONE

        return object : BaseInputConnection(this, true) {
            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                android.util.Log.d("DsSeat", "commitText: $text")
                if (!text.isNullOrEmpty()) {
                    onTextInput?.invoke(text.toString())
                }
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                android.util.Log.d("DsSeat", "deleteSurroundingText: $beforeLength, $afterLength")
                repeat(beforeLength) {
                    onKeyInput?.invoke(KeyEvent.KEYCODE_DEL, 1)
                    onKeyInput?.invoke(KeyEvent.KEYCODE_DEL, 0)
                }
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                val action = if (event.action == KeyEvent.ACTION_UP) 0 else 1
                android.util.Log.d("DsSeat", "sendKeyEvent: keyCode=${event.keyCode}, action=$action")
                onKeyInput?.invoke(event.keyCode, action)
                return true
            }
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        android.util.Log.d("DsSeat", "onKeyDown: $keyCode")
        onKeyInput?.invoke(keyCode, 1)
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        android.util.Log.d("DsSeat", "onKeyUp: $keyCode")
        onKeyInput?.invoke(keyCode, 0)
        return true
    }
}

@OptIn(ExperimentalLayoutApi::class)
@SuppressLint("ClickableViewAccessibility")
@Composable
fun WaylandDisplayScreen(
    containerName: String,
    onNavigateBack: () -> Unit,
    onNavigateToTerminal: (String) -> Unit
) {
    val context = LocalContext.current
    var showControls by remember { mutableStateOf(false) }
    var controlsAutoShown by remember { mutableStateOf(false) }
    var inputMode by remember { mutableStateOf(InputMode.TOUCHPAD) }
    val isKeyboardVisible = WindowInsets.isImeVisible

    // Watch the compositor connection. With no client on the display there
    // is nothing to show, so surface the header (with its back button)
    // instead of leaving the user trapped on a black immersive screen.
    LaunchedEffect(Unit) {
        var wasConnected = true
        while (true) {
            delay(2000)
            val connected = try {
                WaylandNative.nativeGetClientCount() > 0
            } catch (_: Throwable) {
                false
            }
            if (!connected && wasConnected) {
                showControls = true
                controlsAutoShown = true
            } else if (connected && controlsAutoShown) {
                showControls = false
                controlsAutoShown = false
            }
            wasConnected = connected
        }
    }

    val prefs = remember(context) {
        com.droidspaces.app.util.PreferencesManager.getInstance(context)
    }
    var extraKeyRows by remember { mutableStateOf(prefs.getWaylandExtraKeys()) }
    var modifiers by remember { mutableStateOf(mapOf<Int, ModifierState>()) }
    var showExtraEditor by remember { mutableStateOf(false) }
    var surfaceViewRef by remember { mutableStateOf<WaylandSurfaceView?>(null) }

    val handleKeyInput: (Int, Int) -> Unit = { androidKeyCode, action ->
        val evdev = WaylandKeyMapper.toEvdev(androidKeyCode)
        android.util.Log.d("DsSeat", "handleKeyInput: androidKey=$androidKeyCode -> evdev=$evdev, action=$action")
        if (evdev >= 0) {
            WaylandNative.nativeSendKey(evdev, action)
            // If regular key UP, release active unlocked modifiers
            if (action == 0 && !isModifierEvdev(evdev)) {
                val unlocked = modifiers.values.filter { it.active && !it.locked }
                unlocked.reversed().forEach { WaylandNative.nativeSendKey(it.code, 0) }
                modifiers = modifiers.filterValues { it.locked }
            }
        }
    }

    val handleTextInput: (String) -> Unit = { text ->
        android.util.Log.d("DsSeat", "handleTextInput: '$text'")
        text.forEach { ch ->
            val mapped = WaylandKeyMapper.asciiToEvdev(ch)
            if (mapped != null) {
                val (scancode, needsShift) = mapped
                if (needsShift) WaylandNative.nativeSendKey(42, 1)
                WaylandNative.nativeSendKey(scancode, 1)
                WaylandNative.nativeSendKey(scancode, 0)
                if (needsShift) WaylandNative.nativeSendKey(42, 0)
            }
        }
        val unlocked = modifiers.values.filter { it.active && !it.locked }
        unlocked.reversed().forEach { WaylandNative.nativeSendKey(it.code, 0) }
        modifiers = modifiers.filterValues { it.locked }
    }

    val activity = context as? Activity
    val insetsController = remember(activity) {
        activity?.let { WindowCompat.getInsetsController(it.window, it.window.decorView) }
    }

    DisposableEffect(activity) {
        if (insetsController != null) {
            val prevBehavior = insetsController.systemBarsBehavior
            insetsController.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            insetsController.hide(WindowInsetsCompat.Type.systemBars())

            onDispose {
                insetsController.systemBarsBehavior = prevBehavior
                insetsController.show(WindowInsetsCompat.Type.systemBars())
                modifiers = releaseAllModifiers(modifiers)
                WaylandNative.nativeDestroySurface()
            }
        } else {
            onDispose {
                modifiers = releaseAllModifiers(modifiers)
                WaylandNative.nativeDestroySurface()
            }
        }
    }

    BackHandler {
        if (showControls) {
            showControls = false
            controlsAutoShown = false
        } else {
            modifiers = releaseAllModifiers(modifiers)
            onNavigateBack()
        }
    }

    val touchSlop = remember { ViewConfiguration.get(context).scaledTouchSlop.toFloat() }
    var screenW by remember { mutableStateOf(1440f) }
    var screenH by remember { mutableStateOf(3200f) }
    var cursorX by remember { mutableStateOf(720f) }
    var cursorY by remember { mutableStateOf(1600f) }
    var lastTouchX by remember { mutableStateOf(0f) }
    var lastTouchY by remember { mutableStateOf(0f) }
    var startTouchX by remember { mutableStateOf(0f) }
    var startTouchY by remember { mutableStateOf(0f) }
    var touchDownTime by remember { mutableStateOf(0L) }
    var hasMovedPastSlop by remember { mutableStateOf(false) }
    var twoFingerStartY by remember { mutableStateOf(0f) }
    var isLeftButtonHeld by remember { mutableStateOf(false) }

    // System command handler for extra keys
    val handleSystemCommand: (String) -> Unit = { cmd ->
        when (cmd) {
            "toggle_ime" -> {
                val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
                if (isKeyboardVisible) {
                    insetsController?.hide(WindowInsetsCompat.Type.ime())
                    surfaceViewRef?.let { imm?.hideSoftInputFromWindow(it.windowToken, 0) }
                } else {
                    surfaceViewRef?.requestFocus()
                    surfaceViewRef?.let { imm?.showSoftInput(it, InputMethodManager.SHOW_IMPLICIT) }
                    insetsController?.show(WindowInsetsCompat.Type.ime())
                }
            }
            "mouse_left" -> {
                WaylandNative.nativeSendPointerButton(0x110, 1)
                WaylandNative.nativeSendPointerButton(0x110, 0)
            }
            "mouse_right" -> {
                WaylandNative.nativeSendPointerButton(0x111, 1)
                WaylandNative.nativeSendPointerButton(0x111, 0)
            }
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
    ) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { ctx ->
                    WaylandSurfaceView(ctx).apply {
                        surfaceViewRef = this
                        onKeyInput = handleKeyInput
                        onTextInput = handleTextInput

                        holder.addCallback(object : SurfaceHolder.Callback {
                            override fun surfaceCreated(holder: SurfaceHolder) {}

                            override fun surfaceChanged(holder: SurfaceHolder, format: Int, w: Int, h: Int) {
                                if (w < 200 || h < 200) return
                                screenW = w.toFloat()
                                screenH = h.toFloat()

                                val windowManager = ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager
                                val refreshRate = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                                    ctx.display?.refreshRate ?: 60f
                                } else {
                                    @Suppress("DEPRECATION")
                                    windowManager.defaultDisplay.refreshRate
                                }
                                val refreshMhz = (refreshRate * 1000).toInt()

                                val socketPath = "/data/local/tmp/ds-wayland/" +
                                    "${ValidationUtils.waylandSocketDir(containerName)}/wayland-0"
                                WaylandNative.nativeSetSurface(
                                    surface = holder.surface,
                                    width = w,
                                    height = h,
                                    refreshMhz = refreshMhz,
                                    socketPath = socketPath
                                )
                            }

                            override fun surfaceDestroyed(holder: SurfaceHolder) {
                                WaylandNative.nativeDestroySurface()
                            }
                        })

                        setOnTouchListener { view, event ->
                            view.requestFocus()
                            if (event.actionMasked == MotionEvent.ACTION_POINTER_DOWN && event.pointerCount >= 3) {
                                showControls = !showControls
                                controlsAutoShown = false
                                return@setOnTouchListener true
                            }

                        if (inputMode == InputMode.TOUCHPAD) {
                            when (event.actionMasked) {
                                MotionEvent.ACTION_DOWN -> {
                                    startTouchX = event.x
                                    startTouchY = event.y
                                    lastTouchX = event.x
                                    lastTouchY = event.y
                                    touchDownTime = SystemClock.uptimeMillis()
                                    hasMovedPastSlop = false
                                }
                                MotionEvent.ACTION_POINTER_DOWN -> {
                                    if (event.pointerCount == 2) {
                                        twoFingerStartY = (event.getY(0) + event.getY(1)) / 2f
                                    }
                                }
                                MotionEvent.ACTION_MOVE -> {
                                    if (event.pointerCount == 1) {
                                        val dx = (event.x - lastTouchX) * 1.35f
                                        val dy = (event.y - lastTouchY) * 1.35f
                                        if (hypot(event.x - startTouchX, event.y - startTouchY) > touchSlop) {
                                            hasMovedPastSlop = true
                                        }
                                        cursorX = (cursorX + dx).coerceIn(0f, screenW)
                                        cursorY = (cursorY + dy).coerceIn(0f, screenH)
                                        WaylandNative.nativeSendPointerMotion(cursorX, cursorY, dx, dy)
                                        lastTouchX = event.x
                                        lastTouchY = event.y
                                    } else if (event.pointerCount == 2) {
                                        val currentY = (event.getY(0) + event.getY(1)) / 2f
                                        val deltaY = currentY - twoFingerStartY
                                        if (abs(deltaY) > 8f) {
                                            WaylandNative.nativeSendPointerAxis(0, -deltaY * 0.15f, 0)
                                            twoFingerStartY = currentY
                                        }
                                    }
                                }
                                MotionEvent.ACTION_POINTER_UP -> {
                                    if (event.pointerCount == 2 && !hasMovedPastSlop) {
                                        val elapsed = SystemClock.uptimeMillis() - touchDownTime
                                        if (elapsed < 350) {
                                            WaylandNative.nativeSendPointerButton(0x111, 1)
                                            WaylandNative.nativeSendPointerButton(0x111, 0)
                                        }
                                    }
                                }
                                MotionEvent.ACTION_UP -> {
                                    if (isLeftButtonHeld) {
                                        isLeftButtonHeld = false
                                        WaylandNative.nativeSendPointerButton(0x110, 0)
                                    } else if (!hasMovedPastSlop && event.pointerCount == 1) {
                                        val elapsed = SystemClock.uptimeMillis() - touchDownTime
                                        if (elapsed < 250) {
                                            WaylandNative.nativeSendPointerButton(0x110, 1)
                                            WaylandNative.nativeSendPointerButton(0x110, 0)
                                        }
                                    }
                                }
                            }
                        } else {
                            when (event.actionMasked) {
                                MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                                    val pointerIndex = event.actionIndex
                                    val pointerId = event.getPointerId(pointerIndex)
                                    val x = event.getX(pointerIndex)
                                    val y = event.getY(pointerIndex)
                                    WaylandNative.nativeSendTouch(0, pointerId, x, y)
                                    WaylandNative.nativeSendTouchFrame()
                                }
                                MotionEvent.ACTION_MOVE -> {
                                    for (i in 0 until event.pointerCount) {
                                        val pointerId = event.getPointerId(i)
                                        val x = event.getX(i)
                                        val y = event.getY(i)
                                        WaylandNative.nativeSendTouch(2, pointerId, x, y)
                                    }
                                    WaylandNative.nativeSendTouchFrame()
                                }
                                MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                                    val pointerIndex = event.actionIndex
                                    val pointerId = event.getPointerId(pointerIndex)
                                    val x = event.getX(pointerIndex)
                                    val y = event.getY(pointerIndex)
                                    WaylandNative.nativeSendTouch(1, pointerId, x, y)
                                    WaylandNative.nativeSendTouchFrame()
                                }
                            }
                        }
                        true
                    }
                }
            }
        )

        // Top Header Controls
        androidx.compose.animation.AnimatedVisibility(
            visible = showControls,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier
                .align(Alignment.TopCenter)
                .statusBarsPadding()
                .padding(top = 16.dp)
        ) {
            Surface(
                modifier = Modifier
                    .clip(RoundedCornerShape(24.dp))
                    .border(
                        width = 1.dp,
                        color = MaterialTheme.colorScheme.outlineVariant,
                        shape = RoundedCornerShape(24.dp)
                    ),
                color = MaterialTheme.colorScheme.surfaceContainer,
                tonalElevation = 0.dp
            ) {
                Row(
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                    horizontalArrangement = Arrangement.spacedBy(16.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Box(
                        modifier = Modifier
                            .size(36.dp)
                            .clip(RoundedCornerShape(18.dp))
                            .clickable { onNavigateBack() },
                        contentAlignment = Alignment.Center
                    ) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = "Back",
                            tint = MaterialTheme.colorScheme.onSurface
                        )
                    }

                    Text(
                        text = containerName,
                        style = MaterialTheme.typography.titleSmall,
                        color = MaterialTheme.colorScheme.onSurface
                    )

                    Box(
                        modifier = Modifier
                            .size(36.dp)
                            .clip(RoundedCornerShape(18.dp))
                            .clickable { onNavigateToTerminal(containerName) },
                        contentAlignment = Alignment.Center
                    ) {
                        Icon(
                            imageVector = Icons.Default.Terminal,
                            contentDescription = "Terminal",
                            tint = MaterialTheme.colorScheme.primary
                        )
                    }

                    Box(
                        modifier = Modifier
                            .size(36.dp)
                            .clip(RoundedCornerShape(18.dp))
                            .clickable {
                                showControls = false
                                controlsAutoShown = false
                            },
                        contentAlignment = Alignment.Center
                    ) {
                        Icon(
                            imageVector = Icons.Default.Close,
                            contentDescription = "Close overlay",
                            tint = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        }

        // Right-edge Control Pill (vertical).
        // The bottom dock holds extra keys, so the controls move to the side.
        Surface(
            modifier = Modifier
                .align(Alignment.CenterEnd)
                .padding(end = 12.dp)
                .clip(RoundedCornerShape(24.dp))
                .border(
                    width = 1.dp,
                    color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f),
                    shape = RoundedCornerShape(24.dp)
                ),
            color = MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.88f),
            tonalElevation = 0.dp
        ) {
            Column(
                modifier = Modifier.padding(horizontal = 6.dp, vertical = 12.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                // Back: always visible exit, even with no compositor connected
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .clip(RoundedCornerShape(19.dp))
                        .background(MaterialTheme.colorScheme.surfaceContainerHigh)
                        .clickable {
                            modifiers = releaseAllModifiers(modifiers)
                            onNavigateBack()
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                        contentDescription = "Back",
                        tint = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.size(20.dp)
                    )
                }

                // Mode Toggle
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .clip(RoundedCornerShape(19.dp))
                        .background(
                            if (inputMode == InputMode.TOUCHPAD) MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.6f)
                            else MaterialTheme.colorScheme.surfaceContainerHigh
                        )
                        .clickable {
                            inputMode = if (inputMode == InputMode.TOUCHPAD) InputMode.DIRECT_TOUCH else InputMode.TOUCHPAD
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        imageVector = if (inputMode == InputMode.TOUCHPAD) Icons.Default.Mouse else Icons.Default.TouchApp,
                        contentDescription = "Toggle Input Mode",
                        tint = if (inputMode == InputMode.TOUCHPAD) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.size(20.dp)
                    )
                }

                // Left Mouse Button
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .clip(RoundedCornerShape(19.dp))
                        .background(MaterialTheme.colorScheme.surfaceContainerHigh)
                        .clickable {
                            WaylandNative.nativeSendPointerButton(0x110, 1)
                            WaylandNative.nativeSendPointerButton(0x110, 0)
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Text("L", fontWeight = FontWeight.Bold, fontSize = 14.sp,
                        color = MaterialTheme.colorScheme.onSurface)
                }

                // Right Mouse Button
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .clip(RoundedCornerShape(19.dp))
                        .background(MaterialTheme.colorScheme.surfaceContainerHigh)
                        .clickable {
                            WaylandNative.nativeSendPointerButton(0x111, 1)
                            WaylandNative.nativeSendPointerButton(0x111, 0)
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Text("R", fontWeight = FontWeight.Bold, fontSize = 14.sp,
                        color = MaterialTheme.colorScheme.onSurface)
                }

                // Super key
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .clip(RoundedCornerShape(19.dp))
                        .background(MaterialTheme.colorScheme.surfaceContainerHigh)
                        .clickable {
                            WaylandNative.nativeSendKey(125, 1)
                            WaylandNative.nativeSendKey(125, 0)
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        imageVector = Icons.Default.Widgets,
                        contentDescription = "Overview",
                        tint = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.size(20.dp)
                    )
                }

                // Keyboard Toggle
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .clip(RoundedCornerShape(19.dp))
                        .background(
                            if (isKeyboardVisible) MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.6f)
                            else MaterialTheme.colorScheme.surfaceContainerHigh
                        )
                        .clickable {
                            val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
                            if (isKeyboardVisible) {
                                insetsController?.hide(WindowInsetsCompat.Type.ime())
                                surfaceViewRef?.let { imm?.hideSoftInputFromWindow(it.windowToken, 0) }
                            } else {
                                surfaceViewRef?.requestFocus()
                                surfaceViewRef?.let { imm?.showSoftInput(it, InputMethodManager.SHOW_IMPLICIT) }
                                insetsController?.show(WindowInsetsCompat.Type.ime())
                            }
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        imageVector = Icons.Default.Keyboard,
                        contentDescription = "Toggle Keyboard",
                        tint = if (isKeyboardVisible) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.size(20.dp)
                    )
                }
            }
        }

        // Extra keys bar docked at the bottom, directly above Gboard
        WaylandExtraKeysDock(
            rows = extraKeyRows,
            modifiers = modifiers,
            onKeyAction = { key ->
                modifiers = dispatchExtraKey(
                    key = key,
                    modifiers = modifiers,
                    sendText = handleTextInput,
                    onSystemCommand = handleSystemCommand
                )
            },
            onKeyLongPress = { key ->
                if (key.type == WaylandExtraKey.TYPE_MODIFIER) {
                    modifiers = lockModifier(key.code, modifiers)
                }
            },
            onEdit = { showExtraEditor = true },
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .imePadding()
        )
    }

    if (showExtraEditor) {
        WaylandExtraKeysDialog(
            initial = extraKeyRows,
            onDismiss = { showExtraEditor = false },
            onSave = { updated ->
                modifiers = releaseAllModifiers(modifiers)
                prefs.saveWaylandExtraKeys(updated)
                extraKeyRows = updated.ifEmpty { prefs.getWaylandExtraKeys() }
                showExtraEditor = false
            }
        )
    }
}
