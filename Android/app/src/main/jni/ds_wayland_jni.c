#define _GNU_SOURCE
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "anw_hidden.h"
#include "display_consumer.h"
#include "protocol.h"
#include "socket_utils.h"

#define TAG "DsWaylandJni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define BUFFER_COUNT 4

struct render_state {
    pthread_mutex_t lock;
    ANativeWindow *window;
    display_ctx *ctx;
    pthread_t render_thread;
    volatile bool running;

    int screen_w;
    int screen_h;
    uint32_t refresh_mhz;
    uint32_t presented_frame_count;

    int buf_count;
    int dmabuf_fds[MAX_BUFS];
    struct buf_info dmabuf_infos[MAX_BUFS];
    ANativeWindowBuffer *buf_anb[MAX_BUFS];

    char socket_path[256];
};

static struct render_state g_state;
static struct anw_api api;
static bool api_loaded = false;

static int collect_buffers(struct render_state *s)
{
    ANativeWindow *win = s->window;
    int min_undequeued = 0;
    if (api.query) {
        api.query(win, ANATIVEWINDOW_QUERY_MIN_UNDEQUEUED_BUFFERS, &min_undequeued);
    }
    int target = min_undequeued + 2;
    if (target < 4) target = 4;
    if (target > MAX_BUFS) target = MAX_BUFS;

    if (api.setBufferCount) {
        api.setBufferCount(win, (size_t)target);
    }

    int found = 0;
    int queued = 0;

    LOGI("collecting %d buffers for SurfaceView via dequeue/queue", target);

    for (int attempt = 0; attempt < target * 4 && found < target; attempt++) {
        ANativeWindowBuffer *anb = NULL;
        int fence = -1;
        if (api.dequeueBuffer(win, &anb, &fence) != 0 || !anb) {
            LOGE("dequeueBuffer failed at attempt %d", attempt);
            if (fence >= 0)
                close(fence);
            break;
        }
        if (fence >= 0)
            close(fence);

        if (!anb->handle || anb->handle->numFds < 1) {
            LOGE("dequeued buffer has no valid handle");
            api.cancelBuffer(win, anb, -1);
            continue;
        }

        int fd = anb->handle->data[0];
        bool dup_found = false;
        for (int i = 0; i < found; i++) {
            if (s->buf_anb[i] == anb) {
                dup_found = true;
                break;
            }
        }

        /* Queue the buffer back so BufferQueue rotates to another slot */
        api.queueBuffer(win, anb, -1);
        queued++;

        if (dup_found)
            continue;

        int dup_fd = dup(fd);
        if (dup_fd < 0)
            continue;

        s->buf_anb[found] = anb;
        s->dmabuf_fds[found] = dup_fd;
        s->dmabuf_infos[found].stride = (uint32_t)(anb->stride * 4);
        s->dmabuf_infos[found].width  = (uint32_t)anb->width;
        s->dmabuf_infos[found].height = (uint32_t)anb->height;
        s->dmabuf_infos[found].format = (uint32_t)anb->format;
        s->dmabuf_infos[found].modifier = 0;
        s->dmabuf_infos[found].offset = 0;
        LOGI("  collected buf[%d]: anb=%p fd=%d dup_fd=%d %dx%d stride=%d",
             found, (void *)anb, fd, dup_fd, anb->width, anb->height, anb->stride);
        found++;
    }

    /* Drain the queued buffers back to free state */
    for (int i = 0; i < queued; i++) {
        ANativeWindowBuffer *danb = NULL;
        int dfence = -1;
        int rc = -1;
        for (int retry = 0; retry < 5; retry++) {
            rc = api.dequeueBuffer(win, &danb, &dfence);
            if (rc == 0 && danb)
                break;
            if (dfence >= 0) {
                close(dfence);
                dfence = -1;
            }
            danb = NULL;
            usleep(2000);
        }
        if (rc != 0 || !danb)
            break;
        if (dfence >= 0)
            close(dfence);
        api.cancelBuffer(win, danb, -1);
    }

    if (found < 2) {
        LOGE("failed to collect sufficient buffers: found %d (needed at least 2)", found);
        for (int i = 0; i < found; i++) {
            if (s->dmabuf_fds[i] >= 0) {
                close(s->dmabuf_fds[i]);
                s->dmabuf_fds[i] = -1;
            }
        }
        return -1;
    }

    s->buf_count = found;
    LOGI("successfully collected %d unique DMA-BUF buffers", found);
    return 0;
}

