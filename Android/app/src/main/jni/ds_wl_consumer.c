// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * DS_WL consumer implementation (clean-room)
 */

#include "ds_wl_consumer.h"
#include "ds_wl_sock.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <android/log.h>
#define LOG_TAG "DsWlConsumer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct ds_wl_ctx {
  int broker_fd; /* control channel to broker */
  int data_fd;   /* our end of the data channel socketpair */
  int wake_efd;  /* eventfd for waking blocking polls */
};

int ds_wl_connect(ds_wl_ctx **out, const char *broker_path,
                  const int *buf_fds,
                  const struct ds_wl_buf_desc *descs __attribute__((unused)),
                  int buf_count, const struct ds_wl_display_info *disp) {
  ds_wl_ctx *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return -1;
  ctx->broker_fd = -1;
  ctx->data_fd = -1;
  ctx->wake_efd = -1;

  /* Create the data channel socketpair.
   * data_pair[0] = consumer keeps, data_pair[1] = sent to producer via broker */
  int data_pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, data_pair) < 0) {
    LOGE("socketpair: %s", strerror(errno));
    free(ctx);
    return -1;
  }
  ctx->data_fd = data_pair[0];

  /* Create wake eventfd */
  ctx->wake_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (ctx->wake_efd < 0) {
    LOGW("eventfd: %s", strerror(errno));
    /* Non-fatal, wake won't work but everything else does */
  }

  /* Connect to broker */
  ctx->broker_fd = ds_wl_connect_unix(broker_path);
  if (ctx->broker_fd < 0) {
    LOGE("failed to connect to broker at %s", broker_path);
    close(data_pair[1]);
    ds_wl_disconnect(ctx);
    return -1;
  }

  /* Build fd array: buffer fds + data channel producer end */
  int total_fds = buf_count + 1;
  int send_fds[DS_WL_MAX_FDS];
  if (total_fds > DS_WL_MAX_FDS) {
    LOGE("too many fds (%d)", total_fds);
    close(data_pair[1]);
    ds_wl_disconnect(ctx);
    return -1;
  }
  memcpy(send_fds, buf_fds, (size_t)buf_count * sizeof(int));
  send_fds[buf_count] = data_pair[1]; /* last fd is the data channel */

  /* Send ATTACH with display info and all fds */
  struct ds_wl_hdr hdr = {DS_WL_TAG_ATTACH, sizeof(struct ds_wl_display_info)};
  struct ds_wl_display_info info = *disp;
  if (ds_wl_send_msg(ctx->broker_fd, &hdr, &info, send_fds, total_fds) < 0) {
    LOGE("failed to send ATTACH to broker");
    close(data_pair[1]);
    ds_wl_disconnect(ctx);
    return -1;
  }
  close(data_pair[1]); /* broker received it, we no longer need our copy */

  LOGI("connected to broker, %d buffer fds, %ux%u @%u mHz", buf_count,
       disp->width, disp->height, disp->refresh_mhz);

  *out = ctx;
  return 0;
}

int ds_wl_present(ds_wl_ctx *ctx, int buf_idx) {
  if (!ctx || ctx->data_fd < 0)
    return -1;

  /* Tell producer which buffer to render into via PRESENTED event */
  struct ds_wl_input_ev ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = DS_WL_INPUT_PRESENTED;
  /* Reuse the rotation.degrees field to carry the buffer index */
  ev.rotation.degrees = buf_idx;

  if (ds_wl_send_all(ctx->data_fd, &ev, sizeof(ev)) < 0)
    return -1;

  /* Wait for render-done signal from producer */
  struct pollfd pfd[2];
  int nfds = 0;
  pfd[nfds].fd = ctx->data_fd;
  pfd[nfds].events = POLLIN;
  nfds++;
  if (ctx->wake_efd >= 0) {
    pfd[nfds].fd = ctx->wake_efd;
    pfd[nfds].events = POLLIN;
    nfds++;
  }

  int ret = poll(pfd, (nfds_t)nfds, 16); /* 16ms = ~1 frame @60Hz */
  if (ret <= 0)
    return -1; /* timeout or error, best-effort */

  /* Drain wake eventfd if it fired */
  if (ctx->wake_efd >= 0 && (pfd[nfds - 1].revents & POLLIN)) {
    uint64_t val;
    (void)read(ctx->wake_efd, &val, sizeof(val));
    return -1; /* woken up = asked to stop */
  }

  /* Read render-done signal (producer sends back an output event) */
  if (pfd[0].revents & POLLIN) {
    struct ds_wl_output_ev out_ev;
    ssize_t n = recv(ctx->data_fd, &out_ev, sizeof(out_ev), MSG_DONTWAIT);
    if (n <= 0)
      return -1;
    /* Producer may send cursor updates here too; we just return
     * success if we got any data (render is done). */
    return 0;
  }

  return -1;
}

int ds_wl_send_input(ds_wl_ctx *ctx, const struct ds_wl_input_ev *ev) {
  if (!ctx || ctx->data_fd < 0)
    return -1;
  return ds_wl_send_all(ctx->data_fd, ev, sizeof(*ev));
}

void ds_wl_disconnect(ds_wl_ctx *ctx) {
  if (!ctx)
    return;
  if (ctx->broker_fd >= 0)
    close(ctx->broker_fd);
  if (ctx->data_fd >= 0)
    close(ctx->data_fd);
  if (ctx->wake_efd >= 0)
    close(ctx->wake_efd);
  free(ctx);
}

void ds_wl_wake(ds_wl_ctx *ctx) {
  if (!ctx || ctx->wake_efd < 0)
    return;
  uint64_t val = 1;
  (void)write(ctx->wake_efd, &val, sizeof(val));
}

int ds_wl_get_data_fd(ds_wl_ctx *ctx) {
  return ctx ? ctx->data_fd : -1;
}
