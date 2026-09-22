// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * Wayland Display Bridge Broker (clean-room rewrite)
 *
 * Sits between the app-side consumer (which owns ANativeWindow buffers)
 * and the container-side producer (Niri via Anland backend).  Forwards
 * DMA-BUF file descriptors and display metadata over an AF_UNIX socket
 * using SCM_RIGHTS.
 *
 * Protocol tag namespace: DS_WL_TAG_*
 * See PLAN.md for the full design.
 */

#include "droidspace.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ------------------------------------------------------------------ */
/* Wire protocol                                                      */
/* ------------------------------------------------------------------ */

struct ds_wl_hdr {
  uint32_t tag;
  uint32_t len; /* payload bytes after header (0 if none) */
} __attribute__((packed));

#define DS_WL_TAG_ATTACH  0x4100 /* consumer -> broker: fd array + info */
#define DS_WL_TAG_READY   0x4200 /* broker -> producer: fd array ready  */
#define DS_WL_TAG_BIND    0x4300 /* producer -> broker: register        */
#define DS_WL_TAG_DISPLAY 0x4400 /* display metadata passthrough        */

struct ds_wl_display_info {
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t refresh_mhz;
} __attribute__((packed));

#define DS_WL_MAX_FDS 16 /* enough for triple-buffer + side channels */
#define DS_WL_TAG     "[Wayland]"

/* ------------------------------------------------------------------ */
/* Socket helpers: SCM_RIGHTS send/recv                               */
/* ------------------------------------------------------------------ */

/* Send header + optional payload, plus up to nfds file descriptors. */
static int broker_send(int sock, struct ds_wl_hdr *hdr, void *payload,
                       const int *fds, int nfds) {
  struct iovec iov[2];
  int iovcnt = 0;
  iov[iovcnt].iov_base = hdr;
  iov[iovcnt].iov_len = sizeof(*hdr);
  iovcnt++;
  if (payload && hdr->len > 0) {
    iov[iovcnt].iov_base = payload;
    iov[iovcnt].iov_len = hdr->len;
    iovcnt++;
  }

  char cmsgbuf[CMSG_SPACE(DS_WL_MAX_FDS * sizeof(int))];
  struct msghdr msg = {0};
  msg.msg_iov = iov;
  msg.msg_iovlen = (size_t)iovcnt;

  if (fds && nfds > 0) {
    memset(cmsgbuf, 0, sizeof(cmsgbuf));
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = CMSG_SPACE((size_t)nfds * sizeof(int));
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN((size_t)nfds * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, (size_t)nfds * sizeof(int));
  }

  ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
  return n >= 0 ? 0 : -1;
}

/* Receive header + payload buffer, extracting any SCM_RIGHTS fds.
 * Returns bytes read (>= sizeof(hdr)), 0 on EOF, -1 on error. */
static ssize_t broker_recv(int sock, struct ds_wl_hdr *hdr, void *payload,
                           size_t payload_cap, int *fds_out, int *nfds_out) {
  struct iovec iov[2];
  int iovcnt = 0;
  iov[iovcnt].iov_base = hdr;
  iov[iovcnt].iov_len = sizeof(*hdr);
  iovcnt++;
  if (payload && payload_cap > 0) {
    iov[iovcnt].iov_base = payload;
    iov[iovcnt].iov_len = payload_cap;
    iovcnt++;
  }

  char cmsgbuf[CMSG_SPACE(DS_WL_MAX_FDS * sizeof(int))];
  struct msghdr msg = {0};
  msg.msg_iov = iov;
  msg.msg_iovlen = (size_t)iovcnt;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);

  ssize_t n = recvmsg(sock, &msg, 0);
  if (n <= 0) {
    if (nfds_out)
      *nfds_out = 0;
    return n;
  }

  /* Extract file descriptors from ancillary data */
  int count = 0;
  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
       cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      int got = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      if (fds_out) {
        int take = got < (DS_WL_MAX_FDS - count) ? got : (DS_WL_MAX_FDS - count);
        memcpy(fds_out + count, CMSG_DATA(cmsg), (size_t)take * sizeof(int));
        count += take;
        /* Close any excess fds we cannot store */
        for (int i = take; i < got; i++)
          close(((int *)CMSG_DATA(cmsg))[i]);
      } else {
        /* Caller doesn't want fds, close them all */
        for (int i = 0; i < got; i++)
          close(((int *)CMSG_DATA(cmsg))[i]);
      }
    }
  }
  if (nfds_out)
    *nfds_out = count;
  return n;
}

