/*
 * drm-info — 模块 9 的旁路工具：把 KMS 对象关系与 EDID 内容打成人类可读的图。
 *
 * 和 drm-demo 的区别：drm-demo 只做「点亮屏幕」这一条路，drm-info 只读不写，
 * 把 drmModeGetResources() 那一族 API 的全部结果摊开：
 *
 *   connector → encoder → CRTC → framebuffer   （谁在驱动谁）
 *   connector 的 modes[]                       （时序从哪来）
 *   connector 的 EDID blob                     （显示器到底声明了什么）
 *   connector 的属性表                          （DPMS / link-status / …）
 *
 * 用法：
 *   sudo ./drm-info                        # 自动探测 /dev/dri/card0…card9
 *   sudo ./drm-info -D /dev/dri/card0 -c 37
 *          ./drm-info -e /sys/class/drm/card0-DP-1/edid   # 只解码 EDID，无需设备
 *
 * 只依赖 libdrm（和 drm-demo 一样），可以在没有图形会话的纯 TTY 下跑。
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define EDID_BLOCK_LEN 128

/* ------------------------------------------------------------------ */
/* 小工具：名字表与格式化                                              */
/* ------------------------------------------------------------------ */

static const char *conn_type_name(uint32_t type) {
    switch (type) {
    case DRM_MODE_CONNECTOR_VGA:      return "VGA";
    case DRM_MODE_CONNECTOR_DVII:     return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID:     return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA:     return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite:return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO:   return "SVIDEO";
    case DRM_MODE_CONNECTOR_LVDS:     return "LVDS";
    case DRM_MODE_CONNECTOR_Component:return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN:  return "DIN";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_HDMIA:    return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:    return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV:       return "TV";
    case DRM_MODE_CONNECTOR_eDP:      return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL:  return "Virtual";
    case DRM_MODE_CONNECTOR_DSI:      return "DSI";
    case DRM_MODE_CONNECTOR_DPI:      return "DPI";
    case DRM_MODE_CONNECTOR_WRITEBACK:return "Writeback";
    case DRM_MODE_CONNECTOR_SPI:      return "SPI";
    case DRM_MODE_CONNECTOR_USB:      return "USB";
    default:                          return "?";
    }
}

/* 把 mode->flags 翻译成 modetest 风格的短标签（nhsync、interlace…） */
static void mode_flags_str(uint32_t flags, char *out, size_t n) {
    static const struct { uint32_t bit; const char *name; } map[] = {
        { DRM_MODE_FLAG_NHSYNC,   "nhsync"   },
        { DRM_MODE_FLAG_PHSYNC,   "phsync"   },
        { DRM_MODE_FLAG_NVSYNC,   "nvsync"   },
        { DRM_MODE_FLAG_PVSYNC,   "pvsync"   },
        { DRM_MODE_FLAG_INTERLACE,"interlace"},
        { DRM_MODE_FLAG_DBLSCAN,  "dblscan"  },
        { DRM_MODE_FLAG_CSYNC,    "csync"    },
        { DRM_MODE_FLAG_PCSYNC,   "pcsync"   },
        { DRM_MODE_FLAG_NCSYNC,   "ncsync"   },
    };
    size_t used = 0;

    out[0] = '\0';
    for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if (!(flags & map[i].bit))
            continue;
        int k = snprintf(out + used, n - used, "%s%s",
                         used ? "," : "", map[i].name);
        if (k < 0 || (size_t)k >= n - used)
            return;
        used += (size_t)k;
    }
}

