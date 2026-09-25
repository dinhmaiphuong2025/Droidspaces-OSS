// SPDX-License-Identifier: MIT
/*
 * Droidspaces Embedded Wayland Server - Zero-Flicker Hardware Presenter
 * Copyright (C) 2026 Droidspaces contributors
 */

#include "ds_server.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

struct ds_gl_context {
  EGLDisplay display;
  EGLContext context;
  EGLSurface surface;
  EGLConfig config;

  GLuint program;
  GLuint texture_id;
  GLuint vbo;
  int tex_w;
  int tex_h;
  int initialized;
};

static struct ds_gl_context g_gl;
static int g_first_frame_logged = 0;

static PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES = NULL;

static void log_gl_error(const char *tag);

static void resolve_image_procs(void) {
  static int resolved = 0;
  if (resolved) return;
  resolved = 1;
  g_eglCreateImageKHR =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  g_eglDestroyImageKHR =
      (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  g_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
      "glEGLImageTargetTexture2DOES");
}

/* Steady-state frames reuse the texture storage so the driver skips
 * reallocating a full 1440x3200 image on every commit. A size change
 * falls back to TexImage and records the new allocation. */
static void upload_tight_pixels(int width, int height, const uint8_t *data) {
  glBindTexture(GL_TEXTURE_2D, g_gl.texture_id);
  if (g_gl.tex_w == width && g_gl.tex_h == height) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
  } else {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, width, height, 0,
                 GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
    g_gl.tex_w = width;
    g_gl.tex_h = height;
  }
}

/* Clamp the commit damage to the buffer. Returns 0 when there is nothing
 * worth uploading, the caller then keeps the previous texture content. */
static int clamp_damage(struct ds_surface *surf, int width, int height,
                        int *dx, int *dy, int *dw, int *dh) {
  if (!surf || !surf->dmg_valid) return 0;
  int x1 = surf->dmg_x < 0 ? 0 : surf->dmg_x;
  int y1 = surf->dmg_y < 0 ? 0 : surf->dmg_y;
  int x2 = surf->dmg_x + surf->dmg_w > width ? width : surf->dmg_x + surf->dmg_w;
  int y2 = surf->dmg_y + surf->dmg_h > height ? height : surf->dmg_y + surf->dmg_h;
  *dx = x1;
  *dy = y1;
  *dw = x2 - x1;
  *dh = y2 - y1;
  return *dw > 0 && *dh > 0;
}

/* Tight rows with a damage rect go straight into the texture with no copy.
 * Anything else falls back to a full upload, which also covers the first
 * frame, resizes, and clients that never send damage. */
static void upload_subrect(struct ds_surface *surf, int width, int height,
                           const uint8_t *data, size_t stride) {
  int dx, dy, dw, dh;
  if (stride == (size_t)width * 4 && clamp_damage(surf, width, height, &dx, &dy, &dw, &dh) &&
      (dw < width || dh < height)) {
    if (g_gl.tex_w == width && g_gl.tex_h == height) {
      /* SHM rows start at the top while GL texture rows start at the
       * bottom, so cursor damage would land mirrored without this flip. */
      int dst_y = height - dy - dh;
      glBindTexture(GL_TEXTURE_2D, g_gl.texture_id);
      glTexSubImage2D(GL_TEXTURE_2D, 0, dx, dst_y, dw, dh,
                      GL_BGRA_EXT, GL_UNSIGNED_BYTE, data + (size_t)dy * stride + (size_t)dx * 4);
      return;
    }
  }
  upload_tight_pixels(width, height, data);
}

/* Import one single-plane linear dmabuf into the presentation texture.
 * Multi-plane or tiled buffers are skipped, the old frame stays up. */
static void upload_dmabuf_mmap_fallback(struct ds_surface *surf, struct ds_buffer *buf);
/* Last resort when EGLImage import fails: map the dma-buf on CPU and
 * upload like SHM. Works for system-memory buffers, not GPU-private ones. */
