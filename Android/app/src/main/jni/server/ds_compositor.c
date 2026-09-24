// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - Compositor Implementation
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"
#include <stdlib.h>
#include <string.h>

/* A client-owned buffer can vanish while the surface still references it.
 * Disarm first so a later destroy never leaves a dangling pointer. */
static void buffer_destroy_notify(struct wl_listener *listener, void *data) {
  (void)data;
  struct ds_buffer *buf = wl_container_of(listener, buf, destroy_listener);
  struct ds_surface *surf = buf->surface;
  buf->surface = NULL;
  if (!surf) return;
  if (surf->current_buffer == buf) surf->current_buffer = NULL;
  if (surf->pending_buffer == buf) surf->pending_buffer = NULL;
  surf->is_mapped = 0;
}

static void disarm_buffer(struct ds_buffer *buf) {
  if (!buf || buf->is_shm) return;
  wl_list_remove(&buf->destroy_listener.link);
  wl_list_init(&buf->destroy_listener.link);
  buf->surface = NULL;
}

static void arm_buffer(struct ds_surface *surf, struct ds_buffer *buf) {
  if (!buf || buf->is_shm || !buf->resource || buf->surface == surf) return;
  buf->surface = surf;
  buf->destroy_listener.notify = buffer_destroy_notify;
  wl_resource_add_destroy_listener(buf->resource, &buf->destroy_listener);
}

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
    /* Prefer the SHM wrapper; dmabuf buffers carry our descriptor already */
    struct ds_buffer *wrapped = ds_shm_wrap_buffer(buffer_resource);
    if (wrapped) {
      if (surf->pending_buffer && surf->pending_buffer->is_shm) {
        ds_shm_free_buffer(surf->pending_buffer);
      }
      surf->pending_buffer = wrapped;
    } else {
      surf->pending_buffer = wl_resource_get_user_data(buffer_resource);
    }
  } else {
    if (surf->pending_buffer && surf->pending_buffer->is_shm) {
      ds_shm_free_buffer(surf->pending_buffer);
    }
    surf->pending_buffer = NULL;
  }
  surf->pending_sx = sx;
  surf->pending_sy = sy;
}

/* Merge one damaged rect into the pending set. The presenter uploads
 * only this union, so typing in a terminal no longer pushes 18MB. */
static void damage_union(struct ds_surface *surf, int32_t x, int32_t y,
                         int32_t width, int32_t height) {
  if (!surf || width <= 0 || height <= 0) return;
  if (!surf->pend_damage) {
    surf->pend_dx = x;
    surf->pend_dy = y;
    surf->pend_dw = width;
    surf->pend_dh = height;
    surf->pend_damage = 1;
    return;
  }
  int32_t x1 = surf->pend_dx < x ? surf->pend_dx : x;
  int32_t y1 = surf->pend_dy < y ? surf->pend_dy : y;
  int32_t x2 = surf->pend_dx + surf->pend_dw > x + width
                   ? surf->pend_dx + surf->pend_dw
                   : x + width;
  int32_t y2 = surf->pend_dy + surf->pend_dh > y + height
                   ? surf->pend_dy + surf->pend_dh
                   : y + height;
  surf->pend_dx = x1;
  surf->pend_dy = y1;
  surf->pend_dw = x2 - x1;
  surf->pend_dh = y2 - y1;
}

