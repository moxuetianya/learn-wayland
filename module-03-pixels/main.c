/* main.c - 模块 3: 手动像素绘制
 *
 * 不借助任何绘图库, 直接往 wl_shm buffer 的内存里逐像素写值:
 * 正弦渐变背景 + 棋盘格 + 中心圆, 每帧全量重算, 连续重绘成动画。
 *
 * 三个知识点:
 *   - WL_SHM_FORMAT_XRGB8888: 每像素 4 字节, 小端机器上内存里
 *     依次是 B,G,R,X, 合并成 32 位字就是 0xXXRRGGBB;
 *   - stride(行距)寻址: 第 y 行第 x 个像素的地址是 pixels[y*w+x],
 *     stride 恰为 w*4 时才能这么写(通用写法是 y*(stride/4)+x);
 *   - 每帧"新建 pool/buffer → attach → damage → commit → 销毁"
 *     的朴素动画循环, 模块 6 会用 frame callback 改良它。
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
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
    int frame_count;
};

/* 逐像素着色: 每个点由 (x,y,frame) 算出一个颜色。
 * u,v 先归一化到 0..1, 这样窗口无论多大图案比例都一致;
 * t 随帧号推进, 让整幅图随时间流动。 */
static void fill_pixels(uint32_t *pixels, int w, int h, int frame)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* 注意先用 float 除再相除: (float)x/w 才是浮点除法,
             * 直接 x/w 是整数除法, 结果永远为 0, 渐变会塌成一坨 */
            float u = (float)x / w;
            float v = (float)y / h;
            float t = (float)frame * 0.02f;

            /* 动画渐变背景: 三通道用不同频率/相位的正弦,
             * 平滑无带状痕迹; 127+127*sin 把 [-1,1] 映射到 [0,255] */
            uint8_t r = (uint8_t)(127 + 127 * sinf(u * 6.28f + t));
            uint8_t g = (uint8_t)(127 + 127 * sinf(v * 6.28f + t * 1.3f));
            uint8_t b = (uint8_t)(127 + 127 * sinf((u + v) * 3.14f + t * 0.7f));

            /* 棋盘格: (x/32)^(y/32) 的奇偶性翻转颜色,
             * 是检验缩放/对齐是否正确的经典参考图案 */
            int cx = (x / 32) ^ (y / 32);
            if (cx) {
                r = (uint8_t)(r * 0.7f);
                g = (uint8_t)(g * 0.7f);
                b = (uint8_t)(b * 0.7f);
            }

            /* 中心圆: 到窗口中心的归一化距离 < 0.2 即算在圆内。
             * 逐像素距离判断, 不需要任何画圆算法, 简单但开销
             * 与分辨率成正比 —— 软件渲染的本质就是这样 */
            float dx = u - 0.5f, dy = v - 0.5f;
            float dist = sqrtf(dx*dx + dy*dy);
            if (dist < 0.2f) {
                r = 255; g = 255; b = 255;
            }

            /* stride == w*4 时的线性寻址: y*w+x;
             * 打包顺序高位→低位 X,R,G,B, 与模块 2 相同 */
            pixels[y * w + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

/* 每一帧都从零造一块新 buffer。优点: 简单直观, 完全不用管
 * "上一块 buffer 是否已被合成器放手"; 缺点: 每帧一次临时文件 +
 * mmap + 全量重绘。生产客户端应复用 buffer 池(双/三缓冲)并只
 * 上报真正变化过的 damage 区域。 */
static struct wl_buffer *
create_frame(struct client_state *st, int width, int height, int frame)
{
    int stride = width * 4;
    int size   = stride * height;

    /* 与模块 2 相同的"匿名共享内存"手法: 建临时文件后立刻 unlink,
     * 名字消失而 fd 仍有效, 给 wl_shm_create_pool 用完即弃。 */
    char tmpfile[] = "/tmp/wayland-mod3-XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) { perror("mkstemp"); return NULL; }
    unlink(tmpfile);

    if (ftruncate(fd, size) < 0) { perror("ftruncate"); close(fd); return NULL; }

    /* MAP_SHARED: 合成器进程通过对同一 fd 的映射读走像素 */
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) { perror("mmap"); close(fd); return NULL; }

    fill_pixels(pixels, width, height, frame);
    munmap(pixels, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ── callbacks ─────────────────────────────────────────── */

static void xdg_wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t s)
    { (void)d; xdg_wm_base_pong(b, s); }

static void xdg_surface_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    (void)d; xdg_surface_ack_configure(s, serial);
}

