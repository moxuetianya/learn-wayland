/* main.c — 模块 13: Linux 音频栈动手实验
 *
 * 四个子命令, 分别从「内核 ALSA 视角」和「PipeWire 服务器视角」体验同一段声音:
 *
 *   ./audio-lab alsa-list            列出 ALSA PCM 设备 (内核层视角)
 *   ./audio-lab alsa-play [dev] [秒] [频率]   用 libasound 直接播放正弦波
 *                                            dev 默认 "default" (可能被 PipeWire 接管!)
 *                                            可试 "hw:0,0" 直连硬件
 *   ./audio-lab pw-list              列出 PipeWire 图中的 node (服务器层视角)
 *   ./audio-lab pw-play [秒] [频率]  作为 PipeWire 节点播放正弦波
 *
 * 对应文档: ../docs/03-audio-stack.md
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#define DEFAULT_RATE     44100
#define DEFAULT_FREQ     440.0
#define DEFAULT_SECONDS  3

/* ================================================================== */
/* ALSA 部分 — 内核层视角                                              */
/* ================================================================== */

static int cmd_alsa_list(void) {
    void **hints = NULL;
    int n = 0;

    printf("=== ALSA PCM 设备 (snd_device_name_hint) ===\n");
    printf("这些是 libasound 看到的「逻辑设备」。default/plug:*/dmix 是用户态插件,\n");
    printf("hw:x,y 才是直通内核的硬件节点。\n\n");
    printf("  %-24s %-28s %s\n", "NAME", "DESC", "IOID");

    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        fprintf(stderr, "snd_device_name_hint 失败\n");
        return 1;
    }
    for (void **h = hints; *h; h++) {
        char *name  = snd_device_name_get_hint(*h, "NAME");
        char *desc  = snd_device_name_get_hint(*h, "DESC");
        char *ioid  = snd_device_name_get_hint(*h, "IOID");
        printf("  %-24s %-28.28s %s\n",
               name ? name : "?", desc ? desc : "", ioid ? ioid : "");
        free(name); free(desc); free(ioid);
        n++;
    }
    snd_device_name_free_hint(hints);

    printf("\n共 %d 个逻辑设备。\n", n);
    printf("再运行 `cat /proc/asound/cards` 和 `aplay -l` 对照「逻辑设备 vs 物理声卡」。\n");
    return 0;
}

static int cmd_alsa_play(const char *device, int seconds, double freq) {
    snd_pcm_t *pcm = NULL;
    int err;

    /* 一次写入的帧数 (相当于一个 "周期" 的大小, 见 docs/03 §3) */
    const snd_pcm_uframes_t period_frames = 512;
    int16_t *samples = malloc(period_frames * 2 * sizeof(int16_t));
    double phase = 0.0;
    long total_frames = (long)seconds * DEFAULT_RATE;

    printf("=== ALSA 播放: 设备=\"%s\" %d 秒 %.0f Hz ===\n", device, seconds, freq);
    printf("打开中... (若卡住: default 设备可能正被 PipeWire 独占, 或无该设备)\n");

    if ((err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "snd_pcm_open(%s) 失败: %s\n", device, snd_strerror(err));
        free(samples);
        return 1;
    }

    /* 参数协商: 格式 S16_LE / 交错 / 双声道 / 44100 / 允许重采样 / 0.5s 延迟 */
    if ((err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                                  SND_PCM_ACCESS_RW_INTERLEAVED,
                                  2, DEFAULT_RATE, 1, 500000)) < 0) {
        fprintf(stderr, "snd_pcm_set_params 失败: %s\n", snd_strerror(err));
        goto out;
    }
    printf("参数已协商: S16_LE interleaved 2ch %dHz (frame = 4 字节)\n", DEFAULT_RATE);

    printf("播放中... (hw_params 实况: cat /proc/asound/card0/pcm0p/sub0/hw_params)\n");
    while (total_frames > 0) {
        snd_pcm_uframes_t to_write =
            total_frames < (long)period_frames ? (snd_pcm_uframes_t)total_frames
                                               : period_frames;

        /* 填充一个周期的 PCM: 每帧 2 个样本 (立体声), 左右相同 */
        for (snd_pcm_uframes_t i = 0; i < to_write; i++) {
            int16_t v = (int16_t)(sin(phase) * 0.25 * 32767.0);
            samples[i * 2 + 0] = v;   /* L */
            samples[i * 2 + 1] = v;   /* R */
            phase += 2.0 * M_PI * freq / DEFAULT_RATE;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }

        snd_pcm_sframes_t written = snd_pcm_writei(pcm, samples, to_write);
        if (written < 0) {
            if (snd_pcm_recover(pcm, (int)written, 1) < 0) {
                fprintf(stderr, "writei 失败: %s\n", snd_strerror((int)written));
                goto out;
            }
            continue;
        }
        total_frames -= written;
    }

    /* 排空残余数据再关闭, 避免尾部爆音 */
    snd_pcm_drain(pcm);
    printf("完成。思考: 这段声音若是 hw:0,0 直连, 其他应用此刻能同时出声吗?\n");
    printf("      (答案在 docs/03 §4 —— 这就是声音服务器存在的理由)\n");

