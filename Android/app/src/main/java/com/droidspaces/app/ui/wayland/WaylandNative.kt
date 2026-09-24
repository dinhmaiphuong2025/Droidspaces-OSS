package com.droidspaces.app.ui.wayland

import android.view.Surface

object WaylandNative {
    init {
        try {
            System.loadLibrary("ds_wl_server")
            nativeInit()
        } catch (e: UnsatisfiedLinkError) {
            e.printStackTrace()
        }
    }

    external fun nativeInit()

    external fun nativeSetSurface(
        surface: Surface?,
        width: Int,
        height: Int,
        refreshMhz: Int,
        socketPath: String
    ): Boolean

    external fun nativeDestroySurface()

    external fun nativeGetClientCount(): Int

    external fun nativeSendTouch(action: Int, pointerId: Int, x: Float, y: Float)

    external fun nativeSendTouchFrame()

    external fun nativeSendKey(keyCode: Int, action: Int)

    external fun nativeSendPointerMotion(x: Float, y: Float, dx: Float, dy: Float)

    external fun nativeSendPointerButton(button: Int, pressed: Int)

    external fun nativeSendPointerAxis(axis: Int, value: Float, discrete: Int)

    external fun nativeSendDisplayRotation(rotationDeg: Int)
}
