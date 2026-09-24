// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - Seat Implementation
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS (1024 + 9)
#endif
#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0002
#endif

/* Minimal xkb keymap for clients */
static const char minimal_keymap[] =
    "xkb_keymap {\n"
    "    xkb_keycodes { include \"evdev+aliases(qwerty)\" };\n"
    "    xkb_types { include \"complete\" };\n"
    "    xkb_compat { include \"complete\" };\n"
    "    xkb_symbols { include \"pc+us+inet(evdev)\" };\n"
    "    xkb_geometry { include \"pc(pc105)\" };\n"
    "};\n";

static int create_keymap_fd(size_t *size_out) {
  /* strlen, not sizeof: Rust's CString::new in winit rejects trailing '\0' */
  size_t len = strlen(minimal_keymap);
  int fd = -1;

#if defined(__NR_memfd_create)
  fd = (int)syscall(__NR_memfd_create, "ds_keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#elif defined(SYS_memfd_create)
  fd = (int)syscall(SYS_memfd_create, "ds_keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#endif

  if (fd < 0) {
    char path[] = "/data/data/com.droidspaces.app/cache/ds_km_XXXXXX";
    fd = mkstemp(path);
    if (fd >= 0) unlink(path);
  }
  if (fd < 0) {
    char path[] = "/data/user/0/com.droidspaces.app/cache/ds_km_XXXXXX";
    fd = mkstemp(path);
    if (fd >= 0) unlink(path);
  }
  if (fd < 0) {
    DS_LOGE("Failed to create keymap fd: %s", strerror(errno));
    return -1;
  }

  if (ftruncate(fd, (off_t)len) < 0) {
    DS_LOGE("ftruncate keymap fd failed: %s", strerror(errno));
    close(fd);
    return -1;
  }

  void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    if (write(fd, minimal_keymap, len) != (ssize_t)len) {
      DS_LOGE("write keymap fd failed: %s", strerror(errno));
      close(fd);
      return -1;
    }
  } else {
    memcpy(p, minimal_keymap, len);
    munmap(p, len);
  }

#if defined(F_ADD_SEALS) && defined(F_SEAL_SHRINK)
  fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
#endif

  *size_out = len;
  DS_LOGI("Keymap fd created successfully: fd=%d, size=%zu", fd, len);
  return fd;
}

/* Resource destroy callbacks: unlink resources from the seat's lists
 * so we never dereference a freed resource. */

static void pointer_resource_destroy(struct wl_resource *resource) {
  wl_list_remove(wl_resource_get_link(resource));
}

static void keyboard_resource_destroy(struct wl_resource *resource) {
  wl_list_remove(wl_resource_get_link(resource));
}

static void touch_resource_destroy(struct wl_resource *resource) {
  wl_list_remove(wl_resource_get_link(resource));
}

static void seat_resource_destroy(struct wl_resource *resource) {
  wl_list_remove(wl_resource_get_link(resource));
}

static void pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
                               uint32_t serial, struct wl_resource *surface,
                               int32_t hotspot_x, int32_t hotspot_y) {
  (void)client; (void)resource; (void)serial; (void)hotspot_x; (void)hotspot_y;
  if (surface) {
    struct ds_surface *surf = wl_resource_get_user_data(surface);
    if (surf) {
      surf->role = DS_SURFACE_ROLE_CURSOR;
    }
  }
}

static void pointer_release(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static const struct wl_pointer_interface ds_pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release = pointer_release,
};

