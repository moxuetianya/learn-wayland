/* client.c - 模块 11 配套测试客户端: 只依赖 wl_compositor + wl_shm + wl_surface
 *
 * 为什么需要它: weston >= 6 的 weston-simple-shm 在 create_window 里要求
 * 服务端提供 xdg_wm_base 或 zwp_fullscreen_shell_v1 之一, 二者皆无时直接
 * assert(0) 崩溃 (Ubuntu 22.04 的 weston 9.0 复现于 clients/simple-shm.c:201);
 * 课程模块 02~07 的客户端也都走 xdg-shell (模块 12 才实现)。所以模块 11
 * 自带一个"纯 wl_surface"客户端, 协议闭环是:
 *
 *   connect → registry 枚举 (bind wl_compositor / wl_shm)
 *   → create_surface
 *   → 双 wl_shm buffer, wl_buffer.release 事件驱动帧循环
 *   → attach → commit → frame callback done → 画下一帧
 *
 * 双缓冲 + busy 标志验证了合成器的 release 语义: 服务端若忘了发
 * wl_buffer.release, 这里第二帧后就会报 "两个 buffer 都 busy" 退出。
 *
 * 用法 (合成器已在另一个终端/TTY 运行):
 *
 *   ./client            # 一直运行, Ctrl+C 退出
 *   ./client 5          # 只跑 5 秒
 *
 * sudo 跑的合成器若回退到了 /tmp/wayland-runtime, 客户端要显式指定:
 *
 *   XDG_RUNTIME_DIR=/tmp/wayland-runtime WAYLAND_DISPLAY=wayland-0 ./client
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#define WIDTH 256
#define HEIGHT 256

struct buffer {
    struct wl_buffer *wl_buffer;
    uint32_t *pixels;
    size_t size;
    bool busy; /* commit 后所有权交给服务端, release 事件归还 */
};

struct client {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_surface *surface;
    bool has_xrgb;

    struct buffer buffers[2];
    struct wl_callback *frame_cb;

    bool running;
    uint32_t deadline_ms; /* 0 = 永不自动退出 */
    uint64_t frames;
    uint32_t last_fps_ms;
    uint64_t last_fps_count;
};

static const struct wl_callback_listener frame_listener;

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* 三角波: t 在 [lo, hi] 间往返, 周期 2*period 毫秒 (免 libm 的"弹跳") */
static int bounce(int t, int lo, int hi, int period) {
    int range = hi - lo;
    int phase = t % (2 * period);
    if (phase >= period)
        phase = 2 * period - phase;
    return lo + phase * range / period;
}

/* ------------------------------------------------------------------ */
/* 绘制                                                                */
/* ------------------------------------------------------------------ */

static void paint(struct buffer *b, uint32_t t) {
    uint32_t *px = b->pixels;

    /* 流动的对角渐变背景 */
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            uint32_t r = ((uint32_t)(x * 255) / WIDTH + t / 8) & 0xff;
            uint32_t g = ((uint32_t)(y * 255) / HEIGHT + t / 16) & 0xff;
            px[y * WIDTH + x] = 0xFF000000u | (r << 16) | (g << 8) | (255 - r);
        }
    }

    /* 弹跳的白色小球 */
    const int radius = 24;
    int cx = bounce(t, radius, WIDTH - 1 - radius, 1700);
    int cy = bounce(t, radius, HEIGHT - 1 - radius, 2300);
    for (int y = cy - radius; y <= cy + radius; y++) {
        if (y < 0 || y >= HEIGHT)
            continue;
        for (int x = cx - radius; x <= cx + radius; x++) {
            if (x < 0 || x >= WIDTH)
                continue;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= radius * radius)
                px[y * WIDTH + x] = 0xFFFFFFFFu;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 帧循环                                                              */
/* ------------------------------------------------------------------ */

static void redraw(struct client *c, uint32_t t) {
    /* 双缓冲: 只拿"服务端已 release 归还"的那块。若两块都忙, 说明服务端
     * 没实现 release 语义 —— 客户端双缓冲池"饿死"的经典症状。 */
    struct buffer *b = NULL;
    if (!c->buffers[0].busy)
        b = &c->buffers[0];
    else if (!c->buffers[1].busy)
        b = &c->buffers[1];
    if (!b) {
        fprintf(stderr, "两个 buffer 都 busy: 服务端从未发 wl_buffer.release?\n");
        c->running = false;
        return;
    }

    paint(b, t);

    /* frame callback 是"下一帧什么时候画"的节流问答, 每帧重新请求 */
    c->frame_cb = wl_surface_frame(c->surface);
    wl_callback_add_listener(c->frame_cb, &frame_listener, c);

    wl_surface_attach(c->surface, b->wl_buffer, 0, 0);
    wl_surface_damage(c->surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(c->surface);
    b->busy = true;

    c->frames++;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    struct client *c = data;
    (void)time;
    wl_callback_destroy(cb);
    c->frame_cb = NULL;

    uint32_t t = now_ms();
    if (c->deadline_ms && t >= c->deadline_ms) {
        c->running = false;
        return;
    }
    redraw(c, t);

    /* 每秒报告一次 fps。本模块的合成器 commit 后立刻回 frame done,
     * 客户端会远超屏幕刷新率 —— README 练习 5 探讨的"不健康"现象。 */
    if (t - c->last_fps_ms >= 1000) {
        uint32_t dt = t - c->last_fps_ms;
        fprintf(stderr, "fps: %llu\n",
                (unsigned long long)(c->frames - c->last_fps_count) * 1000 / dt);
        c->last_fps_ms = t;
        c->last_fps_count = c->frames;
    }
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

/* ------------------------------------------------------------------ */
/* buffer                                                              */
/* ------------------------------------------------------------------ */

static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
    struct buffer *b = data;
    (void)wl_buffer;
    b->busy = false; /* 服务端用完归还, 客户端可以放心重写 */
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int create_shm_buffer(struct client *c, struct buffer *b) {
    int stride = WIDTH * 4;
    int size = stride * HEIGHT;

    /* mkstemp + unlink = 匿名文件: 名字即刻消失, fd 用完即弃 (模块 2 同款) */
    int fd = -1;
    for (int i = 0; i < 8; i++) {
        char name[] = "/tmp/m11-client-XXXXXX";
        fd = mkstemp(name);
        if (fd >= 0) {
            unlink(name);
            break;
        }
    }
    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }
    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }

    b->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (b->pixels == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }
    b->size = size;

    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, size);
    close(fd);
    if (!pool)
        return -1;
    b->wl_buffer = wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, stride,
                                             WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    if (!b->wl_buffer)
        return -1;

    wl_buffer_add_listener(b->wl_buffer, &buffer_listener, b);
    return 0;
}