/* DRM_MODE_TYPE_* 同理：preferred / driver / userdef … */
static void mode_type_str(uint32_t type, char *out, size_t n) {
    static const struct { uint32_t bit; const char *name; } map[] = {
        { DRM_MODE_TYPE_BUILTIN,  "builtin"  },
        { DRM_MODE_TYPE_CLOCK_C,  "clock-c"  },
        { DRM_MODE_TYPE_CRTC_C,   "crtc-c"   },
        { DRM_MODE_TYPE_PREFERRED,"preferred"},
        { DRM_MODE_TYPE_DEFAULT,  "default"  },
        { DRM_MODE_TYPE_USERDEF,  "userdef"  },
        { DRM_MODE_TYPE_DRIVER,   "driver"   },
    };
    size_t used = 0;

    out[0] = '\0';
    for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if (!(type & map[i].bit))
            continue;
        int k = snprintf(out + used, n - used, "%s%s",
                         used ? "," : "", map[i].name);
        if (k < 0 || (size_t)k >= n - used)
            return;
        used += (size_t)k;
    }
    if (!used)
        snprintf(out, n, "-");
}

/* refresh 是派生量：clock / (htotal * vtotal)，隔行和倍扫要修正 */
static double mode_refresh(const drmModeModeInfo *m) {
    unsigned int num = m->clock * 1000u;
    unsigned int den = m->htotal * m->vtotal;

    if (m->flags & DRM_MODE_FLAG_INTERLACE)
        num *= 2;
    if (m->flags & DRM_MODE_FLAG_DBLSCAN)
        den *= 2;
    if (m->vscan > 1)
        den *= m->vscan;
    return den ? (double)num / (double)den : 0.0;
}

/* ------------------------------------------------------------------ */
/* EDID 解码                                                           */
/* ------------------------------------------------------------------ */

