#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

struct dumb_fb {
    uint32_t fb_id;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
    uint8_t *map;
    uint32_t width;
    uint32_t height;
};

static int find_crtc(int fd, drmModeRes *res, drmModeConnector *conn) {
    /* Try encoder's current CRTC first */
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc) {
            int crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
            if (crtc_id) {
                for (int i = 0; i < res->count_crtcs; i++)
                    if (res->crtcs[i] == (uint32_t)crtc_id)
                        return crtc_id;
            }
        }
    }

    /* Fallback: walk the connector's encoders and their possible CRTCs */
    for (int e = 0; e < conn->count_encoders; e++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (!enc)
            continue;
        for (int i = 0; i < res->count_crtcs; i++) {
            if (enc->possible_crtcs & (1 << i)) {
                int crtc_id = res->crtcs[i];
                drmModeFreeEncoder(enc);
                return crtc_id;
            }
        }
        drmModeFreeEncoder(enc);
    }

    return -1;
}

static void fill_buffer(uint8_t *ptr, int w, int h, int stride) {
    /* Draw a test pattern: red gradient X, green gradient Y, blue constant */
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(ptr + y * stride);
        for (int x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x * 255) / w);
            uint8_t g = (uint8_t)((y * 255) / h);
            uint8_t b = 128;
            row[x] = (uint32_t)(r << 16) | (uint32_t)(g << 8) | b;
        }
    }
}

static int create_dumb_fb(int fd, uint32_t width, uint32_t height,
                          struct dumb_fb *out) {
    struct drm_mode_create_dumb create = {0};
    struct drm_mode_map_dumb map = {0};
    struct drm_mode_destroy_dumb destroy = {0};
    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};

    memset(out, 0, sizeof(*out));

    create.width = width;
    create.height = height;
    create.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return -1;
    }

    out->handle = create.handle;
    out->pitch = create.pitch;
    out->size = create.size;
    out->width = width;
    out->height = height;

    /* Create a framebuffer object referencing the dumb buffer */
    handles[0] = create.handle;
    pitches[0] = create.pitch;
    offsets[0] = 0;
    if (drmModeAddFB2(fd, width, height, DRM_FORMAT_XRGB8888, handles, pitches,
                      offsets, &out->fb_id, 0) < 0) {
        perror("drmModeAddFB2");
        goto err_destroy;
    }

    /* Prepare the buffer for memory mapping */
    map.handle = create.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        goto err_rmfb;
    }

    out->map = mmap(NULL, out->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                    map.offset);
    if (out->map == MAP_FAILED) {
        perror("mmap");
        goto err_rmfb;
    }

    memset(out->map, 0, out->size);
    return 0;

err_rmfb:
    drmModeRmFB(fd, out->fb_id);
err_destroy:
    destroy.handle = create.handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    return -1;
}

static void destroy_dumb_fb(int fd, struct dumb_fb *fb) {
    struct drm_mode_destroy_dumb destroy = {0};

    if (fb->map && fb->map != MAP_FAILED)
        munmap(fb->map, fb->size);
    if (fb->fb_id)
        drmModeRmFB(fd, fb->fb_id);
    if (fb->handle) {
        destroy.handle = fb->handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    memset(fb, 0, sizeof(*fb));
}

/* 打开 DRM 设备: 优先 $DRM_CARD 指定, 否则自动扫描第一个 card[0-9]*。
 * 显卡不一定叫 card0 —— 例如 virtio-gpu 常与 simpledrm 共存时注册为 card1。 */
static int open_drm_card(const char **used) {
    const char *env = getenv("DRM_CARD");
    if (env && *env) {
        *used = env;
        return open(env, O_RDWR | O_CLOEXEC);
    }

    static char path[32];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            *used = path;
            return fd;
        }
    }
    *used = "(none)";
    return -1;
}