out:
    snd_pcm_close(pcm);
    free(samples);
    return 0;
}

/* ================================================================== */
/* PipeWire 部分 — 服务器层视角                                        */
/* ================================================================== */

struct pw_playback {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    double phase;
    double freq;
};

static void on_process(void *userdata) {
    struct pw_playback *p = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    int16_t *samples;
    uint32_t n_frames, n_channels = 2;
    int stride = (int)(sizeof(int16_t) * n_channels);

    if ((b = pw_stream_dequeue_buffer(p->stream)) == NULL) {
        pw_log_warn("没有可用 buffer");
        return;
    }

    buf = b->buffer;
    if (buf->datas[0].data == NULL)
        return;

    /* requested 是图调度器要的帧数 (PipeWire >= 0.3.49 才有此字段);
     * 旧版没有调度器建议, 就用整个 buffer —— 语义等价于 requested == 0 */
#if PW_CHECK_VERSION(0, 3, 49)
    n_frames = b->requested ? (uint32_t)b->requested
                            : (uint32_t)(buf->datas[0].maxsize / stride);
#else
    n_frames = (uint32_t)(buf->datas[0].maxsize / stride);
#endif

    samples = (int16_t *)buf->datas[0].data;
    for (uint32_t i = 0; i < n_frames; i++) {
        int16_t v = (int16_t)(sin(p->phase) * 0.25 * 32767.0);
        samples[i * 2 + 0] = v;
        samples[i * 2 + 1] = v;
        p->phase += 2.0 * M_PI * p->freq / DEFAULT_RATE;
        if (p->phase > 2.0 * M_PI) p->phase -= 2.0 * M_PI;
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->size   = n_frames * (uint32_t)stride;
    buf->datas[0].chunk->stride = (uint32_t)stride;

    pw_stream_queue_buffer(p->stream, b);
}

static void on_state_changed(void *userdata, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error) {
    struct pw_playback *p = userdata;
    printf("  [流状态] %-12s → %s%s%s\n", pw_stream_state_as_string(old),
           pw_stream_state_as_string(state),
           error ? " (" : "", error ? error : (error ? ")" : ""));
    (void)p;
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .process = on_process,
};

static int cmd_pw_play(int seconds, double freq) {
    struct pw_playback data = { .freq = freq };
    const struct spa_pod *params[1];
    uint8_t pod_buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    struct pw_properties *props;
    int ret = 0;

    printf("=== PipeWire 播放: %d 秒 %.0f Hz ===\n", seconds, freq);
    printf("我们将作为一个 node 接入 PipeWire 图 (docs/03 §5),\n");
    printf("由它混音/路由后交给 ALSA。播放时另开终端跑 pw-top 观察本节点!\n\n");

    pw_init(NULL, NULL);

    data.loop = pw_thread_loop_new("audio-lab", NULL);
    if (!data.loop) { perror("pw_thread_loop_new"); return 1; }

    props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Music",
        PW_KEY_APP_NAME,       "learn-wayland-audio-lab",
        NULL);
    if (!props) { fprintf(stderr, "pw_properties_new 失败\n"); return 1; }

    data.stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(data.loop), "audio-lab",
        props, &stream_events, &data);
    if (!data.stream) { fprintf(stderr, "pw_stream_new_simple 失败\n"); return 1; }

    /* 声明支持的原始格式: S16_LE / 2ch / 44100 (与 alsa-play 完全一致的参数!) */
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_S16_LE,
            .channels = 2,
            .rate = DEFAULT_RATE));

    /* AUTOCONNECT: 让会话管理器 (WirePlumber) 自动连到默认输出 */
    pw_stream_connect(data.stream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS |
                      PW_STREAM_FLAG_RT_PROCESS,
                      params, 1);

    if (pw_thread_loop_start(data.loop) < 0) {
        fprintf(stderr, "线程循环启动失败\n");
        return 1;
    }

    printf("播放 %d 秒...\n", seconds);
    sleep(seconds);

    pw_thread_loop_lock(data.loop);
    pw_stream_destroy(data.stream);
    pw_thread_loop_unlock(data.loop);
    pw_thread_loop_stop(data.loop);
    pw_thread_loop_destroy(data.loop);
    pw_deinit();

    printf("完成。对比: alsa-play 直连时 CPU 直接喂硬件; 刚才我们是把 PCM\n");
    printf("写进共享内存 buffer, 由 PipeWire 调度器驱动消费 (docs/03 §5 的图)。\n");
    return ret;
}

/* ---------------- pw-list: 列出图中的 node ---------------- */