static void touch_release(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static const struct wl_touch_interface ds_touch_impl = {
    .release = touch_release,
};

static void keyboard_release(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface ds_keyboard_impl = {
    .release = keyboard_release,
};

/* Forward declaration: used by seat_get_pointer and seat_get_keyboard */
static struct ds_surface *get_target_surface(struct ds_server *server);

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource,
                             uint32_t id) {
  struct ds_seat *seat = wl_resource_get_user_data(resource);
  struct wl_resource *ptr = wl_resource_create(client, &wl_pointer_interface,
                                               wl_resource_get_version(resource), id);
  if (!ptr) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(ptr, &ds_pointer_impl, seat, pointer_resource_destroy);
  wl_list_insert(&seat->pointer_resources, wl_resource_get_link(ptr));
  DS_LOGI("seat: client bound pointer resource=%p", ptr);

  struct ds_surface *surf = get_target_surface(seat->server);
  if (surf && surf->resource && wl_resource_get_client(surf->resource) == client) {
    uint32_t serial = wl_display_next_serial(seat->server->display);
    wl_fixed_t fx = wl_fixed_from_double((double)seat->cursor_x);
    wl_fixed_t fy = wl_fixed_from_double((double)seat->cursor_y);
    wl_pointer_send_enter(ptr, serial, surf->resource, fx, fy);
    if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
      wl_pointer_send_frame(ptr);
    }
    seat->pointer_focus = surf;
    DS_LOGI("seat: immediate pointer focus entered surf=%p for client=%p", surf, client);
  }
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource,
                              uint32_t id) {
  struct ds_seat *seat = wl_resource_get_user_data(resource);
  struct wl_resource *kbd = wl_resource_create(client, &wl_keyboard_interface,
                                               wl_resource_get_version(resource), id);
  if (!kbd) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(kbd, &ds_keyboard_impl, seat, keyboard_resource_destroy);
  wl_list_insert(&seat->keyboard_resources, wl_resource_get_link(kbd));
  DS_LOGI("seat: client bound keyboard resource=%p", kbd);

  if (seat->keymap_fd < 0) {
    seat->keymap_fd = create_keymap_fd(&seat->keymap_size);
  }

  if (seat->keymap_fd >= 0) {
    wl_keyboard_send_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                            seat->keymap_fd, (uint32_t)seat->keymap_size);
  } else {
    DS_LOGE("CRITICAL: seat->keymap_fd < 0, keymap could not be sent to client!");
  }

  /* winit and Smithay require repeat_info on version >= 4; without it the
   * first key event triggers an unwrap-on-None panic inside winit. */
  if (wl_resource_get_version(kbd) >= 4) {
    wl_keyboard_send_repeat_info(kbd, 25, 600);
  }

  struct ds_surface *surf = get_target_surface(seat->server);
  if (surf && surf->resource && wl_resource_get_client(surf->resource) == client) {
    uint32_t serial = wl_display_next_serial(seat->server->display);
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(kbd, serial, surf->resource, &keys);
    wl_array_release(&keys);

    uint32_t mod_serial = wl_display_next_serial(seat->server->display);
    wl_keyboard_send_modifiers(kbd, mod_serial, 0, 0, 0, 0);
    seat->keyboard_focus = surf;
    DS_LOGI("seat: immediate keyboard focus entered surf=%p for client=%p", surf, client);
  }
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource,
                           uint32_t id) {
  struct ds_seat *seat = wl_resource_get_user_data(resource);
  struct wl_resource *tch = wl_resource_create(client, &wl_touch_interface,
                                               wl_resource_get_version(resource), id);
  if (!tch) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(tch, &ds_touch_impl, seat, touch_resource_destroy);
  wl_list_insert(&seat->touch_resources, wl_resource_get_link(tch));
  DS_LOGI("seat: client bound touch resource=%p", tch);
}

static void seat_release(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  DS_LOGW("seat: client requested seat_release for %p", resource);
  wl_resource_destroy(resource);
}

static const struct wl_seat_interface ds_seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  struct ds_seat *seat = data;
  struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface, version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(resource, &ds_seat_impl, seat, seat_resource_destroy);
  wl_list_insert(&seat->base_resources, wl_resource_get_link(resource));
  DS_LOGI("seat: client bound wl_seat resource=%p version=%u", resource, version);

  uint32_t caps = WL_SEAT_CAPABILITY_TOUCH | WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
  wl_seat_send_capabilities(resource, caps);

  if (version >= 2) {
    wl_seat_send_name(resource, "seat0");
  }
}

int ds_seat_init(struct ds_server *server) {
  struct ds_seat *seat = calloc(1, sizeof(*seat));
  if (!seat) return -1;

  seat->server = server;
  wl_list_init(&seat->base_resources);
  wl_list_init(&seat->pointer_resources);
  wl_list_init(&seat->keyboard_resources);
  wl_list_init(&seat->touch_resources);

  seat->keymap_fd = create_keymap_fd(&seat->keymap_size);

  seat->global = wl_global_create(server->display, &wl_seat_interface, 5,
                                  seat, seat_bind);
  if (!seat->global) {
    if (seat->keymap_fd >= 0) close(seat->keymap_fd);
    free(seat);
    return -1;
  }

  server->seat = seat;
  return 0;
}

