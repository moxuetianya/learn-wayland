#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

struct client_state {
    struct wl_display      *display;
    struct wl_registry     *registry;
    struct wl_compositor   *compositor;
    struct wl_shm          *shm;
    struct xdg_wm_base     *wm_base;

    struct wl_surface      *surface;
    struct xdg_surface     *xdg_surface;
    struct xdg_toplevel    *xdg_toplevel;
    /* 当前在途的帧回调。wl_callback 是一次性的：done 一到协议对象即销毁，
     * 所以这里要跟着置 NULL——既防退出路径 double-free，也用来判断是否还有未决的问。 */
    struct wl_callback     *frame_cb;

    int width, height, running;
    int configured;

    /* animation */
    float ball_x, ball_y, ball_vx, ball_vy;
    float ball_r;

    struct timespec last_fps_time;
    int frame_count, fps;
};

static const struct wl_callback_listener frame_listener;

/* ── rendering ─────────────────────────────────────────── */

static void fill_pixels(struct client_state *st, uint32_t *pixels)
{
    int w = st->width, h = st->height;

    /* 全量重绘：每帧从零写满整块 buffer，因此提交时 damage 也报整个 buffer。
     * 生产客户端只重绘/报损变化的区域，合成器就能跳过干净区域的合成。 */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* gradient background */
            uint8_t r = (uint8_t)(30 + 60 * ((float)x / w));
            uint8_t g = (uint8_t)(30 + 80 * ((float)y / h));
            uint8_t b = (uint8_t)(60 + 120 * ((float)(x + y) / (w + h)));

            /* bouncing ball */
            float dx = x - st->ball_x;
            float dy = y - st->ball_y;
            if (dx * dx + dy * dy < st->ball_r * st->ball_r) {
                r = 255; g = 220; b = 40;
            }

            pixels[y * w + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static struct wl_buffer *create_frame(struct client_state *st)
{
    /* stride = 每行字节数。它既决定我们怎么写 pixels[y*w+x]，
     * 也必须原样传给 wl_shm_pool_create_buffer——两边不一致图像就会斜着撕裂。 */
    int stride = st->width * 4;
    int size   = stride * st->height;

    char tmpfile[] = "/tmp/wayland-mod6-XXXXXX";
    /* 每帧新建 pool+buffer 是最省事的简化（正确但慢：一帧一套系统调用与协议对象）。
     * 生产做法：预分配 2-3 块 buffer 轮转，等 wl_buffer.release 事件后再复用。 */
    int fd = mkstemp(tmpfile);
    if (fd < 0) { perror("mkstemp"); return NULL; }
    unlink(tmpfile);

    if (ftruncate(fd, size) < 0) { perror("ftruncate"); close(fd); return NULL; }

    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) { perror("mmap"); close(fd); return NULL; }

    fill_pixels(st, pixels);
    munmap(pixels, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, st->width, st->height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ── animation ─────────────────────────────────────────── */

static void update_ball(struct client_state *st)
{
    /* gravity + constant horizontal velocity
     * 注意：重力是"每帧 +0.6"，隐含 1 帧 = 1/60 秒的假设——frame callback 恰好
     * 让帧率锁在刷新率，60Hz 屏上成立；换 144Hz 屏球速就快 2.4 倍。
     * 正确做法是用 done 事件的 time 参数做真实 dt 积分（见模块练习）。 */
    st->ball_vy += 0.6f;

    st->ball_x += st->ball_vx;
    st->ball_y += st->ball_vy;

    /* bounce off walls */
    if (st->ball_x - st->ball_r < 0) {
        st->ball_x = st->ball_r;
        st->ball_vx = -st->ball_vx;
    }
    if (st->ball_x + st->ball_r > st->width) {
        st->ball_x = st->width - st->ball_r;
        st->ball_vx = -st->ball_vx;
    }
    if (st->ball_y - st->ball_r < 0) {
        st->ball_y = st->ball_r;
        st->ball_vy = -st->ball_vy;
    }
    if (st->ball_y + st->ball_r > st->height) {
        st->ball_y = st->height - st->ball_r;
        /* lose a bit of energy on the floor */
        st->ball_vy = -st->ball_vy * 0.85f;
    }
}

static void update_fps(struct client_state *st)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed = (now.tv_sec  - st->last_fps_time.tv_sec) +
                     (now.tv_nsec - st->last_fps_time.tv_nsec) / 1e9;

    if (elapsed >= 1.0) {
        st->fps = (int)(st->frame_count / elapsed);
        printf("FPS: %d\n", st->fps);
        fflush(stdout);
        st->frame_count = 0;
        st->last_fps_time = now;
    }
}

/* ── frame callback ────────────────────────────────────── */

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)time;
    struct client_state *st = data;
    /* wl_callback 是一次性的：done 之后协议对象即销毁。立刻销毁代理并清空指针，
     * 然后每帧重新 wl_surface_frame()——"问一次→画一帧→再问一次"的环由此而来。 */
    wl_callback_destroy(cb);
    st->frame_cb = NULL;

    /* Render next frame */
    struct wl_buffer *buf = create_frame(st);
    if (buf) {
        wl_surface_attach(st->surface, buf, 0, 0);
        wl_surface_damage_buffer(st->surface, 0, 0, st->width, st->height);

        /* Request next frame callback BEFORE commit so it is queued
            together with this surface state.
            顺序是本模块的灵魂：callback 必须与"这份"提交的 surface 状态绑定，
            done 才对准"这一帧被合成器消费"的时刻。若放到 commit 之后，
            合成器可能立即回 done——退化成真正的 busy-loop（练习：亲手踩一次）。 */
        st->frame_cb = wl_surface_frame(st->surface);
        wl_callback_add_listener(st->frame_cb, &frame_listener, st);

        wl_surface_commit(st->surface);
        /* commit 后立刻销毁 buffer 是安全的简化：请求按序到达服务端，commit 先于
         * destroy 被处理；服务端 wl_shm 实现按未销毁 buffer 数持有 pool 引用，
         * 像素不会在读取中途失效。 */
        wl_buffer_destroy(buf);
    }

    /* Update animation & FPS */
    update_ball(st);
    st->frame_count++;
    update_fps(st);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done
};

