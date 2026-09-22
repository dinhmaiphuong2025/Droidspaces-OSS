// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * ANativeWindow/ANativeWindowBuffer hidden struct access (clean-room)
 *
 * Struct layouts derived from AOSP public headers (Apache-2.0):
 *   system/core/include/system/window.h
 *   system/core/include/cutils/native_handle.h
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

/* ----- android_native_base_t ----- */

struct anw_base {
  int32_t magic;
  int32_t version;
  void *reserved[4];
  void (*incref)(struct anw_base *);
  void (*decref)(struct anw_base *);
};

/* ----- ANativeWindowBuffer ----- */

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
  /* Pad to match AOSP sizeof; the exact tail size varies by pointer
   * width but we never access past `usage`. */
  void *reserved_end[8];
};

/* ----- ANativeWindow (full struct from window.h) ----- */

/*
 * This must match the AOSP ANativeWindow layout exactly so that
 * function pointer offsets are correct.  Fields before the vtable:
 *   common, flags, minSwapInterval, maxSwapInterval, xdpi, ydpi, oem[4]
 */
struct anw_window {
  struct anw_base common;

  /* informational / read-only fields */
  const uint32_t flags;
  const int min_swap_interval;
  const int max_swap_interval;
  const float xdpi;
  const float ydpi;
  intptr_t oem[4];

  /* vtable: function pointers in AOSP-defined order */
  int (*set_swap_interval)(struct anw_window *, int interval);
  int (*dequeue_deprecated)(struct anw_window *, struct anw_buffer **);
  int (*lock_deprecated)(struct anw_window *, struct anw_buffer *);
  int (*queue_deprecated)(struct anw_window *, struct anw_buffer *);
  int (*query)(const struct anw_window *, int what, int *value);
  int (*perform)(struct anw_window *, int operation, ...);
  int (*cancel_deprecated)(struct anw_window *, struct anw_buffer *);
  int (*dequeue_buffer)(struct anw_window *, struct anw_buffer **buf,
                        int *fence_fd);
  int (*queue_buffer)(struct anw_window *, struct anw_buffer *buf,
                      int fence_fd);
  int (*cancel_buffer)(struct anw_window *, struct anw_buffer *buf,
                       int fence_fd);
};

/* ----- Wrapper type ----- */

struct anw_priv {
  ANativeWindow *win;
};

/* Cast to our layout. Safe as long as the struct matches AOSP. */
static inline struct anw_window *anw_raw(struct anw_priv *p) {
  return (struct anw_window *)p->win;
}

/* ----- Buffer operations ----- */

static inline int anw_dequeue(struct anw_priv *p, struct anw_buffer **buf,
                              int *fence_fd) {
  return anw_raw(p)->dequeue_buffer(anw_raw(p), buf, fence_fd);
}

static inline int anw_queue(struct anw_priv *p, struct anw_buffer *buf,
                            int fence_fd) {
  return anw_raw(p)->queue_buffer(anw_raw(p), buf, fence_fd);
}

static inline int anw_cancel(struct anw_priv *p, struct anw_buffer *buf,
                             int fence_fd) {
  return anw_raw(p)->cancel_buffer(anw_raw(p), buf, fence_fd);
}

static inline int anw_query(struct anw_priv *p, int what, int *value) {
  return anw_raw(p)->query(anw_raw(p), what, value);
}

/* ----- Perform operation codes (from window.h) ----- */

#define ANW_PERFORM_API_CONNECT    13
#define ANW_PERFORM_API_DISCONNECT 14
#define ANW_PERFORM_SET_BUFFER_COUNT 4
#define ANW_PERFORM_SET_BUFFERS_FORMAT 9

/* API constants */
#define ANW_API_CPU 2

/* Query constants */
#define ANW_QUERY_MIN_UNDEQUEUED_BUFFERS 3

/* ----- Typed perform helpers ----- */

static inline int anw_connect(struct anw_priv *p) {
  return anw_raw(p)->perform(anw_raw(p), ANW_PERFORM_API_CONNECT, ANW_API_CPU);
}

static inline int anw_disconnect(struct anw_priv *p) {
  return anw_raw(p)->perform(anw_raw(p), ANW_PERFORM_API_DISCONNECT,
                             ANW_API_CPU);
}

static inline int anw_set_buffer_count(struct anw_priv *p, int count) {
  return anw_raw(p)->perform(anw_raw(p), ANW_PERFORM_SET_BUFFER_COUNT, count);
}

static inline int anw_set_buffers_format(struct anw_priv *p, int format) {
  return anw_raw(p)->perform(anw_raw(p), ANW_PERFORM_SET_BUFFERS_FORMAT,
                             format);
}

#endif /* DS_WL_ANW_H */