/* Helper: monotonic time in milliseconds */
static uint32_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Focus management: pointer and keyboard track separate surfaces. When focus
 * changes, leave is sent on the old surface before enter on the new one. */

/* Helper: find the best surface to receive input. Prefers the currently active
 * surface (most recent buffer commit), falling back to the first available
 * surface if none is active yet. */
static struct ds_surface *get_target_surface(struct ds_server *server) {
  if (!server) return NULL;
  if (server->active_surface && server->active_surface->xdg_surf &&
      server->active_surface->role != DS_SURFACE_ROLE_CURSOR) {
    return server->active_surface;
  }
  struct ds_surface *surf;
  wl_list_for_each(surf, &server->surfaces, link) {
    if (surf->xdg_surf && surf->role != DS_SURFACE_ROLE_CURSOR && surf->is_mapped) {
      return surf;
    }
  }
  return NULL;
}

static void ensure_pointer_focus(struct ds_server *server) {
  struct ds_seat *seat = server->seat;
  struct ds_surface *surf = get_target_surface(server);
  if (!surf || !surf->resource) return;
  if (seat->pointer_focus == surf) return;

  uint32_t serial = wl_display_next_serial(server->display);

  /* Send leave to previous focus client */
  if (seat->pointer_focus && seat->pointer_focus->resource) {
    struct wl_client *old_client = wl_resource_get_client(seat->pointer_focus->resource);
    struct wl_resource *res;
    wl_resource_for_each(res, &seat->pointer_resources) {
      if (wl_resource_get_client(res) == old_client) {
        wl_pointer_send_leave(res, serial, seat->pointer_focus->resource);
        if (wl_resource_get_version(res) >= WL_POINTER_FRAME_SINCE_VERSION) {
          wl_pointer_send_frame(res);
        }
      }
    }
  }

  seat->pointer_focus = surf;
  struct wl_client *new_client = wl_resource_get_client(surf->resource);
  wl_fixed_t fx = wl_fixed_from_double((double)seat->cursor_x);
  wl_fixed_t fy = wl_fixed_from_double((double)seat->cursor_y);
  struct wl_resource *res;
  wl_resource_for_each(res, &seat->pointer_resources) {
    if (wl_resource_get_client(res) == new_client) {
      wl_pointer_send_enter(res, serial, surf->resource, fx, fy);
      if (wl_resource_get_version(res) >= WL_POINTER_FRAME_SINCE_VERSION) {
        wl_pointer_send_frame(res);
      }
    }
  }
  DS_LOGI("seat: pointer focus entered surf=%p at (%.1f, %.1f)", surf, seat->cursor_x, seat->cursor_y);
}

static void ensure_keyboard_focus(struct ds_server *server) {
  struct ds_seat *seat = server->seat;
  struct ds_surface *surf = get_target_surface(server);
  if (!surf || !surf->resource) return;
  if (seat->keyboard_focus == surf) return;

  uint32_t serial = wl_display_next_serial(server->display);

  /* Send leave to previous focus client */
  if (seat->keyboard_focus && seat->keyboard_focus->resource) {
    struct wl_client *old_client = wl_resource_get_client(seat->keyboard_focus->resource);
    struct wl_resource *res;
    wl_resource_for_each(res, &seat->keyboard_resources) {
      if (wl_resource_get_client(res) == old_client) {
        wl_keyboard_send_leave(res, serial, seat->keyboard_focus->resource);
      }
    }
  }

  seat->keyboard_focus = surf;
  struct wl_client *new_client = wl_resource_get_client(surf->resource);
  struct wl_array keys;
  wl_array_init(&keys);
  uint32_t mod_serial = wl_display_next_serial(server->display);

  struct wl_resource *res;
  wl_resource_for_each(res, &seat->keyboard_resources) {
    if (wl_resource_get_client(res) == new_client) {
      wl_keyboard_send_enter(res, serial, surf->resource, &keys);
      wl_keyboard_send_modifiers(res, mod_serial, 0, 0, 0, 0);
    }
  }
  wl_array_release(&keys);
  DS_LOGI("seat: keyboard focus entered surf=%p", surf);
}