/* ------------------------------------------------------------------ */
/* Broker state                                                       */
/* ------------------------------------------------------------------ */

struct broker_state {
  int listen_fd;
  int epoll_fd;
  int consumer_fd; /* app side, -1 if not connected */
  int producer_fd; /* container side, -1 if not connected */

  /* Cached buffer fds from most recent ATTACH */
  int buf_fds[DS_WL_MAX_FDS];
  int buf_nfds;
  struct ds_wl_display_info disp;
  int has_attach; /* true after first ATTACH received */
};

static void broker_state_init(struct broker_state *bs) {
  memset(bs, 0, sizeof(*bs));
  bs->listen_fd = -1;
  bs->epoll_fd = -1;
  bs->consumer_fd = -1;
  bs->producer_fd = -1;
}

static void broker_close_peer(struct broker_state *bs, int *fd_ptr) {
  if (*fd_ptr >= 0) {
    epoll_ctl(bs->epoll_fd, EPOLL_CTL_DEL, *fd_ptr, NULL);
    close(*fd_ptr);
    *fd_ptr = -1;
  }
}

static void broker_close_bufs(struct broker_state *bs) {
  for (int i = 0; i < bs->buf_nfds; i++)
    close(bs->buf_fds[i]);
  bs->buf_nfds = 0;
  bs->has_attach = 0;
}

/* Forward cached fds + display info to producer */
static int broker_forward_to_producer(struct broker_state *bs) {
  if (bs->producer_fd < 0 || !bs->has_attach)
    return 0; /* nothing to forward yet */

  /* Send display info first */
  struct ds_wl_hdr disp_hdr = {DS_WL_TAG_DISPLAY, sizeof(bs->disp)};
  if (broker_send(bs->producer_fd, &disp_hdr, &bs->disp, NULL, 0) < 0) {
    ds_warn("%s failed to forward display info", DS_WL_TAG);
    return -1;
  }

  /* Send READY with buffer fds */
  struct ds_wl_hdr rdy_hdr = {DS_WL_TAG_READY, 0};
  if (broker_send(bs->producer_fd, &rdy_hdr, NULL, bs->buf_fds, bs->buf_nfds) <
      0) {
    ds_warn("%s failed to forward buffer fds", DS_WL_TAG);
    return -1;
  }

  return 0;
}

/* ------------------------------------------------------------------ */
/* Main broker loop                                                   */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t broker_running = 1;

static void broker_sigterm(int sig) {
  (void)sig;
  broker_running = 0;
}

