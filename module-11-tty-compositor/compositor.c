/* compositor.c - 最小化 TTY Wayland Compositor
 *
 * 模块 11: 把 DRM/KMS、libinput 和 libwayland-server 三者结合成一个能
 * 真正运行的 compositor。
 *
 *   1. 打开 DRM 设备，找到已连接的 connector/mode，设置 CRTC。
 *   2. 创建带 Unix socket 的 wl_display。
 *   3. 注册 wl_compositor / wl_shm 全局接口。
 *   4. 接受客户端连接。
 *   5. 客户端 commit 一个带 wl_buffer 的 wl_surface 时，把它渲染到屏幕。
 *   6. 用 libinput 读取键盘 / 指针输入。
 *   7. 把 libinput 的 fd 加入 Wayland 事件循环。
 *   8. 按 Ctrl+Alt+Backspace 或 Esc 时干净退出，恢复之前的 CRTC 状态。
 *
 * 必须从纯 TTY (Ctrl+Alt+F3 登录) 运行，且通常需要 sudo:
 *
 *     make
 *     sudo ./compositor
 *
 * 然后在另一个 TTY 或 SSH 里用打印出来的 WAYLAND_DISPLAY 连接客户端:
 *
 *     WAYLAND_DISPLAY=wayland-1 weston-simple-shm
 */

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
#include <drm/drm_fourcc.h>

#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

/* libwayland-server 内部实现的 wl_shm 全局，头文件里没有声明。 */
extern int wl_display_init_shm(struct wl_display *display);

/* ------------------------------------------------------------------ */
/* Types                                                              */
/* ------------------------------------------------------------------ */

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
    int screen_w, screen_h;
    uint8_t *screen_data;
    uint32_t screen_fb_id;
    uint32_t screen_handle;
    uint32_t screen_stride;
    uint32_t screen_size;

    /* libinput */
    struct udev *udev;
    struct libinput *libinput;
    struct wl_event_source *input_source;

    /* clients / surfaces */
    struct wl_list clients;  /* mc_client.link  */
    struct wl_list surfaces; /* mc_surface.link */
    struct wl_listener client_created;
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

    /* pending state (set by attach, applied on commit) */
    struct wl_resource *pending_buffer;

    /* current state (what we scan out) */
    struct wl_resource *buffer;
    struct mc_buffer_watch pending_watch;
    struct mc_buffer_watch current_watch;

    /* pending frame callbacks, applied on commit */
    struct wl_resource *frame_callback;

    int x, y; /* 屏幕上的位置 */
};

struct mc_client {
    struct wl_list link;
    struct mc_compositor *compositor;
    struct wl_client *wl_client;
    struct wl_listener destroy_listener;
};

/* ------------------------------------------------------------------ */
/* Forward declarations                                              */
/* ------------------------------------------------------------------ */

static void mc_repaint(struct mc_compositor *c);

/* ------------------------------------------------------------------ */
/* DRM setup                                                         */
/* ------------------------------------------------------------------ */

/* connector 不能随便配 CRTC: 硬件走线有限制, encoder 的 possible_crtcs
 * 是一张位图, 第 i 位为 1 才表示可以接 res->crtcs[i]。 */
static int find_crtc(int fd, drmModeRes *res, drmModeConnector *conn) {
    /* 优先复用 encoder 当前绑定的 CRTC */
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc) {
            int crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
            if (crtc_id) {
                for (int i = 0; i < res->count_crtcs; i++)
                    if (res->crtcs[i] == (uint32_t)crtc_id)
                        return crtc_id;
            }
        }
    }

    /* 回退: 遍历 connector 的所有 encoder 及其 possible_crtcs */
    for (int e = 0; e < conn->count_encoders; e++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (!enc)
            continue;
        for (int i = 0; i < res->count_crtcs; i++) {
            if (enc->possible_crtcs & (1 << i)) {
                int crtc_id = res->crtcs[i];
                drmModeFreeEncoder(enc);
                return crtc_id;
            }
        }
        drmModeFreeEncoder(enc);
    }

    return -1;
}

