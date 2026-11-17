/* main.c - 模块 2: 第一个 Wayland 窗口
 *
 * 走通客户端渲染的最小完整闭环:
 *   connect → registry 枚举(bind compositor/shm/xdg_wm_base)
 *   → create_surface → 赋予 xdg_toplevel 角色 → 等首次 configure
 *   → 造 wl_shm buffer → attach + commit 首帧上屏。
 *
 * 两个核心机制在本模块首次登场:
 *   - xdg-shell 的 configure / ack_configure 握手: 窗口的尺寸与
 *     状态由 compositor 决定, 客户端"应答之后"才有资格提交像素;
 *   - attach → commit 双阶段模型: attach 只是把 buffer 挂到
 *     pending 状态, commit 才原子地生效到屏幕。
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

struct client_state {
    /* 连接与全局对象代理 —— 模块 1 的成果在这里变成成员 */
    struct wl_display      *display;
    struct wl_registry     *registry;
    struct wl_compositor   *compositor;
    struct wl_shm          *shm;
    struct xdg_wm_base     *wm_base;

    /* 窗口对象链: surface(画布) → xdg_surface(角色容器) → toplevel(窗口) */
    struct wl_surface      *surface;
    struct xdg_surface     *xdg_surface;
    struct xdg_toplevel    *xdg_toplevel;

    /* 本模块只画一帧, 一个 buffer 用到底 */
    struct wl_buffer       *buffer;
    int                     width, height;
    int                     running;
};

/* 造一块 wl_shm buffer 并填入渐变像素。
 * Wayland 传像素最朴素的方式就是共享内存文件:
 * mkstemp 匿名临时文件 → ftruncate 定容 → mmap 填像素
 * → 把 fd 连同尺寸/格式交给 wl_shm_create_pool。 */
