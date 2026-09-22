// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - XDG Shell Implementation
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
static const struct xdg_surface_interface ds_xdg_surface_impl;
static const struct xdg_toplevel_interface ds_xdg_toplevel_impl;

/* XDG Toplevel implementation */
static void toplevel_destroy(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void toplevel_set_parent(struct wl_client *client, struct wl_resource *resource,
                                struct wl_resource *parent) {
  (void)client; (void)resource; (void)parent;
}

static void toplevel_set_title(struct wl_client *client, struct wl_resource *resource,
                               const char *title) {
  (void)client; (void)resource;
  DS_LOGI("xdg_toplevel title: %s", title ? title : "(null)");
}

static void toplevel_set_app_id(struct wl_client *client, struct wl_resource *resource,
                                const char *app_id) {
  (void)client; (void)resource;
  DS_LOGI("xdg_toplevel app_id: %s", app_id ? app_id : "(null)");
}

static void toplevel_show_window_menu(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *seat, uint32_t serial,
                                      int32_t x, int32_t y) {
  (void)client; (void)resource; (void)seat; (void)serial; (void)x; (void)y;
}

static void toplevel_move(struct wl_client *client, struct wl_resource *resource,
                          struct wl_resource *seat, uint32_t serial) {
  (void)client; (void)resource; (void)seat; (void)serial;
}

static void toplevel_resize(struct wl_client *client, struct wl_resource *resource,
                            struct wl_resource *seat, uint32_t serial, uint32_t edges) {
  (void)client; (void)resource; (void)seat; (void)serial; (void)edges;
}

static void toplevel_set_max_size(struct wl_client *client, struct wl_resource *resource,
                                  int32_t width, int32_t height) {
  (void)client; (void)resource; (void)width; (void)height;
}

static void toplevel_set_min_size(struct wl_client *client, struct wl_resource *resource,
                                  int32_t width, int32_t height) {
  (void)client; (void)resource; (void)width; (void)height;
}

static void toplevel_set_maximized(struct wl_client *client, struct wl_resource *resource) {
  (void)client; (void)resource;
}

static void toplevel_unset_maximized(struct wl_client *client, struct wl_resource *resource) {
  (void)client; (void)resource;
}

static void toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *resource,
                                    struct wl_resource *output) {
  (void)client; (void)resource; (void)output;
}

static void toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *resource) {
  (void)client; (void)resource;
}

static void toplevel_set_minimized(struct wl_client *client, struct wl_resource *resource) {
  (void)client; (void)resource;
}

static const struct xdg_toplevel_interface ds_xdg_toplevel_impl = {
    .destroy = toplevel_destroy,
    .set_parent = toplevel_set_parent,
    .set_title = toplevel_set_title,
    .set_app_id = toplevel_set_app_id,
    .show_window_menu = toplevel_show_window_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
    .set_max_size = toplevel_set_max_size,
    .set_min_size = toplevel_set_min_size,
    .set_maximized = toplevel_set_maximized,
    .unset_maximized = toplevel_unset_maximized,
    .set_fullscreen = toplevel_set_fullscreen,
    .unset_fullscreen = toplevel_unset_fullscreen,
    .set_minimized = toplevel_set_minimized,
};

static void toplevel_resource_destroy(struct wl_resource *resource) {
  struct ds_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
  if (!toplevel) return;
  if (toplevel->xdg_surf) {
    toplevel->xdg_surf->toplevel = NULL;
  }
  free(toplevel);
}