static void str_trim(char *s) {
    size_t n = strlen(s);

    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

/* 18 字节 DTD：像素时钟 + 有效/消隐 + 前后肩 + 同步宽 + 尺寸 + 极性 */
static void edid_print_dtd(const uint8_t *b, const char *indent, int index) {
    unsigned int clock = b[0] | (b[1] << 8);            /* 10 kHz 单位 */
    unsigned int ha = b[2] | ((b[4] & 0xf0) << 4);      /* h active */
    unsigned int hb = b[3] | ((b[4] & 0x0f) << 8);      /* h blanking */
    unsigned int va = b[5] | ((b[7] & 0xf0) << 4);      /* v active */
    unsigned int vb = b[6] | ((b[7] & 0x0f) << 8);      /* v blanking */
    unsigned int hfp = b[8] | ((b[11] & 0xc0) << 2);    /* h front porch */
    unsigned int hsw = b[9] | ((b[11] & 0x30) << 4);    /* h sync width */
    unsigned int vfp = (b[10] >> 4) | ((b[11] & 0x0c) << 2);
    unsigned int vsw = (b[10] & 0x0f) | ((b[11] & 0x03) << 4);
    unsigned int hsize = b[12] | ((b[14] & 0xf0) << 4);
    unsigned int vsize = b[13] | ((b[14] & 0x0f) << 8);
    unsigned int htot = ha + hb;
    unsigned int vtot = va + vb;
    double refresh = (htot && vtot)
        ? (double)clock * 10000.0 / ((double)htot * (double)vtot) : 0.0;
    const char *pol = "";

    /* flags: bits[4:3]=11 表示「数字分离同步」，此时 bit2=vsync、bit1=hsync 极性 */
    if (((b[17] >> 3) & 0x3) == 0x3)
        pol = (b[17] & 0x2) ? ((b[17] & 0x4) ? "phsync,pvsync" : "phsync,nvsync")
                            : ((b[17] & 0x4) ? "nhsync,pvsync" : "nhsync,nvsync");

    printf("%sDTD%-2d %ux%u@%.2fHz  clock=%u.%02uMHz  "
           "htot=%u vtot=%u  hfp=%u hsw=%u  vfp=%u vsw=%u  "
           "size=%ux%umm  %s%s\n",
           indent, index, ha, va, refresh, clock / 100, clock % 100,
           htot, vtot, hfp, hsw, vfp, vsw, hsize, vsize,
           pol, (b[17] & 0x80) ? " interlace" : "");
}

/* 4 个描述符槽：前两字节非 0 是 DTD，否则是按 tag 区分的文本/能力块 */
static void edid_print_descriptor(const uint8_t *b, const char *indent, int index) {
    char text[16];

    if (b[0] || b[1]) {
        edid_print_dtd(b, indent, index);
        return;
    }

    switch (b[3]) {
    case 0xfc:                                  /* monitor name */
        memcpy(text, b + 5, 13);
        text[13] = '\0';
        str_trim(text);
        printf("%s描述符%d 显示器名 \"%s\"\n", indent, index, text);
        break;
    case 0xfd:                                  /* monitor range limits */
        printf("%s描述符%d 支持范围 %u-%uHz, 行频 %u-%ukHz, 最大时钟 %uMHz\n",
               indent, index, b[5], b[6], b[7], b[8], b[9] * 10u);
        break;
    case 0xfe:                                  /* ASCII string */
        memcpy(text, b + 5, 13);
        text[13] = '\0';
        str_trim(text);
        printf("%s描述符%d 文本 \"%s\"\n", indent, index, text);
        break;
    case 0xff:
        memcpy(text, b + 5, 13);
        text[13] = '\0';
        str_trim(text);
        printf("%s描述符%d 序列号 \"%s\"\n", indent, index, text);
        break;
    case 0xf7:
        printf("%s描述符%d established timings III\n", indent, index);
        break;
    case 0xfb:
        printf("%s描述符%d 附加白点/色度\n", indent, index);
        break;
    case 0xfa:
        printf("%s描述符%d 附加 standard timings\n", indent, index);
        break;
    default:
        printf("%s描述符%d 未知类型 0x%02x\n", indent, index, b[3]);
        break;
    }
}

/* CEA-861 扩展块：数据块集合（VIC 列表 / 音频 / HDMI VSDB）+ 扩展 DTD */
static void edid_print_cea(const uint8_t *e, const char *indent) {
    unsigned int dtd_off = e[2];
    unsigned int p = 4, vic = 0, blocks = 0;
    bool hdmi_vsdb = false, hf_vsdb = false, audio = false;

    while (p < dtd_off && p + 1 <= EDID_BLOCK_LEN) {
        unsigned int tag = e[p] >> 5;
        unsigned int len = e[p] & 0x1f;

        if (p + 1 + len > EDID_BLOCK_LEN)
            break;
        blocks++;
        if (tag == 1)
            audio = true;
        else if (tag == 2)
            vic += len;                       /* 每个字节一个 VIC */
        else if (tag == 3 && len >= 3) {
            unsigned int oui = e[p + 1] | (e[p + 2] << 8) | (e[p + 3] << 16);

            if (oui == 0x000c03)
                hdmi_vsdb = true;
            else if (oui == 0xc45dd8)
                hf_vsdb = true;
        }
        p += 1 + len;
    }

    printf("%sCEA-861 rev%u: %u 个数据块, %u 个 VIC%s%s%s\n",
           indent, e[1], blocks, vic,
           audio ? ", 音频" : "",
           hdmi_vsdb ? ", HDMI VSDB" : "",
           hf_vsdb ? ", HDMI Forum VSDB(VRR/HDR)" : "");

    if (dtd_off < EDID_BLOCK_LEN) {
        int i = 1;

        for (unsigned int q = dtd_off; q + 18 <= EDID_BLOCK_LEN; q += 18) {
            if (!(e[q] || e[q + 1]))
                break;
            edid_print_dtd(e + q, indent, i++);
        }
    }
}

/* 解码一整段 EDID（可能含多个 128 字节块） */
static void edid_print(const uint8_t *d, size_t len, const char *label) {
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    unsigned int blocks;

    printf("EDID %s（%zu 字节）\n", label, len);
    if (len < EDID_BLOCK_LEN) {
        printf("  ⚠ 不足 128 字节，读不到有效 EDID\n");
        return;
    }
    if (memcmp(d, hdr, sizeof(hdr))) {
        printf("  ⚠ 魔数不是 00 FF FF FF FF FF FF 00，这不是 EDID\n");
        return;
    }

    blocks = 1u + d[126];
    if (len < blocks * EDID_BLOCK_LEN) {
        printf("  ⚠ 声明 %u 块但只有 %zu 字节，按实际长度解析\n",
               blocks, len / EDID_BLOCK_LEN);
        blocks = (unsigned int)(len / EDID_BLOCK_LEN);
    }

    /* 厂商 ID：3 个字母，每个 5 bit，大端装在 0x08-0x09 */
    printf("  厂商 %c%c%c  产品 0x%04x  序列 0x%08x  生产 %u-W%02u  EDID %u.%u  %u 块\n",
           ((d[8] >> 2) & 0x1f) + '@', (((d[8] & 0x3) << 3) | (d[9] >> 5)) + '@',
           (d[9] & 0x1f) + '@',
           d[10] | (d[11] << 8),                         /* 产品码 */
           d[12] | (d[13] << 8) | (d[14] << 16) | ((unsigned int)d[15] << 24),
           d[17] + 1990, d[16], d[18], d[19], blocks);

    for (int i = 0; i < 4; i++)
        edid_print_descriptor(d + 54 + i * 18, "  ", i + 1);

    for (unsigned int k = 1; k < blocks; k++) {
        const uint8_t *e = d + k * EDID_BLOCK_LEN;
        unsigned int sum = 0;

        for (int i = 0; i < EDID_BLOCK_LEN; i++)
            sum += e[i];
        if (sum & 0xff)
            printf("  ⚠ 扩展块 %u 校验和错误（0x%02x）\n", k, sum & 0xff);

        if (e[0] == 0x02)
            edid_print_cea(e, "  ");
        else if (e[0] == 0x70)
            printf("  DisplayID 扩展块（rev %u）\n", e[1]);
        else if (e[0] == 0x40)
            printf("  VESA DTI 扩展块\n");
        else if (e[0] == 0x00)
            printf("  扩展块 %u 为空\n", k);
        else
            printf("  未知扩展块 tag 0x%02x\n", e[0]);
    }

    /* 基础块校验和：128 字节求和应为 0 mod 256 */
    unsigned int sum = 0;

    for (int i = 0; i < EDID_BLOCK_LEN; i++)
        sum += d[i];
    if (sum & 0xff)
        printf("  ⚠ 基础块校验和错误（0x%02x）\n", sum & 0xff);
}

/* ------------------------------------------------------------------ */
/* KMS 枚举                                                            */
/* ------------------------------------------------------------------ */

/* connector 的属性表：EDID 是 blob，DPMS/link-status 是 enum，其余数字/只读 */
static void dump_connector_props(int fd, const drmModeConnector *conn) {
    for (int i = 0; i < conn->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, conn->props[i]);
        uint64_t value = conn->prop_values[i];

        if (!prop)
            continue;

        printf("      %-20s = ", prop->name);
        if (prop->flags & DRM_MODE_PROP_ENUM) {
            const char *name = NULL;

            for (int j = 0; j < prop->count_enums; j++) {
                if ((uint64_t)prop->enums[j].value == value) {
                    name = prop->enums[j].name;
                    break;
                }
            }
            if (name)
                printf("%s(%llu)", name, (unsigned long long)value);
            else
                printf("%llu", (unsigned long long)value);
        } else if (prop->flags & DRM_MODE_PROP_BLOB) {
            drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(fd, (uint32_t)value);

            if (blob) {
                printf("blob %u 字节", blob->length);
                drmModeFreePropertyBlob(blob);
            } else {
                printf("(无)");
            }
        } else {
            printf("%llu", (unsigned long long)value);
        }
        if (prop->flags & DRM_MODE_PROP_IMMUTABLE)
            printf("  [只读]");
        printf("\n");
        drmModeFreeProperty(prop);
    }
}

