// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * Droidspaces Wayland Display Socket Bridge
 *
 * Prepares the Wayland runtime directory on the Android host and
 * bind-mounts the Wayland server socket (wayland-0) into the container
 * filesystem at /run/wayland-0 and /run/user/1000/wayland-0.
 */

#include "droidspace.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int ds_wayland_daemon_start(struct ds_config *cfg) {
  if (!cfg || !cfg->wayland || !is_android() || getuid() != 0)
    return -1;

  /* Ensure socket directory exists with universal access */
  mkdir_p(DS_WAYLAND_SOCK_DIR, 0777);
  chmod(DS_WAYLAND_SOCK_DIR, 0777);

  cfg->wayland_pid = 0; /* Managed by Android App process */
  return 0;
}

void ds_wayland_daemon_stop(struct ds_config *cfg) {
  (void)cfg;
}

int ds_setup_wayland_socket(struct ds_config *cfg) {
  if (!cfg || !cfg->wayland || !is_android())
    return 0;

  const char *src = NULL;
  if (access(DS_WAYLAND_OLDROOT_BRIDGE, F_OK) == 0) {
    src = DS_WAYLAND_OLDROOT_BRIDGE;
  } else if (access(DS_WAYLAND_HOST_BRIDGE, F_OK) == 0) {
    src = DS_WAYLAND_HOST_BRIDGE;
  } else {
    /* If the app hasn't opened Wayland yet, create an empty socket file placeholder
     * so bind-mount succeeds and clients wait on it */
    mkdir_p(DS_WAYLAND_SOCK_DIR, 0777);
    int fd = open(DS_WAYLAND_HOST_BRIDGE, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd >= 0)
      close(fd);
    chmod(DS_WAYLAND_HOST_BRIDGE, 0666);
    src = (access(DS_WAYLAND_OLDROOT_BRIDGE, F_OK) == 0)
              ? DS_WAYLAND_OLDROOT_BRIDGE
              : DS_WAYLAND_HOST_BRIDGE;
  }

  mkdir_p(DS_WAYLAND_CONTAINER_DIR, 0755);

  /* Primary mount: /run/wayland-0 */
  ds_bind_mount_socket(src, DS_WAYLAND_BRIDGE_SOCK, 0, "Wayland");

  /* User mount: /run/user/1000/wayland-0 */
  mkdir_p("/run/user/1000", 0700);
  if (chown("/run/user/1000", 1000, 1000) != 0) {
    /* non-fatal */
  }
  ds_bind_mount_socket(src, "/run/user/1000/wayland-0", 1000, "WaylandUser");

  /* Compatibility mount for legacy scripts checking ds-wayland.sock */
  ds_bind_mount_socket(src, "/run/ds-wayland.sock", 0, "WaylandLegacy");
  ds_bind_mount_socket(src, "/run/display.sock", 0, "WaylandCompat");

  ds_log("[Wayland] display socket bridged to %s and /run/user/1000/wayland-0",
         DS_WAYLAND_BRIDGE_SOCK);
  return 0;
}
