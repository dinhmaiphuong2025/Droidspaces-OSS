// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server (Clean-Room Implementation)
 * Copyright (C) 2026 Droidspaces contributors
 */

#ifndef DS_SERVER_H
#define DS_SERVER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#define DS_LOG_TAG "DsWaylandServer"
#define DS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, DS_LOG_TAG, __VA_ARGS__)
#define DS_LOGW(...) __android_log_print(ANDROID_LOG_WARN, DS_LOG_TAG, __VA_ARGS__)
#define DS_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, DS_LOG_TAG, __VA_ARGS__)
#else
typedef void ANativeWindow;
typedef void AHardwareBuffer;
static inline void AHardwareBuffer_release(AHardwareBuffer *b) { (void)b; }
#define DS_LOG_TAG "DsWaylandServer"
#define DS_LOGI(...) do { printf("[INFO] " __VA_ARGS__); printf("\n"); } while(0)
#define DS_LOGW(...) do { printf("[WARN] " __VA_ARGS__); printf("\n"); } while(0)
#define DS_LOGE(...) do { printf("[ERROR] " __VA_ARGS__); printf("\n"); } while(0)
#endif

#include "linux-dmabuf-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "wayland-server-core.h"
#include "wayland-server-protocol.h"
#include "wayland-server.h"
#include "xdg-shell-server-protocol.h"

#define DRM_FORMAT_ARGB8888 0x34325241
#define DRM_FORMAT_XRGB8888 0x34325258
#define DRM_FORMAT_ABGR8888 0x34324241
#define DRM_FORMAT_XBGR8888 0x34324258

struct ds_server;
struct ds_surface;

/* Buffer representation */
struct ds_buffer {
  struct wl_resource *resource;
  AHardwareBuffer *ahwb;
  struct wl_shm_buffer *shm;
  int dmabuf_fds[4];
  int dmabuf_num_planes;
  uint32_t dmabuf_offsets[4];
  uint32_t dmabuf_strides[4];
  uint64_t dmabuf_modifiers[4];
  int width;
  int height;
  int stride;
  uint32_t format;
  int is_dmabuf;
  int is_shm;
  struct ds_surface *surface;
  struct wl_listener destroy_listener;
};

/* Surface roles */
enum ds_surface_role {
  DS_SURFACE_ROLE_NONE = 0,
  DS_SURFACE_ROLE_TOPLEVEL,
  DS_SURFACE_ROLE_CURSOR,
  DS_SURFACE_ROLE_SUBSURFACE,
};

/* Surface representation */
struct ds_surface {
  struct wl_resource *resource;
  struct ds_server *server;
  enum ds_surface_role role;

  struct ds_buffer *pending_buffer;
  struct ds_buffer *current_buffer;
  int32_t pending_sx;
  int32_t pending_sy;

  struct wl_list frame_callback_list;
  struct wl_listener destroy_listener;

  struct ds_xdg_surface *xdg_surf;
  int width;
  int height;
  int is_mapped;
  struct wl_list link;
};

/* Frame callback wrapper */
struct ds_frame_callback {
  struct wl_resource *resource;
  struct wl_list link;
};

/* XDG Shell surface */
struct ds_xdg_surface {
  struct wl_resource *resource;
  struct ds_surface *surface;
  struct ds_xdg_toplevel *toplevel;
  uint32_t last_serial;
  int configured;
};

/* XDG Toplevel */
struct ds_xdg_toplevel {
  struct wl_resource *resource;
  struct ds_xdg_surface *xdg_surf;
};

/* Output definition */
struct ds_output {
  struct ds_server *server;
  struct wl_global *global;
  int width;
  int height;
  int refresh_mhz;
  int scale;
};

/* Seat definition (Touch, Pointer, Keyboard) */
struct ds_seat {
  struct ds_server *server;
  struct wl_global *global;
  struct wl_resource *seat_resource;

  struct wl_resource *pointer_resource;
  struct wl_resource *touch_resource;
  struct wl_resource *keyboard_resource;

  int keymap_fd;
  size_t keymap_size;

