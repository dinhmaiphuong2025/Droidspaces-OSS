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
#include <string.h>

struct ds_gl_context {
  EGLDisplay display;
  EGLContext context;
  EGLSurface surface;
  EGLConfig config;

  GLuint program;
  GLuint texture_id;
  GLuint vbo;
  int initialized;
};

static struct ds_gl_context g_gl;

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
    "    gl_FragColor = texture2D(u_texture, v_texcoord);\n"
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

static int init_gl(struct ds_server *server) {
  if (g_gl.initialized) return 0;
  if (!server->window) return -1;

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

  g_gl.surface = eglCreateWindowSurface(g_gl.display, g_gl.config, server->window, NULL);
  if (g_gl.surface == EGL_NO_SURFACE) {
    DS_LOGE("eglCreateWindowSurface failed");
    return -1;
  }

  if (!eglMakeCurrent(g_gl.display, g_gl.surface, g_gl.surface, g_gl.context)) {
    DS_LOGE("eglMakeCurrent failed");
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
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad_data), quad_data, GL_STATIC_DRAW);

  glViewport(0, 0, server->width, server->height);
  g_gl.initialized = 1;
  DS_LOGI("Zero-Flicker Hardware Presenter initialized (%dx%d)", server->width, server->height);
  return 0;
}

void ds_presenter_init(struct ds_server *server) {
  (void)server;
}

void ds_presenter_present_surface(struct ds_server *server, struct ds_surface *surf) {
  if (!server || !surf || !surf->current_buffer) return;
  if (init_gl(server) < 0) return;

  eglMakeCurrent(g_gl.display, g_gl.surface, g_gl.surface, g_gl.context);

  glViewport(0, 0, server->width, server->height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glUseProgram(g_gl.program);

  glBindBuffer(GL_ARRAY_BUFFER, g_gl.vbo);
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
  eglSwapBuffers(g_gl.display, g_gl.surface);
}
