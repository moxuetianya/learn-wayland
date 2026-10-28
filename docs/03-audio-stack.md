# 03 — Linux 音频栈：从 PCM 到 PipeWire

> 对应诉求：**涉及音频的部分要有一定讲解**。Wayland 协议本身只管显示与输入、完全不含音频，但一个能用的桌面离不开声音。本文讲清「一段声音从应用代码到扬声器」经过的每一层，以及为什么 Wayland 桌面的标准答案叫 PipeWire。动手部分对应 [模块 13](../module-13-audio/)。

---

## 1. 先建立坐标：音频栈全景

和 [00 号文档](00-big-picture.md) 的图形栈对照着看——音频是一条结构惊人相似的平行世界：

```
┌──────────────────────────────────────────────────────────────────┐
│ 应用        Firefox(看视频) / 音乐播放器 / WebRTC 通话 / 你的代码   │
├──────────────────────────────────────────────────────────────────┤
│ 媒体框架    GStreamer · SDL_mixer · libpipewire · (工具箱音频后端) │
├──────────────────────────────────────────────────────────────────┤
│ 声音服务器  PipeWire 守护进程 (node graph) + WirePlumber (会话管理)│
│            ↳ 混音 · 音量 · 重采样 · 设备路由 · 延时对齐           │
├──────────────────────────────────────────────────────────────────┤
│ 用户库      libasound (ALSA 用户库, 被 PipeWire 接管)              │
├──────────────────────────────────────────────────────────────────┤
│ 内核        ALSA 框架 → 声卡驱动 (HDA/USB Audio/SoC)               │
├──────────────────────────────────────────────────────────────────┤
│ 硬件        codec 芯片 (DAC/ADC + 放大器) + DMA 引擎               │
└──────────────────────────────────────────────────────────────────┘
```

对比图形栈（应用 → toolkit → compositor → DRM/KMS → CRTC），角色一一对应：

| 图形世界 | 音频世界 | 职责 |
|----------|----------|------|
| compositor（唯一碰屏幕的进程） | PipeWire（唯一碰声卡大体路径的进程） | 汇聚多路输入、统一调度硬件 |
| wl_shm/dmabuf buffer | PCM 帧 | 承载媒体数据的单元 |
| DRM/KMS | ALSA | 内核抽象层 |
| 显示器 | 扬声器/麦克风 | 终点 |
| xdg-desktop-portal | 同一个 portal（屏幕共享） | 权限中介 |

**最大的不同：compositor 必须在显示路径上（每个像素都过它的手），PipeWire 却是「后来才必要」的**——纯 ALSA 时代应用可以直接出声，只是体验糟糕（见 §4）。

## 2. 声音的数字表示：PCM

声波是连续的气压曲线，计算机用 **PCM**（脉冲编码调制）离散化它：

```
声波 (模拟)                     PCM 数据
   /\      /\                  44100 次/秒 采样
  /  \    /  \    ──采样+量化──▶ 每个样本 16 bit 量化
 /    \  /    \                 2 声道 (立体声)
      \/      \                 
                               ⇒ 码率 = 44100 × 16 × 2 = 1.41 Mbit/s
```

- **采样率**（Hz）：每秒采多少个点。44100（CD）/48000（专业、视频）/96000；
- **位深**（bit）：每个点用多少位表示强度。16 bit（S16）/24/32 bit（浮点 F32）；
- **声道数**：1（单声道）/2（立体声）/更多；
- **帧 (frame)**：同一时刻所有声道各一个样本。立体声 1 帧 = 2 个样本；
- **周期 (period)**：一次硬件中断搬运的帧数。周期越小延迟越低、CPU 唤醒越频繁。

模块 13 里你将亲手填充这些参数（`SND_PCM_FORMAT_S16_LE`、44100 Hz、2 声道），并算出为什么缓冲区大小是 `帧数 × 4 字节`。

## 3. 内核层：ALSA 与 DMA

**ALSA**（Advanced Linux Sound Architecture）是内核音频框架，职责：

1. 为每张声卡注册设备：`/dev/snd/pcmC0D0p`（card0 Device0 **p**layback）、`pcmC0D0c`（**c**apture）；
2. 驱动 codec 芯片（Intel HDA、USB Audio、SoC I2S…）；
3. 管理 **DMA 环形缓冲区**：CPU 把 PCM 写进内存环，声卡的 DMA 引擎按采样率节拍自动搬运到 DAC——内核不需要每个样本中断一次，只需在周期边界处理中断。