/* Called from ds_compositor.c when a surface dies. Sends leave and resets
 * focus so the next surface gets a fresh enter. Must be called while
 * server->lock is held (surface_resource_destroy already holds it via
 * the event loop). */
void ds_seat_surface_destroyed(struct ds_server *server, struct ds_surface *surf) {
  if (!server || !server->seat || !surf) return;
  struct ds_seat *seat = server->seat;

  if (seat->pointer_focus == surf) {
    if (surf->resource) {
      struct wl_client *old_client = wl_resource_get_client(surf->resource);
      uint32_t serial = wl_display_next_serial(server->display);
      struct wl_resource *res;
      wl_resource_for_each(res, &seat->pointer_resources) {
        if (wl_resource_get_client(res) == old_client) {
          wl_pointer_send_leave(res, serial, surf->resource);
          if (wl_resource_get_version(res) >= WL_POINTER_FRAME_SINCE_VERSION) {
            wl_pointer_send_frame(res);
          }
        }
      }
    }
    seat->pointer_focus = NULL;
  }

  if (seat->keyboard_focus == surf) {
    if (surf->resource) {
      struct wl_client *old_client = wl_resource_get_client(surf->resource);
      uint32_t serial = wl_display_next_serial(server->display);
      struct wl_resource *res;
      wl_resource_for_each(res, &seat->keyboard_resources) {
        if (wl_resource_get_client(res) == old_client) {
          wl_keyboard_send_leave(res, serial, surf->resource);
        }
      }
    }
    seat->keyboard_focus = NULL;
  }
}

static void enqueue_input_event(struct ds_server *server, const struct ds_input_event *ev) {
  if (!server) return;
  pthread_mutex_lock(&server->input_lock);
  uint16_t next_head = (uint16_t)((server->input_head + 1) % 256);
  if (next_head != server->input_tail) {
    server->input_queue[server->input_head] = *ev;
    server->input_head = next_head;
  } else {
    DS_LOGW("seat: input_queue overflow, dropping event type=%d", ev->type);
  }
  pthread_mutex_unlock(&server->input_lock);

  if (server->input_eventfd >= 0) {
    uint64_t one = 1;
    write(server->input_eventfd, &one, sizeof(one));
  }
}

static void dispatch_touch_down(struct ds_server *server, int32_t id, float x, float y) {
  if (!server->seat) return;
  struct ds_surface *surf = get_target_surface(server);
  if (!surf || !surf->resource) return;

  struct wl_client *client = wl_resource_get_client(surf->resource);
  uint32_t serial = wl_display_next_serial(server->display);
  uint32_t time_ms = now_ms();
  wl_fixed_t fx = wl_fixed_from_double((double)x);
  wl_fixed_t fy = wl_fixed_from_double((double)y);

  struct wl_resource *res;
  wl_resource_for_each(res, &server->seat->touch_resources) {
    if (wl_resource_get_client(res) == client) {
      wl_touch_send_down(res, serial, time_ms, surf->resource, id, fx, fy);
    }
  }
}

static void dispatch_touch_motion(struct ds_server *server, int32_t id, float x, float y) {
  if (!server->seat) return;
  struct ds_surface *surf = get_target_surface(server);
  if (!surf || !surf->resource) return;

  struct wl_client *client = wl_resource_get_client(surf->resource);
  uint32_t time_ms = now_ms();
  wl_fixed_t fx = wl_fixed_from_double((double)x);
  wl_fixed_t fy = wl_fixed_from_double((double)y);

  struct wl_resource *res;
  wl_resource_for_each(res, &server->seat->touch_resources) {
    if (wl_resource_get_client(res) == client) {
      wl_touch_send_motion(res, time_ms, id, fx, fy);
    }
  }
}

static void dispatch_touch_up(struct ds_server *server, int32_t id) {
  if (!server->seat) return;
  struct ds_surface *surf = get_target_surface(server);
  if (!surf || !surf->resource) return;

  struct wl_client *client = wl_resource_get_client(surf->resource);
  uint32_t serial = wl_display_next_serial(server->display);
  uint32_t time_ms = now_ms();

  struct wl_resource *res;
  wl_resource_for_each(res, &server->seat->touch_resources) {
    if (wl_resource_get_client(res) == client) {
      wl_touch_send_up(res, serial, time_ms, id);
    }
  }
}

