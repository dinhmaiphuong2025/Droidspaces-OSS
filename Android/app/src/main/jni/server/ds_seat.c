// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - Seat Implementation
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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
  size_t len = sizeof(minimal_keymap);
#if defined(HAVE_MEMFD_CREATE)
  int fd = memfd_create("ds_keymap", MFD_CLOEXEC);
#else
  int fd = -1;
#endif
  if (fd < 0) {
    char path[] = "/data/local/tmp/ds_km_XXXXXX";
    fd = mkstemp(path);
    if (fd >= 0) unlink(path);
  }
  if (fd < 0) return -1;

  if (write(fd, minimal_keymap, len) != (ssize_t)len) {
    close(fd);
    return -1;
  }
  *size_out = len;
  return fd;
}

static void pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
                               uint32_t serial, struct wl_resource *surface,
                               int32_t hotspot_x, int32_t hotspot_y) {
  (void)client; (void)resource; (void)serial; (void)surface; (void)hotspot_x; (void)hotspot_y;
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
  wl_resource_set_implementation(ptr, &ds_pointer_impl, seat, NULL);
  seat->pointer_resource = ptr;
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
  wl_resource_set_implementation(kbd, &ds_keyboard_impl, seat, NULL);
  seat->keyboard_resource = kbd;

  /* Send keymap to keyboard client */
  if (seat->keymap_fd >= 0) {
    wl_keyboard_send_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                            seat->keymap_fd, (uint32_t)seat->keymap_size);
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
  wl_resource_set_implementation(tch, &ds_touch_impl, seat, NULL);
  seat->touch_resource = tch;
}

static void seat_release(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
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

  wl_resource_set_implementation(resource, &ds_seat_impl, seat, NULL);
  seat->seat_resource = resource;

  /* Advertise capabilities: Touch + Pointer + Keyboard */
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

/* Touch dispatch */
void ds_seat_send_touch_down(struct ds_server *server, int32_t id, float x, float y) {
  if (!server || !server->seat || !server->seat->touch_resource) return;
  if (!server->active_surface) return;

  uint32_t serial = wl_display_next_serial(server->display);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t time_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

  wl_fixed_t fx = wl_fixed_from_double((double)x);
  wl_fixed_t fy = wl_fixed_from_double((double)y);

  wl_touch_send_down(server->seat->touch_resource, serial, time_ms,
                     server->active_surface->resource, id, fx, fy);
}

void ds_seat_send_touch_motion(struct ds_server *server, int32_t id, float x, float y) {
  if (!server || !server->seat || !server->seat->touch_resource) return;

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t time_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

  wl_fixed_t fx = wl_fixed_from_double((double)x);
  wl_fixed_t fy = wl_fixed_from_double((double)y);

  wl_touch_send_motion(server->seat->touch_resource, time_ms, id, fx, fy);
}

void ds_seat_send_touch_up(struct ds_server *server, int32_t id) {
  if (!server || !server->seat || !server->seat->touch_resource) return;

  uint32_t serial = wl_display_next_serial(server->display);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t time_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

  wl_touch_send_up(server->seat->touch_resource, serial, time_ms, id);
}

void ds_seat_send_touch_frame(struct ds_server *server) {
  if (!server || !server->seat || !server->seat->touch_resource) return;
  wl_touch_send_frame(server->seat->touch_resource);
}

/* Clients ignore pointer and keyboard traffic until enter assigns focus,
 * so latch focus here instead of trusting the UI layer to order it. */
static void ensure_pointer_focus(struct ds_server *server) {
  struct ds_seat *seat = server->seat;
  struct ds_surface *surf = server->active_surface;
  if (!seat->pointer_resource || !surf) return;
  if (seat->pointer_entered && seat->focus_surface == surf) return;

  uint32_t serial = wl_display_next_serial(server->display);
  wl_fixed_t fx = wl_fixed_from_double((double)seat->cursor_x);
  wl_fixed_t fy = wl_fixed_from_double((double)seat->cursor_y);
  wl_pointer_send_enter(seat->pointer_resource, serial, surf->resource, fx, fy);
  seat->pointer_entered = 1;
  seat->focus_surface = surf;
}

static void ensure_keyboard_focus(struct ds_server *server) {
  struct ds_seat *seat = server->seat;
  struct ds_surface *surf = server->active_surface;
  if (!seat->keyboard_resource || !surf) return;
  if (seat->keyboard_entered && seat->focus_surface == surf) return;

  uint32_t serial = wl_display_next_serial(server->display);
  struct wl_array keys;
  wl_array_init(&keys);
  wl_keyboard_send_enter(seat->keyboard_resource, serial, surf->resource, &keys);
  wl_array_release(&keys);
  seat->keyboard_entered = 1;
  seat->focus_surface = surf;
}

/* Pointer dispatch */
void ds_seat_send_pointer_motion(struct ds_server *server, float x, float y, float dx, float dy) {
  (void)dx; (void)dy;
  if (!server || !server->seat || !server->seat->pointer_resource) return;
  if (!server->active_surface) return;

  server->seat->cursor_x = x;
  server->seat->cursor_y = y;
  ensure_pointer_focus(server);

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t time_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

  wl_fixed_t fx = wl_fixed_from_double((double)x);
  wl_fixed_t fy = wl_fixed_from_double((double)y);

  wl_pointer_send_motion(server->seat->pointer_resource, time_ms, fx, fy);
  wl_pointer_send_frame(server->seat->pointer_resource);
}

void ds_seat_send_pointer_button(struct ds_server *server, uint32_t button, uint32_t state) {
  if (!server || !server->seat || !server->seat->pointer_resource) return;
  if (!server->active_surface) return;
  ensure_pointer_focus(server);

  uint32_t serial = wl_display_next_serial(server->display);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t time_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

  wl_pointer_send_button(server->seat->pointer_resource, serial, time_ms, button, state);
  wl_pointer_send_frame(server->seat->pointer_resource);
}

void ds_seat_send_pointer_axis(struct ds_server *server, uint32_t axis, float value) {
  if (!server || !server->seat || !server->seat->pointer_resource) return;

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t time_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

  wl_fixed_t fval = wl_fixed_from_double((double)value);
  wl_pointer_send_axis(server->seat->pointer_resource, time_ms, axis, fval);
  wl_pointer_send_frame(server->seat->pointer_resource);
}

/* Keyboard dispatch */
void ds_seat_send_key(struct ds_server *server, uint32_t key, uint32_t state) {
  if (!server || !server->seat || !server->seat->keyboard_resource) return;
  if (!server->active_surface) return;
  ensure_keyboard_focus(server);

  uint32_t serial = wl_display_next_serial(server->display);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t time_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

  wl_keyboard_send_key(server->seat->keyboard_resource, serial, time_ms, key, state);
}