```
应用写 PCM ──▶ DMA 环形缓冲区 ──(硬件按采样率自动搬运)──▶ DAC ──▶ 扬声器
                ▲                                            ^
                └────── 每个周期结束触发一次中断，内核补数据 ─────┘
```

这个「写环形缓冲 + 周期中断」的模型，和图形世界的「写 framebuffer + vblank 中断」在结构上是同构的——不同领域的内核设计师收敛到了同一个答案。

> 观察：
> ```bash
> cat /proc/asound/cards          # 有哪些声卡
> ls /dev/snd/                    # pcmC0D0p = card0/device0/playback
> cat /proc/asound/card0/pcm0p/sub0/hw_params   # 播放时: 实际采样率/格式
> ```

## 4. 用户库与「为什么需要声音服务器」

ALSA 自带用户库 libasound，应用理论上可以直接打开 `hw:0,0` 播放。但纯 ALSA 直连有三个致命问题：

1. **独占**：`hw:0,0` 通常一次只允许一个进程打开。你看视频时 QQ 就不能响；
2. **参数僵化**：声卡只接受固定几种采样率/格式。48 kHz 的声卡遇不上 44.1 kHz 的音乐，谁来重采样？
3. **路由与策略**：插上耳机要从扬声器切到耳机、蓝牙耳机连上要迁移流、每个应用单独音量——这些「策略」没地方放。

ALSA 的应对是用户态插件（`dmix` 软件混音、`plug` 自动转换、`default` 复合插件——模块 13 的 `default` 设备会用到），但能力有限。于是诞生了**声音服务器**：

```
历史: aRts(2000) → ESD → PulseAudio(2004) ─┐
                                            ├─▶ PipeWire(2017~): 统一音频+视频
历史: JACK(2002, 专业低延迟) ───────────────┘
```

- **PulseAudio**：解决了混音/音量/路由，成为 GNOME/KDE 标配十几年；
- **JACK**：专业音频界的低延迟实时图，但与普通桌面生态割裂；
- **PipeWire**：同时取代两者——JACK 级的低延迟图架构 + PulseAudio 级的桌面体验，**外加视频流路由**（这点对 Wayland 尤其关键，见 §6）。

## 5. PipeWire 架构：一张实时媒体图

PipeWire 的核心思想：整个系统的音频是一张**节点图 (node graph)**，守护进程驱动它按时钟节拍运转：

```
                          PipeWire 守护进程 (pipewire)
                          ┌─────────────────────────────────────────┐
   Firefox (媒体回放)     │   ┌──────┐      ┌──────┐      ┌───────┐  │      ALSA 内核层
  ┌────────┐   node      │   │ FF   │      │ 混音 │      │ALSA   │  │     ┌──────────┐
  │ 53% ▮▮ │─────────────┼──▶│ 输出 │─────▶│ 节点 │─────▶│ sink  │──┼────▶│ 扬声器    │
  └────────┘  (共享内存   │   └──────┘      │      │      └───────┘  │     └──────────┘
  ┌────────┐   +事件fd)  │   ┌──────┐      │      │      ┌───────┐  │     ┌──────────┐
  │ 音乐    │─────────────┼──▶│ 音乐  │─────▶│ ......│─────▶│蓝牙   │──┼────▶│ 耳机      │
  └────────┘             │   └──────┘      └──────┘      └───────┘  │     └──────────┘
                         │        ▲   WirePlumber (会话管理器)        │
                         └────────┤   决定: 谁连谁、默认设备、策略     │
                                  └── libpipewire (节点们跑在各应用进程里!)
```

要点：

- **节点跑在应用进程里**：Firefox 的「输出节点」在 Firefox 进程内运行，通过共享内存把 PCM 帧写给下游——守护进程只做调度和连接，不搬运数据本体（低延迟的秘密，和 Wayland 传 buffer 的思想如出一辙）；
- **WirePlumber** 是独立的会话管理器：监听设备热插拔、维护「默认输出设备」这类策略。`wpctl` 命令行工具属于它；
- **兼容层**：PipeWire 同时实现 PulseAudio (`pipewire-pulse`) 和 JACK (`pipewire-jack`) 的协议替换库，老应用无感迁移——这就是为什么 `pactl`（PulseAudio 的工具）在纯 PipeWire 系统上照常工作；
- **ALSA 接管**：`pipewire-alsa` 让 `default` 设备重定向进 PipeWire，模块 13 会亲眼验证这条路径。

