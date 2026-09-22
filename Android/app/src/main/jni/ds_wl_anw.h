// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * ANativeWindow/ANativeWindowBuffer hidden struct access (clean-room)
 *
 * Written from the AOSP public header documentation
 * (system/core/include/system/window.h, Apache-2.0) and the NDK public
 * API surface.  Only the struct layouts and vtable offsets needed for
 * dequeueBuffer / queueBuffer / cancelBuffer are reproduced here.
 *
 * Names and helpers intentionally differ from any GPL codebase.
 */

#ifndef DS_WL_ANW_H
#define DS_WL_ANW_H

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <stdint.h>
#include <string.h>

/* ----- native_handle_t (from cutils/native_handle.h, Apache-2.0) ----- */

typedef struct anw_native_handle {
  int version; /* sizeof(anw_native_handle) */
  int num_fds;
  int num_ints;
  int data[0]; /* fds[num_fds] then ints[num_ints] */
} anw_native_handle_t;

/* ----- ANativeWindowBuffer (from system/window.h, Apache-2.0) ----- */

/*
 * android_native_base_t equivalent: the ref-counting header that
 * ANativeWindowBuffer and ANativeWindow both start with.
 */
struct anw_base {
  int32_t magic;
  int32_t version;
  void *reserved[4];
  void (*incref)(void *);
  void (*decref)(void *);
};

/*
 * ANativeWindowBuffer layout.  The fields after `common` are:
 *   width, height, stride, format, usage (deprecated), layerCount, handle
 * This matches the AOSP struct for API levels 26 through 36.
 */
struct anw_buffer {
  struct anw_base common;
  int width;
  int height;
  int stride;
  int format;
  int usage_deprecated;
  uintptr_t layer_count;
  void *reserved_buf[1];
  const anw_native_handle_t *handle;
  uint64_t usage;
  void *reserved_end[8];
};

/* ----- ANativeWindow vtable access ----- */

/*
 * The private half of ANativeWindow sits right after the public
 * ANativeWindow / anw_base header.  We reach the vtable slots via
 * known offsets from the base pointer.
 *
 * The vtable order (from window.h):
 *   [0] setSwapInterval
 *   [1] dequeueBuffer (pre-API-18 legacy, takes 1 fence)
 *   [2] lockBuffer (deprecated)
 *   [3] queueBuffer  (pre-API-18 legacy, takes 1 fence)
 *   [4] query
 *   [5] perform
 *   [6] cancelBuffer (pre-API-18 legacy, takes 1 fence)
 *   [7] dequeueBuffer (modern, returns fence fd)
 *   [8] queueBuffer   (modern, takes fence fd)
 *   [9] cancelBuffer  (modern, takes fence fd)
 */

/* Opaque wrapper so we never dereference a raw ANativeWindow* at the
 * wrong offset.  The actual ANativeWindow pointer is stored inside. */
struct anw_priv {
  ANativeWindow *win;
};

/* vtable function pointer types */
typedef int (*anw_dequeue_fn)(ANativeWindow *, struct anw_buffer **, int *fence);
typedef int (*anw_queue_fn)(ANativeWindow *, struct anw_buffer *, int fence);
typedef int (*anw_cancel_fn)(ANativeWindow *, struct anw_buffer *, int fence);
typedef int (*anw_query_fn)(const ANativeWindow *, int what, int *value);
typedef int (*anw_perform_fn)(ANativeWindow *, int operation, ...);

/*
 * Read a function pointer from the ANativeWindow vtable.
 * The vtable starts at offset sizeof(anw_base) inside ANativeWindow.
 * Each slot is a function pointer (sizeof(void*)).
 */
static inline void *anw_vtable_slot(ANativeWindow *w, int slot) {
  /* Skip anw_base (magic, version, reserved[4], incref, decref) */
  char *base = (char *)w;
  void **vtable = (void **)(base + sizeof(struct anw_base));
  return vtable[slot];
}

/* Modern dequeueBuffer (slot 7): returns fence_fd via out parameter */
static inline int anw_dequeue(struct anw_priv *p, struct anw_buffer **buf,
                              int *fence_fd) {
  anw_dequeue_fn fn = (anw_dequeue_fn)anw_vtable_slot(p->win, 7);
  return fn(p->win, buf, fence_fd);
}

