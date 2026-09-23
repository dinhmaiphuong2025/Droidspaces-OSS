// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - Main Server Lifecycle
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/native_window.h>
#endif

static void ds_wayland_log_handler(const char *fmt, va_list ap) {
  char buf[512];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  DS_LOGE("libwayland: %s", buf);
}

static int on_input_eventfd(int fd, uint32_t mask, void *data) {
  (void)mask;
  struct ds_server *server = data;
  uint64_t val = 0;
  ssize_t ret = read(fd, &val, sizeof(val));
  (void)ret;

  ds_seat_dispatch_queue(server);
  return 1;
}

static void *server_event_thread(void *arg) {
  struct ds_server *server = arg;
  DS_LOGI("Wayland Server event loop thread started");

  while (atomic_load(&server->running)) {
    wl_event_loop_dispatch(server->loop, 16);
    wl_display_flush_clients(server->display);
  }

  DS_LOGI("Wayland Server event loop thread exiting");
  return NULL;
}

struct ds_server *ds_server_create(const char *socket_dir, int width, int height,
                                  int refresh_mhz, ANativeWindow *window) {
  struct ds_server *server = calloc(1, sizeof(*server));
  if (!server) return NULL;

  pthread_mutex_init(&server->lock, NULL);
  wl_list_init(&server->surfaces);

  server->width = width;
  server->height = height;
  server->refresh_mhz = refresh_mhz;
  server->window = window;
  if (socket_dir) {
    strncpy(server->sock_dir, socket_dir, sizeof(server->sock_dir) - 1);
    server->sock_dir[sizeof(server->sock_dir) - 1] = '\0';
  }

  server->display = wl_display_create();
  if (!server->display) {
    DS_LOGE("Failed to create Wayland display");
    free(server);
    return NULL;
  }

  wl_log_set_handler_server(ds_wayland_log_handler);
  pthread_mutex_init(&server->input_lock, NULL);
  server->input_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

  server->loop = wl_display_get_event_loop(server->display);
  if (server->input_eventfd >= 0) {
    server->input_source = wl_event_loop_add_fd(server->loop, server->input_eventfd,
                                                WL_EVENT_READABLE, on_input_eventfd, server);
  }

  /* Set up socket directory */
  if (socket_dir && strlen(socket_dir) > 0) {
    mkdir(socket_dir, 0777);
    chmod(socket_dir, 0777);
    setenv("XDG_RUNTIME_DIR", socket_dir, 1);
  }

  /* Unlink existing wayland-0 socket to prevent EADDRINUSE and avoid auto-increment */
  char sock_path[512];
  snprintf(sock_path, sizeof(sock_path), "%s/wayland-0",
           (socket_dir && strlen(socket_dir) > 0) ? socket_dir : "/data/local/tmp/ds-wayland");
  unlink(sock_path);

  /* Drop the libwayland lock too: after reinstall the app uid changes and
   * a stale lock owned by the old uid blocks the new server. */
  char lock_path[512];
  snprintf(lock_path, sizeof(lock_path), "%s/wayland-0.lock",
           (socket_dir && strlen(socket_dir) > 0) ? socket_dir : "/data/local/tmp/ds-wayland");
  unlink(lock_path);

  if (wl_display_add_socket(server->display, "wayland-0") < 0) {
    DS_LOGE("Failed to add socket wayland-0 to Wayland display: %s", strerror(errno));
    wl_display_destroy(server->display);
    free(server);
    return NULL;
  }
  chmod(sock_path, 0666);
  DS_LOGI("Wayland display listening on: %s", sock_path);

  /* Initialize core Wayland SHM */
  wl_display_init_shm(server->display);

  /* Initialize sub-protocols */
  ds_compositor_init(server);
  ds_xdg_shell_init(server);
  /* dmabuf must stay advertised: Mesa EGL refuses to initialize the Wayland
   * platform without it, and no client can render at all. Clients tolerate
   * the still-empty feedback handlers by falling back as niri already does. */
  ds_dmabuf_init(server);
  ds_output_init(server, width, height, refresh_mhz);
  ds_seat_init(server);
  ds_viewporter_init(server);

  /* Start event loop thread */
  atomic_store(&server->running, 1);
  if (pthread_create(&server->loop_thread, NULL, server_event_thread, server) != 0) {
    DS_LOGE("Failed to create server event loop thread: %s", strerror(errno));
    wl_display_destroy(server->display);
    free(server);
    return NULL;
  }

  return server;
}

/* Swap the native window without touching the display or its clients.
 * Callers must hold server->lock: the event thread presents under it. */
void ds_server_attach_window(struct ds_server *server, ANativeWindow *win,
                             int width, int height) {
  if (!server) return;

  int window_changed = (server->window != win);
  int size_changed = (server->width != width || server->height != height);

  if (window_changed || size_changed) {
    if (server->window && window_changed) {
      ANativeWindow_release(server->window);
    }
    ds_presenter_detach(server);
    server->window = win;
  }

  server->width = width;
  server->height = height;

  if (size_changed) {
    ds_server_enqueue_resize(server, width, height);
  }
  DS_LOGI("Wayland surface attached (%dx%d)%s", width, height,
          window_changed ? " [new window]" : (size_changed ? " [resized]" : " [unchanged]"));
}

/* Drop the native window but keep clients connected for instant resume. */
void ds_server_detach_window(struct ds_server *server) {
  if (!server) return;

  ds_presenter_detach(server);
  if (server->window) {
    ANativeWindow_release(server->window);
    server->window = NULL;
  }
  DS_LOGI("Wayland surface detached, display kept alive");
}

void ds_server_destroy(struct ds_server *server) {
  if (!server) return;

  DS_LOGI("Stopping Wayland Server...");
  atomic_store(&server->running, 0);

  if (server->loop) {
    /* Wake up event loop */
    wl_display_flush_clients(server->display);
  }

  pthread_join(server->loop_thread, NULL);

  /* Event thread is gone, safe to drop GL and window without the lock */
  ds_presenter_detach(server);
  if (server->window) {
    ANativeWindow_release(server->window);
    server->window = NULL;
  }

  if (server->output) {
    if (server->output->global) wl_global_destroy(server->output->global);
    free(server->output);
  }

  if (server->input_source) {
    wl_event_source_remove(server->input_source);
    server->input_source = NULL;
  }
  if (server->input_eventfd >= 0) {
    close(server->input_eventfd);
    server->input_eventfd = -1;
  }
  pthread_mutex_destroy(&server->input_lock);

  if (server->seat) {
    if (server->seat->global) wl_global_destroy(server->seat->global);
    if (server->seat->keymap_fd >= 0) close(server->seat->keymap_fd);
    free(server->seat);
  }

  if (server->compositor_global) wl_global_destroy(server->compositor_global);
  if (server->subcompositor_global) wl_global_destroy(server->subcompositor_global);
  if (server->xdg_wm_base_global) wl_global_destroy(server->xdg_wm_base_global);
  if (server->dmabuf_global) wl_global_destroy(server->dmabuf_global);
  if (server->viewporter_global) wl_global_destroy(server->viewporter_global);

  wl_display_destroy(server->display);
  pthread_mutex_destroy(&server->lock);
  free(server);
  DS_LOGI("Wayland Server destroyed");
}
