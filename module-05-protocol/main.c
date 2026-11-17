#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "hello-client-protocol.h"

struct state {
    struct wl_display  *display;
    struct wl_registry *registry;
    /* scanner 生成的头文件把 'hello' 接口代理成不透明结构体；
     * 它在 wire 上就是一个对象 id，所有请求/事件都经它编解码。 */
    struct hello       *hello;
    /* 收到预期的 sum_result 后置 1，作为下方 dispatch 循环的退出条件：
     * 事件驱动程序不能"跑完就退"，只能等某个事件宣布完成。 */
    int                 done;
};

/* 每个 event 处理函数的签名都由生成的 hello_listener 结构体约定：
 * (data, 代理指针, XML 里声明的各 arg 依次展开)。 */
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
    /* 只有核对到"这就是我那次 add_numbers(3,5) 的回答"才置 done。
     * 真实协议里通常用 serial/请求 id 把多组问答配对，本协议刻意极简。 */
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

/* 字段名与 hello.xml 中 <event> 声明一一对应；scanner 保证字段顺序 =
 * event 的 wire opcode 顺序，所以绝不能手改这个结构体（少一个字段就全错位）。 */
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
        /* bind 的第 4 参是我们"请求"的版本，必须 1 <= v <= 广播的 version；
         * 服务端之后按这个版本对待本次绑定（版本协商发生在 bind，不在连接时）。 */
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
    /* roundtrip = 发一个 sync 请求并等到它的 done：保证服务端把所有 global
     * 广播完、上面的 registry_global 回调全部跑完，后面才能安全检查 st.hello。 */
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
    /* 这两个"函数调用"只是把消息编码进 libwayland 的本地出站缓冲，
     * 此刻还没有一个字节写进 socket（见生成的 hello_say_hello inline 桩）。 */
    hello_say_hello(st.hello, "Hello from client!");
    hello_add_numbers(st.hello, 3, 5);
    /* flush 才真正把缓冲写进 socket；忘了它服务端就"毫无反应"。
     * dispatch 前也会隐式 flush，但显式 flush 让"发送/等待"的边界更清晰。 */
    wl_display_flush(st.display);

    printf("--- 等待事件 ---\n");
    /* dispatch 阻塞等事件，listener 回调在本线程内执行；
     * 三条 event 全部在循环里被听见（包括 bind 时服务端就发出的 server_info）。 */
    while (!st.done && wl_display_dispatch(st.display) != -1)
        ;

    hello_destroy(st.hello);
    wl_registry_destroy(st.registry);
    wl_display_disconnect(st.display);

    printf("\n=== 模块 5 完成 ===\n");
    return EXIT_SUCCESS;
}
