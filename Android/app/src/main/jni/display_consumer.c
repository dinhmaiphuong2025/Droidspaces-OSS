#define _GNU_SOURCE
#include "display_consumer.h"
#include "socket_utils.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

struct display_ctx {
    int      ctrl_fd;
    int      data_fd;
    int      buf_ready_efd;
    int      fence_fd;
    int      shm_fd;
    int      wake_efd;
    volatile uint32_t *shm_ptr;
    uint32_t screen_w, screen_h;
    uint32_t pixel_format;
    bool     fallback;
    bool     buffer_pending;

    pthread_mutex_t frame_lock;
    pthread_mutex_t data_lock;
    pthread_mutex_t event_lock;
    pthread_mutex_t state_lock;

    int              stored_fds[MAX_BUFS];
    struct buf_info  stored_infos[MAX_BUFS];
    int              stored_count;
};

static int create_shm(display_ctx *ctx)
{
#if defined(__NR_memfd_create)
    ctx->shm_fd = (int)syscall(__NR_memfd_create, "ds_buf_select", MFD_CLOEXEC);
#else
    ctx->shm_fd = -1;
#endif

    if (ctx->shm_fd < 0) {
        char path[] = "/data/local/tmp/ds_shm_XXXXXX";
        ctx->shm_fd = mkstemp(path);
        if (ctx->shm_fd >= 0) {
            unlink(path);
        }
    }

    if (ctx->shm_fd < 0)
        return -1;

    if (ftruncate(ctx->shm_fd, sizeof(uint32_t)) < 0) {
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
        return -1;
    }
    ctx->shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE,
                        MAP_SHARED, ctx->shm_fd, 0);
    if (ctx->shm_ptr == MAP_FAILED) {
        ctx->shm_ptr = NULL;
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
        return -1;
    }
    *ctx->shm_ptr = 0;
    return 0;
}

static int send_hello_fds(display_ctx *ctx)
{
    int sv[2], fv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fv) < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    ctx->data_fd  = sv[0];
    ctx->fence_fd = fv[0];

    struct ctrl_msg hdr = { .type = CTRL_MSG_CONSUMER_HELLO, .size = 0 };
    int fds[4] = { ctx->buf_ready_efd, fv[1], sv[1], ctx->shm_fd };
    int ret = send_fds(ctx->ctrl_fd, &hdr, sizeof(hdr), fds, 4);
    close(sv[1]);
    close(fv[1]);
    return ret;
}

int connect_to_daemon(display_ctx **out_ctx, const char *socket_path)
{
    display_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;

    pthread_mutex_init(&ctx->frame_lock, NULL);
    pthread_mutex_init(&ctx->data_lock, NULL);
    pthread_mutex_init(&ctx->event_lock, NULL);
    pthread_mutex_init(&ctx->state_lock, NULL);

    ctx->data_fd = -1;
    ctx->fence_fd = -1;
    ctx->shm_fd = -1;
    ctx->wake_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ctx->stored_count = 0;

    ctx->buf_ready_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ctx->buf_ready_efd < 0) {
        if (ctx->wake_efd >= 0) close(ctx->wake_efd);
        free(ctx);
        return -1;
    }

    if (create_shm(ctx) < 0) {
        if (ctx->wake_efd >= 0) close(ctx->wake_efd);
        close(ctx->buf_ready_efd);
        free(ctx);
        return -1;
    }

    ctx->ctrl_fd = connect_unix(socket_path);
    if (ctx->ctrl_fd < 0) {
        if (ctx->wake_efd >= 0) close(ctx->wake_efd);
        munmap((void *)ctx->shm_ptr, sizeof(uint32_t));
        close(ctx->shm_fd);
        close(ctx->buf_ready_efd);
        free(ctx);
        return -1;
    }

    if (send_hello_fds(ctx) < 0) {
        close(ctx->ctrl_fd);
        if (ctx->data_fd >= 0) close(ctx->data_fd);
        if (ctx->fence_fd >= 0) close(ctx->fence_fd);
        if (ctx->wake_efd >= 0) close(ctx->wake_efd);
        munmap((void *)ctx->shm_ptr, sizeof(uint32_t));
        close(ctx->shm_fd);
        close(ctx->buf_ready_efd);
        free(ctx);
        return -1;
    }

    *out_ctx = ctx;
    return 0;
}

void wake_consumer(display_ctx *ctx)
{
    if (ctx && ctx->wake_efd >= 0) {
        eventfd_t val = 1;
        eventfd_write(ctx->wake_efd, val);
    }
}