/* 创建被 CRTC 扫描的"屏幕" framebuffer, 三步:
 *   1. CREATE_DUMB   内核分配一块线性、CPU 可映射的显存/内存 (dumb buffer)
 *   2. AddFB2        把这块存储包装成 DRM framebuffer (附宽/高/格式/stride)
 *   3. MAP_DUMB+mmap 拿到用户态指针, 此后 CPU 写这里 = 屏幕可见
 *
 * dumb buffer 是"正被扫描的单缓冲": 没有 vsync, 写入撞上扫描位置会撕裂,
 * 换来的是最简显示通路。生产合成器用 GBM + page flip 替代 (见 README)。 */
static int create_screen_fb(struct mc_compositor *c) {
    struct drm_mode_create_dumb create = {0};
    struct drm_mode_map_dumb map = {0};
    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};

    create.width = c->screen_w;
    create.height = c->screen_h;
    create.bpp = 32;
    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return -1;
    }

    c->screen_handle = create.handle;
    c->screen_stride = create.pitch;
    c->screen_size = create.size;

    handles[0] = create.handle;
    pitches[0] = create.pitch;
    offsets[0] = 0;
    if (drmModeAddFB2(c->drm_fd, c->screen_w, c->screen_h, DRM_FORMAT_XRGB8888,
                      handles, pitches, offsets, &c->screen_fb_id, 0) < 0) {
        perror("drmModeAddFB2");
        return -1;
    }

    map.handle = create.handle;
    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        return -1;
    }

    c->screen_data = mmap(NULL, c->screen_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, c->drm_fd, map.offset);
    if (c->screen_data == MAP_FAILED) {
        perror("mmap");
        c->screen_data = NULL;
        return -1;
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
    drmModeRes *res = NULL;
    drmModeConnector *conn = NULL;
    drmModeModeInfo *mode = NULL;
    int crtc_id = -1;
    const char *card_path = NULL;

    c->drm_fd = open_drm_card(&card_path);
    if (c->drm_fd < 0) {
        fprintf(stderr,
                "无法打开任何 /dev/dri/card[0-9]* (可用 DRM_CARD=/dev/dri/cardX 指定)\n");
        fprintf(stderr, "提示: 必须从 TTY 运行且可能需要 sudo\n");
        return -1;
    }
    printf("使用 DRM 设备: %s%s\n", card_path,
           getenv("DRM_CARD") ? " (来自 $DRM_CARD)" : " (自动扫描)");

    /* 成为 DRM master: 模式设置类 ioctl 是特权操作, 且一个 DRM 设备同一
     * 时刻只能有一个 master —— 这就是必须先 stop gdm (放倒 gnome-shell)
     * 再从 TTY 跑本程序的原因。root 直接 drmSetMaster 即可; 普通用户
     * 需要 logind 授权 (gnome-shell 就是这么做的)。 */
    if (drmSetMaster(c->drm_fd) == 0) {
        c->drm_master = true;
    } else {
        fprintf(stderr,
                "警告: drmSetMaster 失败 (%s)，可能已有其它 DRM master。\n"
                "请从纯 TTY 运行 (无 X11/Wayland)。\n",
                strerror(errno));
    }

    res = drmModeGetResources(c->drm_fd);
    if (!res) {
        perror("drmModeGetResources");
        return -1;
    }

    /* 找第一个已连接且带有效模式的 connector */
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *cc = drmModeGetConnector(c->drm_fd, res->connectors[i]);
        if (!cc)
            continue;
        if (cc->connection == DRM_MODE_CONNECTED && cc->count_modes > 0) {
            conn = cc;
            c->conn_id = cc->connector_id;
            break;
        }
        drmModeFreeConnector(cc);
    }

    if (!conn) {
        fprintf(stderr, "找不到已连接且带有效模式的 connector\n");
        drmModeFreeResources(res);
        return -1;
    }

    /* 优先 preferred 模式，否则用第一个 */
    for (int i = 0; i < conn->count_modes; i++) {
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            mode = &conn->modes[i];
            break;
        }
    }
    if (!mode)
        mode = &conn->modes[0];

    c->mode = *mode;
    c->screen_w = mode->hdisplay;
    c->screen_h = mode->vdisplay;

    printf("connector id=%u  模式 %s %dx%d @ %uHz\n", c->conn_id, mode->name,
           c->screen_w, c->screen_h, mode->vrefresh);

    crtc_id = find_crtc(c->drm_fd, res, conn);
    if (crtc_id < 0) {
        fprintf(stderr, "找不到合适的 CRTC\n");
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return -1;
    }
    c->crtc_id = crtc_id;
    printf("CRTC id=%u\n", c->crtc_id);

    /* 保存当前 CRTC 状态用于退出时恢复 */
    c->saved_crtc = drmModeGetCrtc(c->drm_fd, c->crtc_id);

    if (create_screen_fb(c) < 0) {
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return -1;
    }

    printf("screen fb_id=%u handle=%u stride=%u size=%u\n", c->screen_fb_id,
           c->screen_handle, c->screen_stride, c->screen_size);

    /* 让 CRTC 扫描出我们的 framebuffer */
    if (drmModeSetCrtc(c->drm_fd, c->crtc_id, c->screen_fb_id, 0, 0, &c->conn_id,
                       1, &c->mode) < 0) {
        perror("drmModeSetCrtc");
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return -1;
    }

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return 0;
}

