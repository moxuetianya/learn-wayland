# 00 — Linux 现代 GUI 全景地图

> 本文是整个课程的**地图索引**。学任何模块之前先读一遍，建立一个「我在哪一层」的空间感；之后每学一个模块，回来对照地图确认自己的位置。

---

## 1. 为什么需要这张地图

学 Wayland 最常见的困惑不是某个 API 怎么用，而是：

- `wl_shm`、`drmModeSetCrtc`、`libinput`、`GBM`、`logind`……这些东西**谁管谁**？
- 为什么我写的客户端连不上 compositor？为什么 compositor 必须 sudo？
- GTK 应用、Electron 应用、游戏，它们和 Wayland 是什么关系？
- 声音明明和显示无关，为什么大家都说「装 Wayland 要配 PipeWire」？

这些问题的根源都是**缺少一张分层地图**。Linux 桌面是一个 40 年演进出来的、由许多独立项目组成的「联邦」，没有哪一份文档描述全貌。本文就是这张地图。

## 2. 总分层图

从下到上，每一层只依赖它下面的层：

```
┌─────────────────────────────────────────────────────────────────────┐
│ ⑦ 应用层        Firefox / VS Code / 游戏 / 你自己的程序              │
├─────────────────────────────────────────────────────────────────────┤
│ ⑥ 工具箱层      GTK4  ·  Qt6  ·  SDL  ·  Electron  ·  Flutter        │
│                 (把「画窗口/处理输入」翻译成 Wayland 协议)            │
├─────────────────────────────────────────────────────────────────────┤
│ ⑤ 桌面服务层    显示服务器 (Wayland compositor)                      │
│                 Mutter(GNOME) KWin(KDE) sway Weston Hyprland ...     │
│                 ─ ─ ─ ─ ─ 桌面周边服务 ─ ─ ─ ─ ─                     │
│                 xdg-desktop-portal  PipeWire(音频/截屏)              │
│                 polkit(权限)  ibus/fcitx(输入法)                     │
├─────────────────────────────────────────────────────────────────────┤
│ ④ 会话层        显示管理器 GDM/SDDM/greetd  ·  systemd-logind        │
│                 (登录认证、会话生命周期、设备访问授权)                 │
├─────────────────────────────────────────────────────────────────────┤
│ ③ 图形/音频运行库 libwayland(client/server)  ·  Mesa(EGL/OpenGL/     │
│                 Vulkan/GBM)  ·  libinput  ·  libxkbcommon  ·         │
│                 libasound  ·  libpipewire                             │
├─────────────────────────────────────────────────────────────────────┤
│ ② 内核层        DRM/KMS(显示)  ·  evdev(输入)  ·  ALSA(音频)         │
│                 (把硬件抽象成 /dev 下的设备文件)                       │
├─────────────────────────────────────────────────────────────────────┤
│ ① 硬件层        GPU 渲染引擎 · 显示控制器(CRTC) · 显存 ·             │
│                 键盘/鼠标/触摸板 · 音频 codec/DAC                     │
└─────────────────────────────────────────────────────────────────────┘
```

**关键认知 1：Wayland 不是一个程序，而是 ⑤ 层的协议。**
「Wayland」本质上是一份进程间协议（本课程模块 1-5 讲的就是它），compositor 是实现这份协议的服务端程序（模块 8-12 你会亲手写一个）。

**关键认知 2：compositor 是 ⑤ 层唯一的「显示独裁者」。**
所有应用都把画好的像素**交给** compositor，由它统一合成、统一交给内核。没有任何应用能绕过它直接碰屏幕——这就是 Wayland 安全性的来源，也是和 X11 最大的区别。

**关键认知 3：课程教的 libwayland 只是 ③ 层的一块。**
Mesa/EGL（GPU 渲染）、libinput（输入）、PipeWire（音频）都是和 libwayland 平级的运行库，一个真实桌面缺一不可。

## 3. 三条「旅程」——把地图串起来

### 3.1 一帧像素的旅程（应用 → 屏幕）