static void dispatch_touch_frame(struct ds_server *server) {
  if (!server->seat) return;
  struct ds_surface *surf = get_target_surface(server);
  if (!surf || !surf->resource) return;

  struct wl_client *client = wl_resource_get_client(surf->resource);
  struct wl_resource *res;
  wl_resource_for_each(res, &server->seat->touch_resources) {
    if (wl_resource_get_client(res) == client) {
      wl_touch_send_frame(res);
    }
  }
}

static void dispatch_pointer_motion(struct ds_server *server, float x, float y) {
  if (!server->seat) return;
  struct ds_surface *surf = get_target_surface(server);
  if (!surf || !surf->resource) return;

  server->seat->cursor_x = x;
  server->seat->cursor_y = y;
  ensure_pointer_focus(server);

  struct wl_client *client = wl_resource_get_client(surf->resource);
  uint32_t time_ms = now_ms();
  wl_fixed_t fx = wl_fixed_from_double((double)x);
  wl_fixed_t fy = wl_fixed_from_double((double)y);

  struct wl_resource *res;
  wl_resource_for_each(res, &server->seat->pointer_resources) {
    if (wl_resource_get_client(res) == client) {
      wl_pointer_send_motion(res, time_ms, fx, fy);
      if (wl_resource_get_version(res) >= WL_POINTER_FRAME_SINCE_VERSION) {
        wl_pointer_send_frame(res);
      }
    }
  }
}

static void dispatch_pointer_button(struct ds_server *server, uint32_t button, uint32_t state) {
  if (!server->seat) return;
  struct ds_surface *surf = get_target_surface(server);
  if (!surf || !surf->resource) return;

  ensure_pointer_focus(server);
  struct wl_client *client = wl_resource_get_client(surf->resource);
  uint32_t serial = wl_display_next_serial(server->display);
  uint32_t time_ms = now_ms();

  struct wl_resource *res;
  wl_resource_for_each(res, &server->seat->pointer_resources) {
    if (wl_resource_get_client(res) == client) {
      wl_pointer_send_button(res, serial, time_ms, button, state);
      if (wl_resource_get_version(res) >= WL_POINTER_FRAME_SINCE_VERSION) {
        wl_pointer_send_frame(res);
      }
    }
  }
}

static void dispatch_pointer_axis(struct ds_server *server, uint32_t axis, float value) {
  if (!server->seat) return;
  struct ds_surface *surf = get_target_surface(server);
  if (!surf || !surf->resource) return;

  ensure_pointer_focus(server);
  struct wl_client *client = wl_resource_get_client(surf->resource);
  uint32_t time_ms = now_ms();
  wl_fixed_t fval = wl_fixed_from_double((double)value);

  struct wl_resource *res;
  wl_resource_for_each(res, &server->seat->pointer_resources) {
    if (wl_resource_get_client(res) == client) {
      wl_pointer_send_axis(res, time_ms, axis, fval);
      if (wl_resource_get_version(res) >= WL_POINTER_FRAME_SINCE_VERSION) {
        wl_pointer_send_frame(res);
      }
    }
  }
}

static void dispatch_key(struct ds_server *server, uint32_t key, uint32_t state) {
  if (!server->seat) return;
  struct ds_surface *surf = get_target_surface(server);
  if (!surf || !surf->resource) return;

  ensure_keyboard_focus(server);
  struct wl_client *client = wl_resource_get_client(surf->resource);
  uint32_t serial = wl_display_next_serial(server->display);
  uint32_t time_ms = now_ms();

  struct wl_resource *res;
  wl_resource_for_each(res, &server->seat->keyboard_resources) {
    if (wl_resource_get_client(res) == client) {
      wl_keyboard_send_key(res, serial, time_ms, key, state);
    }
  }
}