static struct wl_buffer *
create_buffer(struct client_state *st, int width, int height)
{
    int stride = width * 4;
    int size   = stride * height;

    /* wl_shm pool 的底层就是一个"可 mmap 的文件描述符"。
     * mkstemp + unlink 的组合 = 匿名文件: 名字立刻删掉, fd 依然
     * 有效, 进程退出后存储由内核自动回收, 不在 /tmp 留垃圾。
     * 循环 8 次只是防止随机后缀撞名。 */
    int fd = -1;
    for (int i = 0; i < 8; i++) {
        char name[] = "/tmp/wayland-mod2-XXXXXX";
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

    /* 新建文件长度为 0, 必须先扩到 size, mmap 才映射得到像素空间。 */
    if (ftruncate(fd, size) < 0) {
        fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    /* 必须 MAP_SHARED: compositor 是另一个进程, 它要通过同一个 fd
     * 建立自己的映射才能读到我们写入的像素(模块 11 服务端会看到
     * 对称的另一半)。 */
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    /* XRGB8888 在小端机器上的内存字节序是 [B,G,R,X](模块 3 详述)。
     * 按 uint32 写入时高位→低位为 X,R,G,B, 因此
     * (0xFF<<24)|(r<<16)|(g<<8)|b; X 字节实际被合成器忽略。 */
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
    /* 像素已全部写完, 客户端侧的映射可以解除; 服务端稍后通过
     * 自己对同一文件的映射读取。协议规定: commit 之后客户端不得
     * 再写这块 buffer, 直到收到 wl_buffer.release(本模块一次性
     * 使用、从不复用, 所以不需要等 release)。 */
    munmap(data, size);

    /* pool = "一整块共享内存", buffer = "这块内存上的一扇窗"。
     * buffer 在协议层对 pool 持有引用, 因此创建完 buffer 就可以
     * 立刻销毁 pool 代理并关闭本进程的 fd —— 内核文件对象由
     * buffer(和服务端)继续引用, 不会提前释放。 */
    struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

/* ── xdg_wm_base 事件 ───────────────────────────────────── */

/* ping/pong 是合成器的存活探测(窗口标"未响应"前它会先 ping)。
 * 收到不回 pong 的话, 窗口可能被置灰甚至被强杀,
 * 所以任何 xdg-shell 客户端都必须第一时间原样回 serial。 */
static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

/* ── xdg_surface 事件 ──────────────────────────────────── */

/* configure / ack_configure 是 xdg-shell 的核心握手:
 * 合成器在此告知"这个窗口的尺寸与状态定了", 客户端必须先 ack,
 * 然后才允许带着新尺寸 attach + commit; 首次 configure 到来之前
 * 提交带 buffer 的 commit 是协议错误(会被直接断连)。 */
static void
xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct client_state *st = data;
    /* 规矩的第一步: 先应答, 像素的事放在应答之后。 */
    xdg_surface_ack_configure(xdg_surface, serial);

    /* 首次 configure 才真正开始画: 走到这里我们才知道可用尺寸
     * (toplevel 的 configure 可能已经改写过 st->width/height)。 */
    if (st->buffer == NULL) {
        st->buffer = create_buffer(st, st->width, st->height);
        if (st->buffer == NULL) {
            /* 拿不到画布就没有可提交的内容, 继续循环只会
             * 反复 attach(NULL), 得到一个永远空白的窗口;
             * 不如直接停循环, 让错误在清理路径上收场。 */
            st->running = 0;
            return;
        }
    }
    /* attach 把 buffer 挂到 pending, commit 原子生效 —— 这是
     * attach/commit 模型的首次登场。之后窗口再次 configure 时
     * 重复提交同一块 buffer 即可保持画面。 */
    wl_surface_attach(st->surface, st->buffer, 0, 0);
    wl_surface_commit(st->surface);
}

/* ── xdg_toplevel 事件 ─────────────────────────────────── */

/* width/height 为 0 表示"尺寸客户端自己定"(常见于首次映射);
 * states 数组是 maximized/activated/resizing 等状态位, 本模块
 * 只关心大小。真正"生效"要等随后的 xdg_surface.configure。 */
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

/* 用户点了窗口的关闭按钮。注意协议里没有"客户端请求关窗"这回事:
 * 退出是客户端自己的事 —— 停事件循环, 然后逐层销毁代理对象。 */
static void
xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    struct client_state *st = data;
    st->running = 0;
}

/* ── registry 事件 ─────────────────────────────────────── */

/* 模块 1 的发现机制在此投入使用: 按接口名挑选并 bind。
 * 第 4 个参数是"客户端想要的版本", 实际生效版本
 * = min(要的, 服务端广播的) —— 高版本接口向后兼容低版本用法,
 * 所以固定按自己代码所支持的版本去要, 而不是盲目要最高版。 */
static void
registry_global(void *data, struct wl_registry *registry,
                uint32_t name, const char *interface, uint32_t version)
{
    struct client_state *st = data;
    (void)version;

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

    /* 窗口角色链: wl_surface 是纯像素画布(不提交就什么都不是),
     * xdg_surface 给它装上"角色容器", xdg_toplevel 指明这是
     * "可移动/可缩放的顶层窗口"。只 create_surface 是出不来
     * 窗口的 —— 没有角色的 surface 合成器根本不会展示。 */
    st.surface = wl_compositor_create_surface(st.compositor);
    st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    xdg_surface_add_listener(st.xdg_surface, &xdg_surface_listener, &st);
    st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
    xdg_toplevel_add_listener(st.xdg_toplevel, &xdg_toplevel_listener, &st);
    xdg_toplevel_set_title(st.xdg_toplevel, "模块2 - Hello Wayland");

    /* 这次 commit 故意不带 buffer: 它唯一的目的是把上面的角色
     * 赋值请求送达服务端, 换取第一次 xdg_toplevel.configure ——
     * "空提交"是 xdg-shell 客户端的标准起手式。 */
    wl_surface_commit(st.surface);
    wl_display_roundtrip(st.display);

    printf("窗口已创建. 运行事件循环...\n");
    printf("理解: attach() 附加像素缓冲区, commit() 原子提交给 compositor.\n");
    printf("Compositor 收到 commit 后在下一刷新周期显示.\n");
    printf("关闭窗口或按 Ctrl+C 退出.\n\n");

    /* 事件循环是"拉"模型: dispatch 阻塞读 socket → 解码事件 →
     * 依次调用各 listener → 返回。configure/close 等所有窗口
     * 行为都发生在回调里, main 只负责转发"还活着"这一事实。 */
    while (st.running && wl_display_dispatch(st.display) != -1)
        ;

    /* 销毁顺序 = 依赖关系的反序: 先子对象(toplevel/xdg_surface),
     * 再画布 surface, 最后才是全局代理与连接本身。 */
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