```
 应用进程                          Compositor 进程                内核/硬件
┌───────────────┐   wl_shm fd    ┌──────────────┐   drmModePageFlip   ┌─────────┐
│ 工具箱(Gtk/Qt)│  或 dmabuf     │  收集所有窗口 │ ───(vblank 时)──▶ │  DRM    │
│  渲染像素 ────┼───────────────▶│  合成(可能用 ─│                    │  KMS    │
│  attach+commit│  (共享内存/     │  GPU 加速)    │                    │  驱动   │
└───────────────┘   显存共享)    └──────────────┘                    └────┬────┘
                                                                          │ 编程 CRTC
                                                                     ┌────▼─────────┐
                                                                     │ 显示控制器    │
                                                                     │ 逐行扫描显存 ─┼─▶ 屏幕亮起
                                                                     └──────────────┘
```

1. 应用把像素画进一块 buffer（CPU 画进共享内存，或 GPU 画进显存）；
2. 通过 `wl_shm`（传文件描述符）或 `linux-dmabuf`（传显存句柄）把 buffer **借给** compositor；
3. `wl_surface.attach()` + `commit()` 原子提交这一帧（模块 2/3）；
4. compositor 把所有可见窗口合成为一帧，在 **vblank**（垂直空白期）通过 KMS page flip 切换显示控制器读的 framebuffer（模块 9/11/12）；
5. 显示控制器以像素时钟逐行读出显存，经编码器、连接器（HDMI/DP）发给显示器。

> 完整的显卡侧原理见 [01-gpu-and-display.md](01-gpu-and-display.md)。

### 3.2 一次按键的旅程（键盘 → 应用）

```
物理按键 → 内核 evdev (/dev/input/event3) → libinput (compositor 内)
        → wl_keyboard.key 事件发给焦点窗口 → 客户端 libxkbcommon
        → keysym → 字符 → 应用响应
```

注意方向反转：显示是「应用 → compositor → 屏幕」，输入是「内核 → compositor → 应用」。**compositor 是两条路上唯一的枢纽**——它既是显示的服务器，又是输入的路由器。这就是为什么模块 4（客户端收输入）和模块 10/11（服务端发输入）会从两个视角看到同一次按键。

### 3.3 一段声音的旅程（应用 → 扬声器）

```
应用 (GStreamer/SDL/工具箱) → PipeWire 守护进程 (node graph: 混音/音量/路由)
                            → ALSA 用户库 → 内核 ALSA 驱动 → codec DAC → 扬声器
```

声音**不经过** compositor！Wayland 协议只管显示和输入。桌面音频由独立的 PipeWire 守护进程负责，它与 Wayland 世界的唯一交集是：屏幕共享/录屏时，`xdg-desktop-portal` 用 PipeWire 把**视频流**递给请求它的应用。

> 完整音频栈见 [03-audio-stack.md](03-audio-stack.md)，动手实验在模块 13。

## 4. Wayland vs X11 —— 为什么换架构

```
       X11 (1987)                          Wayland (2008~)
┌─────────┐  ┌─────────┐             ┌─────────┐  ┌─────────┐
│  App A  │  │  App B  │             │  App A  │  │  App B  │
└────┬────┘  └────┬────┘             └────┬────┘  └────┬────┘
     │            │                       │            │
     ▼            ▼                       ▼            ▼
┌─────────────────────────┐        ┌─────────────────────────┐
│   X Server (Xorg)       │        │   Wayland compositor    │
│  合成被外包给独立的      │        │  窗口管理+合成+显示输出  │
│  合成管理器 (Compiz/    │        │  在同一个进程里完成      │
│  Mutter/KWin)           │        │                         │
└───────────┬─────────────┘        └───────────┬─────────────┘
            │                                  │
            ▼                                  ▼
       X11 视频驱动                      DRM/KMS (内核统一接口)
       (每张卡一套, 重复造轮子)          (驱动由内核统一提供)
```

