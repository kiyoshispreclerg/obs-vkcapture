/*
OBS Linux Vulkan/OpenGL game capture
Copyright (C) 2021 David Rosca <nowrep@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "capture.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <sys/un.h>
#include <sys/socket.h>

struct capture_context {
    int connfd;
    bool accepted;
    bool capturing;
    bool no_modifiers;
    bool linear;
    bool map_host;
    bool need_reinit;
    uint8_t device_uuid[16];
    int64_t last_check;
};

// Singleton context backing the global capture API used by the Vulkan layer.
static struct capture_context g_ctx;

static bool get_wine_exe(char *buf, size_t bufsize)
{
    FILE *f = fopen("/proc/self/comm", "r");
    if (!f) {
        return false;
    }
    size_t n = fread(buf, sizeof(char), bufsize, f);
    fclose(f);
    if (n < 1) {
        return false;
    }
    buf[n - 1] = '\0';
    return true;
}

static bool get_exe(char *buf, size_t bufsize)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, PATH_MAX);
    if (n <= 0) {
        return false;
    }
    exe[n] = '\0';
    strncpy(buf, basename(exe), bufsize);
    buf[bufsize - 1] = '\0';
    if (!strcmp(buf, "wine-preloader") || !strcmp(buf, "wine64-preloader")) {
        return get_wine_exe(buf, bufsize);
    }
    return true;
}

static bool ctx_try_connect(struct capture_context *d)
{
    const char sockname[] = "/com/obsproject/vkcapture";

    struct sockaddr_un addr;
    addr.sun_family = PF_LOCAL;
    addr.sun_path[0] = '\0'; // Abstract socket
    memcpy(&addr.sun_path[1], sockname, sizeof(sockname) - 1);

    int sock = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int ret = connect(sock, (const struct sockaddr *)&addr, sizeof(addr.sun_family) + sizeof(sockname));
    if (ret == -1) {
        close(sock);
        return false;
    }

    d->connfd = sock;

    struct capture_client_data cd;
    cd.type = CAPTURE_CLIENT_DATA_TYPE;
    get_exe(cd.exe, sizeof(cd.exe));

    struct msghdr msg = {0};
    struct iovec io = {
        .iov_base = &cd,
        .iov_len = CAPTURE_CLIENT_DATA_SIZE,
    };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    const ssize_t sent = sendmsg(d->connfd, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
        hlog("Socket sendmsg error %s", strerror(errno));
    }

    return true;
}

static void ctx_init(struct capture_context *d)
{
    memset(d, 0, sizeof(*d));
    d->connfd = -1;
}

static void ctx_update_socket(struct capture_context *d)
{
    const int64_t now = os_time_get_nano();
    if (now - d->last_check < 1000000000) {
        return;
    }
    d->last_check = now;

    if (d->connfd < 0 && !ctx_try_connect(d)) {
        return;
    }

    struct capture_control_data control;
    ssize_t n = recv(d->connfd, &control, sizeof(control), 0);
    if (n == sizeof(control)) {
        const bool old_no_modifiers = d->no_modifiers;
        const bool old_linear = d->linear;
        const bool old_map_host = d->map_host;
        d->accepted = control.capturing == 1;
        d->no_modifiers = control.no_modifiers == 1;
        d->linear = control.linear == 1;
        d->map_host = control.map_host == 1;
        memcpy(d->device_uuid, control.device_uuid, 16);
        if (d->capturing && (old_no_modifiers != d->no_modifiers
            || old_linear != d->linear
            || old_map_host != d->map_host)) {
            d->need_reinit = true;
        }
    }
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        if (errno != ECONNRESET) {
            hlog("Socket recv error %s", strerror(errno));
        }
    }
    if (n <= 0) {
        close(d->connfd);
        d->connfd = -1;
        d->accepted = false;
    }
}

static void ctx_init_shtex(struct capture_context *d,
        int width, int height, int format, int strides[4],
        int offsets[4], uint64_t modifier, uint32_t winid,
        bool flip, uint32_t color_space, int nfd, int fds[4])
{
    struct capture_texture_data td = {0};
    td.type = CAPTURE_TEXTURE_DATA_TYPE;
    td.nfd = nfd;
    td.width = width;
    td.height = height;
    td.format = format;
    memcpy(td.strides, strides, sizeof(int) * nfd);
    memcpy(td.offsets, offsets, sizeof(int) * nfd);
    td.modifier = modifier;
    td.winid = winid;
    td.flip = flip;
    td.color_space = color_space;

    struct msghdr msg = {0};

    struct iovec io = {
        .iov_base = &td,
        .iov_len = CAPTURE_TEXTURE_DATA_SIZE,
    };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int) * 4)];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfd);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfd);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfd);

    const ssize_t sent = sendmsg(d->connfd, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
        hlog("Socket sendmsg error %s", strerror(errno));
    }

    d->capturing = true;
    d->need_reinit = false;
}

static void ctx_stop(struct capture_context *d)
{
    d->capturing = false;
}

static bool ctx_should_stop(struct capture_context *d)
{
    return d->capturing && (d->connfd < 0 || !d->accepted || d->need_reinit);
}

static bool ctx_should_init(struct capture_context *d)
{
    return !d->capturing && d->connfd >= 0 && d->accepted;
}

/* ------------------------------------------------------------------ */
/* Singleton API (Vulkan layer)                                       */
/* ------------------------------------------------------------------ */

