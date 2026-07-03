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

#define _GNU_SOURCE

#include <obs-module.h>
#include <obs-nix-platform.h>

#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#include "utils.h"
#include "capture.h"
#include "plugin-macros.h"

#define MAX_WINDOW_LIST 16

#if HAVE_X11_XCB
#include "xcursor-xcb.h"
static xcb_connection_t *xcb = NULL;
#endif

#if HAVE_WAYLAND
#include "wlcursor.h"
static struct wl_display *wl_display = NULL;
static wl_cursor_t *wlcursor = NULL;
#endif

#include <EGL/egl.h>
static uint8_t gl_device_uuid[16];
void (*p_glGetUnsignedBytei_vEXT)(unsigned int target, unsigned int index, unsigned char *data) = NULL;

enum vkcapture_import_attempt {
    IMPORT_DEFAULT = 0,
    IMPORT_NO_MODIFIERS = 1,
    IMPORT_LINEAR = 2,
    IMPORT_LINEAR_HOST_MAPPED = 3,
    IMPORT_FAILURES_MAX = IMPORT_LINEAR_HOST_MAPPED,
};

typedef struct {
    int id;
    int sockfd;
    int activated;
    int buf_id;
    int buf_fds[4];
    int import_failures;
    size_t map_size;
    void *map_memory;
    uint64_t timeout;
    bool unresponsive;
    struct capture_client_data cdata;
    struct capture_texture_data tdata;
} vkcapture_client_t;

static struct {
    bool quit;
    int eventfd;
    pthread_t thread;
    pthread_mutex_t mutex;
    DARRAY(struct pollfd) fds;
    DARRAY(vkcapture_client_t) clients;
} server;

static int source_instances = 0;

typedef struct {
    obs_source_t *source;
    gs_texture_t *texture;
#if HAVE_X11_XCB
    xcb_xcursor_t *xcursor;
    uint32_t root_winid;
#endif
    bool show_cursor;
    bool allow_transparency;
    bool force_hdr;
    int window_mode;        // 0=any, 1=include list, 2=exclude list
    DARRAY(char *) windows; // exe names

    int buf_id;
    int client_id;
    struct capture_texture_data tdata;
    bool was_showing;

} vkcapture_source_t;

static bool server_wakeup();
static void vkcapture_get_hooked(void *data, calldata_t *cd);

static const char *import_attempt_str(enum vkcapture_import_attempt attempt)
{
    switch (attempt) {
    case IMPORT_DEFAULT: return "default";
    case IMPORT_NO_MODIFIERS: return "no modifiers";
    case IMPORT_LINEAR: return "linear";
    case IMPORT_LINEAR_HOST_MAPPED: return "linear host mapped";
    default: return "invalid";
    }
}

static int64_t clock_ns()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000 + t.tv_nsec;
}

static const struct {
    int32_t drm;
    enum gs_color_format gs;
} gs_format_table[] = {
    { DRM_FORMAT_ARGB8888, GS_BGRA },
    { DRM_FORMAT_XRGB8888, GS_BGRX },
    { DRM_FORMAT_ABGR8888, GS_RGBA },
    { DRM_FORMAT_XBGR8888, GS_RGBA },
    { DRM_FORMAT_ARGB2101010, GS_R10G10B10A2 },
    { DRM_FORMAT_XRGB2101010, GS_R10G10B10A2 },
    { DRM_FORMAT_ABGR2101010, GS_R10G10B10A2 },
    { DRM_FORMAT_XBGR2101010, GS_R10G10B10A2 },
    { DRM_FORMAT_ABGR16161616, GS_RGBA16 },
    { DRM_FORMAT_XBGR16161616, GS_RGBA16 },
    { DRM_FORMAT_ABGR16161616F, GS_RGBA16F },
    { DRM_FORMAT_XBGR16161616F, GS_RGBA16F },
};

static enum gs_color_format drm_format_to_gs(int32_t drm)
{
    for (size_t i = 0; i < sizeof(gs_format_table) / sizeof(gs_format_table[0]); ++i) {
        if (gs_format_table[i].drm == drm) {
            return gs_format_table[i].gs;
        }
    }
    return GS_UNKNOWN;
}

static void cursor_create(vkcapture_source_t *ctx)
{
    bool try_xcb = false;

#if HAVE_WAYLAND
    if (obs_get_nix_platform() == OBS_NIX_PLATFORM_WAYLAND) {
        if (!wl_display) {
            wl_display = wl_display_connect(NULL);
            if (!wl_display) {
                blog(LOG_ERROR, "Unable to open Wayland display!");
            }
        }
        if (wl_display && !wlcursor) {
            wlcursor = wl_cursor_init(wl_display);
            if (!wlcursor) {
                try_xcb = true;
            }
        }
    }
#endif
#if HAVE_X11_XCB
    if (try_xcb || obs_get_nix_platform() == OBS_NIX_PLATFORM_X11_EGL) {
        if (!xcb) {
            xcb = xcb_connect(NULL, NULL);
            if (!xcb || xcb_connection_has_error(xcb)) {
                blog(LOG_ERROR, "Unable to open X display!");
            }
        }
        if (xcb) {
            ctx->xcursor = xcb_xcursor_init(xcb);
        }
    }
#endif
}