/* XDG Surface implementation */
static void xdg_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *resource,
                                     uint32_t id) {
  struct ds_xdg_surface *xdg_surf = wl_resource_get_user_data(resource);
  if (!xdg_surf) return;

  struct ds_xdg_toplevel *toplevel = calloc(1, sizeof(*toplevel));
  if (!toplevel) {
    wl_client_post_no_memory(client);
    return;
  }

  toplevel->xdg_surf = xdg_surf;
  xdg_surf->toplevel = toplevel;

  toplevel->resource = wl_resource_create(client, &xdg_toplevel_interface,
                                          wl_resource_get_version(resource), id);
  if (!toplevel->resource) {
    free(toplevel);
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(toplevel->resource, &ds_xdg_toplevel_impl,
                                 toplevel, toplevel_resource_destroy);

  /* Send initial configure to fullscreen */
  struct ds_server *server = xdg_surf->surface->server;
  int w = server ? server->width : 1440;
  int h = server ? server->height : 3200;

  struct wl_array states;
  wl_array_init(&states);
  uint32_t *state = wl_array_add(&states, sizeof(uint32_t));
  if (state) *state = XDG_TOPLEVEL_STATE_ACTIVATED;
  state = wl_array_add(&states, sizeof(uint32_t));
  if (state) *state = XDG_TOPLEVEL_STATE_FULLSCREEN;

  xdg_toplevel_send_configure(toplevel->resource, w, h, &states);
  wl_array_release(&states);

  uint32_t serial = wl_display_next_serial(server->display);
  xdg_surf->last_serial = serial;
  xdg_surface_send_configure(xdg_surf->resource, serial);
}

static void xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id, struct wl_resource *parent,
                                  struct wl_resource *positioner) {
  (void)client; (void)resource; (void)id; (void)parent; (void)positioner;
}

static void xdg_surface_set_window_geometry(struct wl_client *client, struct wl_resource *resource,
                                            int32_t x, int32_t y, int32_t width, int32_t height) {
  (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void xdg_surface_ack_configure(struct wl_client *client, struct wl_resource *resource,
                                      uint32_t serial) {
  (void)client;
  struct ds_xdg_surface *xdg_surf = wl_resource_get_user_data(resource);
  if (xdg_surf) {
    xdg_surf->configured = 1;
    (void)serial;
  }
}

static const struct xdg_surface_interface ds_xdg_surface_impl = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

static void xdg_surface_resource_destroy(struct wl_resource *resource) {
  struct ds_xdg_surface *xdg_surf = wl_resource_get_user_data(resource);
  if (!xdg_surf) return;
  if (xdg_surf->surface) {
    xdg_surf->surface->xdg_surf = NULL;
  }
  free(xdg_surf);
}

/* XDG WM Base implementation */
static void wm_base_destroy(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void wm_base_create_positioner(struct wl_client *client, struct wl_resource *resource,
                                      uint32_t id) {
  (void)client; (void)resource; (void)id;
}

static void wm_base_get_xdg_surface(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t id, struct wl_resource *surface_resource) {
  struct ds_surface *surf = wl_resource_get_user_data(surface_resource);
  if (!surf) return;

  struct ds_xdg_surface *xdg_surf = calloc(1, sizeof(*xdg_surf));
  if (!xdg_surf) {
    wl_client_post_no_memory(client);
    return;
  }

  xdg_surf->surface = surf;
  surf->xdg_surf = xdg_surf;

  xdg_surf->resource = wl_resource_create(client, &xdg_surface_interface,
                                          wl_resource_get_version(resource), id);
  if (!xdg_surf->resource) {
    free(xdg_surf);
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(xdg_surf->resource, &ds_xdg_surface_impl,
                                 xdg_surf, xdg_surface_resource_destroy);
}

static void wm_base_pong(struct wl_client *client, struct wl_resource *resource,
                         uint32_t serial) {
  (void)client; (void)resource; (void)serial;
}

static const struct xdg_wm_base_interface ds_wm_base_impl = {
    .destroy = wm_base_destroy,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
};

static void wm_base_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  (void)data;
  struct wl_resource *resource = wl_resource_create(client, &xdg_wm_base_interface, version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(resource, &ds_wm_base_impl, NULL, NULL);
}

int ds_xdg_shell_init(struct ds_server *server) {
  server->xdg_wm_base_global = wl_global_create(server->display, &xdg_wm_base_interface, 3,
                                                server, wm_base_bind);
  return server->xdg_wm_base_global ? 0 : -1;
}