/* ── callbacks ─────────────────────────────────────────── */

static void xdg_wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t s)
    { (void)d; xdg_wm_base_pong(b, s); }

static void xdg_surface_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    struct client_state *st = d;
    /* xdg-shell 硬规定：必须先 ack_configure，之后才允许 attach buffer。
     * 不 ack 就提交是协议错误，会被合成器直接断连。 */
    xdg_surface_ack_configure(s, serial);

    /* Start the render loop on the first configure.
     * 首个 configure 到达前 surface 还没完成角色分配，不能画；
     * 它一到就渲染首帧 + 请求第一个 frame callback，把整个循环点火。 */
    if (!st->configured) {
        st->configured = 1;

        struct wl_buffer *buf = create_frame(st);
        if (buf) {
            wl_surface_attach(st->surface, buf, 0, 0);
            wl_surface_damage_buffer(st->surface, 0, 0, st->width, st->height);

            st->frame_cb = wl_surface_frame(st->surface);
            wl_callback_add_listener(st->frame_cb, &frame_listener, st);

            wl_surface_commit(st->surface);
            wl_buffer_destroy(buf);
        }
        clock_gettime(CLOCK_MONOTONIC, &st->last_fps_time);
    }
}

static void xdg_toplevel_configure(void *d, struct xdg_toplevel *t,
                                    int32_t w, int32_t h, struct wl_array *arr)
{
    struct client_state *cs = d; (void)t; (void)arr;
    /* w/h 为 0 表示"由客户端自己决定尺寸"（初始 configure 常见），所以只在 >0 时更新；
     * 之后尺寸真正生效要等下一个 xdg_surface.configure（在那里按新尺寸重绘）。 */
    if (w > 0) cs->width = w;
    if (h > 0) cs->height = h;
}

static void xdg_toplevel_close(void *d, struct xdg_toplevel *t)
    { (void)t; ((struct client_state *)d)->running = 0; }

static void registry_global(void *d, struct wl_registry *r,
                             uint32_t n, const char *iface, uint32_t ver)
{
    (void)ver;
    struct client_state *cs = d;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        cs->compositor = wl_registry_bind(r, n, &wl_compositor_interface, 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        cs->shm = wl_registry_bind(r, n, &wl_shm_interface, 1);
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
        cs->wm_base = wl_registry_bind(r, n, &xdg_wm_base_interface, 2);
}

static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n)
    { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg_listener = {
    .global = registry_global, .global_remove = registry_global_remove
};

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping
};

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure
};

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure, .close = xdg_toplevel_close
};

int main(void)
{
    struct client_state st = {
        .width = 640, .height = 480, .running = 1,
        .ball_x = 100, .ball_y = 100,
        .ball_vx = 5.0f, .ball_vy = 0.0f,
        .ball_r = 30.0f,
    };

    printf("=== 模块 6: 帧回调 (wl_callback / frame callback) ===\n");
    printf("理解: wl_surface_frame() 请求一个 wl_callback,\n");
    printf("      compositor 在准备好绘制下一帧时发送 done 事件.\n");
    printf("      在 done 中渲染并再次请求回调 -> 与刷新率同步, 无撕裂.\n\n");

    st.display = wl_display_connect(NULL);
    if (!st.display) { fprintf(stderr, "no display\n"); return 1; }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &reg_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.compositor || !st.shm || !st.wm_base) {
        fprintf(stderr, "missing globals\n"); return 1;
    }

    xdg_wm_base_add_listener(st.wm_base, &wm_base_listener, &st);
    st.surface = wl_compositor_create_surface(st.compositor);
    st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    xdg_surface_add_listener(st.xdg_surface, &xdg_surface_listener, &st);
    st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
    xdg_toplevel_add_listener(st.xdg_toplevel, &xdg_toplevel_listener, &st);
    xdg_toplevel_set_title(st.xdg_toplevel, "模块6 - 帧回调动画");

    /* 无 buffer 的空 commit：只为宣告 surface 角色已就绪，骗出第一个 configure。
     * 此时尚未 ack，attach buffer 是协议错误，所以只能"空手"提交。 */
    wl_surface_commit(st.surface);

    printf("帧回调驱动的动画循环开始 (弹跳球 + FPS 计数)...\n");
    /* 整个动画没有一处 sleep：dispatch 无事件时阻塞，
     * 节奏完全由合成器的 done 事件驱动（= 合成率 ≈ 屏幕刷新率）。 */
    while (st.running && wl_display_dispatch(st.display) != -1)
        ;

    if (st.frame_cb)     wl_callback_destroy(st.frame_cb);
    xdg_toplevel_destroy(st.xdg_toplevel);
    xdg_surface_destroy(st.xdg_surface);
    wl_surface_destroy(st.surface);
    xdg_wm_base_destroy(st.wm_base);
    wl_compositor_destroy(st.compositor);
    wl_shm_destroy(st.shm);
    wl_registry_destroy(st.registry);
    wl_display_disconnect(st.display);
    printf("\n=== 模块 6 完成 ===\n");
    return 0;
}
