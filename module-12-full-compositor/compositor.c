/* compositor.c - Full-Featured Wayland Compositor (Module 12)
 *
 * Features:
 *   - DRM/KMS scan-out with dumb buffers
 *   - Libinput input device handling
 *   - wl_compositor + wl_shm protocol
 *   - xdg-shell protocol (xdg_wm_base, xdg_surface, xdg_toplevel)
 *   - Multiple window compositing with z-order (painter's algorithm)
 *   - Input event forwarding to clients (pointer + keyboard)
 *   - Focus tracking (pointer focus, keyboard focus)
 *   - Interactive window move (xdg_toplevel move request)
 *   - Proper keyboard keymap via xkbcommon
 *
 * Build:   make
 * Run:     sudo ./compositor
 * Connect: WAYLAND_DISPLAY=wayland-1 weston-simple-shm
 * Exit:    Esc, Ctrl+Alt+Backspace. Close window: Alt+F4
 */

#define _GNU_SOURCE
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-server-protocol.h"

extern int wl_display_init_shm(struct wl_display *display);

/* ================================================================== */
/* Data Structures                                                    */
/* ================================================================== */

enum mc_grab_type { GRAB_NONE = 0, GRAB_MOVE, GRAB_RESIZE };

struct mc_compositor;

/* xdg-shell 的"窗口角色"对象：wl_surface 只管像素，窗口语义（标题、
 * 尺寸协商、状态标志）全部记账在这里。一个 surface 最多挂一个 toplevel。 */
struct mc_toplevel {
    struct wl_list link;
    struct mc_compositor *compositor;
    struct mc_surface *surface;
    struct wl_resource *xdg_surface_resource;
    struct wl_resource *xdg_toplevel_resource;
    char *title;
    int32_t geo_x, geo_y, geo_w, geo_h;
    /* 最近一次 configure 的序号：客户端 ack_configure 时带回同一个 serial，
     * 合成器据此知道客户端当前应用的是哪一次尺寸协商 */
    uint32_t configure_serial;
    /* 本实现把"首次携带 buffer 的 commit"当作 mapped；更严格的做法是
     * 只承认 ack_configure 之后的那次 commit */
    bool mapped;
    bool maximized;
    bool fullscreen;
    bool minimized;
    int32_t pending_w, pending_h;
};


/* ── buffer 生命周期跟踪 ─────────────────────────────────────────
 * 客户端销毁 wl_buffer (wl_buffer.destroy) 后, wl_resource 即被释放。
 * 如果我们还握着裸指针, 之后 wl_buffer_send_release() 就是 use-after-free。
 * 教学要点: 服务端引用客户端资源, 必须挂 destroy 监听。 */
struct mc_surface;

struct mc_buffer_watch {
    struct wl_listener listener;   /* 挂在 wl_resource 的 destroy 链上 */
    struct mc_surface *surface;
};
struct mc_surface {
    struct wl_list link;
    struct mc_compositor *compositor;
    struct wl_resource *resource;
    struct wl_resource *pending_buffer;
    struct wl_resource *buffer;
    struct mc_buffer_watch pending_watch;
    struct mc_buffer_watch current_watch;
    struct wl_resource *frame_callback;
    int32_t x, y;
    struct mc_toplevel *toplevel;
};

struct mc_client {
    struct wl_list link;
    struct mc_compositor *compositor;
    struct wl_client *wl_client;
    struct wl_listener destroy_listener;
    struct wl_resource *pointer;
    struct wl_resource *keyboard;
    bool keyboard_keymap_sent;
};

struct mc_seat_resource {
    struct wl_list link;
    struct wl_resource *resource;
    struct mc_compositor *compositor;   /* user_data 是本结构, handler 经此取 c */
};

struct mc_compositor {
    struct wl_display *display;
    struct wl_event_loop *loop;

    /* DRM */
    int drm_fd;
    bool drm_master;
    uint32_t crtc_id;
    uint32_t conn_id;
    drmModeModeInfo mode;
    drmModeCrtc *saved_crtc;
    int32_t screen_w, screen_h;
    uint8_t *screen_data;
    uint32_t screen_fb_id;
    uint32_t screen_handle;
    uint32_t screen_stride;
    uint32_t screen_size;

    /* libinput */
    struct udev *udev;
    struct libinput *libinput;
    struct wl_event_source *input_source;

    /* xkb */
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    /* seat */
    struct wl_global *seat_global;
    struct wl_list seat_resources;
    uint32_t seat_caps;
    int32_t pointer_x, pointer_y;
    struct mc_surface *pointer_focus;
    uint32_t last_button_serial;
    uint32_t button_state;
    struct mc_surface *keyboard_focus;
    uint32_t last_key_serial;

    /* grab state */
    struct mc_toplevel *grabbed_toplevel;
    enum mc_grab_type grab_type;
    int32_t grab_x, grab_y;

    /* tracking */
    struct wl_list clients;
    struct wl_list surfaces;
    struct wl_list toplevels;
    struct wl_listener client_created;
};

/* ================================================================== */
/* Forward Declarations                                               */
/* ================================================================== */

static void mc_repaint(struct mc_compositor *c);
static struct mc_client *find_mc_client(struct mc_compositor *c,
                                         struct wl_client *wl_client);
static void toplevel_send_configure(struct mc_toplevel *t, int32_t w, int32_t h);
static void seat_set_keyboard_focus(struct mc_compositor *c,
                                     struct mc_surface *surface);
static void seat_set_pointer_focus(struct mc_compositor *c,
                                    struct mc_surface *surface);
static void seat_send_modifiers(struct mc_compositor *c);

/* ================================================================== */
/* DRM Backend                                                        */
/* ================================================================== */

static int find_crtc(int fd, drmModeRes *res, drmModeConnector *conn) {
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc) {
            int id = enc->crtc_id;
            drmModeFreeEncoder(enc);
            if (id) {
                for (int i = 0; i < res->count_crtcs; i++)
                    if (res->crtcs[i] == (uint32_t)id) return id;
            }
        }
    }
    for (int e = 0; e < conn->count_encoders; e++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (!enc) continue;
        for (int i = 0; i < res->count_crtcs; i++) {
            if (enc->possible_crtcs & (1 << i)) {
                int id = res->crtcs[i];
                drmModeFreeEncoder(enc);
                return id;
            }
        }
        drmModeFreeEncoder(enc);
    }
    return -1;
}

static int create_screen_fb(struct mc_compositor *c) {
    struct drm_mode_create_dumb create = {0};
    struct drm_mode_map_dumb    map    = {0};
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

    create.width  = (uint32_t)c->screen_w;
    create.height = (uint32_t)c->screen_h;
    create.bpp    = 32;
    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB"); return -1;
    }
    c->screen_handle = create.handle;
    c->screen_stride = create.pitch;
    c->screen_size   = create.size;

    handles[0] = create.handle; pitches[0] = create.pitch; offsets[0] = 0;
    if (drmModeAddFB2(c->drm_fd, (uint32_t)c->screen_w, (uint32_t)c->screen_h,
                      DRM_FORMAT_XRGB8888, handles, pitches, offsets,
                      &c->screen_fb_id, 0) < 0) {
        perror("drmModeAddFB2"); return -1;
    }

    map.handle = create.handle;
    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB"); return -1;
    }
    c->screen_data = mmap(NULL, c->screen_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, c->drm_fd, map.offset);
    if (c->screen_data == MAP_FAILED) {
        perror("mmap"); c->screen_data = NULL; return -1;
    }
    memset(c->screen_data, 0, c->screen_size);
    return 0;
}