struct pw_list_ctx {
    struct pw_thread_loop *loop;
    struct pw_core *core;
    int node_count;
};

/* core 的 done 回调: pw_core_sync 的应答到达 → 唤醒等待中的主逻辑 (roundtrip) */
static void core_done(void *userdata, uint32_t id, int seq) {
    struct pw_list_ctx *ctx = userdata;
    (void)id; (void)seq;
    pw_thread_loop_signal(ctx->loop, false);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = core_done,
};

static void registry_global(void *userdata, uint32_t id, uint32_t permissions,
                            const char *type, uint32_t version,
                            const struct spa_dict *props) {
    struct pw_list_ctx *ctx = userdata;
    (void)id; (void)permissions; (void)version;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    const char *name  = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *desc  = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char *mcls  = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);

    printf("  [node %2d] %-12s %-28.28s %s\n",
           ++ctx->node_count,
           mcls ? mcls : "?",
           name ? name : (desc ? desc : "?"),
           desc ? desc : "");
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
};

static int cmd_pw_list(void) {
    struct pw_list_ctx ctx = {0};
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook listener;

    printf("=== PipeWire 图中的 node (registry 枚举) ===\n");
    printf("每一个 node 都是图中的一个处理单元 (docs/03 §5):\n");
    printf("  Audio/Sink   = 输出设备端点   Audio/Source = 输入(麦克风)端点\n");
    printf("  Stream/Output= 某应用的播放流  Stream/Input = 某应用的录音流\n\n");
    printf("  %-10s %-12s %-28s %s\n", "", "MEDIA-CLASS", "NODE-NAME", "DESCRIPTION");
    printf("  ---------------------------------------------------------------\n");

    pw_init(NULL, NULL);

    ctx.loop = pw_thread_loop_new("audio-lab", NULL);
    if (!ctx.loop) { perror("pw_thread_loop_new"); return 1; }

    if (pw_thread_loop_start(ctx.loop) < 0) return 1;
    pw_thread_loop_lock(ctx.loop);

    context = pw_context_new(pw_thread_loop_get_loop(ctx.loop), NULL, 0);
    if (!context) { fprintf(stderr, "pw_context_new 失败\n"); return 1; }

    core = pw_context_connect(context, NULL, 0);
    if (!core) {
        fprintf(stderr, "无法连接 PipeWire 守护进程 (服务在跑吗? systemctl --user status pipewire)\n");
        return 1;
    }

    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    spa_zero(listener);
    pw_registry_add_listener(registry, &listener, &registry_events, &ctx);

    /* roundtrip 语义: sync 发一个带序号的 ping, done 回调唤醒下面的 wait,
     * 此时 registry 的 global 事件已全部到齐 —— 与模块 1 的
     * wl_display_roundtrip 解决同一个问题 (RPC 世界的通用同步原语) */
    struct spa_hook core_listener;
    spa_zero(core_listener);
    pw_core_add_listener(core, &core_listener, &core_events, &ctx);
    pw_core_sync(core, 0, 0);
    pw_thread_loop_wait(ctx.loop);

    pw_thread_loop_unlock(ctx.loop);
    pw_thread_loop_stop(ctx.loop);

    printf("\n共 %d 个 node。对照 `wpctl status` (WirePlumber 视角) 与 `pactl info`。\n",
           ctx.node_count);
    printf("注意其中的 ALSA sink —— PipeWire 也是通过 ALSA 用户库碰硬件的 (分层不变!)\n");
    return 0;
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

static void usage(const char *prog) {
    fprintf(stderr,
        "用法: %s <子命令> [参数]\n"
        "\n"
        "  alsa-list                       列出 ALSA PCM 设备\n"
        "  alsa-play [device] [秒] [Hz]    ALSA 播放正弦波 (默认 default 3 440)\n"
        "                                  device 可选 hw:0,0 直连硬件\n"
        "  pw-list                         列出 PipeWire 节点\n"
        "  pw-play [秒] [Hz]               作为 PipeWire 节点播放 (默认 3 440)\n",
        prog);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "alsa-list") == 0)
        return cmd_alsa_list();

    if (strcmp(cmd, "alsa-play") == 0) {
        const char *device = argc > 2 ? argv[2] : "default";
        int seconds = argc > 3 ? atoi(argv[3]) : DEFAULT_SECONDS;
        double freq = argc > 4 ? atof(argv[4]) : DEFAULT_FREQ;
        return cmd_alsa_play(device, seconds, freq);
    }

    if (strcmp(cmd, "pw-list") == 0)
        return cmd_pw_list();

    if (strcmp(cmd, "pw-play") == 0) {
        int seconds = argc > 2 ? atoi(argv[2]) : DEFAULT_SECONDS;
        double freq = argc > 3 ? atof(argv[3]) : DEFAULT_FREQ;
        return cmd_pw_play(seconds, freq);
    }

    usage(argv[0]);
    return 1;
}