static void upload_dmabuf_mmap_fallback(struct ds_surface *surf, struct ds_buffer *buf) {
  if (!buf || buf->dmabuf_num_planes != 1 || buf->dmabuf_fds[0] < 0) return;
  if (buf->width <= 0 || buf->height <= 0) return;
  if (buf->format != DRM_FORMAT_ARGB8888 && buf->format != DRM_FORMAT_XRGB8888) {
    return;
  }

  size_t stride = buf->dmabuf_strides[0];
  size_t need = stride * (size_t)buf->height + buf->dmabuf_offsets[0];
  if (stride < (size_t)buf->width * 4) return;

  void *map = mmap(NULL, need, PROT_READ, MAP_SHARED, buf->dmabuf_fds[0], 0);
  if (map == MAP_FAILED) {
    DS_LOGE("dmabuf mmap failed, giving up on this buffer");
    return;
  }

  uint8_t *data = (uint8_t *)map + buf->dmabuf_offsets[0];
  if (stride == (size_t)buf->width * 4) {
    upload_subrect(surf, buf->width, buf->height, data, stride);
  } else {
    size_t row = (size_t)buf->width * 4;
    uint8_t *tight = malloc(row * (size_t)buf->height);
    if (tight) {
      for (int y = 0; y < buf->height; y++) {
        memcpy(tight + (size_t)y * row, data + (size_t)y * stride, row);
      }
      upload_tight_pixels(buf->width, buf->height, tight);
      free(tight);
    }
  }
  munmap(map, need);
  log_gl_error("upload_dmabuf_mmap");

  if (!g_first_frame_logged) {
    g_first_frame_logged = 1;
    DS_LOGI("presented first dmabuf frame via mmap (%dx%d)", buf->width, buf->height);
  }
}

static void upload_dmabuf_buffer(struct ds_surface *surf, struct ds_buffer *buf) {
  if (!buf || buf->dmabuf_num_planes != 1 || buf->dmabuf_fds[0] < 0) return;
  if (buf->width <= 0 || buf->height <= 0) return;
  if (buf->dmabuf_modifiers[0] != 0) {
    /* Tiled or compressed buffers cannot be read as linear rows, so the
     * old frame stays up instead of blocky garbage. */
    static int logged = 0;
    if (!logged) {
      logged = 1;
      DS_LOGI("skipping non-linear dmabuf (modifier 0x%llx)",
              (unsigned long long)buf->dmabuf_modifiers[0]);
    }
    return;
  }
  if (buf->format != DRM_FORMAT_ARGB8888 && buf->format != DRM_FORMAT_XRGB8888) {
    return;
  }

  resolve_image_procs();
  if (!g_eglCreateImageKHR || !g_eglDestroyImageKHR || !g_glEGLImageTargetTexture2DOES) {
    DS_LOGE("dmabuf import procs missing");
    return;
  }

  /* MODIFIER_LO/HI are only valid with
   * EGL_EXT_image_dma_buf_import_modifiers. Drivers without it fail the
   * whole import with EGL_BAD_ACCESS when they appear, so for linear
   * buffers (the only ones accepted above) they stay out. */
  EGLint attrs[] = {
      EGL_WIDTH, buf->width,
      EGL_HEIGHT, buf->height,
      EGL_LINUX_DRM_FOURCC_EXT, (EGLint)buf->format,
      EGL_DMA_BUF_PLANE0_FD_EXT, buf->dmabuf_fds[0],
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)buf->dmabuf_offsets[0],
      EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)buf->dmabuf_strides[0],
      EGL_NONE,
  };

  EGLImageKHR img = g_eglCreateImageKHR(g_gl.display, EGL_NO_CONTEXT,
                                       EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
  if (img == EGL_NO_IMAGE_KHR) {
    DS_LOGE("eglCreateImage failed: 0x%x", eglGetError());
    upload_dmabuf_mmap_fallback(surf, buf);
    return;
  }

  glBindTexture(GL_TEXTURE_2D, g_gl.texture_id);
  g_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
  g_eglDestroyImageKHR(g_gl.display, img);

  if (!g_first_frame_logged) {
    g_first_frame_logged = 1;
    DS_LOGI("presented first dmabuf frame (%dx%d)", buf->width, buf->height);
  }
}
/* Report the first GL/EGL error with a tag so logcat shows what failed */
static void log_gl_error(const char *tag) {
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    DS_LOGE("%s: GL error 0x%x", tag, err);
  }
  EGLint egl_err = eglGetError();
  if (egl_err != EGL_SUCCESS) {
    DS_LOGE("%s: EGL error 0x%x", tag, egl_err);
  }
}

/* Copy one SHM buffer into the presentation texture. Only ARGB/XRGB8888
 * are handled; anything else keeps the previous frame instead of garbage. */
