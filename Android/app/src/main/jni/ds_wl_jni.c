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

#include <android/hardware_buffer.h>
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

#define MAX_BUFS 8 /* maximum buffers we track */

struct render_state {
  struct anw_priv anw;
  ds_wl_ctx *consumer;

  struct anw_buffer *buf_anb[MAX_BUFS]; /* cached ANB pointers */
  int buf_fds[MAX_BUFS]; /* dup'd dma-buf fds extracted from handles */
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

/* Dequeue/queue buffers to discover unique slots and dup their fds.
 * BufferQueue requires minUndequeuedBuffers to stay queued, so we
 * dequeue one at a time, grab the fd, then queue it back to rotate. */
static int collect_buffers(struct render_state *rs, int target) {
  int found = 0;
  int queued = 0;

  for (int attempt = 0; attempt < target * 4 && found < target; attempt++) {
    struct anw_buffer *anb = NULL;
    int fence = -1;
    if (anw_dequeue(&rs->anw, &anb, &fence) != 0 || !anb) {
      if (fence >= 0)
        close(fence);
      break;
    }
    if (fence >= 0)
      close(fence);

    if (!anb->handle || anb->handle->num_fds < 1) {
      anw_cancel(&rs->anw, anb, -1);
      continue;
    }

    /* Check for duplicate (same ANB pointer = same slot) */
    int is_dup = 0;
    for (int i = 0; i < found; i++) {
      if (rs->buf_anb[i] == anb) {
        is_dup = 1;
        break;
      }
    }

    /* Queue it back so BufferQueue rotates to another slot */
    anw_queue(&rs->anw, anb, -1);
    queued++;

    if (is_dup)
      continue;

    int raw_fd = anb->handle->data[0];
    int dup_fd = dup(raw_fd);
    if (dup_fd < 0)
      continue;

    rs->buf_anb[found] = anb;
    rs->buf_fds[found] = dup_fd;
    LOGI("  buf[%d]: anb=%p fd=%d dup=%d %dx%d stride=%d", found, (void *)anb,
         raw_fd, dup_fd, anb->width, anb->height, anb->stride);
    found++;
  }

  /* Drain the queued buffers back to free state */
  for (int i = 0; i < queued; i++) {
    struct anw_buffer *danb = NULL;
    int dfence = -1;
    if (anw_dequeue(&rs->anw, &danb, &dfence) != 0 || !danb) {
      if (dfence >= 0)
        close(dfence);
      break;
    }
    if (dfence >= 0)
      close(dfence);
    anw_cancel(&rs->anw, danb, -1);
  }

  if (found < 2) {
    LOGE("failed to collect enough buffers: got %d (need >= 2)", found);
    for (int i = 0; i < found; i++) {
      close(rs->buf_fds[i]);
      rs->buf_fds[i] = -1;
    }
    return -1;
  }

  rs->buf_count = found;
  LOGI("collected %d unique DMA-BUF buffers", found);
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
    for (int i = 0; i < g_state->buf_count; i++) {
      if (g_state->buf_fds[i] >= 0)
        close(g_state->buf_fds[i]);
    }
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

  /* Query minUndequeuedBuffers to size the pool correctly */
  int min_undequeued = 0;
  anw_query(&rs->anw, ANW_QUERY_MIN_UNDEQUEUED_BUFFERS, &min_undequeued);
  int target = min_undequeued + 2;
  if (target < 4)
    target = 4;
  if (target > MAX_BUFS)
    target = MAX_BUFS;
  anw_set_buffer_count(&rs->anw, target);

  /* Collect buffer fds */
  if (collect_buffers(rs, target) < 0) {
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
    for (int i = 0; i < rs->buf_count; i++) {
      if (rs->buf_fds[i] >= 0)
        close(rs->buf_fds[i]);
    }
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
  for (int i = 0; i < g_state->buf_count; i++) {
    if (g_state->buf_fds[i] >= 0) {
      close(g_state->buf_fds[i]);
      g_state->buf_fds[i] = -1;
    }
  }
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
