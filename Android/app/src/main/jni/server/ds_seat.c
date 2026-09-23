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

/* Resource destroy callbacks: null out the seat's pointer so we never
 * dereference a freed resource from the JNI input path. */

static void pointer_resource_destroy(struct wl_resource *resource) {
  struct ds_seat *seat = wl_resource_get_user_data(resource);
  DS_LOGW("seat: pointer_resource_destroy called for %p (seat->pointer_resource=%p)", resource, seat ? seat->pointer_resource : NULL);
  if (seat && seat->pointer_resource == resource) {
    seat->pointer_resource = NULL;
    seat->pointer_entered = 0;
    seat->pointer_focus = NULL;
  }
}

static void keyboard_resource_destroy(struct wl_resource *resource) {
  struct ds_seat *seat = wl_resource_get_user_data(resource);
  DS_LOGW("seat: keyboard_resource_destroy called for %p (seat->keyboard_resource=%p)", resource, seat ? seat->keyboard_resource : NULL);
  if (seat && seat->keyboard_resource == resource) {
    seat->keyboard_resource = NULL;
    seat->keyboard_entered = 0;
    seat->keyboard_focus = NULL;
  }
}

static void touch_resource_destroy(struct wl_resource *resource) {
  struct ds_seat *seat = wl_resource_get_user_data(resource);
  DS_LOGW("seat: touch_resource_destroy called for %p (seat->touch_resource=%p)", resource, seat ? seat->touch_resource : NULL);
  if (seat && seat->touch_resource == resource)
    seat->touch_resource = NULL;
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
  seat->pointer_resource = ptr;
  DS_LOGI("seat: client bound pointer resource=%p", ptr);
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
  seat->keyboard_resource = kbd;
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
  seat->touch_resource = tch;
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

static void seat_resource_destroy(struct wl_resource *resource) {
  struct ds_seat *seat = wl_resource_get_user_data(resource);
  DS_LOGW("seat: seat_resource_destroy called for %p (seat->seat_resource=%p)", resource, seat ? seat->seat_resource : NULL);
  if (seat && seat->seat_resource == resource) {
    seat->seat_resource = NULL;
  }
}

static void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  struct ds_seat *seat = data;
  struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface, version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(resource, &ds_seat_impl, seat, seat_resource_destroy);
  seat->seat_resource = resource;
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

/* Helper: send pointer frame only when the resource supports it */
static void send_pointer_frame(struct ds_seat *seat) {
  if (seat->pointer_resource &&
      wl_resource_get_version(seat->pointer_resource) >= WL_POINTER_FRAME_SINCE_VERSION) {
    wl_pointer_send_frame(seat->pointer_resource);
  }
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
  if (!seat->pointer_resource || !surf) return;
  if (seat->pointer_entered && seat->pointer_focus == surf) return;

  /* Leave old surface first */
  if (seat->pointer_entered && seat->pointer_focus) {
    uint32_t serial = wl_display_next_serial(server->display);
    wl_pointer_send_leave(seat->pointer_resource, serial, seat->pointer_focus->resource);
    send_pointer_frame(seat);
  }

  uint32_t serial = wl_display_next_serial(server->display);
  wl_fixed_t fx = wl_fixed_from_double((double)seat->cursor_x);
  wl_fixed_t fy = wl_fixed_from_double((double)seat->cursor_y);
  wl_pointer_send_enter(seat->pointer_resource, serial, surf->resource, fx, fy);
  send_pointer_frame(seat);
  seat->pointer_entered = 1;
  seat->pointer_focus = surf;
  DS_LOGI("seat: pointer focus entered surf=%p at (%.1f, %.1f)", surf, seat->cursor_x, seat->cursor_y);
}

static void ensure_keyboard_focus(struct ds_server *server) {
  struct ds_seat *seat = server->seat;
  struct ds_surface *surf = get_target_surface(server);
  if (!seat->keyboard_resource || !surf) return;
  if (seat->keyboard_entered && seat->keyboard_focus == surf) return;

  /* Leave old surface first */
  if (seat->keyboard_entered && seat->keyboard_focus) {
    uint32_t serial = wl_display_next_serial(server->display);
    wl_keyboard_send_leave(seat->keyboard_resource, serial, seat->keyboard_focus->resource);
  }

  uint32_t serial = wl_display_next_serial(server->display);
  struct wl_array keys;
  wl_array_init(&keys);
  wl_keyboard_send_enter(seat->keyboard_resource, serial, surf->resource, &keys);
  wl_array_release(&keys);

  /* Protocol requires modifiers immediately after enter so clients can
   * initialize their xkb state. Without this, winit panics. */
  uint32_t mod_serial = wl_display_next_serial(server->display);
  wl_keyboard_send_modifiers(seat->keyboard_resource, mod_serial, 0, 0, 0, 0);

  seat->keyboard_entered = 1;
  seat->keyboard_focus = surf;
  DS_LOGI("seat: keyboard focus entered surf=%p", surf);
}

/* Called from ds_compositor.c when a surface dies. Sends leave and resets
 * focus so the next surface gets a fresh enter. Must be called while
 * server->lock is held (surface_resource_destroy already holds it via
 * the event loop). */
void ds_seat_surface_destroyed(struct ds_server *server, struct ds_surface *surf) {
  if (!server || !server->seat) return;
  struct ds_seat *seat = server->seat;

  if (seat->pointer_focus == surf) {
    if (seat->pointer_resource && seat->pointer_entered) {
      uint32_t serial = wl_display_next_serial(server->display);
      wl_pointer_send_leave(seat->pointer_resource, serial, surf->resource);
      send_pointer_frame(seat);
    }
    seat->pointer_focus = NULL;
    seat->pointer_entered = 0;
  }

  if (seat->keyboard_focus == surf) {
    if (seat->keyboard_resource && seat->keyboard_entered) {
      uint32_t serial = wl_display_next_serial(server->display);
      wl_keyboard_send_leave(seat->keyboard_resource, serial, surf->resource);
    }
    seat->keyboard_focus = NULL;
    seat->keyboard_entered = 0;
  }
}

/* Touch dispatch */
void ds_seat_send_touch_down(struct ds_server *server, int32_t id, float x, float y) {
  if (!server || !server->seat) return;
  if (!server->seat->touch_resource) {
    DS_LOGW("seat: touch_down dropped: no touch_resource bound");
    return;
  }
  struct ds_surface *surf = get_target_surface(server);
  if (!surf) {
    DS_LOGW("seat: touch_down dropped: no target surface");
    return;
  }
  DS_LOGI("seat: send_touch_down id=%d at (%.1f, %.1f)", id, x, y);

  pthread_mutex_lock(&server->lock);
  uint32_t serial = wl_display_next_serial(server->display);
  uint32_t time_ms = now_ms();

  wl_fixed_t fx = wl_fixed_from_double((double)x);
  wl_fixed_t fy = wl_fixed_from_double((double)y);

  wl_touch_send_down(server->seat->touch_resource, serial, time_ms,
                     surf->resource, id, fx, fy);
  pthread_mutex_unlock(&server->lock);
  wl_display_flush_clients(server->display);
}

void ds_seat_send_touch_motion(struct ds_server *server, int32_t id, float x, float y) {
  if (!server || !server->seat || !server->seat->touch_resource) return;

  pthread_mutex_lock(&server->lock);
  uint32_t time_ms = now_ms();

  wl_fixed_t fx = wl_fixed_from_double((double)x);
  wl_fixed_t fy = wl_fixed_from_double((double)y);

  wl_touch_send_motion(server->seat->touch_resource, time_ms, id, fx, fy);
  pthread_mutex_unlock(&server->lock);
  wl_display_flush_clients(server->display);
}

void ds_seat_send_touch_up(struct ds_server *server, int32_t id) {
  if (!server || !server->seat || !server->seat->touch_resource) return;

  pthread_mutex_lock(&server->lock);
  uint32_t serial = wl_display_next_serial(server->display);
  uint32_t time_ms = now_ms();

  wl_touch_send_up(server->seat->touch_resource, serial, time_ms, id);
  pthread_mutex_unlock(&server->lock);
  wl_display_flush_clients(server->display);
}

void ds_seat_send_touch_frame(struct ds_server *server) {
  if (!server || !server->seat || !server->seat->touch_resource) return;
  pthread_mutex_lock(&server->lock);
  wl_touch_send_frame(server->seat->touch_resource);
  pthread_mutex_unlock(&server->lock);
  wl_display_flush_clients(server->display);
}

/* Pointer dispatch */
void ds_seat_send_pointer_motion(struct ds_server *server, float x, float y, float dx, float dy) {
  (void)dx; (void)dy;
  if (!server || !server->seat || !server->seat->pointer_resource) return;
  if (!get_target_surface(server)) return;

  pthread_mutex_lock(&server->lock);
  server->seat->cursor_x = x;
  server->seat->cursor_y = y;
  ensure_pointer_focus(server);

  uint32_t time_ms = now_ms();
  wl_fixed_t fx = wl_fixed_from_double((double)x);
  wl_fixed_t fy = wl_fixed_from_double((double)y);

  wl_pointer_send_motion(server->seat->pointer_resource, time_ms, fx, fy);
  send_pointer_frame(server->seat);
  pthread_mutex_unlock(&server->lock);
  wl_display_flush_clients(server->display);
}

void ds_seat_send_pointer_button(struct ds_server *server, uint32_t button, uint32_t state) {
  if (!server || !server->seat) return;
  if (!server->seat->pointer_resource) {
    DS_LOGW("seat: pointer_button dropped: no pointer_resource bound");
    return;
  }
  if (!get_target_surface(server)) {
    DS_LOGW("seat: pointer_button dropped: no target surface");
    return;
  }
  DS_LOGI("seat: send_pointer_button btn=0x%x, state=%u", button, state);

  pthread_mutex_lock(&server->lock);
  ensure_pointer_focus(server);

  uint32_t serial = wl_display_next_serial(server->display);
  uint32_t time_ms = now_ms();

  wl_pointer_send_button(server->seat->pointer_resource, serial, time_ms, button, state);
  send_pointer_frame(server->seat);
  pthread_mutex_unlock(&server->lock);
  wl_display_flush_clients(server->display);
}

void ds_seat_send_pointer_axis(struct ds_server *server, uint32_t axis, float value) {
  if (!server || !server->seat || !server->seat->pointer_resource) return;
  if (!get_target_surface(server)) return;

  pthread_mutex_lock(&server->lock);
  ensure_pointer_focus(server);

  uint32_t time_ms = now_ms();
  wl_fixed_t fval = wl_fixed_from_double((double)value);
  wl_pointer_send_axis(server->seat->pointer_resource, time_ms, axis, fval);
  send_pointer_frame(server->seat);
  pthread_mutex_unlock(&server->lock);
  wl_display_flush_clients(server->display);
}

/* Keyboard dispatch */
void ds_seat_send_key(struct ds_server *server, uint32_t key, uint32_t state) {
  if (!server || !server->seat) return;
  if (!server->seat->keyboard_resource) {
    DS_LOGW("seat: send_key dropped: no keyboard_resource bound");
    return;
  }
  if (!get_target_surface(server)) {
    DS_LOGW("seat: send_key dropped: no target surface");
    return;
  }
  DS_LOGI("seat: send_key key=%u, state=%u", key, state);

  pthread_mutex_lock(&server->lock);
  ensure_keyboard_focus(server);

  uint32_t serial = wl_display_next_serial(server->display);
  uint32_t time_ms = now_ms();

  wl_keyboard_send_key(server->seat->keyboard_resource, serial, time_ms, key, state);
  pthread_mutex_unlock(&server->lock);

  wl_display_flush_clients(server->display);
}