static void cursor_destroy(vkcapture_source_t *ctx)
{
#if HAVE_X11_XCB
    if (ctx->xcursor) {
        obs_enter_graphics();
        xcb_xcursor_destroy(ctx->xcursor);
        obs_leave_graphics();
    }
    if (!source_instances) {
        if (xcb) {
            xcb_disconnect(xcb);
            xcb = NULL;
        }
    }
#endif
#if HAVE_WAYLAND
    if (!source_instances) {
        blog(LOG_INFO, "destroy");
        if (wlcursor) {
            wl_cursor_destroy(wlcursor);
            wlcursor = NULL;
        }
        if (wl_display) {
            wl_display_disconnect(wl_display);
            wl_display = NULL;
        }
    }
#endif
}

static bool cursor_enabled(vkcapture_source_t *ctx)
{
#if HAVE_X11_XCB
    if (ctx->xcursor) {
        return true;
    }
#endif
#if HAVE_WAYLAND
    if (wlcursor) {
        return true;
    }
#endif
    return false;
}

static void cursor_update(vkcapture_source_t *ctx)
{
#if HAVE_X11_XCB
    if (ctx->xcursor) {
        if (!ctx->root_winid && ctx->tdata.winid) {
            xcb_query_tree_cookie_t tre_c = xcb_query_tree_unchecked(xcb, ctx->tdata.winid);
            xcb_query_tree_reply_t *tre_r = xcb_query_tree_reply(xcb, tre_c, NULL);
            if (tre_r) {
                ctx->root_winid = tre_r->root;
                free(tre_r);
            }
        }
        xcb_translate_coordinates_cookie_t tr_c;
        if (ctx->root_winid && ctx->tdata.winid) {
            tr_c = xcb_translate_coordinates_unchecked(xcb, ctx->tdata.winid, ctx->root_winid, 0, 0);
        }
        xcb_xfixes_get_cursor_image_cookie_t cur_c = xcb_xfixes_get_cursor_image_unchecked(xcb);
        xcb_xfixes_get_cursor_image_reply_t *cur_r = xcb_xfixes_get_cursor_image_reply(xcb, cur_c, NULL);
        if (ctx->root_winid && ctx->tdata.winid) {
            xcb_translate_coordinates_reply_t *tr_r = xcb_translate_coordinates_reply(xcb, tr_c, NULL);
            if (tr_r) {
                xcb_xcursor_offset(ctx->xcursor, tr_r->dst_x, tr_r->dst_y);
                free(tr_r);
            }
        }
        xcb_xcursor_update(ctx->xcursor, cur_r);
        free(cur_r);
    }
#endif
#if HAVE_WAYLAND
    if (wlcursor) {
        struct pollfd fd;
        fd.fd = wl_display_get_fd(wl_display);
        fd.events = POLLIN;
        if (poll(&fd, 1, 0) > 0) {
            wl_display_dispatch(wl_display);
        }
        wl_display_flush(wl_display);
    }
#endif
}

static void cursor_render(vkcapture_source_t *ctx)
{
#if HAVE_X11_XCB
    if (ctx->xcursor) {
        xcb_xcursor_render(ctx->xcursor);
    }
#endif
#if HAVE_WAYLAND
    if (wlcursor) {
        wl_cursor_render(wlcursor);
    }
#endif
}

static void destroy_texture(vkcapture_source_t *ctx)
{
    if (!ctx->texture) {
        return;
    }

    obs_enter_graphics();
    gs_texture_destroy(ctx->texture);
    obs_leave_graphics();
    ctx->texture = NULL;

    ctx->buf_id = 0;
    memset(&ctx->tdata, 0, sizeof(ctx->tdata));
}

static void vkcapture_source_destroy(void *data)
{
    --source_instances;

    vkcapture_source_t *ctx = data;

    destroy_texture(ctx);
    cursor_destroy(ctx);

    for (size_t i = 0; i < ctx->windows.num; i++) {
        bfree(ctx->windows.array[i]);
    }
    da_free(ctx->windows);

    bfree(ctx);
}

static void vkcapture_source_update(void *data, obs_data_t *settings)
{
    vkcapture_source_t *ctx = data;

    ctx->show_cursor = obs_data_get_bool(settings, "show_cursor");
    ctx->allow_transparency = obs_data_get_bool(settings, "allow_transparency");
    ctx->force_hdr = obs_data_get_bool(settings, "force_hdr");

    // Migrate old single-window setting to new format
    const char *old_window = obs_data_get_string(settings, "window");
    if (old_window && *old_window) {
        if (!strncmp(old_window, "exclude=", 8) && *(old_window + 8)) {
            obs_data_set_int(settings, "window_mode", 2);
            obs_data_set_int(settings, "window_count", 1);
            obs_data_set_string(settings, "window_0", old_window + 8);
        } else if (strncmp(old_window, "exclude=", 8)) {
            obs_data_set_int(settings, "window_mode", 1);
            obs_data_set_int(settings, "window_count", 1);
            obs_data_set_string(settings, "window_0", old_window);
        }
        obs_data_set_string(settings, "window", "");
    }

    for (size_t i = 0; i < ctx->windows.num; i++) {
        bfree(ctx->windows.array[i]);
    }
    da_resize(ctx->windows, 0);

    ctx->window_mode = (int)obs_data_get_int(settings, "window_mode");
    for (int i = 0; i < MAX_WINDOW_LIST; i++) {
        char key[32];
        snprintf(key, sizeof(key), "window_%d", i);
        const char *exe = obs_data_get_string(settings, key);
        if (exe && *exe) {
            char *copy = bstrdup(exe);
            da_push_back(ctx->windows, &copy);
        }
    }
}

