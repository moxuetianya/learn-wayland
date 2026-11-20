# 01 — 显卡的作用：从「算出像素」到「送出像素」

> 对应诉求：**讲清楚显卡到底在干什么**。读本篇 before/after 模块 9（DRM/KMS）效果最佳——模块 9 的每一行代码都能在本文找到硬件层面的对应物。

---

## 1. 显卡不是一个东西，而是四个

日常说的「显卡」实际上是一块 PCB 上的多个独立部件。理解它们的分工，整个 Linux 图形栈的设计会瞬间清晰：

```
                      ┌──────────────────────────────────────────────┐
                      │                显卡 (GPU 卡)                  │
                      │                                              │
   PCIe ──────────────┤  ┌──────────┐   ┌──────────┐   ┌──────────┐  │
  (CPU/内存)          │  │ 显存     │   │ GPU      │   │ 显示引擎 │  │────▶ HDMI/DP
                      │  │ VRAM/GEM │◀─▶│ 渲染引擎  │   │ CRTC ×N  │  │     (给显示器)
                      │  │          │   │ (3D 管线) │   │ + Encoder │  │
                      │  └──────────┘   └──────────┘   └────▲─────┘  │
                      │       ▲                              │        │
                      │       └────── 扫描输出(读取) ─────────┘        │
                      └──────────────────────────────────────────────┘
```

| 部件 | 职责 | 一句话类比 |
|------|------|-----------|
| **显存 (VRAM)** | 存放像素数据和 GPU 中间结果 | 绘画用的纸 |
| **GPU 渲染引擎** | 并行计算，**生产**像素 | 画家 |
| **显示引擎 (CRTC/Encoder)** | 按固定时钟从显存读像素，**发送**给显示器 | 朗读者 |
| **视频编解码引擎** | 硬解 H.264/HEVC/AV1（独立于 GPU 的小核） | 印刷机 |

**最关键的认知：GPU（渲染）和显示引擎（扫描输出）是两个独立部件，可以完全无关地同时工作。**

- 你用 CPU 在 dumb buffer 里写像素（模块 9、11、12 的做法）→ GPU 全程睡觉，显示引擎照样扫描显示；
- 播放视频时 GPU/解码引擎疯狂干活 → 显示引擎只管按时把成品送出去。

这也解释了为什么内核把显示控制接口叫 **KMS**（Kernel Mode Setting，模式设置）而不是「GPU 控制」——它操作的是显示引擎，不是 GPU。

## 2. CPU vs GPU：为什么图形需要专门的处理器

屏幕上有 1920×1080 ≈ 200 万个像素。假设每帧每个像素要做几十次浮点运算（光照、混合、纹理采样），每秒 60 帧——每秒约 **百亿级** 运算。

- **CPU**：4~16 个强核心，每个擅长复杂的串行逻辑（分支、跳转、系统调用）；
- **GPU**：数千个弱核心，每个只会简单算术，但**同时**算。

```
CPU:  ██▁▁▁▁▁▁  ██▁▁▁▁▁▁  ██▁▁▁▁▁▁  ██▁▁▁▁▁▁     (少量强核, 串行逻辑)
GPU:  ▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌    (海量弱核, 数据并行)
```

图形的本质是**对每个像素做同样的事**（data-parallel），正好是 GPU 的菜。所以：

- 合成窗口（把 N 张半透明图片按位置叠成一帧）→ GPU 一趟搞定；
- Cairo 之类的 CPU 软件渲染（模块 7）→ 也行，但全屏 4K 动画会吃力；
- 本课程的 compositor 用 CPU 逐像素拷贝合成（画家算法）→ 教学上清晰，生产级 compositor（Mutter/KWin）全部走 GPU。

## 3. 显存与 GEM：内核怎么管理显存

显存不能像 malloc 那样随意分配——CPU 要能访问它（写像素）、GPU 要能访问它（渲染）、显示引擎要能访问它（扫描）。内核用 **GEM**（Graphics Execution Manager）框架统一管理：

- 一块显存 = 一个 **GEM buffer 对象**，用户态拿到的是一个句柄（handle）；
- `mmap` 把它映射进用户地址空间，CPU 就能直接读写；
- 同一块 buffer 可以被交给 GPU 执行渲染、或被指定为扫描输出源。