void ds_seat_dispatch_queue(struct ds_server *server) {
  if (!server) return;

  struct ds_input_event batch[32];
  while (1) {
    int count = 0;
    pthread_mutex_lock(&server->input_lock);
    while (server->input_tail != server->input_head && count < 32) {
      batch[count++] = server->input_queue[server->input_tail];
      server->input_tail = (uint16_t)((server->input_tail + 1) % 256);
    }
    pthread_mutex_unlock(&server->input_lock);

    if (count == 0) break;

    for (int i = 0; i < count; i++) {
      switch (batch[i].type) {
        case DS_INPUT_KEY:
          dispatch_key(server, batch[i].key.key, batch[i].key.state);
          break;
        case DS_INPUT_POINTER_MOTION:
          dispatch_pointer_motion(server, batch[i].pointer_motion.x, batch[i].pointer_motion.y);
          break;
        case DS_INPUT_POINTER_BUTTON:
          dispatch_pointer_button(server, batch[i].pointer_button.button, batch[i].pointer_button.state);
          break;
        case DS_INPUT_POINTER_AXIS:
          dispatch_pointer_axis(server, batch[i].pointer_axis.axis, batch[i].pointer_axis.value);
          break;
        case DS_INPUT_TOUCH_DOWN:
          dispatch_touch_down(server, batch[i].touch.id, batch[i].touch.x, batch[i].touch.y);
          break;
        case DS_INPUT_TOUCH_MOTION:
          dispatch_touch_motion(server, batch[i].touch.id, batch[i].touch.x, batch[i].touch.y);
          break;
        case DS_INPUT_TOUCH_UP:
          dispatch_touch_up(server, batch[i].touch.id);
          break;
        case DS_INPUT_TOUCH_FRAME:
          dispatch_touch_frame(server);
          break;
        case DS_INPUT_RESIZE:
          server->width = batch[i].resize.width;
          server->height = batch[i].resize.height;
          if (server->output) {
            server->output->width = batch[i].resize.width;
            server->output->height = batch[i].resize.height;
            ds_output_send_current_mode(server->output);
          }
          ds_xdg_shell_resize_all(server);
          break;
      }
    }
  }

  wl_display_flush_clients(server->display);
}

/* JNI producers enqueue events for safe dispatch on the server event loop */
void ds_seat_send_touch_down(struct ds_server *server, int32_t id, float x, float y) {
  struct ds_input_event ev = {
    .type = DS_INPUT_TOUCH_DOWN,
    .touch = { .id = id, .x = x, .y = y }
  };
  enqueue_input_event(server, &ev);
}

void ds_seat_send_touch_motion(struct ds_server *server, int32_t id, float x, float y) {
  struct ds_input_event ev = {
    .type = DS_INPUT_TOUCH_MOTION,
    .touch = { .id = id, .x = x, .y = y }
  };
  enqueue_input_event(server, &ev);
}

void ds_seat_send_touch_up(struct ds_server *server, int32_t id) {
  struct ds_input_event ev = {
    .type = DS_INPUT_TOUCH_UP,
    .touch = { .id = id }
  };
  enqueue_input_event(server, &ev);
}

void ds_seat_send_touch_frame(struct ds_server *server) {
  struct ds_input_event ev = { .type = DS_INPUT_TOUCH_FRAME };
  enqueue_input_event(server, &ev);
}

void ds_seat_send_pointer_motion(struct ds_server *server, float x, float y, float dx, float dy) {
  struct ds_input_event ev = {
    .type = DS_INPUT_POINTER_MOTION,
    .pointer_motion = { .x = x, .y = y, .dx = dx, .dy = dy }
  };
  enqueue_input_event(server, &ev);
}

void ds_seat_send_pointer_button(struct ds_server *server, uint32_t button, uint32_t state) {
  struct ds_input_event ev = {
    .type = DS_INPUT_POINTER_BUTTON,
    .pointer_button = { .button = button, .state = state }
  };
  enqueue_input_event(server, &ev);
}

void ds_seat_send_pointer_axis(struct ds_server *server, uint32_t axis, float value) {
  struct ds_input_event ev = {
    .type = DS_INPUT_POINTER_AXIS,
    .pointer_axis = { .axis = axis, .value = value }
  };
  enqueue_input_event(server, &ev);
}

void ds_seat_send_key(struct ds_server *server, uint32_t key, uint32_t state) {
  struct ds_input_event ev = {
    .type = DS_INPUT_KEY,
    .key = { .key = key, .state = state }
  };
  enqueue_input_event(server, &ev);
}

void ds_server_enqueue_resize(struct ds_server *server, int width, int height) {
  struct ds_input_event ev = {
    .type = DS_INPUT_RESIZE,
    .resize = { .width = width, .height = height }
  };
  enqueue_input_event(server, &ev);
}