/* 从 connector 的属性里取 EDID blob 并解码。
 * 返回 false 表示这个 connector 根本没有 EDID 属性（如 qxl 的 Virtual 口）——
 * 内核 drm_connector_init() 对 VIRTUAL/WRITEBACK 类型默认不挂 EDID 属性。 */
static bool dump_connector_edid(int fd, const drmModeConnector *conn) {
    for (int i = 0; i < conn->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, conn->props[i]);
        bool is_edid, is_blob;

        if (!prop)
            continue;
        is_edid = !strcmp(prop->name, "EDID");
        is_blob = (prop->flags & DRM_MODE_PROP_BLOB) != 0;
        drmModeFreeProperty(prop);
        if (!is_edid)
            continue;
        if (!is_blob)
            return true;

        drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(fd, (uint32_t)conn->prop_values[i]);

        if (!blob)
            return true;
        if (!blob->length) {
            printf("      EDID: 0 字节（无 EDID → 驱动只能用兜底模式表）\n");
        } else {
            printf("      EDID:\n");
            edid_print(blob->data, blob->length, "connector 属性");
        }
        drmModeFreePropertyBlob(blob);
        return true;
    }
    return false;
}

static void dump_connector(int fd, const drmModeConnector *conn) {
    char flags[64], type[64];
    const char *name = conn_type_name(conn->connector_type);

    printf("\n[%u] %s-%u  %s  size=%ux%umm  modes=%d  encoders=%d\n",
           conn->connector_id, name, conn->connector_type_id,
           conn->connection == DRM_MODE_CONNECTED ? "connected" : "disconnected",
           conn->mmWidth, conn->mmHeight, conn->count_modes, conn->count_encoders);

    /* 当前管线：connector → encoder → CRTC → fb */
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);

        if (enc) {
            printf("      encoder %u (possible_crtcs=0x%x) -> ",
                   enc->encoder_id, enc->possible_crtcs);
            if (enc->crtc_id) {
                drmModeCrtc *crtc = drmModeGetCrtc(fd, enc->crtc_id);

                if (crtc) {
                    if (crtc->buffer_id)
                        printf("crtc %u (fb %u, %ux%u@%.2fHz)",
                               crtc->crtc_id, crtc->buffer_id,
                               crtc->mode.hdisplay, crtc->mode.vdisplay,
                               mode_refresh(&crtc->mode));
                    else
                        printf("crtc %u (未启用)", crtc->crtc_id);
                    drmModeFreeCrtc(crtc);
                }
            } else {
                printf("无 CRTC（未点亮）");
            }
            printf("\n");
            drmModeFreeEncoder(enc);
        }
    } else {
        printf("      encoder: 未绑定（这个口没在出图）\n");
    }

    for (int i = 0; i < conn->count_modes; i++) {
        const drmModeModeInfo *m = &conn->modes[i];

        mode_flags_str(m->flags, flags, sizeof(flags));
        mode_type_str(m->type, type, sizeof(type));
        printf("      #%-2d %-12s %7.2fHz %6ukHz  %ux%u  totals %ux%u  %s  %s\n",
               i, m->name, mode_refresh(m), m->clock,
               m->hdisplay, m->vdisplay, m->htotal, m->vtotal,
               flags[0] ? flags : "-", type);
    }

    if (!dump_connector_edid(fd, conn))
        printf("      EDID: 无此属性（VIRTUAL/WRITEBACK 类 connector 默认不挂）\n");
    printf("      属性:\n");
    dump_connector_props(fd, conn);

}