static void broker_loop(const char *sock_path) {
  struct broker_state bs;
  broker_state_init(&bs);

  /* Create AF_UNIX listen socket */
  unlink(sock_path);
  bs.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (bs.listen_fd < 0) {
    ds_error("%s socket: %s", DS_WL_TAG, strerror(errno));
    return;
  }

  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

  if (bind(bs.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ds_error("%s bind(%s): %s", DS_WL_TAG, sock_path, strerror(errno));
    close(bs.listen_fd);
    return;
  }
  chmod(sock_path, 0666);

  if (listen(bs.listen_fd, 2) < 0) {
    ds_error("%s listen: %s", DS_WL_TAG, strerror(errno));
    close(bs.listen_fd);
    unlink(sock_path);
    return;
  }

  bs.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (bs.epoll_fd < 0) {
    ds_error("%s epoll_create1: %s", DS_WL_TAG, strerror(errno));
    close(bs.listen_fd);
    unlink(sock_path);
    return;
  }

  struct epoll_event ev = {.events = EPOLLIN, .data.fd = bs.listen_fd};
  epoll_ctl(bs.epoll_fd, EPOLL_CTL_ADD, bs.listen_fd, &ev);

  signal(SIGTERM, broker_sigterm);
  signal(SIGINT, broker_sigterm);

  ds_log("%s broker listening on %s", DS_WL_TAG, sock_path);

  struct epoll_event events[4];
  while (broker_running) {
    int nev = epoll_wait(bs.epoll_fd, events, 4, 1000);
    if (nev < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    for (int i = 0; i < nev; i++) {
      int fd = events[i].data.fd;

      /* New connection */
      if (fd == bs.listen_fd) {
        int client = accept4(bs.listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0)
          continue;

        /* Read the initial message to identify consumer vs producer */
        struct ds_wl_hdr hdr;
        struct ds_wl_display_info info;
        int fds[DS_WL_MAX_FDS];
        int nfds = 0;
        ssize_t r =
            broker_recv(client, &hdr, &info, sizeof(info), fds, &nfds);
        if (r <= 0) {
          close(client);
          continue;
        }

        if (hdr.tag == DS_WL_TAG_ATTACH) {
          /* Consumer connecting with buffer fds */
          broker_close_peer(&bs, &bs.consumer_fd);
          broker_close_bufs(&bs);
          bs.consumer_fd = client;
          memcpy(bs.buf_fds, fds, (size_t)nfds * sizeof(int));
          bs.buf_nfds = nfds;
          if (hdr.len >= sizeof(info))
            bs.disp = info;
          bs.has_attach = 1;
          ev.events = EPOLLIN;
          ev.data.fd = client;
          epoll_ctl(bs.epoll_fd, EPOLL_CTL_ADD, client, &ev);
          ds_log("%s consumer connected (%d fds, %ux%u)", DS_WL_TAG, nfds,
                 bs.disp.width, bs.disp.height);
          broker_forward_to_producer(&bs);

        } else if (hdr.tag == DS_WL_TAG_BIND) {
          /* Producer registering */
          broker_close_peer(&bs, &bs.producer_fd);
          /* Close any fds that came with BIND (shouldn't have any) */
          for (int j = 0; j < nfds; j++)
            close(fds[j]);
          bs.producer_fd = client;
          ev.events = EPOLLIN;
          ev.data.fd = client;
          epoll_ctl(bs.epoll_fd, EPOLL_CTL_ADD, client, &ev);
          ds_log("%s producer connected", DS_WL_TAG);
          broker_forward_to_producer(&bs);

        } else {
          /* Unknown first message, reject */
          for (int j = 0; j < nfds; j++)
            close(fds[j]);
          close(client);
        }
        continue;
      }

      /* Peer event (data or hangup) */
      if (events[i].events & (EPOLLHUP | EPOLLERR)) {
        if (fd == bs.consumer_fd) {
          ds_log("%s consumer disconnected", DS_WL_TAG);
          broker_close_peer(&bs, &bs.consumer_fd);
          broker_close_bufs(&bs);
        } else if (fd == bs.producer_fd) {
          ds_log("%s producer disconnected", DS_WL_TAG);
          broker_close_peer(&bs, &bs.producer_fd);
        }
        continue;
      }

      /* Data from connected peer: buffer rotation (ATTACH again) */
      if (fd == bs.consumer_fd && (events[i].events & EPOLLIN)) {
        struct ds_wl_hdr hdr;
        struct ds_wl_display_info info;
        int fds[DS_WL_MAX_FDS];
        int nfds = 0;
        ssize_t r =
            broker_recv(fd, &hdr, &info, sizeof(info), fds, &nfds);
        if (r <= 0) {
          ds_log("%s consumer disconnected", DS_WL_TAG);
          broker_close_peer(&bs, &bs.consumer_fd);
          broker_close_bufs(&bs);
          continue;
        }
        if (hdr.tag == DS_WL_TAG_ATTACH) {
          broker_close_bufs(&bs);
          memcpy(bs.buf_fds, fds, (size_t)nfds * sizeof(int));
          bs.buf_nfds = nfds;
          if (hdr.len >= sizeof(info))
            bs.disp = info;
          bs.has_attach = 1;
          broker_forward_to_producer(&bs);
        } else {
          for (int j = 0; j < nfds; j++)
            close(fds[j]);
        }
      }

      /* Data from producer: currently unused but drain to avoid stall */
      if (fd == bs.producer_fd && (events[i].events & EPOLLIN)) {
        struct ds_wl_hdr hdr;
        char discard[256];
        int fds[DS_WL_MAX_FDS];
        int nfds = 0;
        ssize_t r =
            broker_recv(fd, &hdr, discard, sizeof(discard), fds, &nfds);
        if (r <= 0) {
          ds_log("%s producer disconnected", DS_WL_TAG);
          broker_close_peer(&bs, &bs.producer_fd);
        }
        for (int j = 0; j < nfds; j++)
          close(fds[j]);
      }
    }
  }

  /* Cleanup */
  broker_close_peer(&bs, &bs.consumer_fd);
  broker_close_peer(&bs, &bs.producer_fd);
  broker_close_bufs(&bs);
  close(bs.epoll_fd);
  close(bs.listen_fd);
  unlink(sock_path);
  ds_log("%s broker exiting", DS_WL_TAG);
}

/* ------------------------------------------------------------------ */
/* ds_spawn_daemon child wrapper                                      */
/* ------------------------------------------------------------------ */

static void broker_child(int ready_fd, void *user_data) {
  (void)user_data;
  ds_daemon_child_preamble();
  ds_selinux_enter_domain();
  mkdir_p(DS_WAYLAND_SOCK_DIR, 0755);

  /* Signal parent that we are ready (close the pipe without writing) */
  close(ready_fd);

  broker_loop(DS_WAYLAND_HOST_BRIDGE);
  _exit(0);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int ds_wayland_daemon_start(struct ds_config *cfg) {
  if (!cfg || !cfg->wayland || !is_android() || getuid() != 0)
    return -1;

  /* Reuse running daemon if still alive */
  pid_t existing = ds_daemon_read_pid("wayland" DS_EXT_WPID);
  if (existing > 0) {
    cfg->wayland_pid = existing;
    ds_log("%s reusing running broker (pid %d)", DS_WL_TAG, (int)existing);
    return 0;
  }

  /* Clean stale socket */
  unlink(DS_WAYLAND_HOST_BRIDGE);

  pid_t child =
      ds_spawn_daemon(broker_child, cfg, "wayland.log", "Wayland", DS_WL_TAG);
  if (child < 0)
    return -1;

  ds_daemon_write_pid("wayland" DS_EXT_WPID, child);
  cfg->wayland_pid = child;
  return 0;
}

void ds_wayland_daemon_stop(struct ds_config *cfg) {
  if (!is_android())
    return;
  ds_global_daemon_stop(check_wayland_needs, cfg->wayland_pid,
                        &cfg->wayland_pid, "wayland" DS_EXT_WPID,
                        DS_WAYLAND_HOST_BRIDGE, DS_WL_TAG);
}

int ds_setup_wayland_socket(struct ds_config *cfg) {
  if (!cfg || !cfg->wayland || !is_android())
    return 0;
  return ds_bind_mount_socket(DS_WAYLAND_OLDROOT_BRIDGE, DS_WAYLAND_BRIDGE_SOCK,
                              0, "Wayland");
}
