// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - JNI Bridge
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "server/ds_server.h"
#include <jni.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define JNI_METHOD(name)                                                       \
  Java_com_droidspaces_app_ui_wayland_WaylandNative_##name

struct ds_server *ds_server_create(const char *socket_dir, int width,
                                   int height, int refresh_mhz,
                                   ANativeWindow *window);
void ds_server_destroy(struct ds_server *server);

static struct ds_server *g_server = NULL;
static pthread_mutex_t g_server_lock = PTHREAD_MUTEX_INITIALIZER;

JNIEXPORT void JNICALL JNI_METHOD(nativeInit)(JNIEnv *env, jobject thiz) {
  (void)env;
  (void)thiz;
  DS_LOGI("Embedded Wayland Server JNI Initialized");
}

JNIEXPORT jboolean JNICALL JNI_METHOD(nativeSetSurface)(
    JNIEnv *env, jobject thiz, jobject surface, jint width, jint height,
    jint refreshMhz, jstring socketPath) {
  (void)thiz;
  pthread_mutex_lock(&g_server_lock);

  if (!surface) {
    pthread_mutex_unlock(&g_server_lock);
    return JNI_FALSE;
  }

  ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
  if (!win) {
    DS_LOGE("ANativeWindow_fromSurface failed");
    pthread_mutex_unlock(&g_server_lock);
    return JNI_FALSE;
  }

  const char *path_str = (*env)->GetStringUTFChars(env, socketPath, NULL);
  char dir_buf[256];
  if (path_str && strlen(path_str) > 0) {
    strncpy(dir_buf, path_str, sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
    char *dir = dirname(dir_buf);
    strncpy(dir_buf, dir, sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
  } else {
    strncpy(dir_buf, "/data/local/tmp/ds-wayland", sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
  }
  if (path_str) {
    (*env)->ReleaseStringUTFChars(env, socketPath, path_str);
  }

  /* Keep the display and its clients across surface changes. A new server
   * is only needed when there is none, the socket dir moved, or the socket
   * file is gone: it can be deleted externally while the server keeps
   * listening on an unlinked fd no new client can reach. */
  if (g_server && strcmp(g_server->sock_dir, dir_buf) == 0) {
    char sock_path[512];
    snprintf(sock_path, sizeof(sock_path), "%s/wayland-0", dir_buf);
    struct stat st;
    if (stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
      pthread_mutex_lock(&g_server->lock);
      ds_server_attach_window(g_server, win, width, height, refreshMhz);
      pthread_mutex_unlock(&g_server->lock);
      pthread_mutex_unlock(&g_server_lock);
      return JNI_TRUE;
    }
    DS_LOGW("Wayland socket %s missing, rebinding display", sock_path);
    ds_server_destroy(g_server);
    g_server = NULL;
  }

  if (g_server) {
    ds_server_destroy(g_server);
    g_server = NULL;
  }

  g_server = ds_server_create(dir_buf, width, height, refreshMhz, win);
  pthread_mutex_unlock(&g_server_lock);

  return g_server ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL JNI_METHOD(nativeGetClientCount)(JNIEnv *env,
                                                        jobject thiz) {
  (void)env;
  (void)thiz;
  int count = 0;
  pthread_mutex_lock(&g_server_lock);
  if (g_server) {
    /* Same locking order as attach/detach: global lock first, server second */
    pthread_mutex_lock(&g_server->lock);
    count = g_server->client_count;
    pthread_mutex_unlock(&g_server->lock);
  }
  pthread_mutex_unlock(&g_server_lock);
  return (jint)count;
}

JNIEXPORT void JNICALL JNI_METHOD(nativeDestroySurface)(JNIEnv *env,
                                                        jobject thiz) {
  (void)env;
  (void)thiz;
  pthread_mutex_lock(&g_server_lock);
  if (g_server) {
    /* Detach only: clients stay connected for instant resume */
    pthread_mutex_lock(&g_server->lock);
    ds_server_detach_window(g_server);
    pthread_mutex_unlock(&g_server->lock);
  }
  pthread_mutex_unlock(&g_server_lock);
}

JNIEXPORT void JNICALL JNI_METHOD(nativeSendTouch)(JNIEnv *env, jobject thiz,
                                                   jint action, jint pointerId,
                                                   jfloat x, jfloat y) {
  (void)env;
  (void)thiz;
  if (!g_server)
    return;

  /* Seat functions now lock internally */
  switch (action) {
  case 0: /* ACTION_DOWN */
  case 5: /* ACTION_POINTER_DOWN */
    ds_seat_send_touch_down(g_server, pointerId, x, y);
    break;
  case 2: /* ACTION_MOVE */
    ds_seat_send_touch_motion(g_server, pointerId, x, y);
    break;
  case 1: /* ACTION_UP */
  case 6: /* ACTION_POINTER_UP */
  case 3: /* ACTION_CANCEL */
    ds_seat_send_touch_up(g_server, pointerId);
    break;
  }
}

JNIEXPORT void JNICALL JNI_METHOD(nativeSendTouchFrame)(JNIEnv *env,
                                                        jobject thiz) {
  (void)env;
  (void)thiz;
  if (g_server)
    ds_seat_send_touch_frame(g_server);
}

JNIEXPORT void JNICALL JNI_METHOD(nativeSendKey)(JNIEnv *env, jobject thiz,
                                                 jint keyCode, jint action) {
  (void)env;
  (void)thiz;
  if (g_server) {
    /* action: 0 = UP, 1 = DOWN */
    uint32_t state = (action == 1) ? 1 : 0;
    ds_seat_send_key(g_server, (uint32_t)keyCode, state);
  }
}

JNIEXPORT void JNICALL JNI_METHOD(nativeSendPointerMotion)(
    JNIEnv *env, jobject thiz, jfloat x, jfloat y, jfloat dx, jfloat dy) {
  (void)env;
  (void)thiz;
  if (g_server)
    ds_seat_send_pointer_motion(g_server, x, y, dx, dy);
}

JNIEXPORT void JNICALL JNI_METHOD(nativeSendPointerButton)(JNIEnv *env,
                                                           jobject thiz,
                                                           jint button,
                                                           jint pressed) {
  (void)env;
  (void)thiz;
  if (g_server)
    ds_seat_send_pointer_button(g_server, (uint32_t)button, (uint32_t)pressed);
}

JNIEXPORT void JNICALL JNI_METHOD(nativeSendPointerAxis)(
    JNIEnv *env, jobject thiz, jint axis, jfloat value, jint discrete) {
  (void)env;
  (void)thiz;
  (void)discrete;
  if (g_server)
    ds_seat_send_pointer_axis(g_server, (uint32_t)axis, value);
}

JNIEXPORT void JNICALL JNI_METHOD(nativeSendDisplayRotation)(JNIEnv *env,
                                                             jobject thiz,
                                                             jint rotationDeg) {
  (void)env;
  (void)thiz;
  (void)rotationDeg;
}
