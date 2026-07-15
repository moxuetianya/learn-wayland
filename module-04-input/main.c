#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"

struct client_state {
    struct wl_display      *display;
    struct wl_registry     *registry;
    struct wl_compositor   *compositor;
    struct wl_shm          *shm;
    struct xdg_wm_base     *wm_base;
    struct wl_seat         *seat;

    struct wl_surface      *surface;
    struct xdg_surface     *xdg_surface;
    struct xdg_toplevel    *xdg_toplevel;

    struct wl_keyboard     *keyboard;
    struct wl_pointer      *pointer;
    struct xkb_context     *xkb_ctx;
    struct xkb_keymap      *xkb_keymap;
    struct xkb_state       *xkb_state;

    struct wl_buffer       *buffer;
    int width, height;
    int running;
    int pointer_x, pointer_y;
};

/* ── keyboard ──────────────────────────────────────────── */

static void
keyboard_keymap(void *data, struct wl_keyboard *kb,
                uint32_t format, int fd, uint32_t size)
{
    struct client_state *st = data;
    (void)kb;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str == MAP_FAILED) { close(fd); return; }

    struct xkb_keymap *km = xkb_keymap_new_from_string(
        st->xkb_ctx, map_str, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_str, size);
    close(fd);

    struct xkb_state *ks = xkb_state_new(km);
    if (!ks) { xkb_keymap_unref(km); return; }

    xkb_keymap_unref(st->xkb_keymap);
    xkb_state_unref(st->xkb_state);
    st->xkb_keymap = km;
    st->xkb_state  = ks;
}

static void
keyboard_enter(void *data, struct wl_keyboard *kb,
               uint32_t serial, struct wl_surface *surface,
               struct wl_array *keys)
{
    (void)data; (void)kb; (void)serial;
    (void)surface; (void)keys;
    printf("⌨  keyboard enter (窗口获得键盘焦点)\n");
}

static void
keyboard_leave(void *data, struct wl_keyboard *kb,
               uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)kb; (void)serial; (void)surface;
    printf("⌨  keyboard leave (窗口失去键盘焦点)\n");
}