static void *vkcapture_source_create(obs_data_t *settings, obs_source_t *source)
{
    ++source_instances;

    vkcapture_source_t *ctx = bzalloc(sizeof(vkcapture_source_t));
    ctx->source = source;
    da_init(ctx->windows);

    vkcapture_source_update(ctx, settings);
    ctx->was_showing = true;

    cursor_create(ctx);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(
		ph,
		"void get_hooked(out bool hooked, out string executable)",
		vkcapture_get_hooked, ctx);

    UNUSED_PARAMETER(settings);
    return ctx;
}

// Returns the 1-based position of `client` among all connected clients that
// share its executable name (in connection order), and stores the total number
// of those clients in `count` when non-NULL. Must be called with server.mutex held.
static int client_instance_index(const vkcapture_client_t *client, int *count)
{
    int idx = 0;
    int total = 0;
    for (size_t i = 0; i < server.clients.num; i++) {
        const vkcapture_client_t *c = server.clients.array + i;
        if (strcmp(c->cdata.exe, client->cdata.exe)) {
            continue;
        }
        ++total;
        if (c == client) {
            idx = total;
        }
    }
    if (count) {
        *count = total;
    }
    return idx;
}

// Matches a client against a window selection string. The selection is either a
// plain executable name (matches any capture of it) or "exe#N" to pin the Nth
// capture of that executable (see client_instance_index), which lets a source
// target one specific drawable when a program has several (e.g. one per
// monitor). Must be called with server.mutex held.
static bool client_matches_selection(const vkcapture_client_t *client, const char *sel)
{
    const char *hash = strrchr(sel, '#');
    int want_idx = 0;
    if (hash && hash[1]) {
        const char *d = hash + 1;
        while (*d >= '0' && *d <= '9') {
            ++d;
        }
        if (*d == '\0') {
            want_idx = atoi(hash + 1);
        }
    }
    if (want_idx <= 0) {
        hash = NULL;
    }

    const size_t base_len = hash ? (size_t)(hash - sel) : strlen(sel);
    if (strncmp(client->cdata.exe, sel, base_len) || client->cdata.exe[base_len] != '\0') {
        return false;
    }
    if (want_idx && client_instance_index(client, NULL) != want_idx) {
        return false;
    }
    return true;
}

static vkcapture_client_t *find_matching_client(vkcapture_source_t *ctx)
{
    if (ctx->window_mode == 0 || ctx->windows.num == 0) {
        return server.clients.num ? server.clients.array + server.clients.num - 1 : NULL;
    }

    for (size_t i = server.clients.num; i > 0; i--) {
        vkcapture_client_t *c = server.clients.array + i - 1;

        bool in_list = false;
        for (size_t j = 0; j < ctx->windows.num; j++) {
            if (client_matches_selection(c, ctx->windows.array[j])) {
                in_list = true;
                break;
            }
        }

        if ((ctx->window_mode == 1 && in_list) || (ctx->window_mode == 2 && !in_list)) {
            return c;
        }
    }
    return NULL;
}

static vkcapture_client_t *find_client_by_id(int id)
{
    vkcapture_client_t *client = NULL;
    for (size_t i = 0; i < server.clients.num; i++) {
        vkcapture_client_t *c = server.clients.array + i;
        if (c->id == id) {
            client = c;
            break;
        }
    }
    return client;
}

static void vkcapture_get_hooked(void *data, calldata_t *cd)
{
	vkcapture_source_t *ctx = data;

	if(ctx && ctx->client_id) {
		pthread_mutex_lock(&server.mutex);
    	vkcapture_client_t *client = find_client_by_id(ctx->client_id);

        calldata_set_bool(cd, "hooked", !!client);
        calldata_set_string(cd, "executable", client ? client->cdata.exe: "");

		pthread_mutex_unlock(&server.mutex);
        return;
	}

	calldata_set_bool(cd, "hooked", false);
	calldata_set_string(cd, "executable", "");
}

static void fill_capture_control_data(struct capture_control_data *msg, vkcapture_client_t *client)
{
    if (!p_glGetUnsignedBytei_vEXT) {
        obs_enter_graphics();
        p_glGetUnsignedBytei_vEXT = (typeof(p_glGetUnsignedBytei_vEXT))
            eglGetProcAddress("glGetUnsignedBytei_vEXT");
        if (p_glGetUnsignedBytei_vEXT) {
            p_glGetUnsignedBytei_vEXT(0x9597, 0, gl_device_uuid);
        }
        obs_leave_graphics();
    }

    msg->no_modifiers = !!(client->import_failures == IMPORT_NO_MODIFIERS);
    msg->linear = !!(client->import_failures == IMPORT_LINEAR
        || client->import_failures == IMPORT_LINEAR_HOST_MAPPED);
    msg->map_host = !!(client->import_failures == IMPORT_LINEAR_HOST_MAPPED);
    memcpy(msg->device_uuid, gl_device_uuid, 16);
}

