// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - Linux DMA-BUF Implementation
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PLANES 4

struct ds_dmabuf_params {
  struct ds_server *server;
  int fds[MAX_PLANES];
  uint32_t offsets[MAX_PLANES];
  uint32_t strides[MAX_PLANES];
  uint64_t modifiers[MAX_PLANES];
  int plane_count;
};

static void buffer_destroy(struct wl_client *client,
                           struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static const struct wl_buffer_interface ds_buffer_impl = {
    .destroy = buffer_destroy,
};

static void buffer_resource_destroy(struct wl_resource *resource) {
  struct ds_buffer *buf = wl_resource_get_user_data(resource);
  if (!buf)
    return;
  if (buf->mmap_data && buf->mmap_data != MAP_FAILED) {
    munmap(buf->mmap_data, buf->mmap_size);
    buf->mmap_data = NULL;
    buf->mmap_size = 0;
  }
  if (buf->ahwb) {
    AHardwareBuffer_release(buf->ahwb);
    buf->ahwb = NULL;
  }
  for (int i = 0; i < buf->dmabuf_num_planes && i < MAX_PLANES; i++) {
    if (buf->dmabuf_fds[i] >= 0) {
      close(buf->dmabuf_fds[i]);
      buf->dmabuf_fds[i] = -1;
    }
  }
  free(buf);
}

static void params_destroy(struct wl_client *client,
                           struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void params_add(struct wl_client *client, struct wl_resource *resource,
                       int32_t fd, uint32_t plane_idx, uint32_t offset,
                       uint32_t stride, uint32_t modifier_hi,
                       uint32_t modifier_lo) {
  (void)client;
  struct ds_dmabuf_params *params = wl_resource_get_user_data(resource);
  if (!params) {
    close(fd);
    return;
  }

  if (plane_idx >= MAX_PLANES) {
    close(fd);
    zwp_linux_buffer_params_v1_send_failed(resource);
    return;
  }

  params->fds[plane_idx] = fd;
  params->offsets[plane_idx] = offset;
  params->strides[plane_idx] = stride;
  params->modifiers[plane_idx] = ((uint64_t)modifier_hi << 32) | modifier_lo;
  if ((int)plane_idx >= params->plane_count) {
    params->plane_count = plane_idx + 1;
  }
}

static void create_buffer_from_params(struct wl_client *client,
                                      struct wl_resource *params_resource,
                                      uint32_t buffer_id, int32_t width,
                                      int32_t height, uint32_t format,
                                      int is_immed) {
  struct ds_dmabuf_params *params = wl_resource_get_user_data(params_resource);
  if (!params || params->plane_count < 1) {
    if (!is_immed) {
      zwp_linux_buffer_params_v1_send_failed(params_resource);
    }
    return;
  }

  struct ds_buffer *buf = calloc(1, sizeof(*buf));
  if (!buf) {
    wl_client_post_no_memory(client);
    return;
  }

  buf->width = width;
  buf->height = height;
  buf->stride = params->strides[0];
  buf->format = format;
  buf->is_dmabuf = 1;
  wl_list_init(&buf->destroy_listener.link);

  /* Take over the plane fds so they survive the params object */
  buf->dmabuf_num_planes =
      params->plane_count > MAX_PLANES ? MAX_PLANES : params->plane_count;
  for (int i = 0; i < MAX_PLANES; i++) {
    buf->dmabuf_fds[i] = -1;
  }
  for (int i = 0; i < buf->dmabuf_num_planes; i++) {
    buf->dmabuf_fds[i] = params->fds[i];
    buf->dmabuf_offsets[i] = params->offsets[i];
    buf->dmabuf_strides[i] = params->strides[i];
    buf->dmabuf_modifiers[i] = params->modifiers[i];
    params->fds[i] = -1;
  }

  /* Create wl_buffer resource */
  buf->resource =
      wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
  if (!buf->resource) {
    free(buf);
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(buf->resource, &ds_buffer_impl, buf,
                                 buffer_resource_destroy);

  /* Wayland spec: create_immed must NOT send created/failed event.
   * Only the async create request expects zwp_linux_buffer_params_v1.created.
   */
  if (!is_immed) {
    zwp_linux_buffer_params_v1_send_created(params_resource, buf->resource);
  }
}

static void params_create(struct wl_client *client,
                          struct wl_resource *resource, int32_t width,
                          int32_t height, uint32_t format, uint32_t flags) {
  (void)flags;
  /* Not immed: create buffer and send created event */
  uint32_t buffer_id = 0;
  create_buffer_from_params(client, resource, buffer_id, width, height, format,
                            0);
}

static void params_create_immed(struct wl_client *client,
                                struct wl_resource *resource,
                                uint32_t buffer_id, int32_t width,
                                int32_t height, uint32_t format,
                                uint32_t flags) {
  (void)flags;
  create_buffer_from_params(client, resource, buffer_id, width, height, format,
                            1);
}

static const struct zwp_linux_buffer_params_v1_interface ds_params_impl = {
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

static void params_resource_destroy(struct wl_resource *resource) {
  struct ds_dmabuf_params *params = wl_resource_get_user_data(resource);
  if (!params)
    return;
  for (int i = 0; i < params->plane_count; i++) {
    if (params->fds[i] >= 0) {
      close(params->fds[i]);
      params->fds[i] = -1;
    }
  }
  free(params);
}

static void dmabuf_destroy(struct wl_client *client,
                           struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void dmabuf_create_params(struct wl_client *client,
                                 struct wl_resource *resource,
                                 uint32_t params_id) {
  struct ds_server *server = wl_resource_get_user_data(resource);
  struct ds_dmabuf_params *params = calloc(1, sizeof(*params));
  if (!params) {
    wl_client_post_no_memory(client);
    return;
  }

  params->server = server;
  for (int i = 0; i < MAX_PLANES; i++) {
    params->fds[i] = -1;
  }

  struct wl_resource *param_res =
      wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                         wl_resource_get_version(resource), params_id);
  if (!param_res) {
    free(params);
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(param_res, &ds_params_impl, params,
                                 params_resource_destroy);
}

static void dmabuf_get_default_feedback(struct wl_client *client,
                                        struct wl_resource *resource,
                                        uint32_t id) {
  (void)client;
  (void)resource;
  (void)id;
}

static void dmabuf_get_surface_feedback(struct wl_client *client,
                                        struct wl_resource *resource,
                                        uint32_t id,
                                        struct wl_resource *surface) {
  (void)client;
  (void)resource;
  (void)id;
  (void)surface;
}

static const struct zwp_linux_dmabuf_v1_interface ds_dmabuf_impl = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_create_params,
    .get_default_feedback = dmabuf_get_default_feedback,
    .get_surface_feedback = dmabuf_get_surface_feedback,
};

static void dmabuf_bind(struct wl_client *client, void *data, uint32_t version,
                        uint32_t id) {
  struct wl_resource *resource =
      wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(resource, &ds_dmabuf_impl, data, NULL);

  /* Only formats the presenter can upload. Clients fall back to
   * these instead of stalling on buffers that would be skipped. */
  uint32_t formats[] = {
      DRM_FORMAT_ARGB8888,
      DRM_FORMAT_XRGB8888,
  };
  size_t count = sizeof(formats) / sizeof(formats[0]);

  for (size_t i = 0; i < count; i++) {
    if (version >= 3) {
      zwp_linux_dmabuf_v1_send_modifier(resource, formats[i], 0,
                                        0); /* DRM_FORMAT_MOD_LINEAR */
    } else {
      zwp_linux_dmabuf_v1_send_format(resource, formats[i]);
    }
  }
}

int ds_dmabuf_init(struct ds_server *server) {
  server->dmabuf_global = wl_global_create(
      server->display, &zwp_linux_dmabuf_v1_interface, 3, server, dmabuf_bind);
  return server->dmabuf_global ? 0 : -1;
}