static void xdg_toplevel_configure(void *d, struct xdg_toplevel *t,
                                    int32_t w, int32_t h, struct wl_array *st)
{
    struct client_state *cs = d; (void)t; (void)st;
    if (w > 0) cs->width = w;
    if (h > 0) cs->height = h;
}

static void xdg_toplevel_close(void *d, struct xdg_toplevel *t)
    { (void)t; ((struct client_state *)d)->running = 0; }

static void registry_global(void *d, struct wl_registry *r,
                             uint32_t n, const char *iface, uint32_t ver)
{
    /* 同模块 2: 按接口名 bind 需要的全局对象, 版本取"自己代码
     * 所支持的"而不是服务端最高版(min(要的, 广播的)生效)。 */
    struct client_state *cs = d;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        cs->compositor = wl_registry_bind(r, n, &wl_compositor_interface, 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        cs->shm = wl_registry_bind(r, n, &wl_shm_interface, 1);
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
        cs->wm_base = wl_registry_bind(r, n, &xdg_wm_base_interface, 2);
}

static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n)
    { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg_listener = {
    .global = registry_global, .global_remove = registry_global_remove
};

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping
};

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure
};

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure, .close = xdg_toplevel_close
};

int main(void)
{
    struct client_state st = { .width=640, .height=480, .running=1 };

    printf("=== 模块 3: 手动像素绘制 ===\n");
    printf("理解: WL_SHM_FORMAT_XRGB8888 = 每像素 4 字节 (B,G,R,A)\n");
    printf("      像素数据通过共享内存 (wl_shm_pool) 传递给 compositor.\n\n");

    st.display = wl_display_connect(NULL);
    if (!st.display) { fprintf(stderr,"no display\n"); return 1; }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &reg_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.compositor || !st.shm || !st.wm_base) {
        fprintf(stderr,"missing globals\n"); return 1;
    }

    xdg_wm_base_add_listener(st.wm_base, &wm_base_listener, &st);
    st.surface = wl_compositor_create_surface(st.compositor);
    st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    xdg_surface_add_listener(st.xdg_surface, &xdg_surface_listener, &st);
    st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
    xdg_toplevel_add_listener(st.xdg_toplevel, &xdg_toplevel_listener, &st);
    xdg_toplevel_set_title(st.xdg_toplevel, "模块3 - 像素动画");

    /* 空提交: 让 xdg 角色生效以换取首次 configure(同模块 2) */
    wl_surface_commit(st.surface);
    wl_display_roundtrip(st.display);

    /* 朴素动画循环: 每轮 dispatch 先把积压的窗口事件处理完
     * (resize/close), 再画一帧。usleep(16000) 把节奏压在约 60fps
     * —— 这是"拍脑袋"的节拍, 与屏幕刷新率毫无对齐关系,
     * 既可能白画被丢弃的帧, 也可能节奏抖动; 模块 6 的
     * frame callback 才是正统解法。 */
    printf("动画循环开始 (每帧使用新 buffer, 演示 Attach-Commit 模型)...\n");
    while (st.running && wl_display_dispatch(st.display) != -1) {
        struct wl_buffer *buf = create_frame(&st, st.width, st.height, st.frame_count);
        if (buf) {
            wl_surface_attach(st.surface, buf, 0, 0);
            /* damage = "这块区域我改过了", 合成器可据此跳过
             * 无需重绘的部分; 这里保守地报整块 buffer。
             * commit 后立刻 destroy 客户端代理是安全的:
             * 代理没了, 服务端仍持有自己的那份引用。 */
            wl_surface_damage_buffer(st.surface, 0, 0, st.width, st.height);
            wl_surface_commit(st.surface);
            wl_buffer_destroy(buf);
        }
        usleep(16000);
        st.frame_count++;
    }

    xdg_toplevel_destroy(st.xdg_toplevel);
    xdg_surface_destroy(st.xdg_surface);
    wl_surface_destroy(st.surface);
    xdg_wm_base_destroy(st.wm_base);
    wl_compositor_destroy(st.compositor);
    wl_shm_destroy(st.shm);
    wl_registry_destroy(st.registry);
    wl_display_disconnect(st.display);
    printf("\n=== 模块 3 完成 ===\n");
    return 0;
}
