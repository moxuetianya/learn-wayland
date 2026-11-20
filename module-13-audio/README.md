# 模块 13 — 音频栈实验：ALSA 与 PipeWire

> **全景图定位**: [docs/03 音频栈](../docs/03-audio-stack.md) — 声音世界的「模块 1 + 模块 9」：从两个视角亲手发声。
> **前置**: 读一遍 [docs/03](../docs/03-audio-stack.md) · **产出**: 四个命令打通「PCM → ALSA → PipeWire → 扬声器」

## 目标

- 用 **libasound** 直接播放 PCM（内核层直连视角 + 周期/帧的体感）
- 作为 **node 接入 PipeWire 图**（服务器层视角，对照 [模块 8](../module-08-server-skeleton/) 的 registry/bind）
- 亲眼验证「PipeWire 底下还是 ALSA」的分层不变性

## 原理讲解（速览，详见 docs/03）

```
本模块两条播放路径, 同样的 S16_LE/2ch/44100 正弦波:

A. alsa-play default ──▶ libasound 插件 ──▶ (PipeWire 接管 default) ──▶ 硬件
B. pw-play           ──▶ libpipewire ──▶ node graph (混音/路由) ──▶ libasound ──▶ 硬件
                          ↑ WirePlumber 把流自动连到默认 sink (AUTOCONNECT)
```

关键对照点：

| | alsa-play `hw:0,0` | alsa-play `default` | pw-play |
|---|---|---|---|
| 独占设备？ | 是（其他应用静音/失败） | 否（被 PipeWire 混音） | 否 |
| 参数不符时 | 失败 | plug 插件转换 | 图内重采样 |
| 播放时机由谁驱动 | 自己 writei | PipeWire 调度 | `on_process` 回调（图节拍） |

`pw-play` 的 `on_process` 结构与 [模块 6 的帧回调](../module-06-frame-cb/) 是同构的：**都是「渲染发生在回调里、由系统节拍驱动」**——音频按周期节拍，视频按 vblank 节拍。

## 代码走读

[`main.c`](main.c) 四个子命令：

| 函数 | 要点 |
|------|------|
| `cmd_alsa_list` [L34](main.c#L34) | `snd_device_name_hint` 枚举逻辑设备——注意 `default`/`pipewire`/`pulse` 都是**用户态插件**，`hw:x,y` 才是硬件 |
| `cmd_alsa_play` [L59](main.c#L59) | open→set_params（参数协商）→ 周期循环 writei→drain；`snd_pcm_recover` 处理 underrun |
| `cmd_pw_list` [L255](main.c#L255) | context→connect→registry 枚举 node + **roundtrip**（`pw_core_sync`+`done` 回调，对比 `wl_display_roundtrip`） |
| `cmd_pw_play` [L140](main.c#L140) | thread_loop + `pw_stream_new_simple` + `spa_format_audio_raw_build` 声明格式 + `AUTOCONNECT` 接入图 |

## 动手实验

```bash
cd module-13-audio
make

./audio-lab alsa-list
./audio-lab alsa-play default 3 440      # A4 标准音
./audio-lab pw-list
./audio-lab pw-play 3 660                # E5
```

### 观察点（按 docs/03 的章节顺序）

1. **§2 PCM 体感**：把频率从 440 改成 220/880，听八度差；改 `0.25` 振幅因子感受响度=幅度；
2. **§3 周期可见**：播放时 `cat /proc/asound/card0/pcm0p/sub0/hw_params`——你会看到实际协商的 format/rate/period_size，这就是代码里 `snd_pcm_set_params` 的落点；
3. **§4 独占性实验**：先 `./audio-lab alsa-play hw:0,0 10`，同时另开终端再跑一个 `hw:0,0`——第二个报 `Device or resource busy`。然后两个 `pw-play` 同时跑——**混音成功**。这就是声音服务器存在的理由；
4. **§5 图的观察**：跑 `pw-top`（保持运行），再 `pw-play 5`——列表里出现 `audio-lab` 节点，看它的 QUANT（周期）和 CPU 占用；结束后节点消失；
5. **§5 路由实验**：`wpctl status` 找到 sink id，`pw-play` 时用 `pw-cli connect <你的节点id> <目标sink的input port>` 手动改接线路由（进阶）；
6. **分层考古**：`pw-list` 里找到 ALSA sink 节点 → `pw-dump <node>` 看它的 `api.alsa.*` 属性——**PipeWire 也是拿 ALSA 用户库碰硬件的**，分层从未被推翻；
7. **对照模块 1**：`pw-list` 的 registry 枚举 vs 模块 1 的 `wl_registry`——两套系统连服务发现的模式都一样（bind/global）。

## 进阶练习

1. **录音方向**：把 `alsa-play` 改成 capture（`SND_PCM_STREAM_CAPTURE` + `readi`），实现 5 秒录音回放——录音与播放是同一套 source/sink 对称模型，见 [docs/03 §5.1](../docs/03-audio-stack.md)；
2. **可视化**：pw-play 的 `on_process` 里把样本幅度画成字符条（`printf("\r%.*s", n, "▮▮▮...")`)——自制电平表；
3. **双音**：440 + 442 Hz 同时播放，听拍频现象（物理课的浪漫）；
4. **延迟测量**：pw-play 里打时间戳，对比 `pw-top` 的延迟列，估算端到端延迟 = 量化 × 周期数；
5. **成为 portal 视频**（大工程）：读 `xdg-desktop-portal` 的 ScreenCast 接口，把 [模块 12](../module-12-full-compositor/) 的屏幕内容打成 PipeWire 视频流——课程终极交叉练习。

## 常见坑

| 现象 | 原因 |
|------|------|
| alsa-play default 卡死无声 | PipeWire 未运行时 default 指到不存在的 pulse 插件；用 `pw-play` 或检查 `systemctl --user status pipewire` |
| hw:0,0 打开失败 EBUSY | 硬件被 PipeWire 独占——这正是实验观察点 3 想给你看的 |
| pw-play 无声但状态 running | 默认 sink 是虚拟设备（本机无声卡时 `auto_null`）；`wpctl status` 检查路由 |
| 编译缺 asoundlib.h | `libasound2-dev` 未装（setup-vm.sh 已含） |
| pw-list 空或连不上 | 用户级服务问题：`systemctl --user restart pipewire wireplumber` |

## 延伸阅读

- [docs/03-audio-stack.md](../docs/03-audio-stack.md) — 本模块的理论底座
- [PipeWire: audio-src 示例](https://docs.pipewire.org/page_tutorial2.html) — 本代码的官方亲戚

下一模块: [模块 14 — 登录与会话观察](../module-14-session/)
