/*
 * Droidspaces v6 - Wayland Display Bridge Daemon and Socket Manager
 *
 * Manages the embedded Wayland rendezvous broker on Android, passing DMA-BUF
 * descriptors and synchronization fences between the Android display consumer
 * and container Wayland compositors (such as Niri on Qualcomm Adreno).
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspace.h"
#include <sys/epoll.h>

#define DS_CTRL_CONSUMER_HELLO 1
#define DS_CTRL_PRODUCER_HELLO 2
#define DS_CTRL_SCREEN_INFO    7
#define DS_CTRL_REJECT         8
#define DS_CTRL_PICKUP_FDS     9
#define DS_CTRL_FDS_READY      10

#define MAX_DEPOSITED_FDS 8

struct ds_ctrl_msg {
  uint32_t type;
  uint32_t size;
} __attribute__((packed));

struct ds_screen_info {
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t refresh;
} __attribute__((packed));

static int send_ctrl(int fd, uint32_t type) {
  struct ds_ctrl_msg msg = {.type = type, .size = 0};
  ssize_t n = send(fd, &msg, sizeof(msg), MSG_NOSIGNAL);
  return (n == (ssize_t)sizeof(msg)) ? 0 : -1;
}

static int send_screen(int fd, const struct ds_screen_info *info) {
  struct ds_ctrl_msg hdr = {.type = DS_CTRL_SCREEN_INFO,
                            .size = (uint32_t)sizeof(*info)};
  uint8_t buf[sizeof(hdr) + sizeof(*info)];
  memcpy(buf, &hdr, sizeof(hdr));
  memcpy(buf + sizeof(hdr), info, sizeof(*info));
  ssize_t n = send(fd, buf, sizeof(buf), MSG_NOSIGNAL);
  return (n == (ssize_t)sizeof(buf)) ? 0 : -1;
}

static int send_deposited_fds(int sock, const int *fds, int count) {
  struct ds_ctrl_msg hdr = {.type = DS_CTRL_FDS_READY, .size = 0};
  struct iovec iov = {.iov_base = &hdr, .iov_len = sizeof(hdr)};

  char cmsg_buf[CMSG_SPACE(sizeof(int) * MAX_DEPOSITED_FDS)];
  memset(cmsg_buf, 0, sizeof(cmsg_buf));

  struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = cmsg_buf,
      .msg_controllen = sizeof(cmsg_buf),
  };

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)count);
  memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * (size_t)count);

  ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
  return (n == (ssize_t)sizeof(hdr)) ? 0 : -1;
}

static ssize_t recv_ctrl_with_fds(int sock, int *fds, int max_count,
                                  int *count_out, void *data, size_t data_len) {
  struct iovec iov = {.iov_base = data, .iov_len = data_len};
  char cmsg_buf[CMSG_SPACE(sizeof(int) * MAX_DEPOSITED_FDS)];
  memset(cmsg_buf, 0, sizeof(cmsg_buf));

  struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = cmsg_buf,
      .msg_controllen = sizeof(cmsg_buf),
  };

  ssize_t n = recvmsg(sock, &msg, 0);
  if (n <= 0)
    return n;

  *count_out = 0;
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
    int count = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    if (count > max_count)
      count = max_count;
    memcpy(fds, CMSG_DATA(cmsg), sizeof(int) * (size_t)count);
    *count_out = count;
  }

  return n;
}

static void wayland_broker_child_wrapper(int ready_fd, void *user_data) {
  (void)user_data;

  ds_daemon_child_preamble();
  ds_selinux_enter_domain();

  if (mkdir_p(DS_WAYLAND_SOCK_DIR, 0777) < 0) {
    if (write(ready_fd, "\x01", 1) < 0) { /* ignore */
    }
    _exit(1);
  }
  chmod(DS_WAYLAND_SOCK_DIR, 0777);
  unlink(DS_WAYLAND_HOST_BRIDGE);

  int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    if (write(ready_fd, "\x01", 1) < 0) { /* ignore */
    }
    _exit(1);
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  safe_strncpy(addr.sun_path, DS_WAYLAND_HOST_BRIDGE, sizeof(addr.sun_path));

  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(listen_fd);
    if (write(ready_fd, "\x01", 1) < 0) { /* ignore */
    }
    _exit(1);
  }
  chmod(DS_WAYLAND_HOST_BRIDGE, 0666);

  if (listen(listen_fd, 4) < 0) {
    close(listen_fd);
    unlink(DS_WAYLAND_HOST_BRIDGE);
    if (write(ready_fd, "\x01", 1) < 0) { /* ignore */
    }
    _exit(1);
  }

  /* Signal parent that socket is ready and listening */
  close(ready_fd);

  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    close(listen_fd);
    unlink(DS_WAYLAND_HOST_BRIDGE);
    _exit(1);
  }

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.fd = listen_fd;
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

  int consumer_fd = -1;
  int producer_fd = -1;
  int deposited_fds[MAX_DEPOSITED_FDS];
  int deposited_count = 0;
  struct ds_screen_info screen_info;
  int has_screen = 0;

  struct epoll_event events[16];
  while (1) {
    int n = epoll_wait(epoll_fd, events, 16, -1);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;

      if (fd == listen_fd) {
        int client = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client >= 0) {
          struct epoll_event cev = {
              .events = EPOLLIN | EPOLLHUP | EPOLLERR,
              .data.fd = client,
          };
          epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &cev);
        }
        continue;
      }

      if (events[i].events & (EPOLLHUP | EPOLLERR)) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        if (fd == consumer_fd) {
          consumer_fd = -1;
          for (int k = 0; k < deposited_count; k++)
            close(deposited_fds[k]);
          deposited_count = 0;
        } else if (fd == producer_fd) {
          producer_fd = -1;
        }
        close(fd);
        continue;
      }

      if (events[i].events & EPOLLIN) {
        struct ds_ctrl_msg hdr;
        int in_fds[MAX_DEPOSITED_FDS];
        int in_fd_count = 0;
        ssize_t r = recv_ctrl_with_fds(fd, in_fds, MAX_DEPOSITED_FDS,
                                       &in_fd_count, &hdr, sizeof(hdr));
        if (r <= 0) {
          epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
          if (fd == consumer_fd) {
            consumer_fd = -1;
            for (int k = 0; k < deposited_count; k++)
              close(deposited_fds[k]);
            deposited_count = 0;
          } else if (fd == producer_fd) {
            producer_fd = -1;
          }
          close(fd);
          continue;
        }

        if (hdr.type == DS_CTRL_CONSUMER_HELLO) {
          consumer_fd = fd;
          for (int k = 0; k < deposited_count; k++)
            close(deposited_fds[k]);
          deposited_count = in_fd_count;
          for (int k = 0; k < in_fd_count; k++)
            deposited_fds[k] = in_fds[k];

          if (producer_fd >= 0 && deposited_count > 0) {
            if (has_screen)
              send_screen(producer_fd, &screen_info);
            send_deposited_fds(producer_fd, deposited_fds, deposited_count);
            send_ctrl(consumer_fd, DS_CTRL_FDS_READY);
          }
        } else if (hdr.type == DS_CTRL_PRODUCER_HELLO) {
          producer_fd = fd;
          if (has_screen)
            send_screen(producer_fd, &screen_info);
          if (deposited_count > 0) {
            send_deposited_fds(producer_fd, deposited_fds, deposited_count);
            if (consumer_fd >= 0)
              send_ctrl(consumer_fd, DS_CTRL_FDS_READY);
          }
        } else if (hdr.type == DS_CTRL_SCREEN_INFO) {
          if (hdr.size == sizeof(struct ds_screen_info)) {
            if (read(fd, &screen_info, sizeof(screen_info)) ==
                (ssize_t)sizeof(screen_info)) {
              has_screen = 1;
              if (producer_fd >= 0)
                send_screen(producer_fd, &screen_info);
            }
          }
        } else if (hdr.type == DS_CTRL_PICKUP_FDS) {
          if (deposited_count > 0) {
            send_deposited_fds(fd, deposited_fds, deposited_count);
            if (consumer_fd >= 0)
              send_ctrl(consumer_fd, DS_CTRL_FDS_READY);
          }
        }
      }
    }
  }

  for (int k = 0; k < deposited_count; k++)
    close(deposited_fds[k]);
  close(listen_fd);
  close(epoll_fd);
  unlink(DS_WAYLAND_HOST_BRIDGE);
  _exit(0);
}