static void upload_shm_buffer(struct ds_surface *surf, struct ds_buffer *buf) {
  if (!buf || !buf->shm || buf->width <= 0 || buf->height <= 0) return;
  if (buf->format != WL_SHM_FORMAT_ARGB8888 && buf->format != WL_SHM_FORMAT_XRGB8888) {
    return;
  }

  wl_shm_buffer_begin_access(buf->shm);
  uint8_t *data = wl_shm_buffer_get_data(buf->shm);
  int32_t stride = wl_shm_buffer_get_stride(buf->shm);
  if (data && stride >= buf->width * 4) {
    if (stride == buf->width * 4) {
      upload_subrect(surf, buf->width, buf->height, data, (size_t)stride);
    } else {
      /* Padded rows: repack into a tight buffer, ES2 has no row length */
      size_t row = (size_t)buf->width * 4;
      uint8_t *tight = malloc(row * (size_t)buf->height);
      if (tight) {
        for (int y = 0; y < buf->height; y++) {
          memcpy(tight + (size_t)y * row, data + (size_t)y * stride, row);
        }
        upload_tight_pixels(buf->width, buf->height, tight);
        free(tight);
      }
    }
    if (!g_first_frame_logged) {
      g_first_frame_logged = 1;
      DS_LOGI("presented first client frame (%dx%d)", buf->width, buf->height);
    }
  }
  wl_shm_buffer_end_access(buf->shm);
  log_gl_error("upload_shm");
}

static const char *vertex_shader_source =
    "attribute vec4 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = a_position;\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

static const char *fragment_shader_source =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "    vec4 c = texture2D(u_texture, v_texcoord);\n"
    "    gl_FragColor = vec4(c.rgb, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  if (!shader) return 0;

  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);

  GLint compiled = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

/* Display and context live as long as the process. The EGL surface is
 * tied to the current ANativeWindow and is recreated on every swap. */
static int ensure_context(struct ds_server *server) {
  (void)server;
  if (g_gl.context != EGL_NO_CONTEXT) return 0;

  g_gl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_gl.display == EGL_NO_DISPLAY) {
    DS_LOGE("eglGetDisplay failed");
    return -1;
  }

  if (!eglInitialize(g_gl.display, NULL, NULL)) {
    DS_LOGE("eglInitialize failed");
    return -1;
  }

  const EGLint attribs[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_BLUE_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_RED_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
  };

  EGLint num_configs;
  if (!eglChooseConfig(g_gl.display, attribs, &g_gl.config, 1, &num_configs) || num_configs < 1) {
    DS_LOGE("eglChooseConfig failed");
    return -1;
  }

  const EGLint ctx_attribs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
  };

  g_gl.context = eglCreateContext(g_gl.display, g_gl.config, EGL_NO_CONTEXT, ctx_attribs);
  if (g_gl.context == EGL_NO_CONTEXT) {
    DS_LOGE("eglCreateContext failed");
    return -1;
  }

  EGLint format = 0;
  if (eglGetConfigAttrib(g_gl.display, g_gl.config, EGL_NATIVE_VISUAL_ID, &format)) {
    ANativeWindow_setBuffersGeometry(server->window, 0, 0, format);
  }

  g_gl.surface = eglCreateWindowSurface(g_gl.display, g_gl.config, server->window, NULL);
  if (g_gl.surface == EGL_NO_SURFACE) {
    DS_LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
    return -1;
  }

  if (!eglMakeCurrent(g_gl.display, g_gl.surface, g_gl.surface, g_gl.context)) {
    DS_LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
    return -1;
  }

  /* Compile presentation quad shader */
  GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
  if (!vs || !fs) return -1;

  g_gl.program = glCreateProgram();
  glAttachShader(g_gl.program, vs);
  glAttachShader(g_gl.program, fs);
  glBindAttribLocation(g_gl.program, 0, "a_position");
  glBindAttribLocation(g_gl.program, 1, "a_texcoord");
  glLinkProgram(g_gl.program);

  glDeleteShader(vs);
  glDeleteShader(fs);

  glGenTextures(1, &g_gl.texture_id);
  glBindTexture(GL_TEXTURE_2D, g_gl.texture_id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  /* Fullscreen Quad: position (x, y), texcoord (u, v) */
  const GLfloat quad_data[] = {
      /* X,     Y,    U,    V */
      -1.0f, -1.0f, 0.0f, 1.0f,
       1.0f, -1.0f, 1.0f, 1.0f,
      -1.0f,  1.0f, 0.0f, 0.0f,
       1.0f,  1.0f, 1.0f, 0.0f,
  };

  glGenBuffers(1, &g_gl.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, g_gl.vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad_data), quad_data, GL_DYNAMIC_DRAW);

  DS_LOGI("presenter GL ready (%dx%d)", server->width, server->height);
  return 0;
}

