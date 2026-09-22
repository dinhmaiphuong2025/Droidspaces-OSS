// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * DS_WL wire protocol definitions (clean-room)
 *
 * Shared between the broker daemon (backend) and the NDK consumer
 * (app-side JNI library).  All structs are packed and use fixed-width
 * integer types for cross-process safety.
 */

#ifndef DS_WL_PROTO_H
#define DS_WL_PROTO_H

#include <stdint.h>

/* Every message starts with this header */
struct ds_wl_hdr {
  uint32_t tag;
  uint32_t len; /* payload bytes after header (0 if none) */
} __attribute__((packed));

/* Message tags */
#define DS_WL_TAG_ATTACH  0x4100 /* consumer -> broker: fd array + info */
#define DS_WL_TAG_READY   0x4200 /* broker -> producer: fd array ready  */
#define DS_WL_TAG_BIND    0x4300 /* producer -> broker: register        */
#define DS_WL_TAG_DISPLAY 0x4400 /* display metadata passthrough        */

/* Display metadata (payload of DS_WL_TAG_ATTACH and DS_WL_TAG_DISPLAY) */
struct ds_wl_display_info {
  uint32_t width;
  uint32_t height;
  uint32_t format;      /* AHARDWAREBUFFER_FORMAT_* or DRM fourcc */
  uint32_t refresh_mhz; /* e.g. 120000 = 120 Hz */
} __attribute__((packed));

/* Buffer descriptor, sent alongside fd array in ATTACH */
struct ds_wl_buf_desc {
  uint32_t width;
  uint32_t height;
  uint32_t stride; /* in pixels */
  uint32_t format; /* same as display_info.format */
} __attribute__((packed));

/* Maximum number of fds in a single SCM_RIGHTS transfer */
#define DS_WL_MAX_FDS 16

#endif /* DS_WL_PROTO_H */