**两种典型的 buffer 来源：**

| | dumb buffer | GBM/EGL buffer |
|---|---|---|
| 创建方式 | `DRM_IOCTL_MODE_CREATE_DUMB`（模块 9） | Mesa 的 GBM API + EGL 渲染 |
| 谁来写 | CPU（mmap 后 memset/memcpy） | GPU（OpenGL/Vulkan 绘制） |
| 性能 | 慢，但教学清晰 | 快，生产级 compositor 标配 |
| 在哪 | 通常系统内存（线性排布） | 显存，布局由驱动决定 |

## 4. buffer 的跨进程旅行：wl_shm 与 dmabuf

Wayland 世界里，应用画好的 buffer 要交给 compositor，compositor 合成后的 buffer 要交给内核扫描。两条通路：

**wl_shm（本课程模块 1-7、11、12 用的）**

```
应用进程                                compositor 进程
┌──────────────┐   mkstemp→mmap        ┌──────────────┐
│ fd ────────➤ │ ──── 传 fd (unix sock)│ ➤ mmap 同一fd │
│ 写像素到内存  │    (wl_shm_pool)      │ 读到相同内存   │
└──────────────┘                       └──────────────┘
        两进程共享同一块物理内存，零拷贝，但只能 CPU 读写
```

**linux-dmabuf（生产级，GPU 渲染时用）**

```
应用进程                                compositor 进程
┌──────────────┐   EGL/OpenGL 渲染     ┌──────────────┐
│ GBM 分配显存 ─│ ─── 传 dmabuf fd ────▶│ 直接把显存    │──▶ 可直接送去扫描
│ GPU 画进显存  │    (linux-dmabuf 协议)│  交给 GPU 合成 │    (direct scanout!)
└──────────────┘                       └──────────────┘
        显存级共享：应用画完，compositor 甚至可以让显示引擎直接扫它——零合成
```

dmabuf 是 Linux 通用的「跨设备/跨进程 buffer 句柄」机制，也是 Wayland 高性能的基石：浏览器渲染、视频解码、游戏画面都走这条路。

> 这两种「传 fd」到底在内核里做了什么（`SCM_RIGHTS`、dup 语义、为什么
> TCP 不行、`memfd` 怎么建），见
> [07 fd 传递与共享内存](07-fd-passing-and-shared-memory.md)。

## 5. 显示输出通路：像素如何离开显卡

这是理解模块 9 的核心。一块现代显卡里有 N 组显示流水线，每组三段：

```
   framebuffer (显存中的像素)          编码成 HDMI/DP 信号          物理接口
   ┌─────────────┐              ┌─────────────┐              ┌─────────────┐
   │   CRTC      │── 像素流 ───▶ │  Encoder    │── TMDS/DP ──▶│  Connector  │──▶ 显示器
   │ (扫描引擎)   │              │ (HDMI/DP/eDP)│              │ (HDMI-A1/   │
   │             │              │  编码器      │              │  DP-1/DSI-1)│
   └─────────────┘              └─────────────┘              └─────────────┘
    从哪块显存读、                 把像素流编码成                 物理插口，
    读哪个区域、什么时序            接口规定的电信号               可热插拔检测
```

- **Connector**：物理插口，可以查询「插没插显示器、支持哪些分辨率」（模块 9 遍历的就是它）；
- **Encoder**：把 CRTC 的像素流编码成 HDMI/DP 信号，中间桥梁；
- **CRTC**（CRT Controller 的历史名字）：真正的扫描引擎。它持有「从哪块 framebuffer 读、扫描出多大区域、什么时序」的配置。**所谓「设置显示模式」就是配置 CRTC**（`drmModeSetCrtc`）。

> 时序参数的逐字段含义、EDID 的字节结构、DP/HDMI 各自怎么协商链路，
> 见 [05 KMS 对象、EDID 与链路协商](05-kms-edid-and-link.md)。

### 5.1 扫描线：CRTC 是一台不知疲倦的读稿机

CRTC 每秒 60 次（60 Hz）把 framebuffer 从上到下扫一遍：