static void activate_client(vkcapture_source_t *ctx, vkcapture_client_t *client, bool activate)
{
    struct capture_control_data msg = {0};
    if (activate && !client->activated++) {
        msg.capturing = 1;
    } else if (!activate && !--client->activated) {
        msg.capturing = 0;
    } else {
        return;
    }
    fill_capture_control_data(&msg, client);
    client->buf_id = 0;
    for (int i = 0; i < 4; ++i) {
        if (client->buf_fds[i] >= 0) {
            close(client->buf_fds[i]);
            client->buf_fds[i] = -1;
        }
    }
    memset(&client->tdata, 0, sizeof(client->tdata));
    ssize_t ret = write(client->sockfd, &msg, sizeof(msg));
    if (ret != sizeof(msg)) {
        blog(LOG_WARNING, "Socket write error: %s", strerror(errno));
    }
    client->timeout = clock_ns() + 5000000000; // 5s timeout
}

static void vkcapture_source_video_tick(void *data, float seconds)
{
    vkcapture_source_t *ctx = data;

    const bool is_showing = obs_source_showing(ctx->source);

    if (is_showing != ctx->was_showing && ctx->client_id) {
        pthread_mutex_lock(&server.mutex);
        vkcapture_client_t *client = find_client_by_id(ctx->client_id);
        if (client) {
            activate_client(ctx, client, is_showing);

            if (!is_showing) {
                ctx->client_id = 0;
                destroy_texture(ctx);
            }
        }
        pthread_mutex_unlock(&server.mutex);

        ctx->was_showing = is_showing;
    }

    if (!is_showing) {
        return;
    }

    pthread_mutex_lock(&server.mutex);

    if (ctx->client_id) {
        vkcapture_client_t *client = find_client_by_id(ctx->client_id);
        if (!client) {
            ctx->client_id = 0;
            destroy_texture(ctx);
        } else if (ctx->buf_id != client->buf_id) {
            destroy_texture(ctx);
            memcpy(&ctx->tdata, &client->tdata, sizeof(client->tdata));

            blog(LOG_INFO, "Creating texture from dmabuf %dx%d modifier:%" PRIu64,
                    ctx->tdata.width, ctx->tdata.height, ctx->tdata.modifier);

            uint32_t strides[4];
            uint32_t offsets[4];
            uint64_t modifiers[4];
            for (uint8_t i = 0; i < ctx->tdata.nfd; ++i) {
                strides[i] = ctx->tdata.strides[i];
                offsets[i] = ctx->tdata.offsets[i];
                modifiers[i] = ctx->tdata.modifier;
                blog(LOG_INFO, " [%d] fd:%d stride:%d offset:%d", i, client->buf_fds[i], strides[i], offsets[i]);
            }

            if (client->import_failures == IMPORT_LINEAR_HOST_MAPPED) {
                lseek(client->buf_fds[0], 0, SEEK_SET);
                client->map_size = lseek(client->buf_fds[0], 0, SEEK_END);
                client->map_memory = mmap(NULL, client->map_size, PROT_READ, MAP_SHARED, client->buf_fds[0], 0);
                if (client->map_memory == MAP_FAILED) {
                    client->map_memory = NULL;
                    blog(LOG_ERROR, "Failed to map dmabuf '%s'", strerror(errno));
                } else {
                    obs_enter_graphics();
                    ctx->texture = gs_texture_create(ctx->tdata.width, ctx->tdata.height,
                        drm_format_to_gs(ctx->tdata.format), 1, NULL, GS_DYNAMIC);
                    obs_leave_graphics();
                }
            } else {
                obs_enter_graphics();
                ctx->texture = gs_texture_create_from_dmabuf(ctx->tdata.width, ctx->tdata.height,
                    ctx->tdata.format, drm_format_to_gs(ctx->tdata.format), ctx->tdata.nfd, client->buf_fds,
                    strides, offsets, ctx->tdata.modifier != DRM_FORMAT_MOD_INVALID ? modifiers : NULL);
                obs_leave_graphics();
            }

            if (!ctx->texture) {
                if (client->import_failures < IMPORT_FAILURES_MAX) {
                    client->import_failures++;
                    blog(LOG_WARNING, "Asking client to create texture %s",
                        import_attempt_str(client->import_failures));
                    struct capture_control_data msg = {0};
                    msg.capturing = client->activated ? 1 : 0;
                    fill_capture_control_data(&msg, client);
                    ssize_t ret = write(client->sockfd, &msg, sizeof(msg));
                    if (ret != sizeof(msg)) {
                        blog(LOG_WARNING, "Socket write error: %s", strerror(errno));
                    }
                } else {
                    blog(LOG_ERROR, "Could not create texture from dmabuf source");
                }
            }
            ctx->buf_id = client->buf_id;
            client->timeout = 0;
        } else if (client != find_matching_client(ctx)) {
            activate_client(ctx, client, false);
            ctx->client_id = 0;
            destroy_texture(ctx);
        } else if (client->timeout && clock_ns() > client->timeout) {
            blog(LOG_INFO, "Client %d not responding, disconnecting...", client->id);
            client->unresponsive = true;
            server_wakeup();
            ctx->client_id = 0;
            destroy_texture(ctx);
        }
    } else {
        vkcapture_client_t *client = find_matching_client(ctx);
        if (client) {
            activate_client(ctx, client, true);
            ctx->client_id = client->id;
        }
    }

    pthread_mutex_unlock(&server.mutex);

    UNUSED_PARAMETER(seconds);
}

