// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - Output Implementation
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"
#include <stdlib.h>

static void output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  struct ds_output *output = data;
  struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  /* Physical dimensions: approx 70mm x 155mm for Xiaomi 13 Pro */
  wl_output_send_geometry(resource, 0, 0, 70, 155, WL_OUTPUT_SUBPIXEL_NONE,
                          "Xiaomi", "2210132G", WL_OUTPUT_TRANSFORM_NORMAL);

  if (version >= 2) {
    wl_output_send_scale(resource, output->scale > 0 ? output->scale : 1);
  }

  /* Current mode: width, height, refresh rate */
  uint32_t flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
  wl_output_send_mode(resource, flags, output->width, output->height,
                      output->refresh_mhz > 0 ? output->refresh_mhz : 60000);

  if (version >= 2) {
    wl_output_send_done(resource);
  }
}

int ds_output_init(struct ds_server *server, int width, int height, int refresh_mhz) {
  struct ds_output *output = calloc(1, sizeof(*output));
  if (!output) return -1;

  output->server = server;
  output->width = width;
  output->height = height;
  output->refresh_mhz = refresh_mhz;
  output->scale = 1;

  output->global = wl_global_create(server->display, &wl_output_interface, 3,
                                    output, output_bind);
  if (!output->global) {
    free(output);
    return -1;
  }

  server->output = output;
  return 0;
}
