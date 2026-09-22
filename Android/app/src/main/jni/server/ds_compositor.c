// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - Compositor Implementation
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"
#include <stdlib.h>
#include <string.h>

static void surface_destroy(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *buffer_resource, int32_t sx, int32_t sy) {
  (void)client;
  struct ds_surface *surf = wl_resource_get_user_data(resource);
  if (!surf) return;

  if (buffer_resource) {
    surf->pending_buffer = wl_resource_get_user_data(buffer_resource);
  } else {
    surf->pending_buffer = NULL;
  }
  surf->pending_sx = sx;
  surf->pending_sy = sy;
}

static void surface_damage(struct wl_client *client, struct wl_resource *resource,
                           int32_t x, int32_t y, int32_t width, int32_t height) {
  (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
  /* Full frame presentation with zero-flicker: we present the committed buffer directly */
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource,
                          uint32_t callback_id) {
  struct ds_surface *surf = wl_resource_get_user_data(resource);
  if (!surf) return;

  struct ds_frame_callback *cb = malloc(sizeof(*cb));
  if (!cb) return;

  cb->resource = wl_resource_create(client, &wl_callback_interface, 1, callback_id);
  wl_list_insert(surf->frame_callback_list.prev, &cb->link);
}

static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *region_resource) {
  (void)client; (void)resource; (void)region_resource;
}

static void surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
                                     struct wl_resource *region_resource) {
  (void)client; (void)resource; (void)region_resource;
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  struct ds_surface *surf = wl_resource_get_user_data(resource);
  if (!surf || !surf->server) return;

  pthread_mutex_lock(&surf->server->lock);

  /* Apply attached buffer */
  if (surf->pending_buffer) {
    surf->current_buffer = surf->pending_buffer;
    surf->width = surf->pending_buffer->width;
    surf->height = surf->pending_buffer->height;
    surf->is_mapped = 1;
    surf->server->active_surface = surf;

    /* Present directly via ASurfaceControl zero-flicker presenter */
    ds_presenter_present_surface(surf->server, surf);
  }

  /* Trigger any pending frame callbacks with current monotonic time */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t msec = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

  struct ds_frame_callback *cb, *tmp;
  wl_list_for_each_safe(cb, tmp, &surf->frame_callback_list, link) {
    wl_callback_send_done(cb->resource, msec);
    wl_resource_destroy(cb->resource);
    wl_list_remove(&cb->link);
    free(cb);
  }

  surf->pending_buffer = NULL;
  pthread_mutex_unlock(&surf->server->lock);
}

static void surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource,
                                         int32_t transform) {
  (void)client; (void)resource; (void)transform;
}

static void surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource,
                                     int32_t scale) {
  (void)client; (void)resource; (void)scale;
}

static void surface_damage_buffer(struct wl_client *client, struct wl_resource *resource,
                                  int32_t x, int32_t y, int32_t width, int32_t height) {
  (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void surface_offset(struct wl_client *client, struct wl_resource *resource,
                           int32_t sx, int32_t sy) {
  (void)client; (void)resource; (void)sx; (void)sy;
}

static const struct wl_surface_interface ds_surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
};

static void surface_resource_destroy(struct wl_resource *resource) {
  struct ds_surface *surf = wl_resource_get_user_data(resource);
  if (!surf) return;

  struct ds_frame_callback *cb, *tmp;
  wl_list_for_each_safe(cb, tmp, &surf->frame_callback_list, link) {
    wl_resource_destroy(cb->resource);
    wl_list_remove(&cb->link);
    free(cb);
  }

  if (surf->server && surf->server->active_surface == surf) {
    surf->server->active_surface = NULL;
  }

  wl_list_remove(&surf->link);
  free(surf);
}

static void compositor_create_surface(struct wl_client *client, struct wl_resource *resource,
                                      uint32_t id) {
  struct ds_server *server = wl_resource_get_user_data(resource);
  struct ds_surface *surf = calloc(1, sizeof(*surf));
  if (!surf) {
    wl_client_post_no_memory(client);
    return;
  }

  surf->server = server;
  wl_list_init(&surf->frame_callback_list);

  surf->resource = wl_resource_create(client, &wl_surface_interface,
                                      wl_resource_get_version(resource), id);
  if (!surf->resource) {
    free(surf);
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(surf->resource, &ds_surface_impl, surf, surface_resource_destroy);
  wl_list_insert(&server->surfaces, &surf->link);
}

static void region_destroy(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void region_add(struct wl_client *client, struct wl_resource *resource,
                       int32_t x, int32_t y, int32_t width, int32_t height) {
  (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void region_subtract(struct wl_client *client, struct wl_resource *resource,
                            int32_t x, int32_t y, int32_t width, int32_t height) {
  (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static const struct wl_region_interface ds_region_impl = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

static void compositor_create_region(struct wl_client *client, struct wl_resource *resource,
                                     uint32_t id) {
  struct wl_resource *reg = wl_resource_create(client, &wl_region_interface, 1, id);
  if (!reg) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(reg, &ds_region_impl, NULL, NULL);
  (void)resource;
}

static const struct wl_compositor_interface ds_compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  struct ds_server *server = data;
  struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(resource, &ds_compositor_impl, server, NULL);
}

/* Subcompositor implementation */
static void subcompositor_destroy(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void subsurface_destroy(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void subsurface_set_position(struct wl_client *client, struct wl_resource *resource,
                                    int32_t x, int32_t y) {
  (void)client; (void)resource; (void)x; (void)y;
}

static void subsurface_place_above(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *sibling) {
  (void)client; (void)resource; (void)sibling;
}

static void subsurface_place_below(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *sibling) {
  (void)client; (void)resource; (void)sibling;
}

static void subsurface_set_sync(struct wl_client *client, struct wl_resource *resource) {
  (void)client; (void)resource;
}

static void subsurface_set_desync(struct wl_client *client, struct wl_resource *resource) {
  (void)client; (void)resource;
}

static const struct wl_subsurface_interface ds_subsurface_impl = {
    .destroy = subsurface_destroy,
    .set_position = subsurface_set_position,
    .place_above = subsurface_place_above,
    .place_below = subsurface_place_below,
    .set_sync = subsurface_set_sync,
    .set_desync = subsurface_set_desync,
};

static void subcompositor_get_subsurface(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t id, struct wl_resource *surface,
                                        struct wl_resource *parent) {
  (void)resource; (void)surface; (void)parent;
  struct wl_resource *sub = wl_resource_create(client, &wl_subsurface_interface, 1, id);
  if (!sub) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(sub, &ds_subsurface_impl, NULL, NULL);
}

static const struct wl_subcompositor_interface ds_subcompositor_impl = {
    .destroy = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void subcompositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  (void)data;
  struct wl_resource *resource = wl_resource_create(client, &wl_subcompositor_interface, version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(resource, &ds_subcompositor_impl, NULL, NULL);
}

int ds_compositor_init(struct ds_server *server) {
  server->compositor_global = wl_global_create(server->display, &wl_compositor_interface, 4,
                                              server, compositor_bind);
  server->subcompositor_global = wl_global_create(server->display, &wl_subcompositor_interface, 1,
                                                 server, subcompositor_bind);
  return (server->compositor_global && server->subcompositor_global) ? 0 : -1;
}