static void vkcapture_source_render(void *data, gs_effect_t *effect)
{
    vkcapture_source_t *ctx = data;

    if (!ctx->texture) {
        return;
    }

    if (ctx->show_cursor) {
        cursor_update(ctx);
    }

    pthread_mutex_lock(&server.mutex);
    vkcapture_client_t *client = find_client_by_id(ctx->client_id);
    if (!client) {
        pthread_mutex_unlock(&server.mutex);
        return;
    }
    void *memory = client->map_memory;
    int stride = client->tdata.strides[0];
    int fd = client->buf_fds[0];
    pthread_mutex_unlock(&server.mutex);

    if (memory) {
        struct dma_buf_sync sync;
        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
        ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);

        obs_enter_graphics();
        gs_texture_set_image(ctx->texture, memory, stride, false);
        obs_leave_graphics();

        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    }

    const enum gs_color_space color_space = gs_get_color_space();
    const char *tech_name = "Draw";
    float multiplier = 1.f;

    if (color_space == GS_CS_709_EXTENDED) {
        tech_name = "DrawPQ";
        multiplier = 10000.f / obs_get_video_sdr_white_level();
    }

    effect = obs_get_base_effect(ctx->allow_transparency ? OBS_EFFECT_DEFAULT : OBS_EFFECT_OPAQUE);

    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, ctx->texture);

    while (gs_effect_loop(effect, tech_name)) {
        gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);
        gs_draw_sprite(ctx->texture, ctx->tdata.flip ? GS_FLIP_V : 0, 0, 0);
        if (ctx->allow_transparency && ctx->show_cursor) {
            cursor_render(ctx);
        }
    }

    if (!ctx->allow_transparency && ctx->show_cursor) {
        effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        tech_name = "Draw";
        multiplier = 1.f;
        if (color_space == GS_CS_709_SCRGB) {
            tech_name = "DrawMultiply";
            multiplier = obs_get_video_sdr_white_level() / 80.f;
        }
        while (gs_effect_loop(effect, tech_name)) {
            gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);
            cursor_render(ctx);
        }
    }
}

static const char *vkcapture_source_get_name(void *data)
{
    return obs_module_text("GameCapture");
}

static uint32_t vkcapture_source_get_width(void *data)
{
    const vkcapture_source_t *ctx = data;
    return ctx->tdata.width;
}

static uint32_t vkcapture_source_get_height(void *data)
{
    const vkcapture_source_t *ctx = data;
    return ctx->tdata.height;
}

static void vkcapture_source_get_defaults(obs_data_t *defaults)
{
    obs_data_set_default_bool(defaults, "show_cursor", true);
    obs_data_set_default_bool(defaults, "allow_transparency", false);
    obs_data_set_default_bool(defaults, "force_hdr", false);
    obs_data_set_default_int(defaults, "window_mode", 0);
}

static void combo_add_unique(obs_property_t *p, const char *value)
{
    for (size_t j = 0; j < obs_property_list_item_count(p); j++) {
        if (!strcmp(value, obs_property_list_item_string(p, j))) {
            return;
        }
    }
    obs_property_list_add_string(p, value, value);
}

static void populate_window_combo(obs_property_t *p)
{
    pthread_mutex_lock(&server.mutex);
    for (size_t i = 0; i < server.clients.num; i++) {
        vkcapture_client_t *client = server.clients.array + i;

        // Always offer the plain executable, which matches any of its captures.
        combo_add_unique(p, client->cdata.exe);

        // When the same executable has several captures (e.g. one drawable per
        // monitor), also offer an "exe#N" entry to pin a specific one.
        int count = 0;
        const int idx = client_instance_index(client, &count);
        if (count > 1) {
            char value[64];
            snprintf(value, sizeof(value), "%s#%d", client->cdata.exe, idx);
            combo_add_unique(p, value);
        }
    }
    pthread_mutex_unlock(&server.mutex);
}