| 维度 | X11 | Wayland |
|------|-----|---------|
| 合成 | X Server 只管基本重叠，现代效果靠外挂合成管理器，路径绕 | compositor 天生合成，路径直接 |
| 撕裂 | 客户端可以随时覆盖屏幕区域，常见撕裂 | 只有 compositor 能在 vblank 换帧，无撕裂 |
| 安全 | 任何 X 客户端可以截屏/录所有窗口的键盘 | 截屏/录屏必须经 portal 授权 |
| 输入 | X Server 自己解释按键 | compositor 路由原始按键，客户端用 xkbcommon 解释（布局更灵活） |
| 驱动 | 用户态 X 驱动 + 内核 DRM 两套 | 只有一套：内核 DRM/KMS |

X11 通过 XWayland 兼容层继续存活：GNOME/KDE 的 compositor 内嵌一个 X Server，让老应用照常运行（`xeyes` 就跑在这上面）。

## 5. 课程模块地图

每个模块都标注它在全景图中的层位，学的时候随时回来对号入座：

| 模块 | 主题 | 地图层位 | 打通的道路 |
|------|------|----------|-----------|
| [模块 1](../module-01-connection/) | 连接与服务发现 | ③→⑤ libwayland 客户端 | 应用如何找到 compositor |
| [模块 2](../module-02-hello-window/) | 第一个窗口 | ③⑥→⑤ | surface/xdg-shell/shm |
| [模块 3](../module-03-pixels/) | 像素与缓冲区 | ⑥ | 像素格式、attach-commit |
| [模块 4](../module-04-input/) | 键盘与鼠标 | ⑥ 输入反向通路 | wl_seat + xkbcommon |
| [模块 5](../module-05-protocol/) | 协议 XML 与代码生成 | ③ 协议本身 | wayland-scanner |
| [模块 6](../module-06-frame-cb/) | 帧回调与垂直同步 | ③⑤ | 与刷新率同步的动画 |
| [模块 7](../module-07-cairo/) | Cairo 软件渲染 | ⑥ | 2D 库如何落到 shm buffer |
| [模块 8](../module-08-server-skeleton/) | libwayland-server 骨架 | ⑤ | 服务端对象模型 |
| [模块 9](../module-09-drm/) | DRM/KMS | ② | 直接指挥显卡显示 |
| [模块 10](../module-10-libinput/) | libinput | ②③ | 输入的内核侧来源 |
| [模块 11](../module-11-tty-compositor/) | 最小 TTY compositor | ②③⑤ | 三者串成真 compositor |
| [模块 12](../module-12-full-compositor/) | 完整 compositor | ⑤ | xdg-shell/焦点/合成 |
| [模块 13](../module-13-audio/) | 音频栈 | ②③⑤(音频平行世界) | ALSA 与 PipeWire |
| [模块 14](../module-14-session/) | 登录与会话 | ④ | logind/seat/DM |

推荐阅读顺序：**00(本文) → 01(显卡) → 02(启动链路) → 模块 1-7 → 03(音频) → 04(登录) → 模块 8-12 → 模块 13/14**。

原理文档之间的依赖：

```
00 全景地图 ──┬─▶ 01 显卡与显示 ──┬─▶ 02 从电源到像素(启动链路)
              │                   ├─▶ 05 KMS/EDID/链路协商
              │                   └─▶ 07 fd 传递/共享内存
              ├─▶ 03 音频栈
              └─▶ 04 登录与会话 ──┬─▶ 02 的后半段(用户空间部分)
                                  └─▶ 06 会话设备/revoke/多座席
```

## 6. 术语速查表

