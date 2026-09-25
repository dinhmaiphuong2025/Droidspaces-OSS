// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - Output Implementation
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"
#include <stdlib.h>

static void output_release(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static const struct wl_output_interface output_interface_impl = {
    .release = output_release,
};

static void output_resource_destroy(struct wl_resource *resource) {
  wl_list_remove(wl_resource_get_link(resource));
}

static void output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  struct ds_output *output = data;
  struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(resource, &output_interface_impl, output, output_resource_destroy);
  wl_list_insert(&output->resources, wl_resource_get_link(resource));

  /* Physical dimensions: approx 70mm x 155mm in portrait */
  int phys_w = (output->width > output->height) ? 155 : 70;
  int phys_h = (output->width > output->height) ? 70 : 155;
  wl_output_send_geometry(resource, 0, 0, phys_w, phys_h, WL_OUTPUT_SUBPIXEL_NONE,
                          "Xiaomi", "2210132G", WL_OUTPUT_TRANSFORM_NORMAL);

  if (version >= 2) {
    wl_output_send_scale(resource, output->scale > 0 ? output->scale : 1);
  }

  /* Current mode: width, height, refresh rate */
  uint32_t flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
  wl_output_send_mode(resource, flags, output->width, output->height,
                      output->refresh_mhz > 0 ? output->refresh_mhz : 60000);

  /* Send name and description for all versions >= 4 to satisfy modern compositors (Smithay/SCTK) */
  if (version >= 4) {
    wl_output_send_name(resource, "WL-1");
    wl_output_send_description(resource, "Droidspaces Display");
  }

  if (version >= 2) {
    wl_output_send_done(resource);
  }
}

void ds_output_send_current_mode(struct ds_output *output) {
  if (!output) return;

  int phys_w = (output->width > output->height) ? 155 : 70;
  int phys_h = (output->width > output->height) ? 70 : 155;
  uint32_t flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;

  struct wl_resource *resource;
  wl_resource_for_each(resource, &output->resources) {
    wl_output_send_geometry(resource, 0, 0, phys_w, phys_h, WL_OUTPUT_SUBPIXEL_NONE,
                            "Xiaomi", "2210132G", WL_OUTPUT_TRANSFORM_NORMAL);
    if (wl_resource_get_version(resource) >= 2) {
      wl_output_send_scale(resource, output->scale > 0 ? output->scale : 1);
    }
    wl_output_send_mode(resource, flags, output->width, output->height,
                        output->refresh_mhz > 0 ? output->refresh_mhz : 60000);
    if (wl_resource_get_version(resource) >= 4) {
      wl_output_send_name(resource, "WL-1");
      wl_output_send_description(resource, "Droidspaces Display");
    }
    if (wl_resource_get_version(resource) >= 2) {
      wl_output_send_done(resource);
    }
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
  wl_list_init(&output->resources);

  output->global = wl_global_create(server->display, &wl_output_interface, 4,
                                    output, output_bind);
  if (!output->global) {
    free(output);
    return -1;
  }

  server->output = output;
  return 0;
}
