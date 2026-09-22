#ifndef DS_DISPLAY_CONSUMER_H
#define DS_DISPLAY_CONSUMER_H

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

typedef struct display_ctx display_ctx;

int  connect_to_daemon(display_ctx **ctx, const char *socket_path);
void wake_consumer(display_ctx *ctx);
void disconnect_daemon(display_ctx *ctx);
int  set_screen_info(display_ctx *ctx, uint32_t width, uint32_t height, uint32_t format, uint32_t refresh);
int  push_dmabufs(display_ctx *ctx, const int *fds, const struct buf_info *infos, int count);
int  select_dmabuf(display_ctx *ctx, int idx);
int  refresh_done_status(display_ctx *ctx, int *out_fence);
int  push_input_event(display_ctx *ctx, const struct InputEvent *event);
int  push_input_event_with_length(display_ctx *ctx, const struct InputEvent *event, void *payload, size_t size);
int  poll_output_event(display_ctx *ctx, struct OutputEvent *event, int timeout_ms);
int  poll_output_event_extend_data(display_ctx *ctx, void *payload, size_t size, int timeout_ms);

#endif /* DS_DISPLAY_CONSUMER_H */
