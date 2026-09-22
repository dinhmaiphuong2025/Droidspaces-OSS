// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * Socket helpers for the DS_WL consumer (clean-room)
 */

#include "ds_wl_sock.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <android/log.h>
#define LOG_TAG "DsWlSock"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

int ds_wl_connect_unix(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    LOGW("connect(%s): %s", path, strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

int ds_wl_send_all(int fd, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  while (len > 0) {
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

int ds_wl_recv_all(int fd, void *buf, size_t len) {
  char *p = (char *)buf;
  while (len > 0) {
    ssize_t n = recv(fd, p, len, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1; /* EOF */
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

int ds_wl_send_msg(int fd, struct ds_wl_hdr *hdr, void *payload,
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
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
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

  ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
  return n >= 0 ? 0 : -1;
}

ssize_t ds_wl_recv_msg(int fd, struct ds_wl_hdr *hdr, void *payload,
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
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = (size_t)iovcnt;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);

  ssize_t n = recvmsg(fd, &msg, 0);
  if (n <= 0) {
    if (nfds_out)
      *nfds_out = 0;
    return n;
  }

  int count = 0;
  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
       cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      int got = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      if (fds_out) {
        int take =
            got < (DS_WL_MAX_FDS - count) ? got : (DS_WL_MAX_FDS - count);
        memcpy(fds_out + count, CMSG_DATA(cmsg), (size_t)take * sizeof(int));
        count += take;
        for (int i = take; i < got; i++)
          close(((int *)CMSG_DATA(cmsg))[i]);
      } else {
        for (int i = 0; i < got; i++)
          close(((int *)CMSG_DATA(cmsg))[i]);
      }
    }
  }
  if (nfds_out)
    *nfds_out = count;
  return n;
}
