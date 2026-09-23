// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - SHM Buffer Adapter
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"
#include <stdlib.h>

/* Wrap a client wl_buffer in our own descriptor when it is SHM-backed.
 * Returns NULL for non-SHM buffers so the caller can try the dmabuf path.
 * The wrapper holds one shm reference, released by ds_shm_free_buffer. */
struct ds_buffer *ds_shm_wrap_buffer(struct wl_resource *buffer_resource) {
  if (!buffer_resource) return NULL;

  struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer_resource);
  if (!shm) return NULL;

  struct ds_buffer *buf = calloc(1, sizeof(*buf));
  if (!buf) return NULL;

  buf->resource = buffer_resource;
  buf->is_shm = 1;
  buf->shm = wl_shm_buffer_ref(shm);
  buf->width = wl_shm_buffer_get_width(shm);
  buf->height = wl_shm_buffer_get_height(shm);
  buf->stride = wl_shm_buffer_get_stride(shm);
  buf->format = wl_shm_buffer_get_format(shm);
  return buf;
}

void ds_shm_free_buffer(struct ds_buffer *buf) {
  if (!buf) return;
  if (buf->shm) {
    wl_shm_buffer_unref(buf->shm);
    buf->shm = NULL;
  }
  free(buf);
}
