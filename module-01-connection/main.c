#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

static void
global_handler(void *data, struct wl_registry *registry, uint32_t name,
               const char *interface, uint32_t version)
{
    (void)data;
    (void)registry;
    printf("  [%4u] %-40s version %2u\n", name, interface, version);
}

static void
global_remove_handler(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = global_handler,
    .global_remove = global_remove_handler,
};

int main(void)
{
    struct wl_display  *display;
    struct wl_registry *registry;

    printf("=== Wayland 模块 1: 建立连接与发现全局对象 ===\n\n");

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "ERROR: 无法连接到 Wayland compositor.\n"
                "请确保你在 Wayland 会话中运行此程序 (echo $WAYLAND_DISPLAY)\n");
        return EXIT_FAILURE;
    }
    printf("已连接到 display: %s\n", wl_display_get_fd(display) >= 0 ? "OK" : "FAIL");

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    printf("\n--- 等待服务器广播全局对象... ---\n");
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
