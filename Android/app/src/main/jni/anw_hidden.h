#ifndef DS_ANW_HIDDEN_H
#define DS_ANW_HIDDEN_H

#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>

#define ANATIVEWINDOW_QUERY_MIN_UNDEQUEUED_BUFFERS 3

#ifndef NATIVE_HANDLE_H_
#define NATIVE_HANDLE_H_
typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-length-array"
#endif
    int data[0];
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
} native_handle_t;
#endif

typedef struct android_native_base_t {
    int magic;
    int version;
    void *reserved[4];
    void (*incRef)(struct android_native_base_t *base);
    void (*decRef)(struct android_native_base_t *base);
} android_native_base_t;

typedef struct ANativeWindowBuffer {
    android_native_base_t common;
    int width;
    int height;
    int stride;
    int format;
    int usage_deprecated;
    uintptr_t layerCount;
    void *reserved[1];
    const native_handle_t *handle;
    uint64_t usage;
    void *reserved_proc[8 - (sizeof(uint64_t) / sizeof(void *))];
} ANativeWindowBuffer;

enum {
    ANW_API_CONNECT    = 13,
    ANW_API_DISCONNECT = 14,
    ANW_API_CPU        = 2,
};

struct anw_window {
    android_native_base_t common;
    const uint32_t flags;
    const int   minSwapInterval;
    const int   maxSwapInterval;
    const float xdpi;
    const float ydpi;
    intptr_t    oem[4];
    int (*setSwapInterval)(struct anw_window *, int);
    int (*dequeueBuffer_DEPRECATED)(struct anw_window *, ANativeWindowBuffer **);
    int (*lockBuffer_DEPRECATED)(struct anw_window *, ANativeWindowBuffer *);
    int (*queueBuffer_DEPRECATED)(struct anw_window *, ANativeWindowBuffer *);
    int (*query)(const struct anw_window *, int, int *);
    int (*perform)(struct anw_window *, int, ...);
    int (*cancelBuffer_DEPRECATED)(struct anw_window *, ANativeWindowBuffer *);
    int (*dequeueBuffer)(struct anw_window *, ANativeWindowBuffer **, int *fenceFd);
    int (*queueBuffer)(struct anw_window *, ANativeWindowBuffer *, int fenceFd);
    int (*cancelBuffer)(struct anw_window *, ANativeWindowBuffer *, int fenceFd);
};

static inline int anw_api_connect(ANativeWindow *w, int api)
{
    struct anw_window *aw = (struct anw_window *)w;
    return aw->perform(aw, ANW_API_CONNECT, api);
}

static inline int anw_api_disconnect(ANativeWindow *w, int api)
{
    struct anw_window *aw = (struct anw_window *)w;
    return aw->perform(aw, ANW_API_DISCONNECT, api);
}

typedef int (*pfn_ANativeWindow_setBufferCount)(ANativeWindow *, size_t);
typedef int (*pfn_ANativeWindow_setUsage)(ANativeWindow *, uint64_t);

struct anw_api {
    int (*dequeueBuffer)(ANativeWindow *, ANativeWindowBuffer **, int *);
    int (*queueBuffer)(ANativeWindow *, ANativeWindowBuffer *, int);
    int (*cancelBuffer)(ANativeWindow *, ANativeWindowBuffer *, int);
    pfn_ANativeWindow_setBufferCount setBufferCount;
    pfn_ANativeWindow_setUsage setUsage;
};

static inline bool load_anw_api(struct anw_api *out)
{
    void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib)
        return false;

    out->setBufferCount = (pfn_ANativeWindow_setBufferCount)dlsym(lib, "ANativeWindow_setBufferCount");
    out->setUsage = (pfn_ANativeWindow_setUsage)dlsym(lib, "ANativeWindow_setUsage");

    out->dequeueBuffer = NULL;
    out->queueBuffer = NULL;
    out->cancelBuffer = NULL;
    return true;
}

#endif /* DS_ANW_HIDDEN_H */
