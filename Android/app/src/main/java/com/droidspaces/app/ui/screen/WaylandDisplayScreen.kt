package com.droidspaces.app.ui.screen

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.os.Build
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.droidspaces.app.ui.wayland.WaylandNative

@SuppressLint("ClickableViewAccessibility")
@Composable
fun WaylandDisplayScreen(
    containerName: String,
    onNavigateBack: () -> Unit,
    onNavigateToTerminal: (String) -> Unit
) {
    val context = LocalContext.current
    var showControls by remember { mutableStateOf(false) }

    // Immersive full-screen mode: hide system status bar and navigation bar
    val activity = context as? Activity
    DisposableEffect(activity) {
        if (activity != null) {
            val window = activity.window
            val insetsController = WindowCompat.getInsetsController(window, window.decorView)
            val prevBehavior = insetsController.systemBarsBehavior
            insetsController.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            insetsController.hide(WindowInsetsCompat.Type.systemBars())

            onDispose {
                insetsController.systemBarsBehavior = prevBehavior
                insetsController.show(WindowInsetsCompat.Type.systemBars())
                WaylandNative.nativeDestroySurface()
            }
        } else {
            onDispose {
                WaylandNative.nativeDestroySurface()
            }
        }
    }

    BackHandler {
        if (showControls) {
            showControls = false
        } else {
            onNavigateBack()
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
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            val windowManager = ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager
                            val refreshRate = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                                ctx.display?.refreshRate ?: 60f
                            } else {
                                @Suppress("DEPRECATION")
                                windowManager.defaultDisplay.refreshRate
                            }
                            val refreshMhz = (refreshRate * 1000).toInt()

                            val socketPath = "/data/local/tmp/ds-wayland/ds-wayland.sock"
                            WaylandNative.nativeSetSurface(
                                surface = holder.surface,
                                width = width.coerceAtLeast(1),
                                height = height.coerceAtLeast(1),
                                refreshMhz = refreshMhz,
                                socketPath = socketPath
                            )
                        }

                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, w: Int, h: Int) {
                            val windowManager = ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager
                            val refreshRate = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                                ctx.display?.refreshRate ?: 60f
                            } else {
                                @Suppress("DEPRECATION")
                                windowManager.defaultDisplay.refreshRate
                            }
                            val refreshMhz = (refreshRate * 1000).toInt()

                            val socketPath = "/data/local/tmp/ds-wayland/ds-wayland.sock"
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

                    setOnTouchListener { _, event ->
                        // 3-finger tap opens or closes the control overlay
                        if (event.actionMasked == MotionEvent.ACTION_POINTER_DOWN && event.pointerCount >= 3) {
                            showControls = !showControls
                            return@setOnTouchListener true
                        }

                        when (event.actionMasked) {
                            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                                val pointerIndex = event.actionIndex
                                val pointerId = event.getPointerId(pointerIndex)
                                val x = event.getX(pointerIndex)
                                val y = event.getY(pointerIndex)
                                WaylandNative.nativeSendTouch(0, pointerId, x, y)
                            }
                            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                                val pointerIndex = event.actionIndex
                                val pointerId = event.getPointerId(pointerIndex)
                                val x = event.getX(pointerIndex)
                                val y = event.getY(pointerIndex)
                                WaylandNative.nativeSendTouch(1, pointerId, x, y)
                            }
                            MotionEvent.ACTION_MOVE -> {
                                for (i in 0 until event.pointerCount) {
                                    val pointerId = event.getPointerId(i)
                                    val x = event.getX(i)
                                    val y = event.getY(i)
                                    WaylandNative.nativeSendTouch(2, pointerId, x, y)
                                }
                            }
                        }
                        true
                    }
                }
            }
        )

        // Overlay control pill following DESIGN.md flat & surface tokens
        AnimatedVisibility(
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
                            .clickable { showControls = false },
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
    }
}