static void *render_loop(void *arg)
{
    struct render_state *s = (struct render_state *)arg;
    LOGI("render loop started");

    while (s->running) {
        if (!s->ctx) {
            /* Attempt to connect to Droidspaces Wayland broker socket */
            display_ctx *new_ctx = NULL;
            if (connect_to_daemon(&new_ctx, s->socket_path) < 0) {
                usleep(100000); /* 100ms backoff */
                continue;
            }

            LOGI("connected to Wayland bridge daemon, negotiating screen info");
            set_screen_info(new_ctx, (uint32_t)s->screen_w, (uint32_t)s->screen_h, 1, s->refresh_mhz);

            if (push_dmabufs(new_ctx, s->dmabuf_fds, s->dmabuf_infos, s->buf_count) < 0) {
                LOGE("push_dmabufs failed");
                disconnect_daemon(new_ctx);
                usleep(200000);
                continue;
            }
            LOGI("DMABUF push complete, ready to receive frames");

            pthread_mutex_lock(&s->lock);
            s->ctx = new_ctx;
            pthread_mutex_unlock(&s->lock);
        }

        ANativeWindowBuffer *anb = NULL;
        int fence = -1;
        if (api.dequeueBuffer(s->window, &anb, &fence) != 0 || !anb) {
            if (fence >= 0)
                close(fence);
            usleep(8000);
            continue;
        }

        /* CPU-wait acquire fence so SurfaceFlinger finishes reading before Niri overwrites */
        if (fence >= 0) {
            struct pollfd fpfd = { .fd = fence, .events = POLLIN, .revents = 0 };
            poll(&fpfd, 1, 1000);
            close(fence);
            fence = -1;
        }

        int idx = -1;
        for (int i = 0; i < s->buf_count; i++) {
            if (s->buf_anb[i] == anb) {
                idx = i;
                break;
            }
        }

        if (idx < 0) {
            api.cancelBuffer(s->window, anb, -1);
            usleep(8000);
            continue;
        }

        /* Send presentation feedback to producer */
        pthread_mutex_lock(&s->lock);
        if (s->ctx) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            struct InputEvent pev;
            memset(&pev, 0, sizeof(pev));
            pev.type = INPUT_TYPE_PRESENTED;
            pev.presented.buffer_index = (uint32_t)idx;
            pev.presented.frame_seq = s->presented_frame_count++;
            pev.presented.tv_sec = (uint32_t)ts.tv_sec;
            pev.presented.tv_nsec = (uint32_t)ts.tv_nsec;
            push_input_event(s->ctx, &pev);
        }
        pthread_mutex_unlock(&s->lock);

        if (select_dmabuf(s->ctx, idx) < 0) {
            api.cancelBuffer(s->window, anb, -1);
            pthread_mutex_lock(&s->lock);
            display_ctx *old_ctx = s->ctx;
            s->ctx = NULL;
            pthread_mutex_unlock(&s->lock);
            if (old_ctx) disconnect_daemon(old_ctx);
            usleep(50000);
            continue;
        }

        int rfence = -1;
        int status = refresh_done_status(s->ctx, &rfence);
        if (status == 1) {
            /* Frame rendered successfully: send to SurfaceFlinger */
            api.queueBuffer(s->window, anb, rfence);
        } else if (status == 0) {
            /* NoDamage: keep previous frame on screen without flickering */
            api.cancelBuffer(s->window, anb, -1);
            usleep(8000);
        } else {
            /* Error or fallback */
            api.cancelBuffer(s->window, anb, -1);
            pthread_mutex_lock(&s->lock);
            display_ctx *old_ctx = s->ctx;
            s->ctx = NULL;
            pthread_mutex_unlock(&s->lock);
            if (old_ctx) disconnect_daemon(old_ctx);
            usleep(50000);
        }
    }

    pthread_mutex_lock(&s->lock);
    display_ctx *old_ctx = s->ctx;
    s->ctx = NULL;
    pthread_mutex_unlock(&s->lock);
    if (old_ctx) {
        disconnect_daemon(old_ctx);
    }

    LOGI("render loop terminated");
    return NULL;
}