/* ------------------------------------------------------------------ */
/* registry                                                            */
/* ------------------------------------------------------------------ */

static void shm_format(void *data, struct wl_shm *shm, uint32_t format) {
    struct client *c = data;
    (void)shm;
    if (format == WL_SHM_FORMAT_XRGB8888)
        c->has_xrgb = true;
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_format,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    struct client *c = data;
    (void)version; /* v1 足够: attach/damage/frame/commit 都是最早的请求 */
    if (strcmp(interface, "wl_compositor") == 0) {
        c->compositor = wl_registry_bind(registry, name,
                                         &wl_compositor_interface, 1);
    } else if (strcmp(interface, "wl_shm") == 0) {
        c->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(c->shm, &shm_listener, c);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static bool g_running = true;

static void on_sigint(int sig) {
    (void)sig;
    g_running = false;
}

int main(int argc, char **argv) {
    struct client c = {0};
    uint32_t seconds = 0; /* 0 = 一直运行 */
    if (argc > 1)
        seconds = (uint32_t)atoi(argv[1]);

    if (!getenv("XDG_RUNTIME_DIR")) {
        fprintf(stderr,
                "XDG_RUNTIME_DIR 未设置。sudo 运行的合成器通常回退到 "
                "/tmp/wayland-runtime, 试:\n"
                "  XDG_RUNTIME_DIR=/tmp/wayland-runtime WAYLAND_DISPLAY=wayland-0"
                " %s\n",
                argv[0]);
        return 1;
    }

    c.display = wl_display_connect(NULL);
    if (!c.display) {
        fprintf(stderr,
                "无法连接 Wayland 服务端 (WAYLAND_DISPLAY=%s, "
                "XDG_RUNTIME_DIR=%s): %s\n"
                "提示: 名称/目录要与合成器启动时打印的一致; 连到残留的旧 "
                "socket 会报 Connection refused\n",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY")
                                          : "(未设置, 默认 wayland-0)",
                getenv("XDG_RUNTIME_DIR"), strerror(errno));
        return 1;
    }

    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    wl_display_roundtrip(c.display); /* 收 global 列表 */
    wl_display_roundtrip(c.display); /* 收 wl_shm.format 广播 */

    if (!c.compositor || !c.shm) {
        fprintf(stderr, "服务端缺少 wl_compositor / wl_shm 全局\n");
        return 1;
    }
    if (!c.has_xrgb) {
        fprintf(stderr, "服务端 wl_shm 不支持 XRGB8888\n");
        return 1;
    }

    c.surface = wl_compositor_create_surface(c.compositor);
    if (!c.surface) {
        fprintf(stderr, "create_surface 失败\n");
        return 1;
    }

    if (create_shm_buffer(&c, &c.buffers[0]) < 0 ||
        create_shm_buffer(&c, &c.buffers[1]) < 0) {
        fprintf(stderr, "创建 shm buffer 失败\n");
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    if (seconds)
        c.deadline_ms = now_ms() + seconds * 1000;

    printf("已连接: %dx%d 双缓冲, ", WIDTH, HEIGHT);
    if (seconds)
        printf("运行 %u 秒\n", seconds);
    else
        printf("Ctrl+C 退出\n");

    c.running = true;
    c.last_fps_ms = now_ms();
    redraw(&c, now_ms());

    while (c.running && g_running) {
        if (wl_display_dispatch(c.display) < 0)
            break;
    }

    printf("\n退出, 共提交 %llu 帧\n", (unsigned long long)c.frames);

    if (c.frame_cb)
        wl_callback_destroy(c.frame_cb);
    for (int i = 0; i < 2; i++) {
        if (c.buffers[i].wl_buffer)
            wl_buffer_destroy(c.buffers[i].wl_buffer);
        if (c.buffers[i].pixels)
            munmap(c.buffers[i].pixels, c.buffers[i].size);
    }
    wl_surface_destroy(c.surface);
    wl_display_disconnect(c.display);
    return 0;
}
