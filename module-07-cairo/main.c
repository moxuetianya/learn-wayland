#include <cairo.h>
#include <errno.h>
#include <math.h>
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

    int width, height;
    int running;
};

/* ── cairo helpers ─────────────────────────────────────── */

static void draw_rounded_rect(cairo_t *cr,
                              double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* ── rendering ─────────────────────────────────────────── */

static void render_frame(struct client_state *st, unsigned char *pixels) {
    int w = st->width, h = st->height;
    int stride = w * 4;

    cairo_surface_t *csurf = cairo_image_surface_create_for_data(
        pixels, CAIRO_FORMAT_ARGB32, w, h, stride);
    cairo_t *cr = cairo_create(csurf);

    /* gradient background */
    cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, w, h);
    cairo_pattern_add_color_stop_rgb(pat, 0, 0.1, 0.2, 0.4);
    cairo_pattern_add_color_stop_rgb(pat, 1, 0.2, 0.05, 0.15);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);

    /* title text */
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 36);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, 50, 80);
    cairo_show_text(cr, "Cairo on Wayland");

    /* subtitle text */
    cairo_set_font_size(cr, 16);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
    cairo_move_to(cr, 50, 110);
    cairo_show_text(cr, "Anti-aliased shapes & gradients");

    /* rounded rectangle button */
    cairo_set_source_rgb(cr, 0.3, 0.6, 1.0);
    draw_rounded_rect(cr, 200, 150, 240, 60, 12);
    cairo_fill(cr);
    cairo_set_font_size(cr, 20);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, 260, 189);
    cairo_show_text(cr, "Click Me");

    /* circle */
    cairo_set_source_rgb(cr, 1.0, 0.4, 0.2);
    cairo_arc(cr, 120, 350, 50, 0, 2 * M_PI);
    cairo_fill(cr);

    /* bezier curve */
    cairo_set_source_rgb(cr, 0.2, 1.0, 0.4);
    cairo_set_line_width(cr, 3);
    cairo_move_to(cr, 300, 300);
    cairo_curve_to(cr, 400, 250, 500, 400, 550, 350);
    cairo_stroke(cr);

    /* pie chart sector */
    cairo_set_source_rgb(cr, 0.9, 0.3, 0.6);
    cairo_move_to(cr, 500, 400);
    cairo_arc(cr, 500, 400, 60, -M_PI / 4, M_PI / 3);
    cairo_close_path(cr);
    cairo_fill(cr);

    /* pie chart sector 2 */
    cairo_set_source_rgb(cr, 0.3, 0.9, 0.6);
    cairo_arc(cr, 500, 400, 60, M_PI / 3, M_PI);
    cairo_close_path(cr);
    cairo_fill(cr);

    /* pie chart sector 3 */
    cairo_set_source_rgb(cr, 0.9, 0.8, 0.2);
    cairo_arc(cr, 500, 400, 60, M_PI, -M_PI / 4);
    cairo_close_path(cr);
    cairo_fill(cr);

    /* pie chart centre dot */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.3);
    cairo_arc(cr, 500, 400, 12, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(csurf);
}

/* ── buffer creation ───────────────────────────────────── */

static struct wl_buffer *create_buffer(struct client_state *st)
{
    int stride = st->width * 4;
    int size   = stride * st->height;

    char tmpfile[] = "/tmp/wayland-cairo-XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) {
        perror("mkstemp");
        return NULL;
    }
    unlink(tmpfile);

    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }

    unsigned char *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return NULL;
    }

    render_frame(st, pixels);
    munmap(pixels, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, st->width, st->height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ── callbacks ─────────────────────────────────────────── */

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *base,
                             uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static void xdg_surface_configure(void *data, struct xdg_surface *surf,
                                  uint32_t serial)
{
    struct client_state *st = data;
    xdg_surface_ack_configure(surf, serial);

    struct wl_buffer *buf = create_buffer(st);
    if (buf) {
        wl_surface_attach(st->surface, buf, 0, 0);
        wl_surface_damage_buffer(st->surface, 0, 0,
                                 st->width, st->height);
        wl_surface_commit(st->surface);
        wl_buffer_destroy(buf);
    }
}

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *tl,
                                   int32_t w, int32_t h,
                                   struct wl_array *states)
{
    struct client_state *st = data;
    (void)tl;
    (void)states;
    if (w > 0) st->width = w;
    if (h > 0) st->height = h;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *tl)
{
    (void)tl;
    ((struct client_state *)data)->running = 0;
}

static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    (void)version;
    struct client_state *st = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0)
        st->compositor = wl_registry_bind(reg, name,
                                          &wl_compositor_interface, 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        st->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        st->wm_base = wl_registry_bind(reg, name,
                                       &xdg_wm_base_interface, 2);
}

static void registry_global_remove(void *data, struct wl_registry *reg,
                                   uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener reg_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close     = xdg_toplevel_close,
};

/* ── cleanup ───────────────────────────────────────────── */

static void cleanup(struct client_state *st)
{
    if (st->xdg_toplevel) xdg_toplevel_destroy(st->xdg_toplevel);
    if (st->xdg_surface)  xdg_surface_destroy(st->xdg_surface);
    if (st->surface)      wl_surface_destroy(st->surface);
    if (st->wm_base)      xdg_wm_base_destroy(st->wm_base);
    if (st->compositor)   wl_compositor_destroy(st->compositor);
    if (st->shm)          wl_shm_destroy(st->shm);
    if (st->registry)     wl_registry_destroy(st->registry);
    if (st->display)      wl_display_disconnect(st->display);
}

/* ── main ──────────────────────────────────────────────── */

int main(void)
{
    struct client_state st = {
        .width   = 640,
        .height  = 480,
        .running = 1,
    };

    printf("=== 模块 7: Cairo 软件渲染 ===\n");
    printf("理解: Cairo 在 wl_shm buffer 之上绘制反锯齿图形.\n");
    printf("      WL_SHM_FORMAT_ARGB8888 与 Cairo 的 ARGB32 格式兼容.\n");
    printf("      mmap 的像素内存直接传给 cairo_image_surface_create_for_data().\n\n");

    st.display = wl_display_connect(NULL);
    if (!st.display) {
        fprintf(stderr, "无法连接到 Wayland display\n");
        return EXIT_FAILURE;
    }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &reg_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.compositor || !st.shm || !st.wm_base) {
        fprintf(stderr, "缺少必要的 Wayland 全局对象\n");
        cleanup(&st);
        return EXIT_FAILURE;
    }

    xdg_wm_base_add_listener(st.wm_base, &wm_base_listener, &st);
    st.surface = wl_compositor_create_surface(st.compositor);
    st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    xdg_surface_add_listener(st.xdg_surface, &xdg_surface_listener, &st);
    st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
    xdg_toplevel_add_listener(st.xdg_toplevel, &xdg_toplevel_listener, &st);
    xdg_toplevel_set_title(st.xdg_toplevel, "模块7 - Cairo 渲染");

    wl_surface_commit(st.surface);

    printf("Cairo 渲染窗口已启动 (关闭窗口以退出)...\n");
    while (st.running && wl_display_dispatch(st.display) != -1)
        ;

    cleanup(&st);
    printf("\n=== 模块 7 完成 ===\n");
    return EXIT_SUCCESS;
}
