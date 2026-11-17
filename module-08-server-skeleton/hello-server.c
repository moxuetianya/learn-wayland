#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include "hello-server-protocol.h"

/* 每个 bind 了 hello 接口的客户端各有一份，作为 resource 的 user_data。
 * 生命周期跟随 wl_resource：resource 销毁时在 hello_destroy 里释放。 */
struct hello_data {
    struct wl_resource *resource;
};

static void
hello_say_hello(struct wl_client *client, struct wl_resource *resource,
                const char *message)
{
    (void)client;
    /* 请求处理函数：客户端调用 hello_say_hello() 时，libwayland 从 socket
     * 解码请求，按 opcode 查 hello_impl 虚表分发到这里，参数已由生成的
     * 编解码代码拆好包——我们只写业务逻辑。 */
    printf("[server] client says: %s\n", message);
    /* *_send_* 是 wayland-scanner 生成的事件发送内联函数，本质是
     * wl_resource_post_event(resource, opcode, ...) 的薄封装。 */
    hello_send_hello_reply(resource, "Hello back from server!");
}

static void
hello_add_numbers(struct wl_client *client, struct wl_resource *resource,
                  int32_t a, int32_t b)
{
    (void)client;
    printf("[server] computing %d + %d\n", a, b);
    hello_send_sum_result(resource, a, b, a + b);
}

static void
hello_destroy(struct wl_resource *resource)
{
    /* 析构回调（wl_resource_set_implementation 的第 4 个参数）：客户端主动
     * destroy proxy 或连接断开时触发，是释放 resource user_data 的唯一时机，
     * 忘了就是内存泄漏。 */
    struct hello_data *hd = wl_resource_get_user_data(resource);
    printf("[server] client resource destroyed\n");
    free(hd);
}

/* 服务端"虚表"：生成的 struct hello_interface 为每个 request 留了一个
 * 函数指针槽位。缺省 NULL 的槽位一旦被客户端调用，libwayland 会自动
 * 向客户端抛 wl_display.error（无效请求）。 */
static const struct hello_interface hello_impl = {
    .say_hello   = hello_say_hello,
    .add_numbers = hello_add_numbers,
};

static void
hello_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    /* bind 回调：客户端 wl_registry.bind() 时被调用。global 只是向所有
     * 客户端广播的"广告"，此刻才真正创建服务端对象 wl_resource 并挂上
     * 虚表——bind 之后客户端发来的请求才有落点。 */
    (void)data;
    struct hello_data *hd = calloc(1, sizeof(*hd));
    if (!hd)
        return;

    /* id 是客户端在 bind 请求里自选的新对象 id，必须原样传回：
     * 客户端靠它把服务端 resource 和自己刚拿到的 proxy 对上号。 */
    hd->resource = wl_resource_create(client, &hello_interface, version, id);
    if (!hd->resource) {
        free(hd);
        return;
    }

    /* 把 vtable + user_data + 析构函数绑到 resource 上：
     * 从此该客户端的每个 hello 请求都进 hello_impl。 */
    wl_resource_set_implementation(hd->resource, &hello_impl, hd, hello_destroy);

    printf("[server] client bound hello interface v%u\n", version);
    /* bind 完不等请求、立即主动推事件：事件方向（server→client）不需要
     * 任何请求触发，客户端只要挂了 listener 就能收到。 */
    hello_send_server_info(hd->resource, 1, "learn-wayland-module8");
}

int main(void)
{
    struct wl_display *display;

    printf("=== 模块 8: libwayland-server 骨架 ===\n");
    printf("启动 Wayland 服务器, 注册 'hello' 全局接口...\n\n");

    /* wl_display：服务端世界的根。它内部持有事件循环（wl_event_loop）、
     * global 列表和客户端列表，本进程后续的一切都挂在它上面。 */
    display = wl_display_create();
    if (!display) {
        fprintf(stderr, "无法创建 wl_display\n");
        return EXIT_FAILURE;
    }

    /* 在 $XDG_RUNTIME_DIR 下创建 wayland-N 并监听，N 取第一个空闲值：
     * 桌面会话内通常是 wayland-1（wayland-0 已被 GNOME 之类的会话占用），
     * 纯 TTY 下则是 wayland-0。 */
    const char *socket = wl_display_add_socket_auto(display);
    if (!socket) {
        fprintf(stderr, "无法创建 Unix socket\n");
        wl_display_destroy(display);
        return EXIT_FAILURE;
    }
    printf("监听 socket: %s\n", socket);
    printf("设置环境变量: export WAYLAND_DISPLAY=%s\n", socket);

    /* 注册 global：向此后连接的每一个客户端广播"我支持 hello v1"。
     * 第 3 个参数是广播出去的版本上限（不得超过 XML 里 interface 的
     * version，否则 libwayland 直接 abort）。此时还没有任何客户端，
     * "注册"改的只是服务端自己的 global 列表。 */
    wl_global_create(display, &hello_interface, 1, NULL, hello_bind);

    printf("\n服务器运行中... (Ctrl+C 退出)\n");
    printf("在另一个终端运行: cd module-05-protocol && ./hello-client\n\n");

    /* 主循环：内部就是反复 wl_event_loop_dispatch——监听 socket 上的新
     * 连接与既有连接的请求，解码后分发到对应 resource 的虚表，再把队列
     * 中的事件冲刷给客户端。直到 wl_display_terminate 才返回。 */
    wl_display_run(display);

    wl_display_destroy(display);
    return EXIT_SUCCESS;
}