static void
keyboard_key(void *data, struct wl_keyboard *kb,
             uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    struct client_state *st = data;
    (void)kb; (void)time;

    xkb_keysym_t sym = XKB_KEY_NoSymbol;
    if (st->xkb_state) {
        sym = xkb_state_key_get_one_sym(st->xkb_state, key + 8);
    }

    char sym_name[64];
    if (sym != XKB_KEY_NoSymbol) {
        xkb_keysym_get_name(sym, sym_name, sizeof(sym_name));
    } else {
        snprintf(sym_name, sizeof(sym_name), "keycode=%u", key);
    }

    const char *action = (state == WL_KEYBOARD_KEY_STATE_PRESSED) ? "DOWN" : "UP";
    printf("⌨  key %s  ser=%u  time=%u  %s\n", action, serial, time, sym_name);

    if (sym == XKB_KEY_Escape && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        st->running = 0;
    }
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *kb,
                   uint32_t serial, uint32_t mods_depressed,
                   uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
    struct client_state *st = data;
    (void)kb; (void)serial;
    if (st->xkb_state) {
        xkb_state_update_mask(st->xkb_state,
            mods_depressed, mods_latched, mods_locked, 0, 0, group);
    }
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *kb,
                     int32_t rate, int32_t delay)
{
    (void)data; (void)kb;
    printf("⌨  keyboard repeat: rate=%d  delay=%d ms\n", rate, delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap      = keyboard_keymap,
    .enter       = keyboard_enter,
    .leave       = keyboard_leave,
    .key         = keyboard_key,
    .modifiers   = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ── pointer ───────────────────────────────────────────── */

static void
pointer_enter(void *data, struct wl_pointer *pointer,
              uint32_t serial, struct wl_surface *surface,
              wl_fixed_t sx, wl_fixed_t sy)
{
    struct client_state *st = data;
    (void)pointer; (void)serial; (void)surface;
    st->pointer_x = wl_fixed_to_int(sx);
    st->pointer_y = wl_fixed_to_int(sy);
    printf("🖰  pointer enter at (%d, %d)\n", st->pointer_x, st->pointer_y);
}

static void
pointer_leave(void *data, struct wl_pointer *pointer,
              uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)pointer; (void)serial; (void)surface;
    printf("🖰  pointer leave\n");
}

static void
pointer_motion(void *data, struct wl_pointer *pointer,
               uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct client_state *st = data;
    (void)pointer; (void)time;
    st->pointer_x = wl_fixed_to_int(sx);
    st->pointer_y = wl_fixed_to_int(sy);
}

static void
pointer_button(void *data, struct wl_pointer *pointer,
               uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    (void)data; (void)pointer;
    const char *action = (state == WL_POINTER_BUTTON_STATE_PRESSED) ? "DOWN" : "UP";
    printf("🖰  button %u %s  ser=%u  time=%u\n", button, action, serial, time);
}

static void
pointer_axis(void *data, struct wl_pointer *pointer,
             uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer;
    const char *ax = (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) ? "vertical"
                   : (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) ? "horizontal"
                   : "?";
    printf("🖰  axis %s = %.2f  time=%u\n", ax, wl_fixed_to_double(value), time);
}

static const struct wl_pointer_listener pointer_listener = {
    .enter        = pointer_enter,
    .leave        = pointer_leave,
    .motion       = pointer_motion,
    .button       = pointer_button,
    .axis         = pointer_axis,
};

/* ── seat capabilities ─────────────────────────────────── */

static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    struct client_state *st = data;

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !st->keyboard) {
        st->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(st->keyboard, &keyboard_listener, st);
        printf("seat: 键盘已绑定\n");
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && st->keyboard) {
        wl_keyboard_release(st->keyboard);
        st->keyboard = NULL;
        printf("seat: 键盘已解绑\n");
    }

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !st->pointer) {
        st->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(st->pointer, &pointer_listener, st);
        printf("seat: 鼠标已绑定\n");
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && st->pointer) {
        wl_pointer_release(st->pointer);
        st->pointer = NULL;
        printf("seat: 鼠标已解绑\n");
    }
}

static void seat_name(void *d, struct wl_seat *s, const char *name)
    { (void)d; (void)s; printf("seat name: %s\n", name); }

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

/* ── frame ─────────────────────────────────────────────── */