void capture_init()
{
    ctx_init(&g_ctx);
}

void capture_update_socket()
{
    ctx_update_socket(&g_ctx);
}

void capture_init_shtex(
        int width, int height, int format, int strides[4],
        int offsets[4], uint64_t modifier, uint32_t winid,
        bool flip, uint32_t color_space, int nfd, int fds[4])
{
    ctx_init_shtex(&g_ctx, width, height, format, strides, offsets,
            modifier, winid, flip, color_space, nfd, fds);
}

void capture_stop()
{
    ctx_stop(&g_ctx);
}

bool capture_should_stop()
{
    return ctx_should_stop(&g_ctx);
}

bool capture_should_init()
{
    return ctx_should_init(&g_ctx);
}

bool capture_ready()
{
    return g_ctx.capturing;
}

bool capture_allocate_no_modifiers()
{
    return g_ctx.no_modifiers;
}

bool capture_allocate_linear()
{
    return g_ctx.linear;
}

bool capture_allocate_map_host()
{
    return g_ctx.map_host;
}

bool capture_compare_device_uuid(uint8_t uuid[16])
{
    return memcmp(g_ctx.device_uuid, uuid, 16) == 0;
}

/* ------------------------------------------------------------------ */
/* Multi-instance API (OpenGL injection layer)                        */
/* ------------------------------------------------------------------ */

capture_t *capture_create()
{
    capture_t *ctx = malloc(sizeof(*ctx));
    if (ctx) {
        ctx_init(ctx);
    }
    return ctx;
}

void capture_destroy(capture_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->connfd >= 0) {
        close(ctx->connfd);
    }
    free(ctx);
}

void capture_ctx_update_socket(capture_t *ctx)
{
    ctx_update_socket(ctx);
}

void capture_ctx_init_shtex(capture_t *ctx,
        int width, int height, int format, int strides[4],
        int offsets[4], uint64_t modifier, uint32_t winid,
        bool flip, uint32_t color_space, int nfd, int fds[4])
{
    ctx_init_shtex(ctx, width, height, format, strides, offsets,
            modifier, winid, flip, color_space, nfd, fds);
}

void capture_ctx_stop(capture_t *ctx)
{
    ctx_stop(ctx);
}

bool capture_ctx_should_stop(capture_t *ctx)
{
    return ctx_should_stop(ctx);
}

bool capture_ctx_should_init(capture_t *ctx)
{
    return ctx_should_init(ctx);
}

bool capture_ctx_ready(capture_t *ctx)
{
    return ctx->capturing;
}

bool capture_ctx_allocate_no_modifiers(capture_t *ctx)
{
    return ctx->no_modifiers;
}

bool capture_ctx_allocate_linear(capture_t *ctx)
{
    return ctx->linear;
}

bool capture_ctx_allocate_map_host(capture_t *ctx)
{
    return ctx->map_host;
}

bool capture_ctx_compare_device_uuid(capture_t *ctx, uint8_t uuid[16])
{
    return memcmp(ctx->device_uuid, uuid, 16) == 0;
}