static void dump_crtcs(int fd, const drmModeRes *res) {
    printf("\nCRTC:\n");
    for (int i = 0; i < res->count_crtcs; i++) {
        drmModeCrtc *crtc = drmModeGetCrtc(fd, res->crtcs[i]);

        if (!crtc)
            continue;
        if (crtc->buffer_id)
            printf("  [%u] fb %u  %ux%u@%.2fHz  pos=(%d,%d)  已启用\n",
                   crtc->crtc_id, crtc->buffer_id,
                   crtc->mode.hdisplay, crtc->mode.vdisplay,
                   mode_refresh(&crtc->mode), crtc->x, crtc->y);
        else
            printf("  [%u] 未启用（fb 0）\n", crtc->crtc_id);
        drmModeFreeCrtc(crtc);
    }
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "用法: %s [-D /dev/dri/cardN] [-c connector_id] [-e edid_file]\n"
        "  -D  指定 DRM 设备（默认自动探测 /dev/dri/card0…card9）\n"
        "  -c  只打印这个 connector（id 见完整输出，如 37）\n"
        "  -e  只解码该文件里的 EDID（如 /sys/class/drm/card0-DP-1/edid），\n"
        "      不打开设备，因此不需要 root\n"
        "  点亮屏幕用同目录的 drm-demo；本工具只读不写\n",
        argv0);
}

