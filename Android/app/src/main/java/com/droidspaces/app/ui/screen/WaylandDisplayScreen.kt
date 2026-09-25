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
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.isImeVisible
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Keyboard
import androidx.compose.material.icons.filled.Mouse
import androidx.compose.material.icons.filled.TouchApp
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
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.droidspaces.app.ui.theme.JetBrainsMono
import com.droidspaces.app.ui.wayland.WaylandNative
import com.droidspaces.app.util.ValidationUtils
import com.droidspaces.app.util.WaylandExtraKey
import com.droidspaces.app.util.WaylandKeyMapper
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.roundToInt

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
    onNavigateBack: () -> Unit
) {
    val context = LocalContext.current
    var inputMode by remember { mutableStateOf(InputMode.TOUCHPAD) }
    val isKeyboardVisible = WindowInsets.isImeVisible
    var isConnected by remember { mutableStateOf(true) }
    val socketDir = remember(containerName) { ValidationUtils.waylandSocketDir(containerName) }

    // Draggable pill position in px. NaN until the first layout parks it
    // at the right edge, vertically centered.
    var pillX by remember { mutableStateOf(Float.NaN) }
    var pillY by remember { mutableStateOf(0f) }
    var pillW by remember { mutableStateOf(0) }
    var pillH by remember { mutableStateOf(0) }
    var stageW by remember { mutableStateOf(0) }
    var stageH by remember { mutableStateOf(0) }
    val pillMargin = with(LocalDensity.current) { 12.dp.toPx() }

    // Poll the compositor connection so the waiting card shows while no
    // client is on the display and hides itself once one connects.
    LaunchedEffect(Unit) {
        while (true) {
            delay(2000)
            isConnected = try {
                WaylandNative.nativeGetClientCount() > 0
            } catch (_: Throwable) {
                false
            }
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
        modifiers = releaseAllModifiers(modifiers)
        onNavigateBack()
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

    // One keyboard toggle shared by the pill button and the extra keys bar,
    // both directions: shows the IME when hidden, hides it when visible.
    // The hide path keeps focus and covers both window tokens: clearing
    // focus first would unbind the input connection and make the async
    // hide request a no-op, and the SurfaceView lives in its own window
    // apart from the activity decor view.
    val toggleKeyboard: () -> Unit = {
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        if (isKeyboardVisible) {
            insetsController?.hide(WindowInsetsCompat.Type.ime())
            surfaceViewRef?.let { imm?.hideSoftInputFromWindow(it.windowToken, 0) }
            activity?.window?.decorView?.let { imm?.hideSoftInputFromWindow(it.windowToken, 0) }
        } else {
            surfaceViewRef?.requestFocus()
            surfaceViewRef?.let { imm?.showSoftInput(it, InputMethodManager.SHOW_IMPLICIT) }
            insetsController?.show(WindowInsetsCompat.Type.ime())
        }
    }

    // System command handler for extra keys
    val handleSystemCommand: (String) -> Unit = { cmd ->
        when (cmd) {
            "toggle_ime" -> toggleKeyboard()
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .onGloballyPositioned {
                stageW = it.size.width
                stageH = it.size.height
            }
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

                                val socketPath = "/data/local/tmp/ds-wayland/$socketDir/wayland-0"
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
                                        val rawDx = event.x - lastTouchX
                                        val rawDy = event.y - lastTouchY
                                        val dist = hypot(rawDx, rawDy)
                                        // Dynamic acceleration: smooth precision for small movements,
                                        // fast travel for rapid swipes across high-res 1440x3200 display.
                                        val accel = if (dist > 15f) 2.5f else 1.8f
                                        val dx = rawDx * accel
                                        val dy = rawDy * accel
                                        if (hypot(event.x - startTouchX, event.y - startTouchY) > touchSlop) {
                                            hasMovedPastSlop = true
                                        }
                                        // Press and hold without moving grabs for drag,
                                        // the laptop tap-and-a-half gesture.
                                        if (!hasMovedPastSlop && !isLeftButtonHeld &&
                                            SystemClock.uptimeMillis() - touchDownTime > 450) {
                                            isLeftButtonHeld = true
                                            hasMovedPastSlop = true
                                            WaylandNative.nativeSendPointerButton(0x110, 1)
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

        // Waiting card while no compositor is on the display. Shows the
        // socket paths so a missing connection is diagnosable on the spot,
        // and hides itself once a client connects.
        AnimatedVisibility(
            visible = !isConnected,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier.align(Alignment.Center)
        ) {
            Surface(
                modifier = Modifier
                    .padding(horizontal = 24.dp)
                    .clip(RoundedCornerShape(20.dp))
                    .border(
                        width = 1.dp,
                        color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.35f),
                        shape = RoundedCornerShape(20.dp)
                    ),
                color = MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.95f),
                tonalElevation = 0.dp
            ) {
                Column(
                    modifier = Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                    horizontalAlignment = Alignment.Start
                ) {
                    Text(
                        text = "Waiting for compositor",
                        style = MaterialTheme.typography.titleMedium
                    )
                    Text(
                        text = "No compositor is connected to this display yet.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
                    )
                    Text(
                        text = "Host: /data/local/tmp/ds-wayland/$socketDir/wayland-0",
                        style = MaterialTheme.typography.bodySmall.copy(fontFamily = JetBrainsMono)
                    )
                    Text(
                        text = "Container: /run/ds-wayland/$socketDir/wayland-0",
                        style = MaterialTheme.typography.bodySmall.copy(fontFamily = JetBrainsMono)
                    )
                    Text(
                        text = "Check: systemctl status niri",
                        style = MaterialTheme.typography.bodySmall.copy(fontFamily = JetBrainsMono),
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
                    )
                }
            }
        }

        // Control pill, draggable, snaps to the nearest side edge on release.
        // Taps still reach the buttons: the drag detector only consumes
        // gestures past the touch slop.
        Surface(
            modifier = Modifier
                .align(Alignment.TopStart)
                .offset {
                    IntOffset(
                        if (pillX.isNaN()) 0 else pillX.roundToInt(),
                        pillY.roundToInt()
                    )
                }
                .onGloballyPositioned {
                    pillW = it.size.width
                    pillH = it.size.height
                    if (pillX.isNaN() && stageW > 0) {
                        pillX = stageW - pillW - pillMargin
                        pillY = ((stageH - pillH) / 2f).coerceAtLeast(0f)
                    } else if (!pillX.isNaN()) {
                        // Keep the pill on screen across rotation and resize.
                        pillX = pillX.coerceIn(0f, (stageW - pillW).coerceAtLeast(0).toFloat())
                        pillY = pillY.coerceIn(0f, (stageH - pillH).coerceAtLeast(0).toFloat())
                    }
                }
                .pointerInput(stageW, stageH, pillW, pillH) {
                    detectDragGestures(
                        onDragEnd = {
                            val maxX = (stageW - pillW).coerceAtLeast(0).toFloat()
                            val maxY = (stageH - pillH).coerceAtLeast(0).toFloat()
                            pillX = if (pillX + pillW / 2f < stageW / 2f) {
                                pillMargin
                            } else {
                                (maxX - pillMargin).coerceAtLeast(0f)
                            }
                            pillY = pillY.coerceIn(0f, maxY)
                        }
                    ) { change, dragAmount ->
                        change.consume()
                        pillX = (pillX + dragAmount.x)
                            .coerceIn(0f, (stageW - pillW).coerceAtLeast(0).toFloat())
                        pillY = (pillY + dragAmount.y)
                            .coerceIn(0f, (stageH - pillH).coerceAtLeast(0).toFloat())
                    }
                }
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

                // Keyboard Toggle
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .clip(RoundedCornerShape(19.dp))
                        .background(
                            if (isKeyboardVisible) MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.6f)
                            else MaterialTheme.colorScheme.surfaceContainerHigh
                        )
                        .clickable { toggleKeyboard() },
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
