#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
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

    int width, height;
    int running;
    int frame_count;
};

static void fill_pixels(uint32_t *pixels, int w, int h, int frame)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float u = (float)x / w;
            float v = (float)y / h;
            float t = (float)frame * 0.02f;

            /* animated gradient background */
            uint8_t r = (uint8_t)(127 + 127 * sinf(u * 6.28f + t));
            uint8_t g = (uint8_t)(127 + 127 * sinf(v * 6.28f + t * 1.3f));
            uint8_t b = (uint8_t)(127 + 127 * sinf((u + v) * 3.14f + t * 0.7f));

            /* checkerboard overlay */
            int cx = (x / 32) ^ (y / 32);
            if (cx) {
                r = (uint8_t)(r * 0.7f);
                g = (uint8_t)(g * 0.7f);
                b = (uint8_t)(b * 0.7f);
            }

            /* central circle */
            float dx = u - 0.5f, dy = v - 0.5f;
            float dist = sqrtf(dx*dx + dy*dy);
            if (dist < 0.2f) {
                r = 255; g = 255; b = 255;
            }

            pixels[y * w + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static struct wl_buffer *
create_frame(struct client_state *st, int width, int height, int frame)
{
    int stride = width * 4;
    int size   = stride * height;

    char tmpfile[] = "/tmp/wayland-mod3-XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) { perror("mkstemp"); return NULL; }
    unlink(tmpfile);

    if (ftruncate(fd, size) < 0) { perror("ftruncate"); close(fd); return NULL; }

    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) { perror("mmap"); close(fd); return NULL; }

    fill_pixels(pixels, width, height, frame);
    munmap(pixels, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ── callbacks ─────────────────────────────────────────── */

static void xdg_wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t s)
    { (void)d; xdg_wm_base_pong(b, s); }

static void xdg_surface_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    (void)d; xdg_surface_ack_configure(s, serial);
}

static void xdg_toplevel_configure(void *d, struct xdg_toplevel *t,
                                    int32_t w, int32_t h, struct wl_array *st)
{
    struct client_state *cs = d; (void)t; (void)st;
    if (w > 0) cs->width = w;
    if (h > 0) cs->height = h;
}

static void xdg_toplevel_close(void *d, struct xdg_toplevel *t)
    { (void)t; ((struct client_state *)d)->running = 0; }

static void registry_global(void *d, struct wl_registry *r,
                             uint32_t n, const char *iface, uint32_t ver)
{
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
    struct client_state st = { .width=640, .height=480, .running=1 };

    printf("=== 模块 3: 手动像素绘制 ===\n");
    printf("理解: WL_SHM_FORMAT_XRGB8888 = 每像素 4 字节 (B,G,R,A)\n");
    printf("      像素数据通过共享内存 (wl_shm_pool) 传递给 compositor.\n\n");

    st.display = wl_display_connect(NULL);
    if (!st.display) { fprintf(stderr,"no display\n"); return 1; }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &reg_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.compositor || !st.shm || !st.wm_base) {
        fprintf(stderr,"missing globals\n"); return 1;
    }

    xdg_wm_base_add_listener(st.wm_base, &wm_base_listener, &st);
    st.surface = wl_compositor_create_surface(st.compositor);
    st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    xdg_surface_add_listener(st.xdg_surface, &xdg_surface_listener, &st);
    st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
    xdg_toplevel_add_listener(st.xdg_toplevel, &xdg_toplevel_listener, &st);
    xdg_toplevel_set_title(st.xdg_toplevel, "模块3 - 像素动画");

    wl_surface_commit(st.surface);
    wl_display_roundtrip(st.display);

    printf("动画循环开始 (每帧使用新 buffer, 演示 Attach-Commit 模型)...\n");
    while (st.running && wl_display_dispatch(st.display) != -1) {
        struct wl_buffer *buf = create_frame(&st, st.width, st.height, st.frame_count);
        if (buf) {
            wl_surface_attach(st.surface, buf, 0, 0);
            wl_surface_damage_buffer(st.surface, 0, 0, st.width, st.height);
            wl_surface_commit(st.surface);
            wl_buffer_destroy(buf);
        }
        usleep(16000);
        st.frame_count++;
    }

    xdg_toplevel_destroy(st.xdg_toplevel);
    xdg_surface_destroy(st.xdg_surface);
    wl_surface_destroy(st.surface);
    xdg_wm_base_destroy(st.wm_base);
    wl_compositor_destroy(st.compositor);
    wl_shm_destroy(st.shm);
    wl_registry_destroy(st.registry);
    wl_display_disconnect(st.display);
    printf("\n=== 模块 3 完成 ===\n");
    return 0;
}
