/* main.c - 模块 4: 键盘与鼠标输入
 *
 * Wayland 的输入模型围绕 wl_seat 展开: 一个 seat 代表"一组
 * 一起使用的输入设备"(键盘+鼠标+触摸), 它用 capabilities 事件
 * 广播自己有哪些能力, 客户端再按需 get_keyboard / get_pointer。
 *
 * 两条主线:
 *   - 键盘: keymap 事件用"传文件描述符"的方式把 XKB 键位表发给
 *     客户端; 之后每个 key 事件携带 evdev keycode, 客户端经
 *     xkb_state 转成 keysym(要 +8 的历史偏移)再转成可读键名;
 *   - 指针: motion/button/axis 给出 surface 相对坐标,
 *     坐标类型是 wl_fixed_t(24.8 定点数), 带 8 位小数精度。
 */

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

/* keymap 事件: 服务端把编译好的 XKB 键位表发给我们 —— 但不是
 * 装在消息里当字符串传, 而是直接传递一个文件描述符(Wayland
 * 协议的 fd-passing 机制, 经 Unix socket 的 SCM_RIGHTS 完成)。
 * 为什么这样设计: ① 键位表动辄上百 KB, 消息传太低效, mmap 零拷贝;
 * ② 键盘解释权在服务端, 客户端必须用与服务端同一张键位表,
 * 否则自己看到的键名和输入法/其它窗口的解释会不一致。 */
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
    /* 只读映射即可: 键位表由服务端单方面提供, 客户端无权修改 */
    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str == MAP_FAILED) { close(fd); return; }

    /* 两步走: 文本 → xkb_keymap(键位表), keymap → xkb_state
     * (叠加修饰键/布局组状态后的"查询机", 解释 key 事件全靠它) */
    struct xkb_keymap *km = xkb_keymap_new_from_string(
        st->xkb_ctx, map_str, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_str, size);
    close(fd);

    struct xkb_state *ks = xkb_state_new(km);
    if (!ks) { xkb_keymap_unref(km); return; }

    /* 支持热切换: 用户中途改键盘布局时 keymap 事件会再次到来,
     * 先释放旧的 keymap/state 再整体替换。 */
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

    /* 三级转换的第二级: evdev keycode → keysym。
     * key+8 是历史偏移: Wayland 传的是 Linux evdev keycode,
     * 而 XKB 键码从 8 起编号(0-7 留给 X11 遗留), 不加会整体错位。 */
    xkb_keysym_t sym = XKB_KEY_NoSymbol;
    if (st->xkb_state) {
        sym = xkb_state_key_get_one_sym(st->xkb_state, key + 8);
    }

    /* 第三级: keysym → 人类可读名("a"/"Return"/"Escape"),
     * 纯粹为了打印调试; 正经文本输入还应再转 utf8(见 README)。 */
    char sym_name[64];
    if (sym != XKB_KEY_NoSymbol) {
        xkb_keysym_get_name(sym, sym_name, sizeof(sym_name));
    } else {
        snprintf(sym_name, sizeof(sym_name), "keycode=%u", key);
    }

    const char *action = (state == WL_KEYBOARD_KEY_STATE_PRESSED) ? "DOWN" : "UP";
    printf("⌨  key %s  ser=%u  time=%u  %s\n", action, serial, time, sym_name);

    /* 输入驱动业务: Esc 置 running=0, 主循环随之退出 ——
     * 键盘回调里不做任何渲染, 只改状态, 渲染在主循环里做。 */
    if (sym == XKB_KEY_Escape && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        st->running = 0;
    }
}

/* 修饰键状态也由服务端广播: 四个掩码分别对应
 * 按住(depressed)/粘滞(latched)/锁定(CapsLock 类, locked)和布局组。
 * 客户端不必自己跟踪按下史 —— 把掩码灌进 xkb_state,
 * 之后的 key 事件才能解释成 Shift+a → "A" 这样的 keysym。 */
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

/* 连发参数: 按住不放时客户端应按此 rate(每秒次数)/delay(首延迟 ms)
 * 自行生成重复的 key 事件 —— Wayland 服务端不替你连发,
 * 这是与 X11 服务端连发模型的显著不同。 */
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