## 6. Wayland 桌面里的音频：各管一段

现在回答「Wayland 和音频到底是什么关系」——**协议上零关系，桌面上强协作**：

| 场景 | 走什么路 | 说明 |
|------|----------|------|
| 应用播放音乐/提示音 | 应用 → PipeWire → ALSA | 完全不经过 compositor |
| 系统音量/输出设备切换 | WirePlumber + 桌面设置面板 | 桌面只是 PipeWire 的一个客户端 |
| **屏幕共享 / 录屏** | compositor → portal → PipeWire → 请求方 | **唯一的交汇点** |
| 视频会议（OBS/Zoom） | 上同 + 麦克风走 PipeWire | 桌面时代的招牌场景 |

屏幕共享的详细链路（这是 Wayland 安全模型的展示橱窗）：

```
OBS 请求共享屏幕
  └─▶ xdg-desktop-portal (D-Bus)          ── 权限中介, 弹窗询问用户
        └─▶ xdg-desktop-portal-wlr/gnome  ── portal 后端, 与 compositor 协商
              └─▶ compositor 屏幕内容打包成 PipeWire 视频流
                    └─▶ OBS 作为普通 PipeWire 节点接收画面
```

X11 时代任何应用都能用 XSHM/XDAMAGE 直接抓全屏；Wayland 下这条路被焊死，必须经 portal 授权 + PipeWire 递流。**PipeWire 之名由此而来：它同时是音频服务器和 Wayland 桌面的「视频水管」**。

## 7. 动手观察

```bash
# ① 内核层：ALSA 声卡
cat /proc/asound/cards
aplay -l                        # libasound 看到的播放设备 (alsa-utils)

# ② 用户库：ALSA 插件路径
cat ~/.asoundrc /etc/asound.conf 2>/dev/null
aplay -L | head -10             # 逻辑设备列表: default/pulse/dmix...

# ③ 服务器层：PipeWire
systemctl --user status pipewire pipewire-pulse wireplumber --no-pager | head -20
wpctl status                    # 设备树: Sinks(输出) / Sources(输入) / 默认设备
pactl info | head -8            # 兼容层视角: Server Name 会显示 PulseAudio (on PipeWire)

# ④ 图的内部: 节点与连接
pw-cli ls Node 2>/dev/null | head -15
pw-top                          # 实时: 每个节点的量化延迟/占用 (类似 top)

# ⑤ 声音在硬件上流动时
cat /proc/asound/card0/pcm0p/sub0/hw_params   # 播放音乐时非空: 格式/采样率/周期
```

> 虚拟机注意：QEMU/KVM 默认声卡是 `intel-hda` 或 `ac97`，PipeWire 在 VM 里同样正常工作；若无声卡可加 `-device intel-hda -device hda-duplex`。

## 8. 与课程模块的关联

| 本文章节 | 对应动手 |
|----------|----------|
| §2 PCM 帧/周期 | [模块 13](../module-13-audio/) `alsa-play`：手填样本、手配 hw params |
| §3 ALSA 设备 | 模块 13 `alsa-list` 与 `default` vs `hw:` 对比实验 |
| §5 PipeWire 图 | 模块 13 `pw-play`：作为节点接入图，`pw-top` 里观察自己 |
| §6 portal 屏幕共享 | [04 号文档 §7](04-login-and-session.md) portal 一节 |

## 9. 延伸阅读

- [PipeWire 官方文档](https://docs.pipewire.org/) — 含官方示例源码（模块 13 的 `pw-play` 实现即参照 `audio-src` 精简）
- [ALSA 项目](https://www.alsa-project.org/wiki/Main_Page)
- 延伸实验：用 `pw-metadata -n settings` 查看默认采样率；用 `PIPEWIRE_DEBUG=3` 跑模块 13 看图构建过程；把模块 13 改成录音（capture）方向。

---

下一篇：[04-login-and-session.md — 登录、会话与应用如何使用 Wayland](04-login-and-session.md)