static pid_t spawn_wayland(void) {
  return ds_spawn_daemon(wayland_broker_child_wrapper, NULL, "wayland.log",
                         "Wayland", "Wayland");
}

int ds_wayland_daemon_start(struct ds_config *cfg) {
  if (!cfg || !cfg->wayland || !is_android())
    return -1;

  if (getuid() != 0) {
    ds_error("[Wayland] not running as root");
    return -1;
  }

  pid_t existing = ds_daemon_read_pid("wayland.wpid");
  if (existing > 0) {
    ds_log("Wayland: broker already running (PID %d)", (int)existing);
    cfg->wayland_pid = existing;
    return 1;
  }

  unlink(DS_WAYLAND_HOST_BRIDGE);

  ds_log("[Wayland] launching display broker daemon");
  pid_t child = spawn_wayland();
  if (child > 0) {
    cfg->wayland_pid = child;
    ds_daemon_write_pid("wayland.wpid", child);
    return 0;
  }
  return -1;
}

void ds_wayland_daemon_stop(struct ds_config *cfg) {
  if (!cfg)
    return;
  ds_global_daemon_stop(check_wayland_needs, cfg->wayland_pid,
                        &cfg->wayland_pid, "wayland.wpid",
                        DS_WAYLAND_HOST_BRIDGE, "[Wayland]");
}

int ds_setup_wayland_socket(struct ds_config *cfg) {
  if (!is_android() || !cfg->wayland)
    return 0;

  mkdir_p(DS_WAYLAND_CONTAINER_DIR, 01777);
  chmod(DS_WAYLAND_CONTAINER_DIR, 01777);

  /* Since setup_hardware_access runs after pivot_root, host root is at /.old_root */
  const char *src = NULL;
  if (access(DS_WAYLAND_OLDROOT_BRIDGE, F_OK) == 0) {
    src = DS_WAYLAND_OLDROOT_BRIDGE;
  } else if (access(DS_WAYLAND_HOST_BRIDGE, F_OK) == 0) {
    src = DS_WAYLAND_HOST_BRIDGE;
  } else {
    ds_warn("Wayland: bridge socket not found at %s or %s - skipping mount",
            DS_WAYLAND_OLDROOT_BRIDGE, DS_WAYLAND_HOST_BRIDGE);
    return 0;
  }

  const char *dst = DS_WAYLAND_BRIDGE_SOCK;
  if (ds_bind_mount_socket(src, dst, 0, "Wayland") < 0)
    return -1;

  if (ds_bind_mount_socket(src, "/run/display.sock", 0, "WaylandCompat") < 0) {
    /* Optional compatibility path for anland legacy clients */
  }

  ds_log("[Wayland] bridge socket bind-mounted to %s and /run/display.sock", dst);
  return 0;
}
