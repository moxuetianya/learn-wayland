/* main.c - 模块 1: 建立连接与服务发现
 *
 * Wayland 客户端的第一行代码: 连上 compositor 的 Unix domain socket,
 * 拿到 wl_display 连接代理, 再通过 wl_registry 把服务端手里的
 * "全局对象清单"(wl_compositor / wl_shm / xdg_wm_base / wl_seat ...)
 * 整个枚举一遍。后续所有客户端模块都是从这份清单里
 * bind 自己需要的接口开始的, 本模块是它们的公共前奏。
 *
 * 两个关键机制:
 *   - 连接即 socket 发现: wl_display_connect 按 $WAYLAND_DISPLAY
 *     (+ $XDG_RUNTIME_DIR) 定位 socket 文件, 找不到即失败;
 *   - 事件是"拉"来的: 服务端不会打断你的代码, 必须主动调用
 *     wl_display_roundtrip / wl_display_dispatch 交出控制权,
 *     libwayland 才会读 socket 并回调 listener。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

/* registry 的 global 事件: 服务端把自己提供的每个全局对象
 * (interface 名 + 数字 name + 服务端版本) 逐个广播给客户端。
 * name 只是本次连接内的临时编号, 之后 bind 时要拿它作凭据;
 * interface 字符串才是"这是什么东西"(如 "wl_compositor")。 */
static void
global_handler(void *data, struct wl_registry *registry, uint32_t name,
               const char *interface, uint32_t version)
{
    (void)data;
    (void)registry;
    printf("  [%4u] %-40s version %2u\n", name, interface, version);
}

/* global_remove 事件: 服务端撤下某个全局对象(如显示器被拔掉)。
 * 正经客户端收到它必须把对应代理对象置空并停用相关功能,
 * 继续使用已失效的代理会触发协议错误而被断连。
 * 本模块只做一次性枚举, 没有留存任何代理, 留空实现即可。 */
static void
global_remove_handler(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

/* libwayland 的回调注册模式: listener 结构体的槽位与协议事件
 * 一一对应, 只填关心的槽位, 未填的默认 NULL, 事件到达时被
 * 静默忽略 —— 这就是 Wayland 客户端"实现接口子集"的方式。 */
static const struct wl_registry_listener registry_listener = {
    .global        = global_handler,
    .global_remove = global_remove_handler,
};

int main(void)
{
    struct wl_display  *display;
    struct wl_registry *registry;

    printf("=== Wayland 模块 1: 建立连接与发现全局对象 ===\n\n");

    /* 连接 = ① 定位 socket: $WAYLAND_DISPLAY 给出名字(相对名会拼上
     * $XDG_RUNTIME_DIR, 也可以是绝对路径), 未设置时回落到 "wayland-0";
     * ② connect(2) 成功后返回 wl_display 代理。
     * 注意此刻还没有交换任何协议字节, 也没有任何对象存在。 */
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "ERROR: 无法连接到 Wayland compositor.\n"
                "请确保你在 Wayland 会话中运行此程序 (echo $WAYLAND_DISPLAY)\n");
        return EXIT_FAILURE;
    }
    printf("已连接到 display: %s\n", wl_display_get_fd(display) >= 0 ? "OK" : "FAIL");

    /* get_registry 在本地构造 wl_registry 代理并(延迟)发出请求,
     * add_listener 登记回调 —— 但这两步都还不会触发任何事件,
     * 事件要等下面的 roundtrip 真正去读 socket。 */
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    printf("\n--- 等待服务器广播全局对象... ---\n");
    /* roundtrip = 把挂起的请求全部冲发 + 阻塞等服务端处理完毕、
     * 把所有应答事件送回(内部靠 wl_display_sync 请求的 done 事件
     * 当作"处理到这里"的栅栏)。走到这一行之后, 服务端的 global
     * 广播必然已全部到达, 上面的 global_handler 已全部跑完。 */
    wl_display_roundtrip(display);

    printf("\n--- 协议版本协商示例 ---\n");
    printf("客户端连接后, 通过 MIN(requested_version, server_version) 协商版本.\n");
    printf("例如: wl_registry_bind(registry, name, &wl_compositor_interface, 4)\n");
    printf("如果服务器只支持 v5, 协商后使用 v4; 如果服务器只支持 v2, 协商后使用 v2.\n");

    wl_registry_destroy(registry);
    wl_display_disconnect(display);

    printf("\n=== 模块 1 完成 ===\n");
    return EXIT_SUCCESS;
}