  int pointer_entered;
  int keyboard_entered;
  struct ds_surface *pointer_focus;
  struct ds_surface *keyboard_focus;

  float cursor_x;
  float cursor_y;
};

/* Main Server context */
struct ds_server {
  struct wl_display *display;
  struct wl_event_loop *loop;
  pthread_t loop_thread;
  atomic_int running;

  pthread_mutex_t lock;
  ANativeWindow *window;
  char sock_dir[256];
  int width;
  int height;
  int refresh_mhz;

  struct ds_output *output;
  struct ds_seat *seat;

  struct wl_list surfaces;
  struct ds_surface *active_surface;

  /* Globals */
  struct wl_global *compositor_global;
  struct wl_global *subcompositor_global;
  struct wl_global *xdg_wm_base_global;
  struct wl_global *dmabuf_global;
  struct wl_global *viewporter_global;

  /* Thread-safe input dispatch */
  int input_eventfd;
  struct wl_event_source *input_source;
  pthread_mutex_t input_lock;
  struct ds_input_event input_queue[256];
  uint16_t input_head;
  uint16_t input_tail;
};

enum ds_input_type {
  DS_INPUT_KEY = 1,
  DS_INPUT_POINTER_MOTION,
  DS_INPUT_POINTER_BUTTON,
  DS_INPUT_POINTER_AXIS,
  DS_INPUT_TOUCH_DOWN,
  DS_INPUT_TOUCH_MOTION,
  DS_INPUT_TOUCH_UP,
  DS_INPUT_TOUCH_FRAME,
};

struct ds_input_event {
  enum ds_input_type type;
  union {
    struct { uint32_t key; uint32_t state; } key;
    struct { float x; float y; float dx; float dy; } pointer_motion;
    struct { uint32_t button; uint32_t state; } pointer_button;
    struct { uint32_t axis; float value; } pointer_axis;
    struct { int32_t id; float x; float y; } touch;
  };
};

void ds_seat_dispatch_queue(struct ds_server *server);

/* Subsystem initializers */
int ds_compositor_init(struct ds_server *server);
int ds_xdg_shell_init(struct ds_server *server);
int ds_dmabuf_init(struct ds_server *server);
int ds_output_init(struct ds_server *server, int width, int height, int refresh_mhz);
int ds_seat_init(struct ds_server *server);
int ds_viewporter_init(struct ds_server *server);

/* SHM buffer adapter (ds_shm.c) */
struct ds_buffer *ds_shm_wrap_buffer(struct wl_resource *buffer_resource);
void ds_shm_free_buffer(struct ds_buffer *buf);

/* Presenter API */
void ds_presenter_init(struct ds_server *server);
void ds_presenter_present_surface(struct ds_server *server, struct ds_surface *surf);
void ds_presenter_detach(struct ds_server *server);

/* Server window lifecycle: the display and its clients survive surface
 * changes, only the native window and EGL surface are swapped. */
void ds_server_attach_window(struct ds_server *server, ANativeWindow *win,
                             int width, int height);
void ds_server_detach_window(struct ds_server *server);
void ds_xdg_shell_resize_all(struct ds_server *server);

/* Input API (called from JNI) */
void ds_seat_send_touch_down(struct ds_server *server, int32_t id, float x, float y);
void ds_seat_send_touch_motion(struct ds_server *server, int32_t id, float x, float y);
void ds_seat_send_touch_up(struct ds_server *server, int32_t id);
void ds_seat_send_touch_frame(struct ds_server *server);

void ds_seat_send_pointer_motion(struct ds_server *server, float x, float y, float dx, float dy);
void ds_seat_send_pointer_button(struct ds_server *server, uint32_t button, uint32_t state);
void ds_seat_send_pointer_axis(struct ds_server *server, uint32_t axis, float value);

void ds_seat_send_key(struct ds_server *server, uint32_t key, uint32_t state);

/* Notify seat that a surface was destroyed so it can send leave and reset focus */
void ds_seat_surface_destroyed(struct ds_server *server, struct ds_surface *surf);

#endif /* DS_SERVER_H */