static struct wl_buffer *
create_frame(struct client_state *st)
{
    int stride = st->width * 4;
    int size   = stride * st->height;

    char tmpfile[] = "/tmp/wayland-mod4-XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) return NULL;
    unlink(tmpfile);
    if (ftruncate(fd, size) < 0) { close(fd); return NULL; }

    uint32_t *p = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return NULL; }

    for (int y = 0; y < st->height; y++)
        for (int x = 0; x < st->width; x++) {
            uint32_t color = 0xFF202020;
            int gx = x / 32, gy = y / 32;
            if ((gx ^ gy) & 1) color = 0xFF303030;
            if (x == st->pointer_x && y == st->pointer_y) color = 0xFFFF0000;
            else if (abs(x - st->pointer_x) < 8 && abs(y - st->pointer_y) < 8)
                color = 0xFF00FF00;
            p[y * st->width + x] = color;
        }
    munmap(p, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, st->width, st->height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ── xdg callbacks ─────────────────────────────────────── */

static void xdg_ping(void *d, struct xdg_wm_base *b, uint32_t s)
    { (void)d; xdg_wm_base_pong(b, s); }
static void xdg_scfg(void *d, struct xdg_surface *s, uint32_t serial)
    { (void)d; xdg_surface_ack_configure(s, serial); }
static void xdg_tcfg(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st)
{
    struct client_state *cs = d; (void)t; (void)st;
    if (w > 0) cs->width = w; if (h > 0) cs->height = h;
}
static void xdg_close(void *d, struct xdg_toplevel *t)
    { (void)t; ((struct client_state*)d)->running = 0; }

/* ── registry ──────────────────────────────────────────── */

static void reg_global(void *d, struct wl_registry *r,
                        uint32_t n, const char *iface, uint32_t ver)
{
    struct client_state *cs = d;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        cs->compositor = wl_registry_bind(r, n, &wl_compositor_interface, 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        cs->shm = wl_registry_bind(r, n, &wl_shm_interface, 1);
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
        cs->wm_base = wl_registry_bind(r, n, &xdg_wm_base_interface, 2);
    else if (strcmp(iface, wl_seat_interface.name) == 0)
        cs->seat = wl_registry_bind(r, n, &wl_seat_interface, 5);
}
static void reg_rm(void *d, struct wl_registry *r, uint32_t n)
    { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg_listener = {
    .global = reg_global, .global_remove = reg_rm
};
static const struct xdg_wm_base_listener wm_listener = { .ping = xdg_ping };
static const struct xdg_surface_listener xs_listener = { .configure = xdg_scfg };
static const struct xdg_toplevel_listener xt_listener = {
    .configure = xdg_tcfg, .close = xdg_close
};

/* ── main ──────────────────────────────────────────────── */

int main(void)
{
    struct client_state st = { .width=640, .height=480, .running=1 };

    printf("=== 模块 4: 键盘与鼠标输入 ===\n");
    printf("理解: wl_seat 代表一组输入设备 (键盘+鼠标+触摸).\n");
    printf("      capability 事件广播当前可用的输入设备类型.\n");
    printf("      libxkbcommon 将 raw keycode 转为可读键名.\n");
    printf("提示: 移动鼠标/按键查看事件. 按 Esc 退出.\n\n");

    st.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    st.display = wl_display_connect(NULL);
    if (!st.display) { fprintf(stderr,"no display\n"); return 1; }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &reg_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.compositor || !st.shm || !st.wm_base || !st.seat) {
        fprintf(stderr,"missing globals\n"); return 1;
    }

    wl_seat_add_listener(st.seat, &seat_listener, &st);
    xdg_wm_base_add_listener(st.wm_base, &wm_listener, &st);

    st.surface = wl_compositor_create_surface(st.compositor);
    st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    xdg_surface_add_listener(st.xdg_surface, &xs_listener, &st);
    st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
    xdg_toplevel_add_listener(st.xdg_toplevel, &xt_listener, &st);
    xdg_toplevel_set_title(st.xdg_toplevel, "模块4 - 输入调试");

    wl_surface_commit(st.surface);
    wl_display_roundtrip(st.display);

    while (st.running && wl_display_dispatch(st.display) != -1) {
        struct wl_buffer *buf = create_frame(&st);
        if (buf) {
            wl_surface_attach(st.surface, buf, 0, 0);
            wl_surface_damage_buffer(st.surface, 0, 0, st.width, st.height);
            wl_surface_commit(st.surface);
            wl_buffer_destroy(buf);
        }
        usleep(16000);
    }

    if (st.keyboard) wl_keyboard_release(st.keyboard);
    if (st.pointer) wl_pointer_release(st.pointer);
    wl_seat_release(st.seat);
    xdg_toplevel_destroy(st.xdg_toplevel);
    xdg_surface_destroy(st.xdg_surface);
    wl_surface_destroy(st.surface);
    xdg_wm_base_destroy(st.wm_base);
    wl_compositor_destroy(st.compositor);
    wl_shm_destroy(st.shm);
    wl_registry_destroy(st.registry);
    wl_display_disconnect(st.display);

    xkb_state_unref(st.xkb_state);
    xkb_keymap_unref(st.xkb_keymap);
    xkb_context_unref(st.xkb_ctx);

    printf("\n=== 模块 4 完成 ===\n");
    return 0;
}
