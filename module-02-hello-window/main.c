#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

    struct wl_buffer       *buffer;
    int                     width, height;
    int                     running;
};

static struct wl_buffer *
create_buffer(struct client_state *st, int width, int height)
{
    int stride = width * 4;
    int size   = stride * height;

    int fd = -1;
    for (int i = 0; i < 8; i++) {
        char name[] = "/wayland-module2-XXXXXX";
        fd = mkstemp(name);
        if (fd >= 0) {
            unlink(name);
            break;
        }
    }
    if (fd < 0) {
        fprintf(stderr, "mkstemp failed: %s\n", strerror(errno));
        return NULL;
    }

    if (ftruncate(fd, size) < 0) {
        fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    uint32_t *pixels = (uint32_t *)data;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float fy = (float)y / height;
            float fx = (float)x / width;

            uint8_t r = (uint8_t)(255 * fy);
            uint8_t g = (uint8_t)(255 * (1.0f - fy));
            uint8_t b = (uint8_t)(255 * fx);

            pixels[y * width + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
    munmap(data, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

/* ── xdg_wm_base 事件 ───────────────────────────────────── */

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

/* ── xdg_surface 事件 ──────────────────────────────────── */

static void
xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct client_state *st = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    if (st->buffer == NULL) {
        st->buffer = create_buffer(st, st->width, st->height);
    }
    wl_surface_attach(st->surface, st->buffer, 0, 0);
    wl_surface_commit(st->surface);
}

/* ── xdg_toplevel 事件 ─────────────────────────────────── */

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                       int32_t width, int32_t height, struct wl_array *states)
{
    struct client_state *st = data;
    (void)toplevel;
    (void)states;
    if (width > 0) st->width = width;
    if (height > 0) st->height = height;
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    struct client_state *st = data;
    st->running = 0;
}

/* ── registry 事件 ─────────────────────────────────────── */

static void
registry_global(void *data, struct wl_registry *registry,
                uint32_t name, const char *interface, uint32_t version)
{
    struct client_state *st = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        st->compositor = wl_registry_bind(registry, name,
                                          &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        st->shm = wl_registry_bind(registry, name,
                                   &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        st->wm_base = wl_registry_bind(registry, name,
                                       &xdg_wm_base_interface, 2);
    }
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

/* ── listeners ─────────────────────────────────────────── */

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close     = xdg_toplevel_close,
};

/* ── main ──────────────────────────────────────────────── */

int main(void)
{
    struct client_state st = {0};
    st.width  = 640;
    st.height = 480;
    st.running = 1;

    printf("=== Wayland 模块 2: Hello Wayland 窗口 ===\n");
    printf("创建一个 640x480 的渐变窗口 (xdg-shell)\n\n");

    st.display = wl_display_connect(NULL);
    if (!st.display) {
        fprintf(stderr, "无法连接 Wayland compositor\n");
        return EXIT_FAILURE;
    }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &registry_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.compositor || !st.shm || !st.wm_base) {
        fprintf(stderr, "缺少必要的全局接口 (compositor=%p, shm=%p, wm_base=%p)\n",
                (void*)st.compositor, (void*)st.shm, (void*)st.wm_base);
        return EXIT_FAILURE;
    }

    xdg_wm_base_add_listener(st.wm_base, &xdg_wm_base_listener, &st);

    st.surface = wl_compositor_create_surface(st.compositor);
    st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    xdg_surface_add_listener(st.xdg_surface, &xdg_surface_listener, &st);
    st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
    xdg_toplevel_add_listener(st.xdg_toplevel, &xdg_toplevel_listener, &st);
    xdg_toplevel_set_title(st.xdg_toplevel, "模块2 - Hello Wayland");

    wl_surface_commit(st.surface);
    wl_display_roundtrip(st.display);

    printf("窗口已创建. 运行事件循环...\n");
    printf("理解: attach() 附加像素缓冲区, commit() 原子提交给 compositor.\n");
    printf("Compositor 收到 commit 后在下一刷新周期显示.\n");
    printf("关闭窗口或按 Ctrl+C 退出.\n\n");

    while (st.running && wl_display_dispatch(st.display) != -1)
        ;

    printf("\n清理资源...\n");
    if (st.buffer) wl_buffer_destroy(st.buffer);
    xdg_toplevel_destroy(st.xdg_toplevel);
    xdg_surface_destroy(st.xdg_surface);
    wl_surface_destroy(st.surface);
    xdg_wm_base_destroy(st.wm_base);
    wl_compositor_destroy(st.compositor);
    wl_shm_destroy(st.shm);
    wl_registry_destroy(st.registry);
    wl_display_disconnect(st.display);

    printf("=== 模块 2 完成 ===\n");
    return EXIT_SUCCESS;
}
