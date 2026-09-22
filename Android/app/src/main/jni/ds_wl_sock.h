// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * Socket helpers for the DS_WL consumer (clean-room)
 *
 * AF_UNIX connection, SCM_RIGHTS send/recv, blocking I/O primitives.
 */

#ifndef DS_WL_SOCK_H
#define DS_WL_SOCK_H

#include "ds_wl_proto.h"

/* Connect to an AF_UNIX stream socket at `path`.
 * Returns fd >= 0 on success, -1 on error. */
int ds_wl_connect_unix(const char *path);

/* Send `len` bytes reliably (handles partial writes).
 * Returns 0 on success, -1 on error. */
int ds_wl_send_all(int fd, const void *buf, size_t len);

/* Receive exactly `len` bytes (handles partial reads).
 * Returns 0 on success, -1 on error/EOF. */
int ds_wl_recv_all(int fd, void *buf, size_t len);

/* Send header + optional payload + optional SCM_RIGHTS fds.
 * Returns 0 on success, -1 on error. */
int ds_wl_send_msg(int fd, struct ds_wl_hdr *hdr, void *payload,
                   const int *fds, int nfds);

/* Receive header + optional payload + optional SCM_RIGHTS fds.
 * Returns bytes read (>= sizeof(hdr)), 0 on EOF, -1 on error. */
ssize_t ds_wl_recv_msg(int fd, struct ds_wl_hdr *hdr, void *payload,
                       size_t payload_cap, int *fds_out, int *nfds_out);

#endif /* DS_WL_SOCK_H */