/* 打开 DRM 设备: 优先 $DRM_CARD, 否则自动扫描第一个 card[0-9]*。
 * 显卡不一定叫 card0 —— 例如 virtio-gpu 常注册为 card1。 */
static int open_drm_card(const char **used) {
    const char *env = getenv("DRM_CARD");
    if (env && *env) {
        *used = env;
        return open(env, O_RDWR | O_CLOEXEC);
    }

    static char path[32];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            *used = path;
            return fd;
        }
    }
    *used = "(none)";
    return -1;
}

static int init_drm(struct mc_compositor *c) {
    const char *card_path = NULL;
    c->drm_fd = open_drm_card(&card_path);
    if (c->drm_fd < 0) {
        fprintf(stderr, "cannot open any /dev/dri/card[0-9]* (set DRM_CARD=/dev/dri/cardX)\n");
        fprintf(stderr, "Must run from TTY with sudo\n"); return -1;
    }
    printf("using DRM device: %s%s\n", card_path,
           getenv("DRM_CARD") ? " (from $DRM_CARD)" : " (auto-scan)");
    if (drmSetMaster(c->drm_fd) == 0) c->drm_master = true;
    else fprintf(stderr, "Warning: drmSetMaster failed (%s)\n", strerror(errno));

    drmModeRes *res = drmModeGetResources(c->drm_fd);
    if (!res) { perror("drmModeGetResources"); return -1; }

    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *cc = drmModeGetConnector(c->drm_fd, res->connectors[i]);
        if (!cc) continue;
        if (cc->connection == DRM_MODE_CONNECTED && cc->count_modes > 0) {
            conn = cc; c->conn_id = cc->connector_id; break;
        }
        drmModeFreeConnector(cc);
    }
    if (!conn) { fprintf(stderr, "No connected connector\n"); drmModeFreeResources(res); return -1; }

    drmModeModeInfo *mode = NULL;
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mode = &conn->modes[i]; break; }
    if (!mode) mode = &conn->modes[0];

    c->mode = *mode; c->screen_w = mode->hdisplay; c->screen_h = mode->vdisplay;
    printf("Connector %u  %s  %dx%d@%uHz\n", c->conn_id, mode->name,
           c->screen_w, c->screen_h, mode->vrefresh);

    int crtc = find_crtc(c->drm_fd, res, conn);
    if (crtc < 0) { fprintf(stderr, "No CRTC\n"); drmModeFreeConnector(conn); drmModeFreeResources(res); return -1; }
    c->crtc_id = (uint32_t)crtc;
    c->saved_crtc = drmModeGetCrtc(c->drm_fd, c->crtc_id);

    if (create_screen_fb(c) < 0) { drmModeFreeConnector(conn); drmModeFreeResources(res); return -1; }

    if (drmModeSetCrtc(c->drm_fd, c->crtc_id, c->screen_fb_id, 0, 0,
                       &c->conn_id, 1, &c->mode) < 0) {
        perror("drmModeSetCrtc"); drmModeFreeConnector(conn); drmModeFreeResources(res); return -1;
    }
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return 0;
}