static int open_card(const char *path, char *used, size_t n) {
    char buf[64];

    if (path) {
        int fd = open(path, O_RDWR | O_CLOEXEC);

        if (fd < 0)
            fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
            snprintf(used, n, "%s", path);
        return fd;
    }

    for (int i = 0; i < 10; i++) {
        snprintf(buf, sizeof(buf), "/dev/dri/card%d", i);
        int fd = open(buf, O_RDWR | O_CLOEXEC);

        if (fd < 0)
            fd = open(buf, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            snprintf(used, n, "%s", buf);
            return fd;
        }
    }
    return -1;
}

static int decode_edid_file(const char *path) {
    uint8_t *buf = NULL;
    size_t cap = 0, len = 0;
    FILE *f = fopen(path, "rb");

    if (!f) {
        fprintf(stderr, "打开 %s 失败: %s\n", path, strerror(errno));
        return 1;
    }
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 512;
            uint8_t *nbuf = realloc(buf, cap);

            if (!nbuf) {
                fprintf(stderr, "内存不足\n");
                free(buf);
                fclose(f);
                return 1;
            }
            buf = nbuf;
        }
        size_t got = fread(buf + len, 1, cap - len, f);

        len += got;
        if (got == 0)
            break;
    }
    fclose(f);
    edid_print(buf, len, path);
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    const char *device = NULL;
    const char *edid_file = NULL;
    long only_connector = -1;
    char used[64] = "";
    int opt, fd, ret = 0;

    while ((opt = getopt(argc, argv, "D:c:e:h")) != -1) {
        switch (opt) {
        case 'D': device = optarg; break;
        case 'c': only_connector = strtol(optarg, NULL, 10); break;
        case 'e': edid_file = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    if (edid_file)
        return decode_edid_file(edid_file);

    fd = open_card(device, used, sizeof(used));
    if (fd < 0) {
        fprintf(stderr,
                "打不开 DRM 设备%s%s: %s\n"
                "  提示：需要 root 或 video 组；用 `ls /dev/dri` 确认节点存在\n",
                device ? " " : "", device ? device : "（/dev/dri/card0…9）",
                strerror(errno));
        return 1;
    }

    drmVersion *ver = drmGetVersion(fd);

    printf("=== %s ===\n", used);
    if (ver) {
        printf("驱动   %s %d.%d.%d (%s)%s%s\n",
               ver->name, ver->version_major, ver->version_minor,
               ver->version_patchlevel, ver->date,
               ver->desc ? " — " : "", ver->desc ? ver->desc : "");
        drmFreeVersion(ver);
    }

    drmModeRes *res = drmModeGetResources(fd);

    if (!res) {
        fprintf(stderr, "drmModeGetResources 失败: %s（不是 KMS 设备？）\n",
                strerror(errno));
        close(fd);
        return 1;
    }

    printf("资源   CRTC×%d  connector×%d  encoder×%d  fb×%d\n",
           res->count_crtcs, res->count_connectors,
           res->count_encoders, res->count_fbs);
    printf("尺寸区间 %ux%u .. %ux%u\n",
           res->min_width, res->min_height, res->max_width, res->max_height);

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);

        if (!conn)
            continue;
        if (only_connector < 0 || (uint32_t)only_connector == conn->connector_id)
            dump_connector(fd, conn);
        drmModeFreeConnector(conn);
    }

    if (only_connector < 0)
        dump_crtcs(fd, res);

    drmModeFreeResources(res);
    close(fd);
    return ret;
}