int main(void) {
    int fd;
    const char *card_path = NULL;
    drmModeRes *resources = NULL;
    drmModeConnector *conn = NULL;
    drmModeModeInfo *mode = NULL;
    drmModeCrtc *saved_crtc = NULL;
    struct dumb_fb fb = {0};
    int crtc_id = -1;
    uint32_t conn_id = 0;
    int became_master = 0;
    int ret = 1;

    printf("=== 模块 9: DRM/KMS 直接屏幕控制 ===\n");
    printf("从 TTY 运行: Ctrl+Alt+F3 登录后执行 sudo ./drm-demo\n\n");

    fd = open_drm_card(&card_path);
    if (fd < 0) {
        fprintf(stderr, "无法打开任何 /dev/dri/card[0-9]* (可用 DRM_CARD=/dev/dri/cardX 指定)\n");
        fprintf(stderr, "提示: 必须从 TTY 运行且可能需要 sudo\n");
        return 1;
    }
    printf("使用 DRM 设备: %s%s\n", card_path,
           getenv("DRM_CARD") ? " (来自 $DRM_CARD)" : " (自动扫描)");

    /* Become DRM master so we are allowed to change modes. On a TTY without
     * a running compositor this normally succeeds; if another master (X11 /
     * Wayland) holds the device it fails with EBUSY / EACCES. */
    if (drmSetMaster(fd) == 0) {
        became_master = 1;
    } else {
        fprintf(stderr,
                "警告: drmSetMaster 失败 (%s)，可能有其它 DRM master 正在运行。\n"
                "请从纯 TTY 运行 (无 X11/Wayland)。\n",
                strerror(errno));
    }

    resources = drmModeGetResources(fd);
    if (!resources) {
        perror("drmModeGetResources");
        goto cleanup;
    }

    printf("资源: CRTCs=%d, connectors=%d, encoders=%d, fbs=%d\n",
           resources->count_crtcs, resources->count_connectors,
           resources->count_encoders, resources->count_fbs);

    /* Find the first connected connector that has at least one mode */
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, resources->connectors[i]);
        if (!c)
            continue;

        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            conn = c;
            conn_id = c->connector_id;
            printf("已连接的 connector id=%u，模式数=%d\n", conn_id,
                   c->count_modes);
            break;
        }
        drmModeFreeConnector(c);
    }

    if (!conn) {
        fprintf(stderr, "找不到已连接且带有效模式的 connector\n");
        goto cleanup;
    }

    /* Prefer the connector's preferred mode, otherwise the first one */
    for (int i = 0; i < conn->count_modes; i++) {
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            mode = &conn->modes[i];
            break;
        }
    }
    if (!mode)
        mode = &conn->modes[0];

    printf("选中的模式: %s %ux%u @ %uHz\n", mode->name, mode->hdisplay,
           mode->vdisplay, mode->vrefresh);

    /* Find a CRTC we can drive this connector with */
    crtc_id = find_crtc(fd, resources, conn);
    if (crtc_id < 0) {
        fprintf(stderr, "找不到合适的 CRTC\n");
        goto cleanup;
    }
    printf("选中的 CRTC id=%d\n", crtc_id);

    /* Save current CRTC state so we can restore it later */
    saved_crtc = drmModeGetCrtc(fd, crtc_id);

    /* Create and fill a dumb buffer */
    if (create_dumb_fb(fd, mode->hdisplay, mode->vdisplay, &fb) < 0) {
        fprintf(stderr, "创建 dumb buffer 失败\n");
        goto cleanup;
    }
    printf("dumb buffer: fb_id=%u handle=%u pitch=%u size=%lu\n", fb.fb_id,
           fb.handle, fb.pitch, (unsigned long)fb.size);

    fill_buffer(fb.map, fb.width, fb.height, fb.pitch);

    /* Program the CRTC to scan out our framebuffer */
    if (drmModeSetCrtc(fd, crtc_id, fb.fb_id, 0, 0, &conn_id, 1, mode) < 0) {
        perror("drmModeSetCrtc");
        goto cleanup;
    }

    printf("已显示测试图案，5 秒后恢复...\n");
    sleep(5);

    ret = 0;

cleanup:
    /* Restore the original CRTC configuration */
    if (saved_crtc) {
        if (crtc_id >= 0) {
            drmModeSetCrtc(fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
                           saved_crtc->x, saved_crtc->y, &conn_id, 1,
                           &saved_crtc->mode);
        }
        drmModeFreeCrtc(saved_crtc);
    }

    destroy_dumb_fb(fd, &fb);

    if (conn)
        drmModeFreeConnector(conn);
    if (resources)
        drmModeFreeResources(resources);

    if (became_master)
        drmDropMaster(fd);

    close(fd);
    return ret;
}
