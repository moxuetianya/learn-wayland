#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include "hello-server-protocol.h"

struct hello_data {
    struct wl_resource *resource;
};

static void
hello_say_hello(struct wl_client *client, struct wl_resource *resource,
                const char *message)
{
    (void)client;
    printf("[server] client says: %s\n", message);
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
    struct hello_data *hd = wl_resource_get_user_data(resource);
    printf("[server] client resource destroyed\n");
    free(hd);
}

static const struct hello_interface hello_impl = {
    .say_hello   = hello_say_hello,
    .add_numbers = hello_add_numbers,
};

static void
hello_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct hello_data *hd = calloc(1, sizeof(*hd));
    if (!hd)
        return;

    hd->resource = wl_resource_create(client, &hello_interface, version, id);
    if (!hd->resource) {
        free(hd);
        return;
    }

    wl_resource_set_implementation(hd->resource, &hello_impl, hd, hello_destroy);

    printf("[server] client bound hello interface v%u\n", version);
    hello_send_server_info(hd->resource, 1, "learn-wayland-module8");
}

int main(void)
{
    struct wl_display *display;

    printf("=== 模块 8: libwayland-server 骨架 ===\n");
    printf("启动 Wayland 服务器, 注册 'hello' 全局接口...\n\n");

    display = wl_display_create();
    if (!display) {
        fprintf(stderr, "无法创建 wl_display\n");
        return EXIT_FAILURE;
    }

    const char *socket = wl_display_add_socket_auto(display);
    if (!socket) {
        fprintf(stderr, "无法创建 Unix socket\n");
        wl_display_destroy(display);
        return EXIT_FAILURE;
    }
    printf("监听 socket: %s\n", socket);
    printf("设置环境变量: export WAYLAND_DISPLAY=%s\n", socket);

    wl_global_create(display, &hello_interface, 1, NULL, hello_bind);

    printf("\n服务器运行中... (Ctrl+C 退出)\n");
    printf("在另一个终端运行: cd module-05-protocol && ./hello-client\n\n");

    wl_display_run(display);

    wl_display_destroy(display);
    return EXIT_SUCCESS;
}
