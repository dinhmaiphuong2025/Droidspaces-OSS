// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidspaces contributors

/*
 * Input/output event definitions for the DS_WL data channel (clean-room)
 *
 * These travel over the socketpair between consumer (app) and producer
 * (container), NOT through the broker.  Packed structs, fixed-width.
 */

#ifndef DS_WL_INPUT_H
#define DS_WL_INPUT_H

#include <stdint.h>

/* Consumer -> producer event types */
#define DS_WL_INPUT_TOUCH       1
#define DS_WL_INPUT_KEY         2
#define DS_WL_INPUT_PTR_MOTION  3
#define DS_WL_INPUT_PTR_BUTTON  4
#define DS_WL_INPUT_PTR_AXIS    5
#define DS_WL_INPUT_TOUCH_FRAME 6
#define DS_WL_INPUT_ROTATION    7
#define DS_WL_INPUT_PRESENTED   8

struct ds_wl_input_ev {
  uint32_t type;
  union {
    struct {
      int32_t action;
      int32_t pointer_id;
      float x;
      float y;
    } touch;

    struct {
      int32_t code;
      int32_t action; /* 0 = up, 1 = down */
    } key;

    struct {
      float x;
      float y;
      float dx;
      float dy;
    } ptr_motion;

    struct {
      int32_t button;
      int32_t pressed; /* 0 = released, 1 = pressed */
    } ptr_button;

    struct {
      int32_t axis;
      float value;
      int32_t discrete;
    } ptr_axis;

    struct {
      int32_t degrees;
    } rotation;

    /* TOUCH_FRAME and PRESENTED carry no payload beyond the type */
    uint8_t _pad[24];
  };
} __attribute__((packed));

/* Producer -> consumer event types */
#define DS_WL_OUTPUT_CURSOR_POS 1
#define DS_WL_OUTPUT_CURSOR_IMG 2

struct ds_wl_output_ev {
  uint32_t type;
  union {
    struct {
      float x;
      float y;
      int32_t visible;
    } cursor_pos;

    struct {
      uint32_t width;
      uint32_t height;
      uint32_t hotspot_x;
      uint32_t hotspot_y;
      /* Pixel data follows as width*height*4 bytes (ARGB8888) */
    } cursor_img;

    uint8_t _pad[24];
  };
} __attribute__((packed));

#endif /* DS_WL_INPUT_H */