| 术语 | 一句话解释 | 详见 |
|------|-----------|------|
| **GPU** | 显卡里的并行处理器，负责「算出」像素 | [01](01-gpu-and-display.md) |
| **显示控制器 / CRTC** | 显卡里独立于 GPU 的扫描引擎，负责「送出」像素 | [01](01-gpu-and-display.md) |
| **显存 / VRAM / GEM** | 显卡本地内存；内核用 GEM 框架管理 buffer 对象 | [01](01-gpu-and-display.md) |
| **framebuffer (fb)** | 一块装着一帧画面的内存，扫描引擎逐行读取它 | [01](01-gpu-and-display.md) |
| **connector** | 一个物理输出口；热插拔状态与模式表都挂在它上面 | [05](05-kms-edid-and-link.md) |
| **mode (模式)** | 一组像素时钟 + 时序参数（分辨率/刷新率由它算出） | [05](05-kms-edid-and-link.md) |
| **EDID** | 显示器里的只读能力表：时序、格式、尺寸、范围 | [05](05-kms-edid-and-link.md) |
| **DPCD** | DP 的链路状态寄存器区，EDID 之外的另一半信息 | [05](05-kms-edid-and-link.md) |
| **DDC / AUX** | 读 EDID 的边带通道：HDMI 走 I2C，DP 走 AUX | [05](05-kms-edid-and-link.md) |
| **DRM/KMS** | 内核显示子系统的统称；KMS 指「模式设置」(分辨率/刷新率) | [01](01-gpu-and-display.md) |
| **DRM master** | 拥有 KMS 控制权的唯一进程（就是 compositor） | [01](01-gpu-and-display.md) · [06](06-seat-devices-and-revoke.md) |
| **page flip** | 在 vblank 期间原子地切换扫描的 framebuffer，防撕裂 | [01](01-gpu-and-display.md) |
| **vblank** | CRT 时代的垂直消隐期，如今是换帧的安全窗口 | [01](01-gpu-and-display.md) |
| **wl_shm** | 「共享内存 buffer」协议：应用传 fd 给 compositor | 模块 2 |
| **dmabuf** | 「显存 buffer」跨进程共享机制，GPU 渲染的零拷贝通路 | [01](01-gpu-and-display.md) · [07](07-fd-passing-and-shared-memory.md) |
| **SCM_RIGHTS** | AF_UNIX 上传 fd 的机制（内核装一份 dup；TCP/UDP 不行） | [07](07-fd-passing-and-shared-memory.md) |
| **memfd** | 匿名共享内存文件；`mmap` + 传 fd 是 Wayland `wl_shm` 的基础 | [07](07-fd-passing-and-shared-memory.md) |
| **EGL/OpenGL/Vulkan** | 应用指挥 GPU 干活的 API，由 Mesa 实现 | [01](01-gpu-and-display.md) |
| **GBM** | Mesa 提供的「给 EGL 分配可扫描显存 buffer」的抽象 | [01](01-gpu-and-display.md) |
| **evdev** | 内核输入设备的统一接口 `/dev/input/event*` | 模块 10 |
| **libinput** | 用户态输入加工库：合并 evdev + 手势/加速度/去抖 | 模块 10 |
| **seat** | 一套「键盘+鼠标+显示器+扬声器」的组合，logind 管理 | [04](04-login-and-session.md) · [06](06-seat-devices-and-revoke.md) |
| **session** | 一次登录产生的会话，拥有它的 seat 的使用权 | [04](04-login-and-session.md) |
| **TakeDevice** | logind 把设备 fd（dup）交给会话控制者的 D-Bus 方法 | [06](06-seat-devices-and-revoke.md) |
| **revoke** | 让已发出的 evdev fd 立即失效（`EVIOCREVOKE`），不可逆 | [06](06-seat-devices-and-revoke.md) |
| **VT (虚拟终端)** | 一台机器上的多个「虚拟屏幕」，Ctrl+Alt+F3 切换的那个 | [02](02-boot-to-pixels.md) · [06](06-seat-devices-and-revoke.md) |
| **PAM** | 可插拔认证模块：login/GDM 的密码校验都委托给它 | [04](04-login-and-session.md) |
| **显示管理器 (DM)** | 图形登录界面(GDM/SDDM)背后的程序 | [04](04-login-and-session.md) |
| **ALSA** | 内核音频框架 + 用户库 | [03](03-audio-stack.md) |
| **PipeWire** | 现代音频/视频路由守护进程，Wayland 桌面的标配 | [03](03-audio-stack.md) |
| **portal** | 应用向桌面请求特权(截屏/选文件)的中介服务 | [04](04-login-and-session.md) |
| **XWayland** | 兼容层：让 X11 应用跑在 Wayland 桌面上 | 本文 §4 |

---

下一步：深入 [01-gpu-and-display.md — 显卡的作用](01-gpu-and-display.md)。