static void surface_damage(struct wl_client *client, struct wl_resource *resource,
                           int32_t x, int32_t y, int32_t width, int32_t height) {
  (void)client;
  damage_union(wl_resource_get_user_data(resource), x, y, width, height);
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

static void surface_send_frames(struct ds_server *server, struct ds_surface *surf,
                                uint32_t msec);

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  struct ds_surface *surf = wl_resource_get_user_data(resource);
  if (!surf || !surf->server) return;

  pthread_mutex_lock(&surf->server->lock);

  /* Apply attached buffer */
  if (surf->pending_buffer) {
    if (surf->current_buffer && surf->current_buffer != surf->pending_buffer) {
      if (surf->current_buffer->is_shm) {
        ds_shm_free_buffer(surf->current_buffer);
      } else {
        disarm_buffer(surf->current_buffer);
      }
    }
    surf->current_buffer = surf->pending_buffer;
    arm_buffer(surf, surf->current_buffer);
    surf->width = surf->pending_buffer->width;
    surf->height = surf->pending_buffer->height;
    surf->is_mapped = 1;

    /* Hand the accumulated damage to the presenter. A commit without
     * damage means a full upload, e.g. the first frame or a resize. */
    if (surf->pend_damage) {
      surf->dmg_x = surf->pend_dx;
      surf->dmg_y = surf->pend_dy;
      surf->dmg_w = surf->pend_dw;
      surf->dmg_h = surf->pend_dh;
      surf->dmg_valid = 1;
      surf->pend_damage = 0;
    } else {
      surf->dmg_valid = 0;
    }

    if (surf->xdg_surf && surf->role != DS_SURFACE_ROLE_CURSOR) {
      surf->server->active_surface = surf;

      /* Present directly via ASurfaceControl zero-flicker presenter */
      ds_presenter_present_surface(surf->server, surf);
    }

    /* Release the buffer so double-buffered clients keep submitting frames.
     * Without this, clients stall after their buffers are all busy. */
    if (surf->current_buffer->resource) {
      wl_buffer_send_release(surf->current_buffer->resource);
    }
  }

  /* Frame callbacks are paced to the output refresh so a client cannot
   * flood the event thread with full-frame commits. Sporadic frames still
   * go out immediately, only bursts wait for the next tick. */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t msec = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
  surface_send_frames(surf->server, surf, msec);

  surf->pending_buffer = NULL;
  pthread_mutex_unlock(&surf->server->lock);
}

static uint32_t frame_interval_ms(struct ds_server *server) {
  if (server && server->refresh_mhz > 0) {
    uint32_t hz = (uint32_t)server->refresh_mhz / 1000;
    if (hz > 0) return 1000 / hz;
  }
  return 16;
}

static void flush_frame_callbacks(struct ds_surface *surf, uint32_t msec) {
  struct ds_frame_callback *cb, *tmp;
  wl_list_for_each_safe(cb, tmp, &surf->frame_callback_list, link) {
    wl_callback_send_done(cb->resource, msec);
    wl_resource_destroy(cb->resource);
    wl_list_remove(&cb->link);
    free(cb);
  }
  surf->last_frame_ms = msec;
}

static void surface_send_frames(struct ds_server *server, struct ds_surface *surf,
                                uint32_t msec) {
  if (wl_list_empty(&surf->frame_callback_list)) return;
  uint32_t interval = frame_interval_ms(server);
  if (msec - surf->last_frame_ms >= interval) {
    flush_frame_callbacks(surf, msec);
    return;
  }
  if (server->frame_timer) {
    wl_event_source_timer_update(server->frame_timer, (int)(interval - (msec - surf->last_frame_ms)));
  } else {
    flush_frame_callbacks(surf, msec);
  }
}

int ds_frame_timer_tick(void *data) {
  struct ds_server *server = data;
  if (!server) return 0;

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t msec = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

  pthread_mutex_lock(&server->lock);
  struct ds_surface *surf;
  wl_list_for_each(surf, &server->surfaces, link) {
    if (!wl_list_empty(&surf->frame_callback_list)) {
      flush_frame_callbacks(surf, msec);
    }
  }
  pthread_mutex_unlock(&server->lock);
  return 0;
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
  (void)client;
  /* Buffer coordinates match surface coordinates here: no scale or
   * transform support exists elsewhere in this server either. */
  damage_union(wl_resource_get_user_data(resource), x, y, width, height);
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

  /* Send leave events and reset focus tracking in the seat */
  ds_seat_surface_destroyed(surf->server, surf);

  if (surf->current_buffer && surf->current_buffer->is_shm) {
    if (surf->pending_buffer == surf->current_buffer) {
      surf->pending_buffer = NULL;
    }
    ds_shm_free_buffer(surf->current_buffer);
    surf->current_buffer = NULL;
  } else if (surf->current_buffer) {
    disarm_buffer(surf->current_buffer);
    surf->current_buffer = NULL;
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
  (void)resource; (void)parent;
  struct ds_surface *surf = wl_resource_get_user_data(surface);
  if (surf) {
    surf->role = DS_SURFACE_ROLE_SUBSURFACE;
  }
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