void disconnect_daemon(display_ctx *ctx)
{
    if (!ctx)
        return;

    wake_consumer(ctx);

    pthread_mutex_lock(&ctx->frame_lock);
    pthread_mutex_lock(&ctx->data_lock);
    pthread_mutex_lock(&ctx->event_lock);
    pthread_mutex_lock(&ctx->state_lock);

    if (ctx->ctrl_fd >= 0) {
        close(ctx->ctrl_fd);
        ctx->ctrl_fd = -1;
    }
    if (ctx->data_fd >= 0) {
        close(ctx->data_fd);
        ctx->data_fd = -1;
    }
    if (ctx->fence_fd >= 0) {
        close(ctx->fence_fd);
        ctx->fence_fd = -1;
    }
    if (ctx->shm_ptr) {
        munmap((void *)ctx->shm_ptr, sizeof(uint32_t));
        ctx->shm_ptr = NULL;
    }
    if (ctx->shm_fd >= 0) {
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
    }
    if (ctx->buf_ready_efd >= 0) {
        close(ctx->buf_ready_efd);
        ctx->buf_ready_efd = -1;
    }
    if (ctx->wake_efd >= 0) {
        close(ctx->wake_efd);
        ctx->wake_efd = -1;
    }

    pthread_mutex_unlock(&ctx->state_lock);
    pthread_mutex_unlock(&ctx->event_lock);
    pthread_mutex_unlock(&ctx->data_lock);
    pthread_mutex_unlock(&ctx->frame_lock);

    pthread_mutex_destroy(&ctx->state_lock);
    pthread_mutex_destroy(&ctx->event_lock);
    pthread_mutex_destroy(&ctx->data_lock);
    pthread_mutex_destroy(&ctx->frame_lock);

    free(ctx);
}

int set_screen_info(display_ctx *ctx, uint32_t width, uint32_t height, uint32_t format, uint32_t refresh)
{
    if (!ctx)
        return -1;

    struct screen_info info = {
        .width   = width,
        .height  = height,
        .format  = format,
        .refresh = refresh,
    };
    struct ctrl_msg hdr = {
        .type = CTRL_MSG_SCREEN_INFO,
        .size = sizeof(info),
    };

    if (send_all(ctx->ctrl_fd, &hdr, sizeof(hdr)) < 0 ||
        send_all(ctx->ctrl_fd, &info, sizeof(info)) < 0)
        return -1;

    ctx->screen_w = width;
    ctx->screen_h = height;
    ctx->pixel_format = format;
    return 0;
}

int push_dmabufs(display_ctx *ctx, const int *fds, const struct buf_info *infos, int count)
{
    if (!ctx || count <= 0 || count > MAX_BUFS)
        return -1;

    pthread_mutex_lock(&ctx->data_lock);
    ctx->stored_count = count;
    for (int i = 0; i < count; i++) {
        ctx->stored_fds[i] = fds[i];
        ctx->stored_infos[i] = infos[i];
    }

    struct data_msg dhdr = {
        .type = DATA_MSG_BUFS_READY,
        .size = count * sizeof(struct buf_info),
    };
    int ret = 0;
    if (send_fds(ctx->data_fd, &dhdr, sizeof(dhdr), fds, count) < 0 ||
        send_all(ctx->data_fd, infos, count * sizeof(struct buf_info)) < 0)
        ret = -1;

    pthread_mutex_unlock(&ctx->data_lock);
    return ret;
}

int select_dmabuf(display_ctx *ctx, int idx)
{
    if (!ctx || idx < 0 || idx >= ctx->stored_count)
        return -1;

    pthread_mutex_lock(&ctx->frame_lock);
    pthread_mutex_lock(&ctx->state_lock);

    if (!ctx->shm_ptr || ctx->buf_ready_efd < 0) {
        pthread_mutex_unlock(&ctx->state_lock);
        pthread_mutex_unlock(&ctx->frame_lock);
        return -1;
    }

    *ctx->shm_ptr = (uint32_t)idx;
    eventfd_t val = 1;
    int event_ret = eventfd_write(ctx->buf_ready_efd, val);
    if (event_ret == 0)
        ctx->buffer_pending = true;

    pthread_mutex_unlock(&ctx->state_lock);
    pthread_mutex_unlock(&ctx->frame_lock);
    return event_ret;
}

