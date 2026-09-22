// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - Viewporter Implementation
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"

static void viewport_destroy(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void viewport_set_source(struct wl_client *client, struct wl_resource *resource,
                                wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height) {
  (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void viewport_set_destination(struct wl_client *client, struct wl_resource *resource,
                                     int32_t width, int32_t height) {
  (void)client; (void)resource; (void)width; (void)height;
}

static const struct wp_viewport_interface ds_viewport_impl = {
    .destroy = viewport_destroy,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

static void viewporter_destroy(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void viewporter_get_viewport(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t id, struct wl_resource *surface) {
  (void)resource; (void)surface;
  struct wl_resource *vp = wl_resource_create(client, &wp_viewport_interface, 1, id);
  if (!vp) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(vp, &ds_viewport_impl, NULL, NULL);
}

static const struct wp_viewporter_interface ds_viewporter_impl = {
    .destroy = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};

static void viewporter_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  (void)data;
  struct wl_resource *resource = wl_resource_create(client, &wp_viewporter_interface, version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(resource, &ds_viewporter_impl, NULL, NULL);
}

int ds_viewporter_init(struct ds_server *server) {
  server->viewporter_global = wl_global_create(server->display, &wp_viewporter_interface, 1,
                                               server, viewporter_bind);
  return server->viewporter_global ? 0 : -1;
}