static int ensure_surface(struct ds_server *server) {
  if (g_gl.surface != EGL_NO_SURFACE) return 0;
  if (!server->window) return -1;

  EGLint format = 0;
  if (eglGetConfigAttrib(g_gl.display, g_gl.config, EGL_NATIVE_VISUAL_ID, &format)) {
    ANativeWindow_setBuffersGeometry(server->window, 0, 0, format);
  }

  g_gl.surface = eglCreateWindowSurface(g_gl.display, g_gl.config, server->window, NULL);
  if (g_gl.surface == EGL_NO_SURFACE) {
    DS_LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
    return -1;
  }

  if (!eglMakeCurrent(g_gl.display, g_gl.surface, g_gl.surface, g_gl.context)) {
    DS_LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
    eglDestroySurface(g_gl.display, g_gl.surface);
    g_gl.surface = EGL_NO_SURFACE;
    return -1;
  }

  glViewport(0, 0, server->width, server->height);
  return 0;
}

static int init_gl(struct ds_server *server) {
  if (ensure_context(server) < 0) return -1;
  return ensure_surface(server);
}

/* Drop the EGL surface without touching the context. The caller must hold
 * server->lock, or the event thread must already be stopped. */
void ds_presenter_detach(struct ds_server *server) {
  (void)server;
  if (g_gl.display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(g_gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (g_gl.surface != EGL_NO_SURFACE) {
    eglDestroySurface(g_gl.display, g_gl.surface);
    g_gl.surface = EGL_NO_SURFACE;
  }
}

void ds_presenter_init(struct ds_server *server) {
  (void)server;
}

static uint64_t g_present_total_ns = 0;
static int g_present_count = 0;

void ds_presenter_present_surface(struct ds_server *server, struct ds_surface *surf) {
  if (!server || !surf || !surf->current_buffer) return;
  if (init_gl(server) < 0) return;

  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  eglMakeCurrent(g_gl.display, g_gl.surface, g_gl.surface, g_gl.context);

  glViewport(0, 0, server->width, server->height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  /* Upload client pixels. Without this the quad stays black. */
  if (surf->current_buffer->is_shm && surf->current_buffer->shm) {
    upload_shm_buffer(surf, surf->current_buffer);
  } else if (surf->current_buffer->is_dmabuf) {
    upload_dmabuf_buffer(surf, surf->current_buffer);
  }

  glUseProgram(g_gl.program);

  /* Aspect ratio correction: fit buffer into viewport without stretching */
  float qx = 1.0f;
  float qy = 1.0f;
  if (surf->width > 0 && surf->height > 0 && server->width > 0 && server->height > 0) {
    float buf_aspect = (float)surf->width / (float)surf->height;
    float win_aspect = (float)server->width / (float)server->height;
    if (buf_aspect > win_aspect * 1.001f) {
      /* Buffer is wider than window: letterbox top and bottom */
      qy = win_aspect / buf_aspect;
    } else if (buf_aspect < win_aspect * 0.999f) {
      /* Buffer is taller than window: pillarbox left and right */
      qx = buf_aspect / win_aspect;
    }
  }

  const GLfloat quad_data[] = {
      -qx, -qy, 0.0f, 1.0f,
       qx, -qy, 1.0f, 1.0f,
      -qx,  qy, 0.0f, 0.0f,
       qx,  qy, 1.0f, 0.0f,
  };

  glBindBuffer(GL_ARRAY_BUFFER, g_gl.vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad_data), quad_data);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *)0);

  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *)(2 * sizeof(GLfloat)));

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_gl.texture_id);

  /* Draw Quad */
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);

  /* SwapBuffers only when a real frame was committed -> ZERO FLICKER */
  if (!eglSwapBuffers(g_gl.display, g_gl.surface)) {
    DS_LOGE("eglSwapBuffers failed");
  }
  log_gl_error("present");

  /* Throttled timing so logcat shows real present cost without spamming */
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  g_present_total_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                        (uint64_t)(t1.tv_nsec - t0.tv_nsec);
  if (++g_present_count >= 120) {
    DS_LOGI("presenter: 120 frames avg %.1fms", (double)g_present_total_ns / 120000000.0);
    g_present_total_ns = 0;
    g_present_count = 0;
  }
}