static void fini_drm(struct mc_compositor *c) {
    /* 恢复原始 CRTC 配置 */
    if (c->saved_crtc) {
        drmModeSetCrtc(c->drm_fd, c->saved_crtc->crtc_id,
                       c->saved_crtc->buffer_id, c->saved_crtc->x,
                       c->saved_crtc->y, &c->conn_id, 1, &c->saved_crtc->mode);
        drmModeFreeCrtc(c->saved_crtc);
        c->saved_crtc = NULL;
    }

    if (c->screen_data && c->screen_data != MAP_FAILED)
        munmap(c->screen_data, c->screen_size);
    if (c->screen_fb_id)
        drmModeRmFB(c->drm_fd, c->screen_fb_id);
    if (c->screen_handle) {
        struct drm_mode_destroy_dumb destroy = {0};
        destroy.handle = c->screen_handle;
        drmIoctl(c->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }

    if (c->drm_master)
        drmDropMaster(c->drm_fd);
    if (c->drm_fd >= 0)
        close(c->drm_fd);
}

/* ------------------------------------------------------------------ */
/* Rendering (painter's algorithm, 无 z-order)                       */
/* ------------------------------------------------------------------ */

static void paint_surface(struct mc_compositor *c, struct mc_surface *s) {
    struct wl_shm_buffer *shm;
    uint8_t *src;
    int32_t bw, bh, bstride;
    uint32_t format;

    if (!s->buffer)
        return;

    shm = wl_shm_buffer_get(s->buffer);
    if (!shm)
        return; /* 我们只处理 wl_shm buffer */

    format = wl_shm_buffer_get_format(shm);
    if (format != WL_SHM_FORMAT_XRGB8888 && format != WL_SHM_FORMAT_ARGB8888)
        return;

    bw = wl_shm_buffer_get_width(shm);
    bh = wl_shm_buffer_get_height(shm);
    bstride = wl_shm_buffer_get_stride(shm);

    /* begin/end_access 维护共享池 mmap 的引用计数: 防止我们正读像素时,
     * 客户端销毁 wl_shm_pool 导致底层映射被回收 */
    wl_shm_buffer_begin_access(shm);
    src = wl_shm_buffer_get_data(shm);
    if (!src) {
        wl_shm_buffer_end_access(shm);
        return;
    }

    /* 逐行拷贝到屏幕，做裁剪。屏幕 stride 与 buffer stride 可能不同。 */
    for (int32_t row = 0; row < bh; row++) {
        int dy = s->y + row;
        if (dy < 0 || dy >= c->screen_h)
            continue;

        uint32_t *dst_row =
            (uint32_t *)(c->screen_data + (size_t)dy * c->screen_stride);
        uint32_t *src_row = (uint32_t *)(src + (size_t)row * bstride);

        for (int32_t col = 0; col < bw; col++) {
            int dx = s->x + col;
            if (dx < 0 || dx >= c->screen_w)
                continue;
            /* 强制不透明 (XRGB)，忽略 alpha */
            dst_row[dx] = src_row[col] | 0xFF000000u;
        }
    }

    wl_shm_buffer_end_access(shm);
}

static void mc_repaint(struct mc_compositor *c) {
    struct mc_surface *s;

    if (!c->screen_data)
        return;

    /* 清屏为黑 (XRGB8888) */
    for (int y = 0; y < c->screen_h; y++) {
        uint32_t *row = (uint32_t *)(c->screen_data + (size_t)y * c->screen_stride);
        for (int x = 0; x < c->screen_w; x++)
            row[x] = 0xFF000000u;
    }

    /* 画家算法: 按插入顺序绘制所有 surface */
    wl_list_for_each(s, &c->surfaces, link) {
        paint_surface(c, s);
    }

    /* dumb buffer 是直接扫描出的，无需 page flip，写入即可见。
     * 触发一次 dirty 让某些驱动刷新。*/
    drmModeDirtyFB(c->drm_fd, c->screen_fb_id, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* wl_surface implementation                                         */
/* ------------------------------------------------------------------ */


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
    if (!s)
        return;
    wl_list_remove(&s->link);
    wl_list_remove(&s->pending_watch.listener.link);
    wl_list_remove(&s->current_watch.listener.link);
    if (s->frame_callback)
        wl_resource_destroy(s->frame_callback);
    struct mc_compositor *c = s->compositor;
    free(s);
    if (c)
        mc_repaint(c);
}

static void surface_handle_destroy(struct wl_client *client,
                                   struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void surface_attach(struct wl_client *client,
                           struct wl_resource *resource,
                           struct wl_resource *buffer, int32_t x, int32_t y) {
    (void)client;
    (void)x;
    (void)y;
    struct mc_surface *s = wl_resource_get_user_data(resource);
    /* Wayland 的原子更新模型: attach 只把 buffer 记到 pending,
     * commit 时才一次性生效 (客户端可在 commit 前多次 attach 覆盖) */
    s->pending_buffer = buffer;
    watch_buffer(&s->pending_watch, s, buffer);
}


static void surface_damage(struct wl_client *client, struct wl_resource *resource,
                           int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource,
                          uint32_t callback) {
    struct mc_surface *s = wl_resource_get_user_data(resource);

    struct wl_resource *cb =
        wl_resource_create(client, &wl_callback_interface, 1, callback);
    if (!cb) {
        wl_client_post_no_memory(client);
        return;
    }
    /* 只保留最近一次；旧的直接丢弃 (最小实现) */
    s->frame_callback = cb;
}

static void surface_set_opaque_region(struct wl_client *client,
                                      struct wl_resource *resource,
                                      struct wl_resource *region) {
    (void)client;
    (void)resource;
    (void)region;
}

static void surface_set_input_region(struct wl_client *client,
                                     struct wl_resource *resource,
                                     struct wl_resource *region) {
    (void)client;
    (void)resource;
    (void)region;
}

static void surface_commit(struct wl_client *client,
                           struct wl_resource *resource) {
    (void)client;
    struct mc_surface *s = wl_resource_get_user_data(resource);
    struct mc_compositor *c = s->compositor;

    /* release 语义: 客户端 commit 后就交出了 buffer 的写权, 直到我们发
     * wl_buffer.release 它才能复用这块存储。漏掉这步, 客户端的双缓冲池
     * 会立刻"饿死", 应用表现为卡死在第一帧。
     * 客户端可能已销毁它 — watch 机制保证 s->buffer 要么有效要么为 NULL */
    if (s->buffer && s->buffer != s->pending_buffer)
        wl_buffer_send_release(s->buffer);

    /* pending -> current: commit 是唯一的状态提升点 (监听随之换防到 current) */
    s->buffer = s->pending_buffer;
    watch_buffer(&s->current_watch, s, s->buffer);
    watch_buffer(&s->pending_watch, s, NULL);

    mc_repaint(c);

    /* frame callback 是一次性的 (协议规定 done 后即销毁, 客户端每帧
     * 重新请求)。简化: commit 后立即触发。真实合成器应等这帧真正上屏
     * (page flip 完成) 再触发, 否则客户端帧率不会与屏幕刷新对齐。 */
    if (s->frame_callback) {
        wl_callback_send_done(s->frame_callback,
                              (uint32_t)(wl_display_get_serial(c->display)));
        wl_resource_destroy(s->frame_callback);
        s->frame_callback = NULL;
    }
}

static void surface_set_buffer_transform(struct wl_client *client,
                                         struct wl_resource *resource,
                                         int32_t transform) {
    (void)client;
    (void)resource;
    (void)transform;
}

static void surface_set_buffer_scale(struct wl_client *client,
                                     struct wl_resource *resource,
                                     int32_t scale) {
    (void)client;
    (void)resource;
    (void)scale;
}

static void surface_damage_buffer(struct wl_client *client,
                                  struct wl_resource *resource, int32_t x,
                                  int32_t y, int32_t width, int32_t height) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void surface_offset(struct wl_client *client,
                           struct wl_resource *resource, int32_t x, int32_t y) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_handle_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
};

/* ------------------------------------------------------------------ */
/* wl_region implementation (最小 stub)                              */
/* ------------------------------------------------------------------ */

static void region_destroy(struct wl_client *client,
                           struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void region_add(struct wl_client *client, struct wl_resource *resource,
                       int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void region_subtract(struct wl_client *client,
                            struct wl_resource *resource, int32_t x, int32_t y,
                            int32_t width, int32_t height) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static const struct wl_region_interface region_impl = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

/* ------------------------------------------------------------------ */
/* wl_compositor implementation                                      */
/* ------------------------------------------------------------------ */

static void compositor_create_surface(struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t id) {
    struct mc_compositor *c = wl_resource_get_user_data(resource);

    struct mc_surface *surface = calloc(1, sizeof(*surface));
    if (!surface) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->compositor = c;
    surface->resource = wl_resource_create(
        client, &wl_surface_interface, wl_resource_get_version(resource), id);
    if (!surface->resource) {
        free(surface);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(surface->resource, &surface_impl, surface,
                                   surface_destroy);

    /* 简单布局: 新 surface 从左上角 (0,0) 开始 */
    surface->x = 0;
    surface->y = 0;

    wl_list_init(&surface->pending_watch.listener.link);
    wl_list_init(&surface->current_watch.listener.link);

    wl_list_insert(c->surfaces.prev, &surface->link);
    printf("客户端创建 surface (id=%u)\n", id);
}

static void compositor_create_region(struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t id) {
    struct wl_resource *region = wl_resource_create(
        client, &wl_region_interface, wl_resource_get_version(resource), id);
    if (!region) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(region, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
    struct mc_compositor *c = data;

    struct wl_resource *resource =
        wl_resource_create(client, &wl_compositor_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &compositor_impl, c, NULL);
}

/* ------------------------------------------------------------------ */
/* Client bookkeeping                                                */
/* ------------------------------------------------------------------ */

static void client_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct mc_client *mcc = wl_container_of(listener, mcc, destroy_listener);
    printf("客户端断开连接\n");
    wl_list_remove(&mcc->link);
    wl_list_remove(&mcc->destroy_listener.link);
    free(mcc);
}

static void client_created(struct wl_listener *listener, void *data) {
    struct mc_compositor *c = wl_container_of(listener, c, client_created);
    struct wl_client *wl_client = data;

    struct mc_client *mcc = calloc(1, sizeof(*mcc));
    if (!mcc)
        return;
    mcc->compositor = c;
    mcc->wl_client = wl_client;
    mcc->destroy_listener.notify = client_destroy;
    wl_client_add_destroy_listener(wl_client, &mcc->destroy_listener);
    wl_list_insert(&c->clients, &mcc->link);
    printf("新客户端连接\n");
}

/* ------------------------------------------------------------------ */
/* libinput integration                                              */
/* ------------------------------------------------------------------ */

static int open_restricted(const char *path, int flags, void *user_data) {
    (void)user_data;
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *user_data) {
    (void)user_data;
    close(fd);
}

static const struct libinput_interface libinput_iface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

static void handle_keyboard(struct mc_compositor *c,
                            struct libinput_event *ev) {
    struct libinput_event_keyboard *kev =
        libinput_event_get_keyboard_event(ev);
    uint32_t key = libinput_event_keyboard_get_key(kev);
    enum libinput_key_state state =
        libinput_event_keyboard_get_key_state(kev);

    /* 用静态变量跟踪修饰键的按下状态 */
    static bool ctrl = false;
    static bool alt = false;

    if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL)
        ctrl = (state == LIBINPUT_KEY_STATE_PRESSED);
    if (key == KEY_LEFTALT || key == KEY_RIGHTALT)
        alt = (state == LIBINPUT_KEY_STATE_PRESSED);

    if (state != LIBINPUT_KEY_STATE_PRESSED)
        return;

    printf("按键: code=%u  ctrl=%d alt=%d\n", key, ctrl, alt);

    /* 退出组合键 */
    if (key == KEY_ESC || (ctrl && alt && key == KEY_BACKSPACE)) {
        printf("收到退出组合键，正在退出...\n");
        wl_display_terminate(c->display);
    }
}

static void handle_pointer_motion(struct mc_compositor *c,
                                  struct libinput_event *ev) {
    (void)c;
    struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
    double dx = libinput_event_pointer_get_dx(pev);
    double dy = libinput_event_pointer_get_dy(pev);
    printf("指针移动: dx=%.1f dy=%.1f\n", dx, dy);
}

static void handle_pointer_button(struct mc_compositor *c,
                                  struct libinput_event *ev) {
    (void)c;
    struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
    uint32_t button = libinput_event_pointer_get_button(pev);
    enum libinput_button_state state =
        libinput_event_pointer_get_button_state(pev);
    printf("指针按键: button=%u state=%s\n", button,
           state == LIBINPUT_BUTTON_STATE_PRESSED ? "按下" : "释放");
}

static int on_input_data(int fd, uint32_t mask, void *data) {
    (void)fd;
    (void)mask;
    struct mc_compositor *c = data;
    struct libinput_event *ev;

    /* fd 可读 != 事件就绪: 必须先 dispatch 让 libinput 把原始 evdev
     * 数据解析进事件队列, 之后 get_event 才取得到东西 */
    if (libinput_dispatch(c->libinput) != 0) {
        perror("libinput_dispatch");
        return 0;
    }

    while ((ev = libinput_get_event(c->libinput))) {
        switch (libinput_event_get_type(ev)) {
        case LIBINPUT_EVENT_KEYBOARD_KEY:
            handle_keyboard(c, ev);
            break;
        case LIBINPUT_EVENT_POINTER_MOTION:
            handle_pointer_motion(c, ev);
            break;
        case LIBINPUT_EVENT_POINTER_BUTTON:
            handle_pointer_button(c, ev);
            break;
        default:
            break;
        }
        libinput_event_destroy(ev);
    }

    return 0;
}

static int init_libinput(struct mc_compositor *c) {
    c->udev = udev_new();
    if (!c->udev) {
        fprintf(stderr, "udev_new 失败\n");
        return -1;
    }

    c->libinput =
        libinput_udev_create_context(&libinput_iface, c, c->udev);
    if (!c->libinput) {
        fprintf(stderr, "libinput_udev_create_context 失败\n");
        udev_unref(c->udev);
        c->udev = NULL;
        return -1;
    }

    /* "seat0" 是默认 seat；纯 TTY 下通常就是它 */
    if (libinput_udev_assign_seat(c->libinput, "seat0") != 0) {
        fprintf(stderr, "libinput_udev_assign_seat 失败\n");
        libinput_unref(c->libinput);
        c->libinput = NULL;
        udev_unref(c->udev);
        c->udev = NULL;
        return -1;
    }

    return 0;
}

static void fini_libinput(struct mc_compositor *c) {
    if (c->input_source)
        wl_event_source_remove(c->input_source);
    if (c->libinput)
        libinput_unref(c->libinput);
    if (c->udev)
        udev_unref(c->udev);
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                           */
/* ------------------------------------------------------------------ */

static void cleanup(struct mc_compositor *c) {
    fini_libinput(c);

    if (c->display) {
        wl_display_destroy_clients(c->display);
        wl_display_destroy(c->display);
        c->display = NULL;
    }

    fini_drm(c);
}

/* ------------------------------------------------------------------ */
/* main                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
    struct mc_compositor compositor = {0};
    const char *socket;

    /* sudo/SSH 环境会清空 XDG_RUNTIME_DIR, 导致 socket 创建失败。
     * 回退到共享目录 /tmp/wayland-runtime, 客户端可用
     * XDG_RUNTIME_DIR=/tmp/wayland-runtime WAYLAND_DISPLAY=wayland-N 连接。 */
    if (!getenv("XDG_RUNTIME_DIR")) {
        mkdir("/tmp/wayland-runtime", 0755);
        setenv("XDG_RUNTIME_DIR", "/tmp/wayland-runtime", 1);
    }

    compositor.drm_fd = -1;
    wl_list_init(&compositor.clients);
    wl_list_init(&compositor.surfaces);

    printf("=== 模块 11: 最小化 TTY Wayland Compositor ===\n");
    printf("从 TTY 运行 (Ctrl+Alt+F3 登录): sudo ./compositor\n\n");

    /* 1. DRM/KMS */
    if (init_drm(&compositor) < 0) {
        fprintf(stderr, "DRM 初始化失败\n");
        cleanup(&compositor);
        return 1;
    }

    /* 2. libinput */
    if (init_libinput(&compositor) < 0) {
        fprintf(stderr, "libinput 初始化失败 (输入将不可用)\n");
        /* 输入失败不致命，可以继续显示 */
    }

    /* 3. wl_display + socket */
    compositor.display = wl_display_create();
    if (!compositor.display) {
        fprintf(stderr, "wl_display_create 失败\n");
        cleanup(&compositor);
        return 1;
    }
    compositor.loop = wl_display_get_event_loop(compositor.display);

    /* 跟踪客户端连接 / 断开 */
    compositor.client_created.notify = client_created;
    wl_display_add_client_created_listener(compositor.display,
                                           &compositor.client_created);

    /* 自动挑一个没被占用的名字 (wayland-1, wayland-2, ...), socket 创建在
     * $XDG_RUNTIME_DIR 下; 返回值就是客户端要设置的 WAYLAND_DISPLAY */
    socket = wl_display_add_socket_auto(compositor.display);
    if (!socket) {
        fprintf(stderr, "wl_display_add_socket_auto 失败\n");
        cleanup(&compositor);
        return 1;
    }

    /* root 运行 + 普通用户客户端连接时, socket 默认 0755 会让客户端因无写权限
     * connect 失败 (EACCES)。教学场景放开连接权限;
     * 生产 compositor 走 logind 授权 + 同用户运行, 不存在此问题。 */
    {
        char sock_path[256];
        snprintf(sock_path, sizeof(sock_path), "%s/%s",
                 getenv("XDG_RUNTIME_DIR"), socket);
        if (chmod(sock_path, 0777) != 0)
            fprintf(stderr, "警告: chmod %s 失败 (%s)\n", sock_path,
                    strerror(errno));
    }

    /* 4. 注册全局: wl_compositor */
    if (!wl_global_create(compositor.display, &wl_compositor_interface, 4,
                          &compositor, compositor_bind)) {
        fprintf(stderr, "无法创建 wl_compositor 全局\n");
        cleanup(&compositor);
        return 1;
    }

    /* wl_shm: 直接用 libwayland-server 内建实现，自动注册 wl_shm 全局
     * 并提供 wl_shm_buffer_* 访问接口。 */
    if (wl_display_init_shm(compositor.display) != 0) {
        fprintf(stderr, "wl_display_init_shm 失败\n");
        cleanup(&compositor);
        return 1;
    }

    /* 5. 把 libinput 的 fd 加入事件循环: 单线程事件循环是合成器的标准
     * 形态 —— socket 上客户端的请求和 fd 上的输入事件都在 wl_display_run
     * 的同一轮询里分发, 因此各处理函数之间不需要锁。 */
    if (compositor.libinput) {
        compositor.input_source = wl_event_loop_add_fd(
            compositor.loop, libinput_get_fd(compositor.libinput),
            WL_EVENT_READABLE, on_input_data, &compositor);
        if (!compositor.input_source)
            fprintf(stderr, "无法把 libinput fd 加入事件循环\n");
    }

    printf("\nCompositor 已启动。\n");
    printf("  WAYLAND_DISPLAY=%s\n", socket);
    printf("  连接客户端示例: WAYLAND_DISPLAY=%s weston-simple-shm\n", socket);
    printf("  退出: Esc 或 Ctrl+Alt+Backspace\n\n");

    /* 首次清屏 */
    mc_repaint(&compositor);

    /* 6. 主循环 */
    wl_display_run(compositor.display);

    printf("\n正在清理并恢复 CRTC...\n");
    cleanup(&compositor);
    return 0;
}