static void update_window_list_visibility(obs_properties_t *props, obs_data_t *settings, int mode)
{
    int active = 0;
    for (int i = 0; i < MAX_WINDOW_LIST; i++) {
        char combo_key[32], btn_key[32];
        snprintf(combo_key, sizeof(combo_key), "window_%d", i);
        snprintf(btn_key, sizeof(btn_key), "remove_%d", i);
        const char *val = obs_data_get_string(settings, combo_key);
        bool has_value = val && *val;
        if (has_value) active++;
        bool visible = mode != 0 && has_value;
        obs_property_t *combo = obs_properties_get(props, combo_key);
        obs_property_t *btn = obs_properties_get(props, btn_key);
        if (combo) obs_property_set_visible(combo, visible);
        if (btn) obs_property_set_visible(btn, visible);
    }
    obs_property_t *add_btn = obs_properties_get(props, "add_window");
    if (add_btn) obs_property_set_visible(add_btn, mode != 0 && active < MAX_WINDOW_LIST);
}

static bool window_mode_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
    int mode = (int)obs_data_get_int(settings, "window_mode");
    update_window_list_visibility(props, settings, mode);
    return true;
}

static bool add_window_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
    vkcapture_source_t *ctx = obs_properties_get_param(props);
    if (!ctx) return false;

    obs_data_t *settings = obs_source_get_settings(ctx->source);

    // Find the first empty slot and show it
    int active = 0;
    int free_slot = -1;
    for (int i = 0; i < MAX_WINDOW_LIST; i++) {
        char key[32];
        snprintf(key, sizeof(key), "window_%d", i);
        const char *val = obs_data_get_string(settings, key);
        if (val && *val) {
            active++;
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot >= 0) {
        char combo_key[32], btn_key[32];
        snprintf(combo_key, sizeof(combo_key), "window_%d", free_slot);
        snprintf(btn_key, sizeof(btn_key), "remove_%d", free_slot);
        obs_property_t *combo = obs_properties_get(props, combo_key);
        obs_property_t *btn = obs_properties_get(props, btn_key);
        if (combo) obs_property_set_visible(combo, true);
        if (btn) obs_property_set_visible(btn, true);
        active++;
    }

    obs_property_t *add_btn = obs_properties_get(props, "add_window");
    if (add_btn) obs_property_set_visible(add_btn, active < MAX_WINDOW_LIST);

    obs_data_release(settings);
    return true;
}

static bool remove_window_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
    vkcapture_source_t *ctx = obs_properties_get_param(props);
    if (!ctx) return false;

    obs_data_t *settings = obs_source_get_settings(ctx->source);

    const char *name = obs_property_name(p);
    int idx = atoi(name + 7); // "remove_" = 7 chars

    char combo_key[32], btn_key[32];
    snprintf(combo_key, sizeof(combo_key), "window_%d", idx);
    snprintf(btn_key, sizeof(btn_key), "remove_%d", idx);

    obs_data_set_string(settings, combo_key, "");

    obs_property_t *combo = obs_properties_get(props, combo_key);
    obs_property_t *btn = obs_properties_get(props, btn_key);
    if (combo) obs_property_set_visible(combo, false);
    if (btn) obs_property_set_visible(btn, false);

    // A slot just freed up, so the add button should be visible
    obs_property_t *add_btn = obs_properties_get(props, "add_window");
    if (add_btn) obs_property_set_visible(add_btn, true);

    obs_data_release(settings);
    return true;
}

static obs_properties_t *vkcapture_source_get_properties(void *data)
{
    vkcapture_source_t *ctx = data;

    obs_data_t *settings = ctx ? obs_source_get_settings(ctx->source) : NULL;
    int mode = settings ? (int)obs_data_get_int(settings, "window_mode") : 0;

    obs_properties_t *props = obs_properties_create();
    obs_properties_set_param(props, ctx, NULL);

    obs_property_t *mode_prop = obs_properties_add_list(props, "window_mode",
        obs_module_text("CaptureMode"),
        OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode_prop, obs_module_text("CaptureAnyWindow"), 0);
    obs_property_list_add_int(mode_prop, obs_module_text("CaptureIncludeOnly"), 1);
    obs_property_list_add_int(mode_prop, obs_module_text("CaptureExclude"), 2);
    obs_property_set_modified_callback(mode_prop, window_mode_changed);

    int active = 0;
    for (int i = 0; i < MAX_WINDOW_LIST; i++) {
        char combo_key[32], btn_key[32], combo_label[32];
        snprintf(combo_key, sizeof(combo_key), "window_%d", i);
        snprintf(btn_key, sizeof(btn_key), "remove_%d", i);
        snprintf(combo_label, sizeof(combo_label), "Window %d", i + 1);

        const char *val = settings ? obs_data_get_string(settings, combo_key) : "";
        bool has_value = val && *val;
        if (has_value) active++;

        obs_property_t *combo = obs_properties_add_list(props, combo_key, combo_label,
            OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
        populate_window_combo(combo);
        obs_property_set_visible(combo, mode != 0 && has_value);

        obs_property_t *btn = obs_properties_add_button(props, btn_key,
            obs_module_text("RemoveWindow"), remove_window_clicked);
        obs_property_set_visible(btn, mode != 0 && has_value);
    }

    obs_property_t *add_btn = obs_properties_add_button(props, "add_window",
        obs_module_text("AddWindow"), add_window_clicked);
    obs_property_set_visible(add_btn, mode != 0 && active < MAX_WINDOW_LIST);

    if (!ctx || cursor_enabled(ctx)) {
        obs_properties_add_bool(props, "show_cursor", obs_module_text("CaptureCursor"));
    }

    obs_properties_add_bool(props, "allow_transparency", obs_module_text("AllowTransparency"));
    obs_properties_add_bool(props, "force_hdr", obs_module_text("ForceHDR"));

    if (settings) obs_data_release(settings);
    return props;
}

enum gs_color_space vkcapture_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
    vkcapture_source_t *ctx = data;

    enum gs_color_space color_space = ctx->tdata.color_space;

    if (ctx->force_hdr) {
        color_space = GS_CS_709_EXTENDED;
    }

    UNUSED_PARAMETER(count);
    UNUSED_PARAMETER(preferred_spaces);
    return color_space;
}