static void fini_drm(struct mc_compositor *c) {
    if (c->saved_crtc) {
        drmModeSetCrtc(c->drm_fd, c->saved_crtc->crtc_id,
                       c->saved_crtc->buffer_id,
                       c->saved_crtc->x, c->saved_crtc->y,
                       &c->conn_id, 1, &c->saved_crtc->mode);
        drmModeFreeCrtc(c->saved_crtc); c->saved_crtc = NULL;
    }
    if (c->screen_data && c->screen_data != MAP_FAILED)
        munmap(c->screen_data, c->screen_size);
    if (c->screen_fb_id) drmModeRmFB(c->drm_fd, c->screen_fb_id);
    if (c->screen_handle) {
        struct drm_mode_destroy_dumb d = { .handle = c->screen_handle };
        drmIoctl(c->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    if (c->drm_master) drmDropMaster(c->drm_fd);
    if (c->drm_fd >= 0) close(c->drm_fd);
}

/* ================================================================== */
/* Rendering (painter's algorithm)                                    */
/* ================================================================== */

static void paint_surface(struct mc_compositor *c, struct mc_surface *s) {
    if (!s->buffer) return;
    struct wl_shm_buffer *shm = wl_shm_buffer_get(s->buffer);
    if (!shm) return;
    uint32_t fmt = wl_shm_buffer_get_format(shm);
    if (fmt != WL_SHM_FORMAT_XRGB8888 && fmt != WL_SHM_FORMAT_ARGB8888) return;

    int32_t bw = wl_shm_buffer_get_width(shm);
    int32_t bh = wl_shm_buffer_get_height(shm);
    int32_t bstride = wl_shm_buffer_get_stride(shm);

    wl_shm_buffer_begin_access(shm);
    uint8_t *src = wl_shm_buffer_get_data(shm);
    if (!src) { wl_shm_buffer_end_access(shm); return; }

    for (int32_t row = 0; row < bh; row++) {
        int32_t dy = s->y + row;
        if (dy < 0 || dy >= c->screen_h) continue;
        uint32_t *dst = (uint32_t *)(c->screen_data + (size_t)dy * c->screen_stride);
        uint32_t *src_row = (uint32_t *)(src + (size_t)row * bstride);
        for (int32_t col = 0; col < bw; col++) {
            int32_t dx = s->x + col;
            if (dx < 0 || dx >= c->screen_w) continue;
            dst[dx] = src_row[col] | 0xFF000000u;
        }
    }
    wl_shm_buffer_end_access(shm);
}

static void mc_repaint(struct mc_compositor *c) {
    if (!c->screen_data) return;
    for (int y = 0; y < c->screen_h; y++) {
        uint32_t *row = (uint32_t *)(c->screen_data + (size_t)y * c->screen_stride);
        for (int x = 0; x < c->screen_w; x++) row[x] = 0xFF000000u;
    }
    struct mc_surface *s;
    wl_list_for_each(s, &c->surfaces, link) paint_surface(c, s);
    drmModeDirtyFB(c->drm_fd, c->screen_fb_id, NULL, 0);
}

/* ================================================================== */
/* XKB Keymap                                                         */
/* ================================================================== */

/* keymap 文本有几十 KB，走 socket 逐字节传太浪费；Wayland 的做法是写进
 * memfd 后把 fd 交给客户端，客户端 mmap 直接共享同一份内容 */
static int create_keymap_fd(struct mc_compositor *c) {
    char *str = xkb_keymap_get_as_string(c->xkb_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t len = strlen(str);
    int fd = memfd_create("xkb-keymap", MFD_CLOEXEC);
    if (fd < 0) { free(str); return -1; }
    if (ftruncate(fd, (off_t)len) < 0) { close(fd); free(str); return -1; }
    if (write(fd, str, len) != (ssize_t)len) { close(fd); free(str); return -1; }
    lseek(fd, 0, SEEK_SET);
    free(str);
    return fd;
}

static int init_xkb(struct mc_compositor *c) {
    c->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!c->xkb_ctx) { fprintf(stderr, "xkb_context_new failed\n"); return -1; }
    struct xkb_rule_names names = { .rules = "evdev", .model = "pc105", .layout = "us" };
    c->xkb_keymap = xkb_keymap_new_from_names(c->xkb_ctx, &names,
                                               XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!c->xkb_keymap) {
        fprintf(stderr, "xkb_keymap_new_from_names failed\n");
        xkb_context_unref(c->xkb_ctx); c->xkb_ctx = NULL; return -1;
    }
    c->xkb_state = xkb_state_new(c->xkb_keymap);
    if (!c->xkb_state) {
        fprintf(stderr, "xkb_state_new failed\n");
        xkb_keymap_unref(c->xkb_keymap); xkb_context_unref(c->xkb_ctx);
        c->xkb_ctx = NULL; return -1;
    }
    return 0;
}

static void fini_xkb(struct mc_compositor *c) {
    if (c->xkb_state)    xkb_state_unref(c->xkb_state);
    if (c->xkb_keymap)   xkb_keymap_unref(c->xkb_keymap);
    if (c->xkb_ctx)      xkb_context_unref(c->xkb_ctx);
}

/* ================================================================== */
/* wl_pointer / wl_keyboard / wl_touch implementations                */
/* ================================================================== */

static void pointer_set_cursor(struct wl_client *client,
                                struct wl_resource *resource,
                                uint32_t serial, struct wl_resource *surface,
                                int32_t hx, int32_t hy) {
    (void)client; (void)resource; (void)serial; (void)surface; (void)hx; (void)hy;
}

static void pointer_release(struct wl_client *client, struct wl_resource *r) {
    (void)client; wl_resource_destroy(r);
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor, .release = pointer_release,
};

static void keyboard_release(struct wl_client *client, struct wl_resource *r) {
    (void)client; wl_resource_destroy(r);
}

static const struct wl_keyboard_interface keyboard_impl = {
    .release = keyboard_release,
};

static void touch_release(struct wl_client *client, struct wl_resource *r) {
    (void)client; wl_resource_destroy(r);
}

static const struct wl_touch_interface touch_impl = {
    .release = touch_release,
};

/* ================================================================== */
/* Seat: wl_seat implementation                                       */
/* ================================================================== */

static void seat_resource_destroy(struct wl_resource *resource) {
    struct mc_seat_resource *sr = wl_resource_get_user_data(resource);
    if (sr) { wl_list_remove(&sr->link); free(sr); }
}

static void seat_get_pointer(struct wl_client *client,
                              struct wl_resource *resource, uint32_t id) {
    struct mc_seat_resource *sr = wl_resource_get_user_data(resource);
    struct mc_compositor *c = sr->compositor;
    struct mc_client *mcc = find_mc_client(c, client);
    struct wl_resource *ptr = wl_resource_create(
        client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    if (!ptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(ptr, &pointer_impl, c, NULL);
    if (mcc) mcc->pointer = ptr;

    if (c->pointer_focus && wl_resource_get_client(c->pointer_focus->resource) == client) {
        wl_pointer_send_enter(ptr, wl_display_next_serial(c->display),
                              c->pointer_focus->resource,
                              wl_fixed_from_int(c->pointer_x),
                              wl_fixed_from_int(c->pointer_y));
    }
}

static void seat_get_keyboard(struct wl_client *client,
                               struct wl_resource *resource, uint32_t id) {
    struct mc_seat_resource *sr = wl_resource_get_user_data(resource);
    struct mc_compositor *c = sr->compositor;
    struct mc_client *mcc = find_mc_client(c, client);
    struct wl_resource *kbd = wl_resource_create(
        client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    if (!kbd) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(kbd, &keyboard_impl, c, NULL);
    if (mcc) { mcc->keyboard = kbd; mcc->keyboard_keymap_sent = false; }

    /* 客户端可能在焦点已经存在之后才 bind seat：迟到的 keyboard 资源要
     * 补发一次 enter，否则这个客户端永远收不到按键 */
    if (c->keyboard_focus && wl_resource_get_client(c->keyboard_focus->resource) == client)
        seat_set_keyboard_focus(c, c->keyboard_focus);
}

static void seat_get_touch(struct wl_client *client,
                            struct wl_resource *resource, uint32_t id) {
    struct mc_seat_resource *sr = wl_resource_get_user_data(resource);
    struct mc_compositor *c = sr->compositor;
    struct wl_resource *t = wl_resource_create(
        client, &wl_touch_interface, wl_resource_get_version(resource), id);
    if (!t) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(t, &touch_impl, c, NULL);
}

static void seat_release(struct wl_client *client, struct wl_resource *r) {
    (void)client; wl_resource_destroy(r);
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer, .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch, .release = seat_release,
};

static void seat_bind(struct wl_client *client, void *data,
                       uint32_t version, uint32_t id) {
    struct mc_compositor *c = data;
    struct mc_seat_resource *sr = calloc(1, sizeof(*sr));
    if (!sr) { wl_client_post_no_memory(client); return; }
    sr->resource = wl_resource_create(client, &wl_seat_interface, version, id);
    if (!sr->resource) { free(sr); wl_client_post_no_memory(client); return; }
    /* user_data 必须传 sr (堆上的 seat 资源记录), 而不是 c:
     * seat_resource_destroy 会把它当 mc_seat_resource 释放 —— 传 c 会
     * 在客户端断开时 free 栈上的 compositor 结构并踩坏内存 */
    sr->compositor = c;
    wl_resource_set_implementation(sr->resource, &seat_impl, sr, seat_resource_destroy);
    wl_list_insert(&c->seat_resources, &sr->link);
    wl_seat_send_capabilities(sr->resource, c->seat_caps);
    /* name 事件是 wl_seat v4 才引入的: 对旧版本客户端发送属于协议违规,
     * 会让 listener 槽位为 NULL 的简化客户端 (如 weston-simple-*) 直接 abort */
    if (wl_resource_get_version(sr->resource) >= WL_SEAT_NAME_SINCE_VERSION)
        wl_seat_send_name(sr->resource, "seat0");
}

static void seat_set_pointer_focus(struct mc_compositor *c,
                                    struct mc_surface *surface) {
    if (c->pointer_focus == surface) return;
    if (c->pointer_focus) {
        struct mc_client *old = find_mc_client(
            c, wl_resource_get_client(c->pointer_focus->resource));
        if (old && old->pointer) {
            uint32_t serial = wl_display_next_serial(c->display);
            wl_pointer_send_leave(old->pointer, serial, c->pointer_focus->resource);
        }
    }
    c->pointer_focus = surface;
    if (surface) {
        struct mc_client *mc = find_mc_client(
            c, wl_resource_get_client(surface->resource));
        if (mc && mc->pointer) {
            uint32_t serial = wl_display_next_serial(c->display);
            wl_pointer_send_enter(mc->pointer, serial, surface->resource,
                                   wl_fixed_from_int(c->pointer_x),
                                   wl_fixed_from_int(c->pointer_y));
        }
    }
}

static void seat_set_keyboard_focus(struct mc_compositor *c,
                                     struct mc_surface *surface) {
    if (c->keyboard_focus == surface) return;
    if (c->keyboard_focus) {
        struct mc_client *old = find_mc_client(
            c, wl_resource_get_client(c->keyboard_focus->resource));
        if (old && old->keyboard) {
            uint32_t serial = wl_display_next_serial(c->display);
            wl_keyboard_send_leave(old->keyboard, serial, c->keyboard_focus->resource);
        }
    }
    c->keyboard_focus = surface;
    if (surface) {
        struct mc_client *mc = find_mc_client(
            c, wl_resource_get_client(surface->resource));
        if (mc && mc->keyboard) {
            /* keymap 必须先于第一个 enter 送达，客户端才能解读随后的
             * keycode；每个客户端只需要发一次 */
            if (!mc->keyboard_keymap_sent) {
                int fd = create_keymap_fd(c);
                if (fd >= 0) {
                    char *str = xkb_keymap_get_as_string(c->xkb_keymap,
                                                          XKB_KEYMAP_FORMAT_TEXT_V1);
                    wl_keyboard_send_keymap(mc->keyboard,
                                             WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                             fd, (uint32_t)strlen(str));
                    close(fd); free(str);
                }
                mc->keyboard_keymap_sent = true;
            }
            uint32_t serial = wl_display_next_serial(c->display);
            struct wl_array keys;
            wl_array_init(&keys);
            wl_keyboard_send_enter(mc->keyboard, serial, surface->resource, &keys);
            wl_array_release(&keys);
            seat_send_modifiers(c);
        }
    }
}

/* 简化：把修饰键状态广播给所有客户端。真实实现只发给键盘焦点客户端——
 * 其他客户端不应该知道你按着 Shift */
static void seat_send_modifiers(struct mc_compositor *c) {
    xkb_mod_mask_t dep = xkb_state_serialize_mods(c->xkb_state, XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t lat = xkb_state_serialize_mods(c->xkb_state, XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t loc = xkb_state_serialize_mods(c->xkb_state, XKB_STATE_MODS_LOCKED);
    xkb_mod_mask_t grp = xkb_state_serialize_layout(c->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    struct mc_client *mcc;
    wl_list_for_each(mcc, &c->clients, link) {
        if (mcc->keyboard) {
            wl_keyboard_send_modifiers(mcc->keyboard,
                                        wl_display_next_serial(c->display),
                                        dep, lat, loc, grp);
        }
    }
}

/* ================================================================== */
/* wl_region                                                          */
/* ================================================================== */

static void region_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void region_add(struct wl_client *c, struct wl_resource *r,
                        int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void region_subtract(struct wl_client *c, struct wl_resource *r,
                             int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static const struct wl_region_interface region_impl = {
    .destroy = region_destroy, .add = region_add, .subtract = region_subtract,
};

/* ================================================================== */
/* wl_surface                                                         */
/* ================================================================== */


static void buffer_watched_destroyed(struct wl_listener *l, void *data) {
    struct mc_buffer_watch *w = wl_container_of(l, w, listener);
    struct wl_resource *res = data;
    struct mc_surface *s = w->surface;
    if (s->pending_buffer == res) s->pending_buffer = NULL;
    if (s->buffer == res)         s->buffer = NULL;
    wl_list_remove(&l->link);
    wl_list_init(&l->link);
}

/* 令 w 开始监听 buf 的销毁; buf==NULL 表示取消监听 */
static void watch_buffer(struct mc_buffer_watch *w, struct mc_surface *s,
                         struct wl_resource *buf) {
    wl_list_remove(&w->listener.link);          /* 若已在监听先摘除 (已 init 则 no-op) */
    wl_list_init(&w->listener.link);
    if (buf) {
        w->surface = s;
        w->listener.notify = buffer_watched_destroyed;
        wl_resource_add_destroy_listener(buf, &w->listener);
    }
}
static void surface_destroy(struct wl_resource *resource) {
    struct mc_surface *s = wl_resource_get_user_data(resource);
    if (!s) return;
    struct mc_compositor *c = s->compositor;
    if (s->toplevel) { s->toplevel->surface = NULL; s->toplevel = NULL; }
    /* surface 可能随时被客户端单方面销毁：凡是存了裸指针的地方都要在
     * 析构路径里清掉，否则下一次 repaint / 输入路由就是 use-after-free */
    if (c->pointer_focus == s) c->pointer_focus = NULL;
    if (c->keyboard_focus == s) c->keyboard_focus = NULL;
    if (c->grabbed_toplevel && c->grabbed_toplevel->surface == s) {
        c->grabbed_toplevel = NULL; c->grab_type = GRAB_NONE;
    }
    wl_list_remove(&s->link);
    wl_list_remove(&s->pending_watch.listener.link);
    wl_list_remove(&s->current_watch.listener.link);
    if (s->buffer) wl_buffer_send_release(s->buffer);
    if (s->frame_callback) wl_resource_destroy(s->frame_callback);
    free(s);
    if (c) mc_repaint(c);
}

static void surface_handle_destroy(struct wl_client *client, struct wl_resource *r) {
    (void)client; wl_resource_destroy(r);
}

static void surface_attach(struct wl_client *client, struct wl_resource *r,
                            struct wl_resource *buffer, int32_t x, int32_t y) {
    (void)client; (void)x; (void)y;
    struct mc_surface *s = wl_resource_get_user_data(r);
    s->pending_buffer = buffer;
    watch_buffer(&s->pending_watch, s, buffer);
}


static void surface_damage(struct wl_client *c, struct wl_resource *r,
                            int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}

static void surface_frame(struct wl_client *client, struct wl_resource *r, uint32_t cb) {
    struct mc_surface *s = wl_resource_get_user_data(r);
    struct wl_resource *rc = wl_resource_create(client, &wl_callback_interface, 1, cb);
    if (!rc) { wl_client_post_no_memory(client); return; }
    if (s->frame_callback) wl_resource_destroy(s->frame_callback);
    s->frame_callback = rc;
}

static void surface_set_opaque_region(struct wl_client *c, struct wl_resource *r,
                                       struct wl_resource *reg) { (void)c; (void)r; (void)reg; }
static void surface_set_input_region(struct wl_client *c, struct wl_resource *r,
                                      struct wl_resource *reg) { (void)c; (void)r; (void)reg; }

static void surface_commit(struct wl_client *client, struct wl_resource *r) {
    (void)client;
    struct mc_surface *s = wl_resource_get_user_data(r);
    struct mc_compositor *c = s->compositor;
    /* 客户端可能已销毁旧 buffer — watch 机制保证指针安全 */
    if (s->buffer && s->buffer != s->pending_buffer)
        wl_buffer_send_release(s->buffer);
    s->buffer = s->pending_buffer;
    /* 首次携带 buffer 的 commit 即视为窗口"可见"（mapped） */
    watch_buffer(&s->current_watch, s, s->buffer);
    watch_buffer(&s->pending_watch, s, NULL);
    if (s->toplevel && !s->toplevel->mapped && s->buffer) s->toplevel->mapped = true;
    mc_repaint(c);
    if (s->frame_callback) {
        wl_callback_send_done(s->frame_callback, (uint32_t)wl_display_get_serial(c->display));
        wl_resource_destroy(s->frame_callback);
        s->frame_callback = NULL;
    }
}

static void surface_set_buffer_transform(struct wl_client *c, struct wl_resource *r, int32_t t) {
    (void)c; (void)r; (void)t;
}
static void surface_set_buffer_scale(struct wl_client *c, struct wl_resource *r, int32_t s) {
    (void)c; (void)r; (void)s;
}
static void surface_damage_buffer(struct wl_client *c, struct wl_resource *r,
                                   int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void surface_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
    (void)c; (void)r; (void)x; (void)y;
}

static const struct wl_surface_interface surface_impl = {
    .destroy              = surface_handle_destroy,
    .attach               = surface_attach,
    .damage               = surface_damage,
    .frame                = surface_frame,
    .set_opaque_region    = surface_set_opaque_region,
    .set_input_region     = surface_set_input_region,
    .commit               = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale     = surface_set_buffer_scale,
    .damage_buffer        = surface_damage_buffer,
    .offset               = surface_offset,
};

/* ================================================================== */
/* wl_compositor                                                      */
/* ================================================================== */

static void compositor_create_surface(struct wl_client *client,
                                       struct wl_resource *resource, uint32_t id) {
    struct mc_compositor *c = wl_resource_get_user_data(resource);
    struct mc_surface *s = calloc(1, sizeof(*s));
    if (!s) { wl_client_post_no_memory(client); return; }
    s->compositor = c; s->x = 0; s->y = 0;
    s->resource = wl_resource_create(client, &wl_surface_interface,
                                      wl_resource_get_version(resource), id);
    if (!s->resource) { free(s); wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(s->resource, &surface_impl, s, surface_destroy);
    /* 插到链表尾 = 最顶层：绘制按头→尾（画家算法），命中测试按尾→头 */
    wl_list_init(&s->pending_watch.listener.link);
    wl_list_init(&s->current_watch.listener.link);
    wl_list_insert(c->surfaces.prev, &s->link);
}

static void compositor_create_region(struct wl_client *client,
                                      struct wl_resource *resource, uint32_t id) {
    struct wl_resource *r = wl_resource_create(
        client, &wl_region_interface, wl_resource_get_version(resource), id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface, .create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data,
                             uint32_t version, uint32_t id) {
    struct mc_compositor *c = data;
    struct wl_resource *r = wl_resource_create(client, &wl_compositor_interface, version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &compositor_impl, c, NULL);
}

/* ================================================================== */
/* xdg-shell: xdg_wm_base                                             */
/* ================================================================== */

static void xdg_wm_base_destroy(struct wl_client *client, struct wl_resource *r) {
    (void)client; wl_resource_destroy(r);
}

static void xdg_wm_base_create_positioner(struct wl_client *client,
                                           struct wl_resource *resource, uint32_t id) {
    struct wl_resource *pos = wl_resource_create(
        client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
    if (!pos) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(pos, NULL, NULL, NULL);
}

static void xdg_wm_base_pong(struct wl_client *client, struct wl_resource *r, uint32_t serial) {
    (void)client; (void)r; (void)serial;
}

/* ================================================================== */
/* xdg-shell: toplevel_send_configure                                 */
/* ================================================================== */

/* xdg-shell 规定两个事件成对出现：xdg_toplevel.configure 携带内容
 * （尺寸 + 状态位），紧随其后的 xdg_surface.configure 携带本次协商的
 * serial，客户端 ack 时带回同一个 serial 对账 */
static void toplevel_send_configure(struct mc_toplevel *t, int32_t w, int32_t h) {
    t->pending_w = w; t->pending_h = h;
    t->configure_serial = wl_display_next_serial(t->compositor->display);
    struct wl_array states;
    wl_array_init(&states);
    if (t->maximized) {
        uint32_t *s = wl_array_add(&states, sizeof(uint32_t));
        if (s) *s = XDG_TOPLEVEL_STATE_MAXIMIZED;
    }
    if (t->fullscreen) {
        uint32_t *s = wl_array_add(&states, sizeof(uint32_t));
        if (s) *s = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    xdg_toplevel_send_configure(t->xdg_toplevel_resource, w, h, &states);
    xdg_surface_send_configure(t->xdg_surface_resource, t->configure_serial);
    wl_array_release(&states);
}

/* ================================================================== */
/* xdg-shell: xdg_surface                                             */
/* ================================================================== */

static void xdg_surface_resource_destroy(struct wl_resource *resource) {
    struct mc_toplevel *t = wl_resource_get_user_data(resource);
    if (!t) return;
    if (t->xdg_surface_resource)
        wl_resource_set_user_data(t->xdg_surface_resource, NULL);
    if (t->xdg_toplevel_resource)
        wl_resource_set_user_data(t->xdg_toplevel_resource, NULL);
    if (t->surface) t->surface->toplevel = NULL;
    wl_list_remove(&t->link);
    free(t->title);
    free(t);
}

static void xdg_surface_destroy(struct wl_client *client, struct wl_resource *r) {
    (void)client; wl_resource_destroy(r);
}

static void xdg_surface_get_popup(struct wl_client *client, struct wl_resource *r,
                                   uint32_t id, struct wl_resource *parent,
                                   struct wl_resource *pos) {
    (void)client; (void)r; (void)id; (void)parent; (void)pos;
}

static void xdg_surface_set_window_geometry(struct wl_client *client,
                                             struct wl_resource *r,
                                             int32_t x, int32_t y,
                                             int32_t w, int32_t h) {
    (void)client;
    struct mc_toplevel *t = wl_resource_get_user_data(r);
    t->geo_x = x; t->geo_y = y; t->geo_w = w; t->geo_h = h;
}

static void xdg_surface_ack_configure(struct wl_client *client,
                                       struct wl_resource *r, uint32_t serial) {
    (void)client;
    struct mc_toplevel *t = wl_resource_get_user_data(r);
    if (serial == t->configure_serial) { /* acknowledged */ }
}

/* ================================================================== */
/* xdg-shell: xdg_toplevel                                            */
/* ================================================================== */

static void toplevel_resource_destroy(struct wl_resource *resource) {
    struct mc_toplevel *t = wl_resource_get_user_data(resource);
    if (!t) return;
    struct mc_compositor *c = t->compositor;
    if (c->grabbed_toplevel == t) { c->grabbed_toplevel = NULL; c->grab_type = GRAB_NONE; }
    if (c->keyboard_focus == t->surface) c->keyboard_focus = NULL;
    if (c->pointer_focus == t->surface) c->pointer_focus = NULL;
    if (t->surface) t->surface->toplevel = NULL;
    /* prevent the xdg_surface destructor from double-freeing */
    if (t->xdg_surface_resource)
        wl_resource_set_user_data(t->xdg_surface_resource, NULL);
    if (t->xdg_toplevel_resource)
        wl_resource_set_user_data(t->xdg_toplevel_resource, NULL);
    wl_list_remove(&t->link);
    free(t->title);
    free(t);
}

static void xdg_toplevel_destroy(struct wl_client *client, struct wl_resource *r) {
    (void)client; wl_resource_destroy(r);
}

static void xdg_toplevel_set_parent(struct wl_client *client, struct wl_resource *r,
                                     struct wl_resource *parent) {
    (void)client; (void)r; (void)parent;
}

static void xdg_toplevel_set_title(struct wl_client *client, struct wl_resource *r,
                                    const char *title) {
    (void)client;
    struct mc_toplevel *t = wl_resource_get_user_data(r);
    free(t->title);
    t->title = strdup(title);
    printf("Window: \"%s\"\n", t->title);
}

static void xdg_toplevel_set_app_id(struct wl_client *client, struct wl_resource *r,
                                     const char *app_id) {
    (void)client; (void)r; (void)app_id;
}

static void xdg_toplevel_show_window_menu(struct wl_client *client, struct wl_resource *r,
                                           struct wl_resource *seat, uint32_t serial,
                                           int32_t x, int32_t y) {
    (void)client; (void)r; (void)seat; (void)serial; (void)x; (void)y;
}

static void xdg_toplevel_move(struct wl_client *client, struct wl_resource *r,
                               struct wl_resource *seat, uint32_t serial) {
    (void)client; (void)seat; (void)serial;
    struct mc_toplevel *t = wl_resource_get_user_data(r);
    if (!t->surface) return;
    /* 记录按下点相对窗口左上角的偏移；此后 motion 全部被合成器截获，
     * 用 pointer - offset 反算窗口位置，直到按键释放 */
    t->compositor->grabbed_toplevel = t;
    t->compositor->grab_type = GRAB_MOVE;
    t->compositor->grab_x = t->compositor->pointer_x - t->surface->x;
    t->compositor->grab_y = t->compositor->pointer_y - t->surface->y;
    printf("Move: offset=(%d,%d)\n", t->compositor->grab_x, t->compositor->grab_y);
}

static void xdg_toplevel_resize(struct wl_client *client, struct wl_resource *r,
                                 struct wl_resource *seat, uint32_t serial, uint32_t edges) {
    (void)client; (void)r; (void)seat; (void)serial; (void)edges;
}

static void xdg_toplevel_set_max_size(struct wl_client *client, struct wl_resource *r,
                                       int32_t w, int32_t h) { (void)client; (void)r; (void)w; (void)h; }
static void xdg_toplevel_set_min_size(struct wl_client *client, struct wl_resource *r,
                                       int32_t w, int32_t h) { (void)client; (void)r; (void)w; (void)h; }

static void xdg_toplevel_set_maximized(struct wl_client *client, struct wl_resource *r) {
    (void)client; struct mc_toplevel *t = wl_resource_get_user_data(r);
    t->maximized = true;
    toplevel_send_configure(t, t->compositor->screen_w, t->compositor->screen_h);
}

static void xdg_toplevel_unset_maximized(struct wl_client *client, struct wl_resource *r) {
    (void)client; struct mc_toplevel *t = wl_resource_get_user_data(r);
    t->maximized = false; toplevel_send_configure(t, 0, 0);
}

static void xdg_toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *r,
                                         struct wl_resource *output) {
    (void)client; (void)output; struct mc_toplevel *t = wl_resource_get_user_data(r);
    t->fullscreen = true;
    toplevel_send_configure(t, t->compositor->screen_w, t->compositor->screen_h);
}

static void xdg_toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *r) {
    (void)client; struct mc_toplevel *t = wl_resource_get_user_data(r);
    t->fullscreen = false; toplevel_send_configure(t, 0, 0);
}

static void xdg_toplevel_set_minimized(struct wl_client *client, struct wl_resource *r) {
    (void)client;
    struct mc_toplevel *t = wl_resource_get_user_data(r);
    t->minimized = true;
}

static const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy           = xdg_toplevel_destroy,
    .set_parent        = xdg_toplevel_set_parent,
    .set_title         = xdg_toplevel_set_title,
    .set_app_id        = xdg_toplevel_set_app_id,
    .show_window_menu  = xdg_toplevel_show_window_menu,
    .move              = xdg_toplevel_move,
    .resize            = xdg_toplevel_resize,
    .set_max_size      = xdg_toplevel_set_max_size,
    .set_min_size      = xdg_toplevel_set_min_size,
    .set_maximized     = xdg_toplevel_set_maximized,
    .unset_maximized   = xdg_toplevel_unset_maximized,
    .set_fullscreen    = xdg_toplevel_set_fullscreen,
    .unset_fullscreen  = xdg_toplevel_unset_fullscreen,
    .set_minimized     = xdg_toplevel_set_minimized,
};

/*
 * The xdg_surface_get_toplevel needs to wire up both xdg_surface and
 * xdg_toplevel implementations on resource creation.  We override the
 * get_toplevel entry in xdg_surface_impl at the call-site below.
 */
static const struct xdg_surface_interface xdg_surface_impl_init;

static void xdg_surface_real_get_toplevel(struct wl_client *client,
                                           struct wl_resource *resource,
                                           uint32_t id) {
    struct mc_toplevel *t = wl_resource_get_user_data(resource);

    t->xdg_toplevel_resource = wl_resource_create(
        client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    if (!t->xdg_toplevel_resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(t->xdg_toplevel_resource,
                                    &xdg_toplevel_impl, t, toplevel_resource_destroy);
    wl_resource_set_implementation(resource, &xdg_surface_impl_init, t,
                                    xdg_surface_resource_destroy);
    /* 协议要求：客户端在收到第一个 configure 之前不得 commit。
     * (0,0) 表示"尺寸由客户端自己定" */
    toplevel_send_configure(t, 0, 0);
}

static const struct xdg_surface_interface xdg_surface_impl_init = {
    .destroy             = xdg_surface_destroy,
    .get_toplevel        = xdg_surface_real_get_toplevel,
    .get_popup           = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure       = xdg_surface_ack_configure,
};

static void xdg_wm_base_real_get_xdg_surface(struct wl_client *client,
                                              struct wl_resource *resource,
                                              uint32_t id,
                                              struct wl_resource *surface_res) {
    struct mc_compositor *c = wl_resource_get_user_data(resource);
    struct mc_surface *surface = wl_resource_get_user_data(surface_res);
    if (!surface) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE, "invalid surface");
        return;
    }
    /* 角色唯一性：一个 surface 只能有一个角色，重复赋角色是协议错误 */
    if (surface->toplevel) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                               "surface already has a role"); return;
    }

    struct mc_toplevel *t = calloc(1, sizeof(*t));
    if (!t) { wl_client_post_no_memory(client); return; }
    t->compositor = c;
    t->surface    = surface;
    t->xdg_surface_resource = wl_resource_create(
        client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    if (!t->xdg_surface_resource) { free(t); wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(t->xdg_surface_resource,
                                    &xdg_surface_impl_init, t,
                                    xdg_surface_resource_destroy);
    surface->toplevel = t;
    wl_list_insert(&c->toplevels, &t->link);
}

/* Override the xdg_wm_base_impl get_xdg_surface with the real one */
static const struct xdg_wm_base_interface xdg_wm_base_real_impl = {
    .destroy           = xdg_wm_base_destroy,
    .create_positioner = xdg_wm_base_create_positioner,
    .get_xdg_surface   = xdg_wm_base_real_get_xdg_surface,
    .pong              = xdg_wm_base_pong,
};

static void xdg_wm_base_real_bind(struct wl_client *client, void *data,
                                   uint32_t version, uint32_t id) {
    struct mc_compositor *c = data;
    if (version > 3) version = 3;
    struct wl_resource *r = wl_resource_create(client, &xdg_wm_base_interface, version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &xdg_wm_base_real_impl, c, NULL);
}

/* ================================================================== */
/* Window Management                                                  */
/* ================================================================== */

/* 从链表尾（最顶层）向头做矩形命中测试，所以点击总是先问最上层窗口。
 * 简化：直接拿 buffer 尺寸当窗口边界，忽略 set_window_geometry 与
 * input region */
static struct mc_surface *surface_at(struct mc_compositor *c, int32_t sx, int32_t sy) {
    struct mc_surface *s;
    /* iterate tail->head (topmost first) */
    struct wl_list *pos;
    for (pos = c->surfaces.prev; pos != &c->surfaces; pos = pos->prev) {
        s = wl_container_of(pos, s, link);
        if (!s->buffer) continue;
        struct wl_shm_buffer *shm = wl_shm_buffer_get(s->buffer);
        if (!shm) continue;
        int32_t bw = wl_shm_buffer_get_width(shm);
        int32_t bh = wl_shm_buffer_get_height(shm);
        if (sx >= s->x && sx < s->x + bw && sy >= s->y && sy < s->y + bh)
            return s;
    }
    return NULL;
}

/* 置顶 = 摘下来插到链表尾。z 序没有独立的数据结构，就是链表顺序本身 */
static void raise_surface(struct mc_compositor *c, struct mc_surface *s) {
    wl_list_remove(&s->link);
    wl_list_insert(c->surfaces.prev, &s->link);
}

/* ================================================================== */
/* Client Bookkeeping                                                 */
/* ================================================================== */

static struct mc_client *find_mc_client(struct mc_compositor *c,
                                         struct wl_client *wl_client) {
    struct mc_client *mcc;
    wl_list_for_each(mcc, &c->clients, link)
        if (mcc->wl_client == wl_client) return mcc;
    return NULL;
}

/* 客户端断开时清掉可能指向它的焦点/grab，再 repaint 盖掉它留下的像素 */
static void client_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct mc_client *mcc = wl_container_of(listener, mcc, destroy_listener);
    struct mc_compositor *c = mcc->compositor;
    if (c->pointer_focus && wl_resource_get_client(c->pointer_focus->resource) == mcc->wl_client)
        c->pointer_focus = NULL;
    if (c->keyboard_focus && wl_resource_get_client(c->keyboard_focus->resource) == mcc->wl_client)
        c->keyboard_focus = NULL;
    if (c->grabbed_toplevel && c->grabbed_toplevel->surface &&
        wl_resource_get_client(c->grabbed_toplevel->surface->resource) == mcc->wl_client) {
        c->grabbed_toplevel = NULL; c->grab_type = GRAB_NONE;
    }
    printf("Client disconnected\n");
    wl_list_remove(&mcc->link);
    wl_list_remove(&mcc->destroy_listener.link);
    free(mcc);
    mc_repaint(c);
}

static void client_created(struct wl_listener *listener, void *data) {
    struct mc_compositor *c = wl_container_of(listener, c, client_created);
    struct wl_client *wl_client = data;
    struct mc_client *mcc = calloc(1, sizeof(*mcc));
    if (!mcc) return;
    mcc->compositor = c; mcc->wl_client = wl_client;
    mcc->destroy_listener.notify = client_destroy;
    wl_client_add_destroy_listener(wl_client, &mcc->destroy_listener);
    wl_list_insert(&c->clients, &mcc->link);
    printf("Client connected\n");
}

/* ================================================================== */
/* Input Handling (libinput -> wl_pointer / wl_keyboard)              */
/* ================================================================== */

static int open_restricted(const char *path, int flags, void *data) {
    (void)data; int fd = open(path, flags); return fd < 0 ? -errno : fd;
}
static void close_restricted(int fd, void *data) { (void)data; close(fd); }
static const struct libinput_interface li_iface = {
    .open_restricted = open_restricted, .close_restricted = close_restricted,
};

static void handle_key_event(struct mc_compositor *c, struct libinput_event *ev) {
    struct libinput_event_keyboard *kev = libinput_event_get_keyboard_event(ev);
    uint32_t key   = libinput_event_keyboard_get_key(kev);
    uint32_t state = libinput_event_keyboard_get_key_state(kev);
    uint32_t time  = libinput_event_keyboard_get_time(kev);

    static bool ctrl = false, alt = false;
    if (key == KEY_LEFTCTRL  || key == KEY_RIGHTCTRL)  ctrl = (state == LIBINPUT_KEY_STATE_PRESSED);
    if (key == KEY_LEFTALT   || key == KEY_RIGHTALT)   alt  = (state == LIBINPUT_KEY_STATE_PRESSED);

    if (state == LIBINPUT_KEY_STATE_PRESSED) {
        if (key == KEY_ESC) { printf("ESC, terminating\n"); wl_display_terminate(c->display); return; }
        if (ctrl && alt && key == KEY_BACKSPACE) {
            printf("Ctrl+Alt+Backspace, terminating\n"); wl_display_terminate(c->display); return;
        }
        /* close 是"请求"不是"命令"：只发一个事件，客户端自愿销毁——
         * 它完全可以先弹"要不要保存"，甚至假装没听见 */
        if (alt && key == KEY_F4 && c->keyboard_focus && c->keyboard_focus->toplevel) {
            xdg_toplevel_send_close(c->keyboard_focus->toplevel->xdg_toplevel_resource);
            return;
        }
    }

    if (!c->keyboard_focus) return;
    struct mc_client *mcc = find_mc_client(c, wl_resource_get_client(c->keyboard_focus->resource));
    if (!mcc || !mcc->keyboard) return;

    uint32_t serial = wl_display_next_serial(c->display);
    c->last_key_serial = serial;
    wl_keyboard_send_key(mcc->keyboard, serial, time, key,
                          state == LIBINPUT_KEY_STATE_PRESSED
                              ? WL_KEYBOARD_KEY_STATE_PRESSED
                              : WL_KEYBOARD_KEY_STATE_RELEASED);

    /* libinput 给的是 evdev keycode（从 0 计），xkbcommon 的 keycode 从 8
     * 计（历史偏移），两套编号恒差 8 */
    xkb_keycode_t xkb_code = key + 8;
    xkb_state_update_key(c->xkb_state, xkb_code,
                          state == LIBINPUT_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP);
    seat_send_modifiers(c);
}

static void handle_pointer_motion(struct mc_compositor *c, struct libinput_event *ev) {
    struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
    double dx = libinput_event_pointer_get_dx(pev);
    double dy = libinput_event_pointer_get_dy(pev);
    uint32_t time = libinput_event_pointer_get_time(pev);

    c->pointer_x += (int32_t)dx; c->pointer_y += (int32_t)dy;
    if (c->pointer_x < 0) c->pointer_x = 0;
    if (c->pointer_y < 0) c->pointer_y = 0;
    if (c->pointer_x >= c->screen_w) c->pointer_x = c->screen_w - 1;
    if (c->pointer_y >= c->screen_h) c->pointer_y = c->screen_h - 1;

    /* grab 期间 motion 不再路由给客户端，全部用于搬窗口 */
    if (c->grab_type == GRAB_MOVE && c->grabbed_toplevel && c->grabbed_toplevel->surface) {
        c->grabbed_toplevel->surface->x = c->pointer_x - c->grab_x;
        c->grabbed_toplevel->surface->y = c->pointer_y - c->grab_y;
        mc_repaint(c);
        return;
    }

    /* 每次移动都重新做命中测试；命中变了 seat_set_pointer_focus 会发 leave/enter */
    struct mc_surface *s = surface_at(c, c->pointer_x, c->pointer_y);
    seat_set_pointer_focus(c, s);

    /* 已知简化：协议要求 surface 局部坐标，这里发的是屏幕全局坐标 */
    if (c->pointer_focus) {
        struct mc_client *mcc = find_mc_client(
            c, wl_resource_get_client(c->pointer_focus->resource));
        if (mcc && mcc->pointer) {
            wl_pointer_send_motion(mcc->pointer, time,
                                    wl_fixed_from_int(c->pointer_x),
                                    wl_fixed_from_int(c->pointer_y));
        }
    }
}

static void handle_pointer_button(struct mc_compositor *c, struct libinput_event *ev) {
    struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
    uint32_t button = libinput_event_pointer_get_button(pev);
    uint32_t state  = libinput_event_pointer_get_button_state(pev);
    uint32_t time   = libinput_event_pointer_get_time(pev);

    uint32_t wl_btn = 0;
    switch (button) {
    case BTN_LEFT:   wl_btn = BTN_LEFT;   break;
    case BTN_MIDDLE: wl_btn = BTN_MIDDLE; break;
    case BTN_RIGHT:  wl_btn = BTN_RIGHT;  break;
    default: return;
    }

    uint32_t serial = wl_display_next_serial(c->display);

    if (state == LIBINPUT_BUTTON_STATE_PRESSED) {
        c->button_state |= (1u << (wl_btn - BTN_MOUSE));
        struct mc_surface *s = surface_at(c, c->pointer_x, c->pointer_y);
        /* 点击 = 抬升 + 抢键盘焦点（click-to-focus）：
         * 桌面"点谁谁置顶、打字给谁"的全部实现就在这一行 */
        if (s) { raise_surface(c, s); mc_repaint(c); seat_set_keyboard_focus(c, s); }
    } else {
        c->button_state &= ~(1u << (wl_btn - BTN_MOUSE));
        /* 任意按键释放都结束 grab，拖拽到此为止 */
        if (c->grab_type != GRAB_NONE) { c->grab_type = GRAB_NONE; c->grabbed_toplevel = NULL; }
    }

    c->last_button_serial = serial;

    if (!c->pointer_focus) return;
    struct mc_client *mcc = find_mc_client(c, wl_resource_get_client(c->pointer_focus->resource));
    if (mcc && mcc->pointer) {
        wl_pointer_send_button(mcc->pointer, serial, time, wl_btn,
                                state == LIBINPUT_BUTTON_STATE_PRESSED
                                    ? WL_POINTER_BUTTON_STATE_PRESSED
                                    : WL_POINTER_BUTTON_STATE_RELEASED);
    }
}

static void handle_pointer_axis(struct mc_compositor *c, struct libinput_event *ev) {
    struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
    if (!libinput_event_pointer_has_axis(pev, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) return;
    double val = libinput_event_pointer_get_axis_value(pev, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
    uint32_t time = libinput_event_pointer_get_time(pev);
    if (!c->pointer_focus) return;
    struct mc_client *mcc = find_mc_client(c, wl_resource_get_client(c->pointer_focus->resource));
    if (mcc && mcc->pointer) {
        wl_pointer_send_axis(mcc->pointer, time,
                              WL_POINTER_AXIS_VERTICAL_SCROLL,
                              wl_fixed_from_double(val));
    }
}

static int on_input_data(int fd, uint32_t mask, void *data) {
    (void)fd; (void)mask;
    struct mc_compositor *c = data;
    if (libinput_dispatch(c->libinput) != 0) { perror("libinput_dispatch"); return 0; }
    struct libinput_event *ev;
    while ((ev = libinput_get_event(c->libinput))) {
        switch (libinput_event_get_type(ev)) {
        case LIBINPUT_EVENT_KEYBOARD_KEY:      handle_key_event(c, ev);      break;
        case LIBINPUT_EVENT_POINTER_MOTION:    handle_pointer_motion(c, ev); break;
        case LIBINPUT_EVENT_POINTER_BUTTON:    handle_pointer_button(c, ev); break;
        case LIBINPUT_EVENT_POINTER_AXIS:      handle_pointer_axis(c, ev);   break;
        default: break;
        }
        libinput_event_destroy(ev);
    }
    return 0;
}

static int init_libinput(struct mc_compositor *c) {
    c->udev = udev_new();
    if (!c->udev) { fprintf(stderr, "udev_new failed\n"); return -1; }
    c->libinput = libinput_udev_create_context(&li_iface, c, c->udev);
    if (!c->libinput) {
        fprintf(stderr, "libinput context failed\n"); udev_unref(c->udev); c->udev = NULL; return -1;
    }
    if (libinput_udev_assign_seat(c->libinput, "seat0") != 0) {
        fprintf(stderr, "libinput assign seat failed\n");
        libinput_unref(c->libinput); c->libinput = NULL; udev_unref(c->udev); c->udev = NULL; return -1;
    }
    return 0;
}

static void fini_libinput(struct mc_compositor *c) {
    if (c->input_source) wl_event_source_remove(c->input_source);
    if (c->libinput)     libinput_unref(c->libinput);
    if (c->udev)         udev_unref(c->udev);
}

/* ================================================================== */
/* Cleanup                                                            */
/* ================================================================== */

static void cleanup(struct mc_compositor *c) {
    fini_libinput(c);
    if (c->display) {
        wl_display_destroy_clients(c->display);
        wl_display_destroy(c->display);
        c->display = NULL;
    }
    fini_xkb(c);
    fini_drm(c);
}

/* ================================================================== */
/* main                                                               */
/* ================================================================== */

int main(void) {
    struct mc_compositor c = {0};

    /* sudo/SSH 环境会清空 XDG_RUNTIME_DIR, 导致 socket 创建失败。
     * 回退到共享目录 /tmp/wayland-runtime, 客户端可用
     * XDG_RUNTIME_DIR=/tmp/wayland-runtime WAYLAND_DISPLAY=wayland-N 连接。 */
    if (!getenv("XDG_RUNTIME_DIR")) {
        mkdir("/tmp/wayland-runtime", 0755);
        setenv("XDG_RUNTIME_DIR", "/tmp/wayland-runtime", 1);
    }

    c.drm_fd = -1;
    c.seat_caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
    wl_list_init(&c.clients);
    wl_list_init(&c.surfaces);
    wl_list_init(&c.toplevels);
    wl_list_init(&c.seat_resources);

    printf("=== Module 12: Full-Featured Wayland Compositor ===\n");
    printf("Run from TTY: sudo ./compositor\n\n");

    if (init_drm(&c) < 0)      { fprintf(stderr, "DRM init failed\n");       cleanup(&c); return 1; }
    if (init_xkb(&c) < 0)      { fprintf(stderr, "XKB init failed\n");       cleanup(&c); return 1; }
    if (init_libinput(&c) < 0) { fprintf(stderr, "libinput init failed\n"); /* non-fatal */ }

    c.display = wl_display_create();
    if (!c.display) { fprintf(stderr, "wl_display_create failed\n"); cleanup(&c); return 1; }
    c.loop = wl_display_get_event_loop(c.display);

    c.client_created.notify = client_created;
    wl_display_add_client_created_listener(c.display, &c.client_created);

    const char *socket = wl_display_add_socket_auto(c.display);
    if (!socket) { fprintf(stderr, "socket failed\n"); cleanup(&c); return 1; }

    /* root 运行 + 普通用户客户端连接时, socket 默认 0755 会让客户端因无写权限
     * connect 失败 (EACCES)。教学场景放开连接权限;
     * 生产 compositor 走 logind 授权 + 同用户运行, 不存在此问题。 */
    {
        char sock_path[256];
        snprintf(sock_path, sizeof(sock_path), "%s/%s",
                 getenv("XDG_RUNTIME_DIR"), socket);
        if (chmod(sock_path, 0777) != 0)
            fprintf(stderr, "warning: chmod %s failed (%s)\n", sock_path,
                    strerror(errno));
    }

    if (!wl_global_create(c.display, &wl_compositor_interface, 4, &c, compositor_bind)) {
        fprintf(stderr, "wl_compositor global failed\n"); cleanup(&c); return 1;
    }
    if (wl_display_init_shm(c.display) != 0) {
        fprintf(stderr, "wl_display_init_shm failed\n"); cleanup(&c); return 1;
    }
    c.seat_global = wl_global_create(c.display, &wl_seat_interface, 5, &c, seat_bind);
    if (!c.seat_global) { fprintf(stderr, "wl_seat global failed\n"); cleanup(&c); return 1; }
    if (!wl_global_create(c.display, &xdg_wm_base_interface, 3, &c, xdg_wm_base_real_bind)) {
        fprintf(stderr, "xdg_wm_base global failed\n"); cleanup(&c); return 1;
    }

    if (c.libinput) {
        c.input_source = wl_event_loop_add_fd(c.loop, libinput_get_fd(c.libinput),
                                               WL_EVENT_READABLE, on_input_data, &c);
        if (!c.input_source) fprintf(stderr, "add libinput fd failed\n");
    }

    printf("\nCompositor running. WAYLAND_DISPLAY=%s\n", socket);
    printf("  Connect: WAYLAND_DISPLAY=%s weston-simple-shm\n", socket);
    printf("  Exit: Esc or Ctrl+Alt+Backspace  |  Close window: Alt+F4\n\n");

    mc_repaint(&c);
    wl_display_run(c.display);

    printf("\nShutting down...\n");
    cleanup(&c);
    return 0;
}