```
   ┌────────────────────────┐
   │ ➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤  │ ← 第 1 行：从左到右按像素时钟逐个读出
   │ ➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤  │ ← 第 2 行：行结束时 hsync（行同步脉冲）
   │        ...             │
   │ ➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤➤  │ ← 最后一行
   └────────────────────────┘
      整帧结束时 vsync（场同步脉冲）→ 回到第 1 行
```

每个像素按像素时钟（如 148.5 MHz）依次送出；行尾发 hsync，帧尾发 vsync——这套时序从 CRT 显像管时代原封不动继承到 HDMI/DP。

### 5.2 vblank 与 page flip：为什么会有「撕裂」

CRTC 是自顾自的读稿机：它只认「framebuffer 指针」，不知道应用什么时候在改写内容。如果 CRTC 扫到一半时应用改了下半部分：

```
   ┌──────────────┐        ┌──────────────┐
   │  旧帧上半部   │        │  新帧上半部   │   一帧里出现两个时间点的画面
   │  新帧下半部   │        │  旧帧下半部   │   → 「撕裂」(tearing)
   └──────────────┘        └──────────────┘
```

解决方案：**只在 vblank（帧与帧之间的垂直空白期，CRTC 不读内存）切换 framebuffer 指针**。

- 模块 9/11/12 用的是 dumb buffer 直接写 + `drmModeSetCrtc`——写入立即生效，可能撕裂（教学上可接受）；
- 生产级 compositor 用 **page flip**（`drmModePageFlip` / atomic commit）：提前把新帧挂上，内核在下一个 vblank 原子地切换指针，还顺手给 compositor 发一个事件通知「换帧完成」——模块 6 的 `wl_surface.frame()` 帧回调，其硬件源头就是它。

### 5.3 Plane：CRTC 上叠的图层

现代显示引擎还支持 **plane**（图层）：一个 CRTC 可以同时从多块 buffer 取样叠加（硬件合成）：

| Plane | 用途 |
|-------|------|
| primary plane | 主桌面 framebuffer |
| overlay plane | 视频窗口独占一层（省一次 GPU 合成） |
| cursor plane | 鼠标指针（所以指针永不卡顿） |

生产级 compositor 的「direct scanout」优化：如果全屏只有一个窗口，直接把客户端 buffer 挂到 primary plane，**GPU 完全不用参与**，功耗大降。本课程模块 11/12 的软件合成是最原始的对照形态。

## 6. GPU 怎么被指挥：Mesa/EGL/OpenGL/Vulkan

显示引擎由 KMS 管，那 GPU 由谁管？**用户态的 Mesa**：

```
 应用 (Firefox / 游戏 / GTK)
   │  OpenGL / Vulkan / EGL API
   ▼
 Mesa (开源驱动栈: i915→iris, amdgpu→radeonsi, ...)
   │  生成 GPU 指令流 + 通过 GBM 分配显存
   ▼
 /dev/dri/renderD128  ← 「渲染节点」：专管 GPU，人人可用，不碰显示
   ▼
 内核 DRM 驱动 → GPU 执行
```

注意 `/dev/dri` 下有两类设备节点——这是多用户安全的关键：

| 节点 | 权限 | 用途 |
|------|------|------|
| `card0` | 需要授权（DRM master 机制，[模块 9](../module-09-drm/) 亲自体验） | 显示控制（KMS） |
| `renderD128` | 普通用户可用（`video`/`render` 组） | 纯 GPU 计算 |

**同一时刻只能有一个 DRM master**（通常就是 compositor），这是内核防止多个程序抢屏幕的互斥机制。你在模块 9 里调用的 `drmSetMaster()` 就是在申请这把钥匙；在已有桌面会话的机器上运行会失败——那一刻你就理解了为什么 compositor 必须从干净 TTY 启动。

## 7. 把整条路连起来

现在用一张图总结「一帧像素」在硬件层面的完整路径（对照 [00 号文档 §3.1](00-big-picture.md)）：