static struct obs_source_info vkcapture_input = {
    .id = "vkcapture-source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .get_name = vkcapture_source_get_name,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE,
    .create = vkcapture_source_create,
    .destroy = vkcapture_source_destroy,
    .update = vkcapture_source_update,
    .video_tick = vkcapture_source_video_tick,
    .video_render = vkcapture_source_render,
    .get_width = vkcapture_source_get_width,
    .get_height = vkcapture_source_get_height,
    .get_defaults = vkcapture_source_get_defaults,
    .get_properties = vkcapture_source_get_properties,
    .icon_type = OBS_ICON_TYPE_GAME_CAPTURE,
    .video_get_color_space = vkcapture_get_color_space,
};

static bool server_wakeup()
{
    uint64_t q = 1;
    return write(server.eventfd, &q, sizeof(q)) == sizeof(q);
}

static void server_add_fd(int fd, int events)
{
    struct pollfd p;
    p.fd = fd;
    p.events = events;
    da_push_back(server.fds, &p);
}

static void server_remove_fd(int fd)
{
    for (size_t i = 0; i < server.fds.num; ++i) {
        struct pollfd *p = server.fds.array + i;
        if (p->fd == fd) {
            da_erase(server.fds, i);
            break;
        }
    }
}

static bool server_has_event_on_fd(int fd)
{
    for (size_t i = 0; i < server.fds.num; ++i) {
        struct pollfd *p = server.fds.array + i;
        if (p->fd == fd && p->revents) {
            return true;
        }
    }
    return false;
}

static void server_cleanup_client(vkcapture_client_t *client)
{
    pthread_mutex_lock(&server.mutex);

    blog(LOG_INFO, "Client %d disconnected", client->id);

    close(client->sockfd);
    server_remove_fd(client->sockfd);

    if (client->map_memory) {
        munmap(client->map_memory, client->map_size);
        client->map_memory = NULL;
    }

    for (int i = 0; i < 4; ++i) {
        if (client->buf_fds[i] >= 0) {
            close(client->buf_fds[i]);
            client->buf_fds[i] = -1;
        }
    }

    da_erase_item(server.clients, client);

    pthread_mutex_unlock(&server.mutex);
}