/* 指针进入 surface: 服务端已把全局指针位置换算成本窗口内的坐标。
 * wl_fixed_t 是 24.8 定点数(8 位小数) —— 整数像素在高 DPI 缩放、
 * 触控板细腻移动下精度不够, 所以协议里坐标全是定点数。 */
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
    /* motion 事件非常高频(触控板下每秒成百上千次), 打印会刷屏,
     * 这里只更新状态; 画面在主循环里按固定节奏重绘。 */
    st->pointer_x = wl_fixed_to_int(sx);
    st->pointer_y = wl_fixed_to_int(sy);
}

static void
pointer_button(void *data, struct wl_pointer *pointer,
               uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    (void)data; (void)pointer;
    /* button 编号是 linux/input-event-codes.h 里的 BTN_* 常量
     * (如 0x110=左键, 0x111=中键, 0x112=右键), 不是鼠标"第几键"。
     * serial 记住它: 弹出菜单/抓取时要回传"引发它的那次 serial"。 */
    const char *action = (state == WL_POINTER_BUTTON_STATE_PRESSED) ? "DOWN" : "UP";
    printf("🖰  button %u %s  ser=%u  time=%u\n", button, action, serial, time);
}

/* 滚轮/触控板滚动以 axis 事件表达: value 是定点数"刻度",
 * 向上/向左为负; 触控板平滑滚动会带小数、一格滚轮通常是 ±1。
 * 离散滚轮与平滑滚动在 v5+ 协议里有 axis_source/axis_stop 细分。 */
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

/* 能力广播: 位掩码告知当前 seat 有哪几类设备, 而且热插拔时会
 * 再次触发(拔掉键盘 → 位清零 → 我们 release 代理)。因此写成
 * "边沿触发"式: 位出现且尚未绑定才 get, 位消失且已绑定才 release,
 * 重复的 capabilities 事件就不会反复建/拆对象。 */
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

/* seat 名字(如 "seat0")。只有多 seat 系统 —— 例如带多套
 * 独立键盘/显示器的公共信息亭 —— 才会出现别的名字,
 * 桌面环境里基本只有 seat0, 打印出来仅为验证通路。 */
static void seat_name(void *d, struct wl_seat *s, const char *name)
    { (void)d; (void)s; printf("seat name: %s\n", name); }

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

/* ── frame ─────────────────────────────────────────────── */

/* 每帧全量重绘: 深灰棋盘格背景 + 以指针坐标为中心的十字准星。
 * 输入(motion 只改 pointer_x/y) → 渲染 → 提交, 构成最原始的
 * 交互回路; 与模块 3 一样没有 frame callback, 节奏全靠 usleep。 */
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

    /* 逐像素判断: 距指针 8 像素内画绿框, 精确命中画红点。
     * 坐标来自 motion/enter 事件换算的 surface 相对坐标 ——
     * 客户端永远不知道指针在屏幕上的绝对位置, 这是 Wayland
     * 防止窗口互相打探的设计。 */
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

    /* 注意顺序: xkb 上下文必须先于连接建立 —— seat 一旦 bind,
     * keymap 事件随时可能到来, 回调里立刻就要用它编译键位表。 */
    st.display = wl_display_connect(NULL);
    if (!st.display) { fprintf(stderr,"no display\n"); return 1; }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &reg_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.compositor || !st.shm || !st.wm_base || !st.seat) {
        fprintf(stderr,"missing globals\n"); return 1;
    }

    /* seat 监听必须先挂好: capabilities 一般在下次 roundtrip 内到达,
     * 它会顺手 get_keyboard/get_pointer, keymap 事件紧随其后。 */
    wl_seat_add_listener(st.seat, &seat_listener, &st);
    xdg_wm_base_add_listener(st.wm_base, &wm_listener, &st);

    st.surface = wl_compositor_create_surface(st.compositor);
    st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    xdg_surface_add_listener(st.xdg_surface, &xs_listener, &st);
    st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
    xdg_toplevel_add_listener(st.xdg_toplevel, &xt_listener, &st);
    xdg_toplevel_set_title(st.xdg_toplevel, "模块4 - 输入调试");

    /* 空提交让 xdg 角色生效, 换取首次 configure(同模块 2/3) */
    wl_surface_commit(st.surface);
    wl_display_roundtrip(st.display);

    /* 交互回路: dispatch 处理输入事件(回调只改状态) → 全量重绘一帧
     * → usleep 压节奏。打印类事件(按键/点击)都在回调里直接输出。 */
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

    /* 销毁仍是依赖反序, 输入对象排在最前: 先 release 键盘/指针
     * 代理, 再 release seat 本身(协议规定 v5 起用 release 代替
     * destroy, 让服务端知道我们是"好公民"式退出)。 */
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
