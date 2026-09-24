// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * Droidspaces Wayland Display Socket Bridge
 *
 * Prepares the Wayland runtime directory on the Android host and
 * bind-mounts the Wayland runtime directory into the container
 * filesystem at /run/ds-wayland, symlinking wayland-0 to standard paths.
 */

#include "droidspace.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

int ds_wayland_daemon_start(struct ds_config *cfg) {
  if (!cfg || !cfg->wayland || !is_android() || getuid() != 0)
    return -1;

  /* Ensure host socket directory exists with universal access */
  mkdir_p(DS_WAYLAND_SOCK_DIR, 0777);
  chmod(DS_WAYLAND_SOCK_DIR, 0777);

  cfg->wayland_pid = 0; /* Managed by Android App process */
  return 0;
}

void ds_wayland_daemon_stop(struct ds_config *cfg) {
  (void)cfg;
}

/* Directory segment for a container's isolated display socket. Mirrors
 * ValidationUtils.waylandSocketDir() in the Android app: only letters,
 * digits, '_' and '-' survive, so ".." collapses and traversal is
 * impossible. Empty result falls back to "default". */
static void ds_wayland_socket_dir(const char *name, char *out, size_t size) {
  if (!out || size == 0)
    return;
  size_t j = 0;
  if (name) {
    for (size_t i = 0; name[i] != '\0' && j + 1 < size; i++) {
      char c = name[i];
      int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
      if (keep)
        out[j++] = c;
    }
  }
  if (j == 0) {
    const char *fb = "default";
    size_t k;
    for (k = 0; k + 1 < size && fb[k] != '\0'; k++)
      out[k] = fb[k];
    out[k] = '\0';
    return;
  }
  out[j] = '\0';
}

void ds_wayland_socket_path(const char *container_name, char *out, size_t size) {
  char dir[128];
  ds_wayland_socket_dir(container_name, dir, sizeof(dir));
  snprintf(out, size, "%s/%s/%s", DS_WAYLAND_SOCK_DIR, dir,
           DS_WAYLAND_DISPLAY_SOCK);
}

int ds_setup_wayland_socket(struct ds_config *cfg) {
  if (!cfg || !cfg->wayland || !is_android())
    return 0;

  const char *src = "/.old_root/data/local/tmp/ds-wayland";
  if (access(src, F_OK) != 0) {
    src = "/data/local/tmp/ds-wayland";
  }

  /* Ensure host dir exists */
  mkdir_p(src, 0777);
  chmod(src, 0777);

  /* Mount host socket directory to /run/ds-wayland in container */
  mkdir_p("/run/ds-wayland", 0777);
  chmod("/run/ds-wayland", 0777);
  if (mount(src, "/run/ds-wayland", NULL, MS_BIND, NULL) != 0) {
    ds_warn("[Wayland] failed to bind-mount socket dir: %s", strerror(errno));
  }

  /* Setup symlinks inside container. They point at this container's own
   * isolated socket, not the old shared flat one. */
  char dir[128];
  ds_wayland_socket_dir(cfg->container_name, dir, sizeof(dir));
  char target[256];
  snprintf(target, sizeof(target), "/run/ds-wayland/%s/%s", dir,
           DS_WAYLAND_DISPLAY_SOCK);

  mkdir_p("/run/user/1000", 0700);
  if (chown("/run/user/1000", 1000, 1000) != 0) {
    /* non-fatal */
  }

  unlink("/run/user/1000/wayland-0");
  unlink("/run/wayland-0");
  unlink("/run/ds-wayland.sock");

  if (symlink(target, "/run/user/1000/wayland-0") != 0) {
    /* non-fatal */
  }
  if (symlink(target, "/run/wayland-0") != 0) {
    /* non-fatal */
  }
  if (symlink(target, "/run/ds-wayland.sock") != 0) {
    /* non-fatal */
  }

  ds_log("[Wayland] directory %s bridged to /run/ds-wayland and symlinked to %s", src,
         target);
  return 0;
}
