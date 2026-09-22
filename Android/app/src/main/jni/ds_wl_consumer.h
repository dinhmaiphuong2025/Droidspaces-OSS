// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * DS_WL consumer API (clean-room)
 *
 * Manages the connection to the broker, buffer fd passing, input
 * dispatch via the data channel, and render-done signaling.
 */

#ifndef DS_WL_CONSUMER_H
#define DS_WL_CONSUMER_H

#include "ds_wl_input.h"
#include "ds_wl_proto.h"

typedef struct ds_wl_ctx ds_wl_ctx;

/*
 * Connect to the broker and send the initial ATTACH with buffer fds
 * and display metadata.  Creates an internal socketpair for the data
 * channel (input events + render-done signals) and passes one end
 * through the broker to the producer.
 *
 * On success, *out is allocated and must be freed with ds_wl_disconnect.
 * Returns 0 on success, -1 on error.
 */
int ds_wl_connect(ds_wl_ctx **out, const char *broker_path,
                  const int *buf_fds, const struct ds_wl_buf_desc *descs,
                  int buf_count, const struct ds_wl_display_info *disp);

/*
 * Tell the producer to render into buffer `buf_idx`, then block until
 * the render-done signal arrives on the data channel.
 * Returns the fence fd (caller must close), or -1 on error.
 */
int ds_wl_present(ds_wl_ctx *ctx, int buf_idx);

/*
 * Send an input event to the producer via the data channel.
 * Returns 0 on success, -1 on error.
 */
int ds_wl_send_input(ds_wl_ctx *ctx, const struct ds_wl_input_ev *ev);

/*
 * Disconnect from the broker, close all fds, free context.
 */
void ds_wl_disconnect(ds_wl_ctx *ctx);

/*
 * Wake the consumer out of a blocking poll (thread-safe).
 */
void ds_wl_wake(ds_wl_ctx *ctx);

/*
 * Get the data channel fd for polling (used by render loop).
 */
int ds_wl_get_data_fd(ds_wl_ctx *ctx);

#endif /* DS_WL_CONSUMER_H */