int refresh_done_status(display_ctx *ctx, int *out_fence)
{
    if (out_fence)
        *out_fence = -1;

    if (!ctx)
        return -1;

    pthread_mutex_lock(&ctx->frame_lock);
    pthread_mutex_lock(&ctx->state_lock);
    int fence_fd = ctx->fence_fd;
    pthread_mutex_unlock(&ctx->state_lock);

    if (fence_fd < 0) {
        pthread_mutex_unlock(&ctx->frame_lock);
        return -1;
    }

    struct pollfd pfd[2] = {
        { .fd = fence_fd, .events = POLLIN, .revents = 0 },
        { .fd = ctx->wake_efd, .events = POLLIN, .revents = 0 },
    };
    int pfd_count = (ctx->wake_efd >= 0) ? 2 : 1;
    int ret = poll(pfd, pfd_count, 5000);
    if (ret <= 0 || (ctx->wake_efd >= 0 && (pfd[1].revents & POLLIN)) || !(pfd[0].revents & POLLIN)) {
        pthread_mutex_unlock(&ctx->frame_lock);
        return -1;
    }

    int rfence = -1;
    char b = 0;
    struct iovec iov = { .iov_base = &b, .iov_len = 1 };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg;
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg.buf,
        .msg_controllen = sizeof(cmsg.buf),
    };

    ssize_t n = recvmsg(fence_fd, &msg, MSG_DONTWAIT);
    if (n <= 0) {
        pthread_mutex_unlock(&ctx->frame_lock);
        return -1;
    }

    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (c && c->cmsg_type == SCM_RIGHTS)
        memcpy(&rfence, CMSG_DATA(c), sizeof(int));

    pthread_mutex_lock(&ctx->state_lock);
    ctx->buffer_pending = false;
    pthread_mutex_unlock(&ctx->state_lock);
    pthread_mutex_unlock(&ctx->frame_lock);

    if (b == 2) {
        /* NoDamage signal from producer: buffer not rendered */
        if (rfence >= 0)
            close(rfence);
        return 0;
    }

    if (out_fence)
        *out_fence = rfence;
    return 1;
}

int push_input_event(display_ctx *ctx, const struct InputEvent *event)
{
    if (!ctx || ctx->data_fd < 0)
        return -1;

    pthread_mutex_lock(&ctx->data_lock);
    struct data_msg dhdr = {
        .type = DATA_MSG_INPUT_EVENT,
        .size = sizeof(struct InputEvent),
    };
    int ret = 0;
    if (send_all(ctx->data_fd, &dhdr, sizeof(dhdr)) < 0 ||
        send_all(ctx->data_fd, event, sizeof(struct InputEvent)) < 0)
        ret = -1;

    pthread_mutex_unlock(&ctx->data_lock);
    return ret;
}

int push_input_event_with_length(display_ctx *ctx, const struct InputEvent *event, void *payload, size_t size)
{
    if (!ctx || ctx->data_fd < 0)
        return -1;

    pthread_mutex_lock(&ctx->data_lock);
    struct data_msg dhdr = {
        .type = DATA_MSG_INPUT_EVENT,
        .size = (uint32_t)(sizeof(struct InputEvent) + size),
    };
    int ret = 0;
    if (send_all(ctx->data_fd, &dhdr, sizeof(dhdr)) < 0 ||
        send_all(ctx->data_fd, event, sizeof(struct InputEvent)) < 0 ||
        (size > 0 && send_all(ctx->data_fd, payload, size) < 0))
        ret = -1;

    pthread_mutex_unlock(&ctx->data_lock);
    return ret;
}

int poll_output_event(display_ctx *ctx, struct OutputEvent *event, int timeout_ms)
{
    if (!ctx || ctx->data_fd < 0)
        return -1;

    struct pollfd pfd = { .fd = ctx->data_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0 || !(pfd.revents & POLLIN))
        return ret;

    struct data_msg hdr;
    if (recv_all(ctx->data_fd, &hdr, sizeof(hdr)) < 0)
        return -1;

    if (hdr.type != DATA_MSG_OUTPUT_EVENT) {
        if (hdr.size > 0) {
            uint8_t drain[256];
            uint32_t rem = hdr.size;
            while (rem > 0) {
                size_t chunk = rem < sizeof(drain) ? rem : sizeof(drain);
                if (recv_all(ctx->data_fd, drain, chunk) < 0)
                    return -1;
                rem -= (uint32_t)chunk;
            }
        }
        return 0;
    }

    if (recv_all(ctx->data_fd, event, sizeof(struct OutputEvent)) < 0)
        return -1;

    return 1;
}

int poll_output_event_extend_data(display_ctx *ctx, void *payload, size_t size, int timeout_ms)
{
    if (!ctx || ctx->data_fd < 0)
        return -1;

    struct pollfd pfd = { .fd = ctx->data_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0 || !(pfd.revents & POLLIN))
        return ret;

    if (recv_all(ctx->data_fd, payload, size) < 0)
        return -1;

    return 1;
}
