#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "hello-client-protocol.h"

struct state {
    struct wl_display  *display;
    struct wl_registry *registry;
    struct hello       *hello;
    int                 done;
};

static void hello_reply_handler(void *data, struct hello *hello, const char *reply)
{
    (void)data; (void)hello;
    printf("[event] hello_reply: %s\n", reply);
}

static void sum_result_handler(void *data, struct hello *hello,
                               int32_t a, int32_t b, int32_t sum)
{
    (void)data; (void)hello;
    printf("[event] sum_result: %d + %d = %d\n", a, b, sum);
    if (sum == 3+5) {
        ((struct state *)data)->done = 1;
    }
}

static void server_info_handler(void *data, struct hello *hello,
                                uint32_t proto_ver, const char *name)
{
    (void)data; (void)hello;
    printf("[event] server_info: protocol_version=%u  server=%s\n",
           proto_ver, name);
}

static const struct hello_listener hello_listener = {
    .hello_reply = hello_reply_handler,
    .sum_result  = sum_result_handler,
    .server_info = server_info_handler,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
    struct state *st = data;
    printf("[registry] found global: %s v%u (id=%u)\n", interface, version, name);

    if (strcmp(interface, hello_interface.name) == 0) {
        st->hello = wl_registry_bind(registry, name, &hello_interface, 1);
        printf("[registry] bound hello interface v1\n");
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_remove,
};

int main(void)
{
    struct state st = {0};

    printf("=== 模块 5: 自定义 Wayland 协议 ===\n");
    printf("本模块演示:\n");
    printf("  1. 编写协议 XML (hello.xml)\n");
    printf("  2. 使用 wayland-scanner 生成 C header/code\n");
    printf("  3. 客户端使用生成的 API 发送请求\n");
    printf("  4. 理解 interface/request/event/enum 定义\n\n");

    st.display = wl_display_connect(NULL);
    if (!st.display) {
        fprintf(stderr, "无法连接 Wayland compositor.\n");
        fprintf(stderr, "提示: 此模块需要配合模块 8 的 server 一起运行\n\n");
        return EXIT_FAILURE;
    }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &registry_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.hello) {
        printf("\n当前 compositor 不提供 'hello' 全局接口.\n");
        printf("这是正常的——hello 是我们自己定义的协议.\n");
        printf("请先运行模块 8 的 server, 它注册了这个接口:\n");
        printf("  cd ../module-08-server-skeleton && ./hello-server &\n");
        printf("  cd ../module-05-protocol && ./hello-client\n\n");
        wl_registry_destroy(st.registry);
        wl_display_disconnect(st.display);
        return 0;
    }

    hello_add_listener(st.hello, &hello_listener, &st);

    printf("\n--- 发送请求 ---\n");
    hello_say_hello(st.hello, "Hello from client!");
    hello_add_numbers(st.hello, 3, 5);
    wl_display_flush(st.display);

    printf("--- 等待事件 ---\n");
    while (!st.done && wl_display_dispatch(st.display) != -1)
        ;

    hello_destroy(st.hello);
    wl_registry_destroy(st.registry);
    wl_display_disconnect(st.display);

    printf("\n=== 模块 5 完成 ===\n");
    return EXIT_SUCCESS;
}
