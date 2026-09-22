// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * JNI bridge + render loop for the DS_WL consumer (clean-room)
 *
 * Registers native methods for WaylandNative.kt, manages the
 * ANativeWindow lifecycle, runs the render loop on a dedicated thread,
 * and dispatches input events from the Kotlin UI layer.
 */

#include "ds_wl_anw.h"
#include "ds_wl_consumer.h"
#include "ds_wl_input.h"
#include "ds_wl_proto.h"

#include <errno.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#define LOG_TAG "DsWlServer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

#define BUF_COUNT 3 /* triple buffering */

struct render_state {
  struct anw_priv anw;
  ds_wl_ctx *consumer;

  struct anw_buffer *buffers[BUF_COUNT];
  int fence_fds[BUF_COUNT];
  int buf_fds[BUF_COUNT]; /* dma-buf fds extracted from handles */
  int buf_count;

  pthread_t thread;
  atomic_int running;
  int width;
  int height;
  int refresh_mhz;
};

static struct render_state *g_state;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Buffer management                                                  */
/* ------------------------------------------------------------------ */

/* Extract the first fd from a buffer's native_handle */
static int buf_get_fd(struct anw_buffer *buf) {
  if (!buf || !buf->handle || buf->handle->num_fds < 1)
    return -1;
  return buf->handle->data[0]; /* first fd in the handle */
}

/* Dequeue all buffers from ANativeWindow and extract their fds */
static int collect_buffers(struct render_state *rs) {
  for (int i = 0; i < BUF_COUNT; i++) {
    int fence = -1;
    int err = anw_dequeue(&rs->anw, &rs->buffers[i], &fence);
    if (err != 0) {
      LOGE("dequeueBuffer[%d] failed: %d", i, err);
      /* Cancel already-dequeued buffers */
      for (int j = 0; j < i; j++)
        anw_cancel(&rs->anw, rs->buffers[j], -1);
      return -1;
    }
    rs->fence_fds[i] = fence;
    rs->buf_fds[i] = buf_get_fd(rs->buffers[i]);
    if (rs->buf_fds[i] < 0) {
      LOGE("buffer[%d] has no fd in handle", i);
      for (int j = 0; j <= i; j++)
        anw_cancel(&rs->anw, rs->buffers[j], -1);
      return -1;
    }
  }
  rs->buf_count = BUF_COUNT;

  /* Cancel all buffers back so we can re-dequeue them one at a time
   * during the render loop */
  for (int i = 0; i < BUF_COUNT; i++) {
    if (rs->fence_fds[i] >= 0) {
      close(rs->fence_fds[i]);
      rs->fence_fds[i] = -1;
    }
    anw_cancel(&rs->anw, rs->buffers[i], -1);
    rs->buffers[i] = NULL;
  }

  return 0;
}

/* ------------------------------------------------------------------ */
/* Render loop                                                        */
/* ------------------------------------------------------------------ */

static void *render_thread(void *arg) {
  struct render_state *rs = (struct render_state *)arg;

  LOGI("render loop started (%dx%d @%d mHz)", rs->width, rs->height,
       rs->refresh_mhz);

  while (atomic_load(&rs->running)) {
    /* Dequeue a buffer from SurfaceFlinger */
    struct anw_buffer *buf = NULL;
    int fence_fd = -1;
    int err = anw_dequeue(&rs->anw, &buf, &fence_fd);
    if (err != 0) {
      LOGW("dequeueBuffer failed (%d), retrying", err);
      usleep(16000);
      continue;
    }

    /* Wait for the dequeue fence if present */
    if (fence_fd >= 0) {
      struct pollfd pfd = {.fd = fence_fd, .events = POLLIN};
      poll(&pfd, 1, 16); /* best-effort 16ms timeout */
      close(fence_fd);
    }

    /* Find which buffer index this is */
    int buf_idx = -1;
    int this_fd = buf_get_fd(buf);
    for (int i = 0; i < rs->buf_count; i++) {
      if (rs->buf_fds[i] == this_fd) {
        buf_idx = i;
        break;
      }
    }

    if (buf_idx < 0) {
      /* Unknown buffer, just queue it back */
      LOGW("unknown buffer fd=%d, queuing anyway", this_fd);
      anw_queue(&rs->anw, buf, -1);
      continue;
    }

    /* Tell the producer to render into this buffer and wait */
    int present_err = ds_wl_present(rs->consumer, buf_idx);

    /* Always queue the buffer (fix flicker: never cancel on NoDamage) */
    anw_queue(&rs->anw, buf, -1);

    if (present_err < 0 && !atomic_load(&rs->running))
      break; /* woken up to stop */

    /* Tiny yield to avoid busy-spinning when producer is slow */
    if (present_err < 0)
      usleep(1000);
  }

  LOGI("render loop exiting");
  return NULL;
}