/* Modern queueBuffer (slot 8): takes fence_fd, -1 = no fence */
static inline int anw_queue(struct anw_priv *p, struct anw_buffer *buf,
                            int fence_fd) {
  anw_queue_fn fn = (anw_queue_fn)anw_vtable_slot(p->win, 8);
  return fn(p->win, buf, fence_fd);
}

/* Modern cancelBuffer (slot 9): takes fence_fd, -1 = no fence */
static inline int anw_cancel(struct anw_priv *p, struct anw_buffer *buf,
                             int fence_fd) {
  anw_cancel_fn fn = (anw_cancel_fn)anw_vtable_slot(p->win, 9);
  return fn(p->win, buf, fence_fd);
}

/* query (slot 4) */
static inline int anw_query(struct anw_priv *p, int what, int *value) {
  anw_query_fn fn = (anw_query_fn)anw_vtable_slot(p->win, 4);
  return fn(p->win, what, value);
}

/* perform (slot 5) */
static inline int anw_perform(struct anw_priv *p, int op, ...) {
  /* For the operations we need we always pass exactly one int or pointer
   * argument, so a direct cast is safe.  For multi-arg performs, use
   * the explicit helper below. */
  anw_perform_fn fn = (anw_perform_fn)anw_vtable_slot(p->win, 5);
  /* We cannot forward varargs; callers should use the typed helpers. */
  return fn(p->win, op);
}

/* ----- Perform operation codes (from window.h) ----- */

#define ANW_PERFORM_SET_USAGE        0 /* (uint64_t usage) */
#define ANW_PERFORM_CONNECT          1 /* (int api) */
#define ANW_PERFORM_DISCONNECT       2 /* (int api) */
#define ANW_PERFORM_SET_BUFFER_COUNT 4 /* (int count) */
#define ANW_PERFORM_SET_BUFFERS_FORMAT   9  /* (int format) */
#define ANW_PERFORM_API_CONNECT     13 /* (int api) */
#define ANW_PERFORM_API_DISCONNECT  14 /* (int api) */
#define ANW_PERFORM_SET_BUFFERS_TIMESTAMP 15 /* (int64_t timestamp) */

/* API constants for connect/disconnect */
#define ANW_API_CPU    1
#define ANW_API_MEDIA  2
#define ANW_API_CAMERA 3

/* Query constants */
#define ANW_QUERY_WIDTH       0
#define ANW_QUERY_HEIGHT      1
#define ANW_QUERY_FORMAT      2
#define ANW_QUERY_MIN_UNDEQUEUED_BUFFERS 11

/* Typed perform helpers for the operations we actually use */
static inline int anw_connect(struct anw_priv *p) {
  anw_perform_fn fn = (anw_perform_fn)anw_vtable_slot(p->win, 5);
  return fn(p->win, ANW_PERFORM_API_CONNECT, ANW_API_CPU);
}

static inline int anw_disconnect(struct anw_priv *p) {
  anw_perform_fn fn = (anw_perform_fn)anw_vtable_slot(p->win, 5);
  return fn(p->win, ANW_PERFORM_API_DISCONNECT, ANW_API_CPU);
}

static inline int anw_set_buffer_count(struct anw_priv *p, int count) {
  anw_perform_fn fn = (anw_perform_fn)anw_vtable_slot(p->win, 5);
  return fn(p->win, ANW_PERFORM_SET_BUFFER_COUNT, count);
}

static inline int anw_set_buffers_format(struct anw_priv *p, int format) {
  anw_perform_fn fn = (anw_perform_fn)anw_vtable_slot(p->win, 5);
  return fn(p->win, ANW_PERFORM_SET_BUFFERS_FORMAT, format);
}

static inline int anw_set_usage(struct anw_priv *p, uint64_t usage) {
  anw_perform_fn fn = (anw_perform_fn)anw_vtable_slot(p->win, 5);
  return fn(p->win, ANW_PERFORM_SET_USAGE, usage);
}

#endif /* DS_WL_ANW_H */