static void *server_thread_run(void *data)
{
    const char sockname[] = "/com/obsproject/vkcapture";

    int bufid = 0;
    int clientid = 0;

    da_init(server.fds);
    da_init(server.clients);

    struct sockaddr_un addr;
    addr.sun_family = PF_LOCAL;
    addr.sun_path[0] = '\0'; // Abstract socket
    memcpy(&addr.sun_path[1], sockname, sizeof(sockname) - 1);

    int sockfd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int ret = bind(sockfd, (const struct sockaddr *)&addr, sizeof(addr.sun_family) + sizeof(sockname));
    if (ret < 0) {
        blog(LOG_ERROR, "Cannot bind unix socket to %s: %d", addr.sun_path, errno);
        return NULL;
    }

    ret = listen(sockfd, 1);
    if (ret < 0) {
        blog(LOG_ERROR, "Cannot listen on unix socket bound to %s: %d", addr.sun_path, errno);
        return NULL;
    }

    server_add_fd(sockfd, POLLIN);
    server_add_fd(server.eventfd, POLLIN);

    while (true) {
        int ret = poll(server.fds.array, server.fds.num, -1);
        if (ret <= 0) {
            continue;
        }

        if (server_has_event_on_fd(server.eventfd)) {
            uint64_t q;
            if (read(server.eventfd, &q, sizeof(q)) != sizeof(q)) {
                blog(LOG_ERROR, "Failed to read from eventfd %s", strerror(errno));
            }
            if (server.quit) {
                break;
            }
        }

        if (server_has_event_on_fd(sockfd)) {
            int clientfd = accept4(sockfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (clientfd >= 0) {
                vkcapture_client_t client = {0};
                memset(&client.buf_fds, -1, sizeof(client.buf_fds));
                client.id = ++clientid;
                client.sockfd = clientfd;
                pthread_mutex_lock(&server.mutex);
                da_push_back(server.clients, &client);
                pthread_mutex_unlock(&server.mutex);
                server_add_fd(client.sockfd, POLLIN);
                struct ucred cred = {0};
                socklen_t cred_len = sizeof(cred);
                if (getsockopt(client.sockfd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0) {
                    blog(LOG_WARNING, "Failed to get socket credentials: %s", strerror(errno));
                }
                blog(LOG_INFO, "Client %d connected (pid=%d)", client.id, cred.pid);
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED) {
                    blog(LOG_ERROR, "Cannot accept unix socket: %s", strerror(errno));
                }
            }
        }

        for (size_t i = 0; i < server.clients.num; i++) {
            vkcapture_client_t *client = server.clients.array + i;
            if (client->unresponsive) {
                server_cleanup_client(client);
                continue;
            }
            if (!server_has_event_on_fd(client->sockfd)) {
                continue;
            }

            uint8_t buf[CAPTURE_TEXTURE_DATA_SIZE];
            struct msghdr msg = {0};
            struct iovec io = {
                .iov_base = buf,
                .iov_len = CAPTURE_TEXTURE_DATA_SIZE,
            };
            msg.msg_iov = &io;
            msg.msg_iovlen = 1;

            char cmsg_buf[CMSG_SPACE(sizeof(int)) * 4];
            msg.msg_control = cmsg_buf;
            msg.msg_controllen = sizeof(cmsg_buf);

            while (true) {
                const ssize_t n = recvmsg(client->sockfd, &msg, MSG_NOSIGNAL);
                if (n == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno != ECONNRESET) {
                        blog(LOG_ERROR, "Socket recv error: %s", strerror(errno));
                    }
                }
                if (n <= 0) {
                    server_cleanup_client(client);
                    break;
                }

                if (buf[0] == CAPTURE_CLIENT_DATA_TYPE) {
                    if (io.iov_len != CAPTURE_CLIENT_DATA_SIZE) {
                        server_cleanup_client(client);
                        break;
                    }
                    pthread_mutex_lock(&server.mutex);
                    memcpy(&client->cdata, buf, CAPTURE_CLIENT_DATA_SIZE);
                    pthread_mutex_unlock(&server.mutex);
                    break;
                } else if (buf[0] == CAPTURE_TEXTURE_DATA_TYPE) {
                    pthread_mutex_lock(&server.mutex);
                    memcpy(&client->tdata, buf, CAPTURE_TEXTURE_DATA_SIZE);
                    pthread_mutex_unlock(&server.mutex);

                    struct cmsghdr *cmsgh = CMSG_FIRSTHDR(&msg);
                    if (!cmsgh || cmsgh->cmsg_level != SOL_SOCKET || cmsgh->cmsg_type != SCM_RIGHTS) {
                        server_cleanup_client(client);
                        break;
                    }

                    const size_t nfd = (cmsgh->cmsg_len - sizeof(struct cmsghdr)) / sizeof(int);

                    int buf_fds[4] = {-1, -1, -1, -1};
                    for (size_t i = 0; i < nfd; ++i) {
                        buf_fds[i] = ((int*)CMSG_DATA(cmsgh))[i];
                    }

                    if (io.iov_len != CAPTURE_TEXTURE_DATA_SIZE || client->tdata.nfd != nfd) {
                        for (size_t i = 0; i < nfd; ++i) {
                            close(buf_fds[i]);
                        }
                        server_cleanup_client(client);
                        break;
                    }

                    pthread_mutex_lock(&server.mutex);
                    for (int i = 0; i < 4; ++i) {
                        if (client->buf_fds[i] >= 0) {
                            close(client->buf_fds[i]);
                        }
                        client->buf_fds[i] = buf_fds[i];
                    }
                    client->buf_id = ++bufid;
                    pthread_mutex_unlock(&server.mutex);
                }
            }
        }
    }

    while (server.clients.num) {
        server_cleanup_client(server.clients.array);
    }

    close(sockfd);

    da_free(server.clients);
    da_free(server.fds);

    return NULL;
}

bool obs_module_load(void)
{
    enum obs_nix_platform_type platform = obs_get_nix_platform();
#if HAVE_WAYLAND || LIBOBS_API_MAJOR_VER >= 30
    if (platform != OBS_NIX_PLATFORM_X11_EGL && platform != OBS_NIX_PLATFORM_WAYLAND) {
#else
    if (platform != OBS_NIX_PLATFORM_X11_EGL) {
#endif
        blog(LOG_ERROR, "linux-vkcapture cannot run on non-EGL platforms");
        return false;
    }

    server.eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (server.eventfd < 0) {
        blog(LOG_ERROR, "Failed to create eventfd: %s", strerror(errno));
        return false;
    }

    pthread_mutex_init(&server.mutex, NULL);
    if (pthread_create(&server.thread, NULL, server_thread_run, NULL) != 0) {
        blog(LOG_ERROR, "Failed to create thread");
        return false;
    }
    pthread_setname_np(server.thread, PLUGIN_NAME);

    obs_register_source(&vkcapture_input);
    blog(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

    return true;
}

void obs_module_unload()
{
    server.quit = true;
    if (server_wakeup()) {
        pthread_join(server.thread, NULL);
    }

    blog(LOG_INFO, "plugin unloaded");
}

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("David Rosca <nowrep@gmail.com>")
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
    return PLUGIN_NAME;
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return obs_module_text("Description");
}