/* ------------------------------------------------------------------ */
/* JNI methods                                                        */
/* ------------------------------------------------------------------ */

#define JNI_PREFIX(name)                                                       \
  Java_com_droidspaces_app_ui_wayland_WaylandNative_##name

JNIEXPORT void JNICALL JNI_PREFIX(nativeInit)(JNIEnv *env __attribute__((unused)),
                                              jobject thiz __attribute__((unused))) {
  LOGI("nativeInit (ds_wl_server clean-room)");
}

JNIEXPORT jboolean JNICALL JNI_PREFIX(nativeSetSurface)(
    JNIEnv *env, jobject thiz __attribute__((unused)), jobject surface,
    jint width, jint height, jint refresh_mhz, jstring socket_path) {

  pthread_mutex_lock(&g_lock);

  /* Tear down previous session if any */
  if (g_state) {
    LOGW("nativeSetSurface called while already active, tearing down");
    atomic_store(&g_state->running, 0);
    if (g_state->consumer)
      ds_wl_wake(g_state->consumer);
    pthread_join(g_state->thread, NULL);
    ds_wl_disconnect(g_state->consumer);
    anw_disconnect(&g_state->anw);
    ANativeWindow_release(g_state->anw.win);
    free(g_state);
    g_state = NULL;
  }

  if (!surface) {
    pthread_mutex_unlock(&g_lock);
    return JNI_FALSE;
  }

  struct render_state *rs = calloc(1, sizeof(*rs));
  if (!rs) {
    pthread_mutex_unlock(&g_lock);
    return JNI_FALSE;
  }

  rs->anw.win = ANativeWindow_fromSurface(env, surface);
  if (!rs->anw.win) {
    LOGE("ANativeWindow_fromSurface failed");
    free(rs);
    pthread_mutex_unlock(&g_lock);
    return JNI_FALSE;
  }

  rs->width = width;
  rs->height = height;
  rs->refresh_mhz = refresh_mhz;

  /* Configure the native window */
  ANativeWindow_setBuffersGeometry(rs->anw.win, width, height,
                                   AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
  anw_connect(&rs->anw);
  anw_set_buffer_count(&rs->anw, BUF_COUNT);

  /* Collect buffer fds */
  if (collect_buffers(rs) < 0) {
    LOGE("failed to collect buffers");
    anw_disconnect(&rs->anw);
    ANativeWindow_release(rs->anw.win);
    free(rs);
    pthread_mutex_unlock(&g_lock);
    return JNI_FALSE;
  }

  /* Connect to broker */
  const char *path = (*env)->GetStringUTFChars(env, socket_path, NULL);
  struct ds_wl_display_info disp = {
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      .refresh_mhz = (uint32_t)refresh_mhz,
  };

  int con_err = ds_wl_connect(&rs->consumer, path, rs->buf_fds, NULL,
                              rs->buf_count, &disp);
  (*env)->ReleaseStringUTFChars(env, socket_path, path);

  if (con_err < 0) {
    LOGE("ds_wl_connect failed");
    anw_disconnect(&rs->anw);
    ANativeWindow_release(rs->anw.win);
    free(rs);
    pthread_mutex_unlock(&g_lock);
    return JNI_FALSE;
  }

  /* Start render loop thread */
  atomic_store(&rs->running, 1);
  if (pthread_create(&rs->thread, NULL, render_thread, rs) != 0) {
    LOGE("pthread_create failed: %s", strerror(errno));
    ds_wl_disconnect(rs->consumer);
    anw_disconnect(&rs->anw);
    ANativeWindow_release(rs->anw.win);
    free(rs);
    pthread_mutex_unlock(&g_lock);
    return JNI_FALSE;
  }

  g_state = rs;
  LOGI("surface set: %dx%d @%d mHz", width, height, refresh_mhz);
  pthread_mutex_unlock(&g_lock);
  return JNI_TRUE;
}

JNIEXPORT void JNICALL JNI_PREFIX(nativeDestroySurface)(
    JNIEnv *env __attribute__((unused)),
    jobject thiz __attribute__((unused))) {
  pthread_mutex_lock(&g_lock);
  if (!g_state) {
    pthread_mutex_unlock(&g_lock);
    return;
  }

  LOGI("destroying surface");
  atomic_store(&g_state->running, 0);
  if (g_state->consumer)
    ds_wl_wake(g_state->consumer);
  pthread_join(g_state->thread, NULL);
  ds_wl_disconnect(g_state->consumer);
  anw_disconnect(&g_state->anw);
  ANativeWindow_release(g_state->anw.win);
  free(g_state);
  g_state = NULL;
  pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------ */
/* Input dispatch                                                     */
/* ------------------------------------------------------------------ */

static int send_input(const struct ds_wl_input_ev *ev) {
  /* Lock-free: g_state read is safe because it is only set/cleared
   * under g_lock but we only read consumer pointer (stable while
   * render loop runs). */
  struct render_state *rs = g_state;
  if (!rs || !rs->consumer)
    return -1;
  return ds_wl_send_input(rs->consumer, ev);
}

JNIEXPORT void JNICALL JNI_PREFIX(nativeSendTouch)(
    JNIEnv *env __attribute__((unused)),
    jobject thiz __attribute__((unused)),
    jint action, jint pointer_id, jfloat x, jfloat y) {
  struct ds_wl_input_ev ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = DS_WL_INPUT_TOUCH;
  ev.touch.action = action;
  ev.touch.pointer_id = pointer_id;
  ev.touch.x = x;
  ev.touch.y = y;
  send_input(&ev);
}

JNIEXPORT void JNICALL JNI_PREFIX(nativeSendTouchFrame)(
    JNIEnv *env __attribute__((unused)),
    jobject thiz __attribute__((unused))) {
  struct ds_wl_input_ev ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = DS_WL_INPUT_TOUCH_FRAME;
  send_input(&ev);
}

JNIEXPORT void JNICALL JNI_PREFIX(nativeSendKey)(
    JNIEnv *env __attribute__((unused)),
    jobject thiz __attribute__((unused)),
    jint key_code, jint action) {
  struct ds_wl_input_ev ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = DS_WL_INPUT_KEY;
  ev.key.code = key_code;
  ev.key.action = action;
  send_input(&ev);
}

JNIEXPORT void JNICALL JNI_PREFIX(nativeSendPointerMotion)(
    JNIEnv *env __attribute__((unused)),
    jobject thiz __attribute__((unused)),
    jfloat x, jfloat y, jfloat dx, jfloat dy) {
  struct ds_wl_input_ev ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = DS_WL_INPUT_PTR_MOTION;
  ev.ptr_motion.x = x;
  ev.ptr_motion.y = y;
  ev.ptr_motion.dx = dx;
  ev.ptr_motion.dy = dy;
  send_input(&ev);
}

JNIEXPORT void JNICALL JNI_PREFIX(nativeSendPointerButton)(
    JNIEnv *env __attribute__((unused)),
    jobject thiz __attribute__((unused)),
    jint button, jint pressed) {
  struct ds_wl_input_ev ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = DS_WL_INPUT_PTR_BUTTON;
  ev.ptr_button.button = button;
  ev.ptr_button.pressed = pressed;
  send_input(&ev);
}

JNIEXPORT void JNICALL JNI_PREFIX(nativeSendPointerAxis)(
    JNIEnv *env __attribute__((unused)),
    jobject thiz __attribute__((unused)),
    jint axis, jfloat value, jint discrete) {
  struct ds_wl_input_ev ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = DS_WL_INPUT_PTR_AXIS;
  ev.ptr_axis.axis = axis;
  ev.ptr_axis.value = value;
  ev.ptr_axis.discrete = discrete;
  send_input(&ev);
}

JNIEXPORT void JNICALL JNI_PREFIX(nativeSendDisplayRotation)(
    JNIEnv *env __attribute__((unused)),
    jobject thiz __attribute__((unused)),
    jint rotation_deg) {
  struct ds_wl_input_ev ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = DS_WL_INPUT_ROTATION;
  ev.rotation.degrees = rotation_deg;
  send_input(&ev);
}