static void stop_render_thread(void)
{
    pthread_t t = 0;
    pthread_mutex_lock(&g_state.lock);
    if (g_state.running) {
        g_state.running = false;
        if (g_state.ctx) {
            wake_consumer(g_state.ctx);
        }
        t = g_state.render_thread;
        g_state.render_thread = 0;
    }
    pthread_mutex_unlock(&g_state.lock);

    if (t != 0) {
        pthread_join(t, NULL);
    }
}

JNIEXPORT void JNICALL
Java_com_droidspaces_app_ui_wayland_WaylandNative_nativeInit(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    pthread_mutex_init(&g_state.lock, NULL);
    if (!api_loaded) {
        load_anw_api(&api);
        api_loaded = true;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_droidspaces_app_ui_wayland_WaylandNative_nativeSetSurface(
    JNIEnv *env, jclass clazz, jobject jsurface, jint width, jint height, jint refreshMhz, jstring jpath)
{
    (void)clazz;
    stop_render_thread();

    pthread_mutex_lock(&g_state.lock);

    if (g_state.window) {
        anw_api_disconnect(g_state.window, ANW_API_CPU);
        ANativeWindow_release(g_state.window);
        g_state.window = NULL;
    }

    for (int i = 0; i < g_state.buf_count; i++) {
        if (g_state.dmabuf_fds[i] >= 0) {
            close(g_state.dmabuf_fds[i]);
            g_state.dmabuf_fds[i] = -1;
        }
    }
    g_state.buf_count = 0;

    if (!jsurface) {
        pthread_mutex_unlock(&g_state.lock);
        return JNI_TRUE;
    }

    ANativeWindow *win = ANativeWindow_fromSurface(env, jsurface);
    if (!win) {
        pthread_mutex_unlock(&g_state.lock);
        return JNI_FALSE;
    }

    struct anw_window *aw = (struct anw_window *)win;
    api.dequeueBuffer = (int (*)(ANativeWindow *, ANativeWindowBuffer **, int *))(void *)aw->dequeueBuffer;
    api.queueBuffer   = (int (*)(ANativeWindow *, ANativeWindowBuffer *, int))(void *)aw->queueBuffer;
    api.cancelBuffer  = (int (*)(ANativeWindow *, ANativeWindowBuffer *, int))(void *)aw->cancelBuffer;

    anw_api_connect(win, ANW_API_CPU);

    ANativeWindow_setBuffersGeometry(win, width, height, WINDOW_FORMAT_RGBA_8888);

    g_state.window = win;
    g_state.screen_w = width;
    g_state.screen_h = height;
    g_state.refresh_mhz = (uint32_t)refreshMhz;

    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (path) {
        strncpy(g_state.socket_path, path, sizeof(g_state.socket_path) - 1);
        (*env)->ReleaseStringUTFChars(env, jpath, path);
    } else {
        strncpy(g_state.socket_path, "/data/local/tmp/ds-wayland/ds-wayland.sock", sizeof(g_state.socket_path) - 1);
    }

    if (collect_buffers(&g_state) < 0) {
        LOGE("failed to collect buffers for native window");
        anw_api_disconnect(win, ANW_API_CPU);
        ANativeWindow_release(win);
        g_state.window = NULL;
        pthread_mutex_unlock(&g_state.lock);
        return JNI_FALSE;
    }

    g_state.running = true;
    if (pthread_create(&g_state.render_thread, NULL, render_loop, &g_state) != 0) {
        g_state.running = false;
        pthread_mutex_unlock(&g_state.lock);
        return JNI_FALSE;
    }

    pthread_mutex_unlock(&g_state.lock);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_droidspaces_app_ui_wayland_WaylandNative_nativeDestroySurface(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    stop_render_thread();

    pthread_mutex_lock(&g_state.lock);
    if (g_state.window) {
        anw_api_disconnect(g_state.window, ANW_API_CPU);
        ANativeWindow_release(g_state.window);
        g_state.window = NULL;
    }
    for (int i = 0; i < g_state.buf_count; i++) {
        if (g_state.dmabuf_fds[i] >= 0) {
            close(g_state.dmabuf_fds[i]);
            g_state.dmabuf_fds[i] = -1;
        }
    }
    g_state.buf_count = 0;
    pthread_mutex_unlock(&g_state.lock);
}

static inline void safe_send_input(const struct InputEvent *ev)
{
    pthread_mutex_lock(&g_state.lock);
    if (g_state.ctx) {
        push_input_event(g_state.ctx, ev);
    }
    pthread_mutex_unlock(&g_state.lock);
}

JNIEXPORT void JNICALL
Java_com_droidspaces_app_ui_wayland_WaylandNative_nativeSendTouch(
    JNIEnv *env, jclass clazz, jint action, jint pointerId, jfloat x, jfloat y)
{
    (void)env;
    (void)clazz;

    struct InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_TYPE_TOUCH;
    ev.touch.action = action;
    ev.touch.pointer_id = pointerId;
    ev.touch.x = x;
    ev.touch.y = y;

    safe_send_input(&ev);
}

JNIEXPORT void JNICALL
Java_com_droidspaces_app_ui_wayland_WaylandNative_nativeSendTouchFrame(
    JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;

    struct InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_TYPE_TOUCH_FRAME;

    safe_send_input(&ev);
}

JNIEXPORT void JNICALL
Java_com_droidspaces_app_ui_wayland_WaylandNative_nativeSendKey(
    JNIEnv *env, jclass clazz, jint keyCode, jint action)
{
    (void)env;
    (void)clazz;

    struct InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_TYPE_KEY;
    ev.key.keycode = keyCode;
    ev.key.action = action;

    safe_send_input(&ev);
}

JNIEXPORT void JNICALL
Java_com_droidspaces_app_ui_wayland_WaylandNative_nativeSendPointerMotion(
    JNIEnv *env, jclass clazz, jfloat x, jfloat y, jfloat dx, jfloat dy)
{
    (void)env;
    (void)clazz;

    struct InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_TYPE_POINTER_MOTION;
    ev.pointer_motion.x = x;
    ev.pointer_motion.y = y;
    ev.pointer_motion.dx = dx;
    ev.pointer_motion.dy = dy;

    safe_send_input(&ev);
}

JNIEXPORT void JNICALL
Java_com_droidspaces_app_ui_wayland_WaylandNative_nativeSendPointerButton(
    JNIEnv *env, jclass clazz, jint button, jint pressed)
{
    (void)env;
    (void)clazz;

    struct InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_TYPE_POINTER_BUTTON;
    ev.pointer_button.button = (uint32_t)button;
    ev.pointer_button.pressed = pressed;

    safe_send_input(&ev);
}

JNIEXPORT void JNICALL
Java_com_droidspaces_app_ui_wayland_WaylandNative_nativeSendPointerAxis(
    JNIEnv *env, jclass clazz, jint axis, jfloat value, jint discrete)
{
    (void)env;
    (void)clazz;

    struct InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_TYPE_POINTER_AXIS;
    ev.pointer_axis.axis = (uint32_t)axis;
    ev.pointer_axis.value = value;
    ev.pointer_axis.discrete = discrete;

    safe_send_input(&ev);
}

JNIEXPORT void JNICALL
Java_com_droidspaces_app_ui_wayland_WaylandNative_nativeSendDisplayRotation(
    JNIEnv *env, jclass clazz, jint rotationDeg)
{
    (void)env;
    (void)clazz;

    struct InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_TYPE_DISPLAY_ROTATION;
    ev.display_rotation.angle_deg = (uint32_t)rotationDeg;

    safe_send_input(&ev);
}