```
┌──────────┐    ┌──────────────────────────── 显卡 ────────────────────────────┐
│ 应用 CPU  │    │  ┌─────────┐  GPU 渲染   ┌─────────┐  扫描    ┌──────────┐   │    ┌──────┐
│ 或 GPU ──┼──▶ │  │  显存   │◀──────────▶ │ (buffer)│────────▶ │  CRTC    │───┼──▶ │ 显示器│
│ 渲染     │    │  │ (GEM)   │   dmabuf    │  frame- │  像素时钟 │ Encoder  │   │    │      │
└──────────┘    │  └─────────┘             │  buffer │          │ Connector│   │    └──────┘
                │        ▲                 └─────────┘          └──────────┘   │
                └────────┼────────────────────────────────────────────────────-┘
                         │ CPU mmap 写入 (wl_shm / dumb buffer, 本课程路线)
                ┌────────┴────────┐
                │  compositor 合成 │ (模块 11/12: CPU 逐像素; 生产级: GPU)
                └─────────────────┘
```

- 本课程路线（CPU）：应用 mmap 写共享内存 → compositor 读出、逐像素合成到 dumb buffer → `drmModeSetCrtc` 让 CRTC 扫描它；
- 生产级路线（GPU）：应用 OpenGL 渲染到 GBM buffer → dmabuf 传给 compositor → GPU 合成 → vblank 时 page flip。

**两件事没有变：framebuffer 是像素的家，CRTC 是唯一的出口。**

## 8. 动手观察清单

在你的 Linux 机器（或课程 VM）上验证本文：

```bash
# 1. 看有哪些 DRM 设备节点
ls -l /dev/dri/
#    card0 (显示控制, master 互斥)  renderD128 (GPU 渲染, 普通用户可用)

# 2. 列出连接器/CRTC/plane（需要 drm-info 工具: sudo apt install drm-info）
drm-info | head -80

# 3. 看 GPU 是谁在驱动
lspci | grep -i vga
lsmod | grep -E 'i915|amdgpu|nouveau|virtio_gpu|vmwgfx'

# 4. 看 GEM buffer 占用（字节）
cat /sys/kernel/debug/dri/0/gem_objects   # 需 root

# 5. 感受 vblank 频率（每秒 60 次左右的中断/事件）
sudo cat /sys/kernel/debug/dri/0/crtc-0/vblank/ 2>/dev/null || \
grep -r . /sys/kernel/debug/dri/0/crtc-*/ 2>/dev/null | head
```

> 虚拟机注意：QEMU/KVM 里 GPU 是 `virtio-gpu`（设备节点**不一定叫 card0**，与 simpledrm 共存时常见 `card1`，用 `ls /dev/dri/` 确认——课程程序会自动扫描，也可用 `DRM_CARD=/dev/dri/cardX` 指定），VMware 里是 `vmwgfx`。它们同样完整实现 DRM/KMS，模块 9-12 在 VM 里照常工作。

## 9. 与课程模块的关联

| 本文章节 | 对应模块 |
|----------|----------|
| §3 GEM/dumb buffer | [模块 9](../module-09-drm/) `create_dumb_fb()` |
| §5 显示通路 Connector→Encoder→CRTC | 模块 9 的 `find_crtc()` 遍历逻辑 |
| §5.2 vblank/撕裂 | [模块 6](../module-06-frame-cb/) 帧回调；模块 11/12 的 repaint |
| §6 DRM master | 模块 9 `drmSetMaster()`；[04 号文档](04-login-and-session.md) 会讲 logind 如何授权 |
| §7 dmabuf 生产路线 | 超出本课程范围，见延伸阅读 |

## 10. 延伸阅读

- [DRM/KMS 内核文档](https://docs.kernel.org/driver-api/drm.html) — 内核侧权威文档
- [Mesa 项目](https://www.mesa3d.org/) — GPU 驱动栈
- [etnaviv 项目的一篇 GEM/Buffer 讲解](https://gitlab.freedesktop.org/mesa/mesa) — Mesa 源码
- 延伸动手：把模块 11 的合成改成 `drmModePageFlip` 异步翻页（有 vblank 事件、无撕裂），是天然的进阶练习

---

下一篇：[02-boot-to-pixels.md — 从按下电源到看到桌面](02-boot-to-pixels.md)
