# Learn Wayland — 从零到 Compositor 开发

> 基于 Wayland 协议的渐进式学习课程：从客户端到服务端，覆盖协议核心、DRM/KMS、libinput、音频栈与登录会话——**原理与动手双线并进**。

## 课程地图（先看这里）

整个课程 = **5 篇原理文档**（建立全景坐标）+ **14 个动手模块**（在坐标上行走）：

```
                     ┌─────────────── 原理丛书 docs/ ───────────────┐
                     │                                              │
   00 全景地图 ──┬──▶ 01 显卡与显示 ──▶ 02 从电源到像素              │
                │                                              │
                ├──▶ 03 音频栈                                  │
                └──▶ 04 登录与会话                              │
                     └──────────────────────────────────────────────┘
                                  ▲ 每个模块 README 标注全景图定位
                     ┌───────────┴────────────────────────────────┐
   阶段一(1-5) 客户端基础   阶段三(8-10) 服务端积木                │
   阶段二(6-7)  渲染进阶    阶段四(11-12) 完整 Compositor          │
   阶段五(13-14) 系统上下文: 音频栈 · 登录与会话                    │
                     └────────────────────────────────────────────┘
```

| 如果你想弄懂… | 先读 | 再做 |
|----------------|------|------|
| Linux 桌面整体架构（分层地图） | [docs/00](docs/00-big-picture.md) | 全部模块对照定位 |
| 显卡到底在干什么（GPU/CRTC/显存） | [docs/01](docs/01-gpu-and-display.md) | 模块 9 → 11 → 12 |
| 开机后画面如何一步步出现 | [docs/02](docs/02-boot-to-pixels.md) | 模块 14 观察启动日志 |
| 声音是如何播出来的 | [docs/03](docs/03-audio-stack.md) | 模块 13 |
| 登录/GDM/会话如何管理、app 如何用 Wayland | [docs/04](docs/04-login-and-session.md) | 模块 14 |

## 前置条件

- 熟悉 Linux 环境与 C 语言编程
- 了解 Unix domain socket 基本概念
- 已对 Wayland 协议有基本认知（了解 `wl_display`、`wl_surface`、`wl_compositor`、`xdg-shell` 等概念）

## 推荐虚拟机环境

| 发行版 | 版本 | 说明 |
|--------|------|------|
| Ubuntu | **24.04 LTS** (推荐) | LTS 长期支持，Wayland 1.22，稳定可靠 |
| Ubuntu | 26.04（开发中） | 较新但可能不稳定 |
| Debian | 12 Bookworm | 同样可用 |

本课程以 **Ubuntu 24.04** 为基准环境，所有代码均在此环境下测试。

> **提示**: 虚拟机中安装系统后，启动时如需切换到 TTY 测试 Compositor，按 `Ctrl+Alt+F3` 进入 tty3。

## 项目结构

```
learn-wayland/
├── README.md                     # 本文件（课程地图）
├── setup-vm.sh                   # 一键安装所有依赖
├── Makefile                      # 顶层构建 (make client|server|system)
├── docs/                         # ★ 原理丛书（系统性讲解）
│   ├── 00-big-picture.md         #   Linux 现代 GUI 全景地图（索引）
│   ├── 01-gpu-and-display.md     #   显卡的作用: 渲染/扫描输出/DRM-KMS
│   ├── 02-boot-to-pixels.md      #   从按下电源到看到桌面
│   ├── 03-audio-stack.md         #   ALSA/PipeWire 音频栈
│   └── 04-login-and-session.md   #   登录/会话/DM/app 如何用 Wayland
├── module-01-connection/         # 模块 1-7: 客户端开发
├── module-02-hello-window/
├── module-03-pixels/
├── module-04-input/
├── module-05-protocol/
├── module-06-frame-cb/
├── module-07-cairo/
├── module-08-server-skeleton/    # 模块 8-12: 服务端开发
├── module-09-drm/
├── module-10-libinput/
├── module-11-tty-compositor/
├── module-12-full-compositor/
├── module-13-audio/              # 模块 13-14: 系统上下文（动手）
└── module-14-session/
```

> 每个模块目录内都有自己的 `README.md`：**全景图定位 → 原理讲解 → 代码走读 → 动手实验（含观察点）→ 进阶练习 → 常见坑**。

## 课程大纲

---

### 第〇阶段：建立全景（0.5 天）

五篇原理文档按需精读（至少读完 [00 全景地图](docs/00-big-picture.md)），后续模块随时回查。

---

### 第一阶段：Wayland 客户端开发（1-5 天）

#### 模块 1 — 建立连接与服务发现

**目标**: 理解 `wl_display`、`wl_registry`、事件循环三大核心概念的运作方式。

- 连接到 Wayland Compositor 并获取 `wl_display`
- 通过 `wl_registry` 发现服务器支持的全局对象
- 实现事件循环 (`wl_display_dispatch`)
- 打印所有可用的全局接口及版本

> **代码**: [module-01-connection/](module-01-connection/)

#### 模块 2 — 第一个 Wayland 窗口

**目标**: 掌握 `wl_compositor` → `wl_surface` → `xdg-shell` 的完整窗口创建流程。

- 使用 `xdg_wm_base` 创建 `xdg_toplevel`（真正的窗口角色）
- 使用 `wl_shm` 创建共享内存缓冲区
- 填充像素数据并提交到屏幕
- 理解 `attach → commit` 模型

> **代码**: [module-02-hello-window/](module-02-hello-window/)

#### 模块 3 — 手动像素缓冲区与图形绘制

**目标**: 深入理解像素格式、颜色空间与帧缓冲区结构。

- 理解 `WL_SHM_FORMAT_XRGB8888` 等像素格式
- 手写简单的图形 primitive（画线、填充矩形、渐变）
- 实现简单的动画帧（连续重绘）

> **代码**: [module-03-pixels/](module-03-pixels/)

#### 模块 4 — 键盘与鼠标输入处理

**目标**: 掌握 Wayland 的输入模型（`wl_seat` → `wl_keyboard` → `wl_pointer`）。

- 监听 `wl_keyboard` 按键事件并打印 keycode
- 使用 `libxkbcommon` 将 keycode 转换为可读键名
- 监听 `wl_pointer` 移动/点击事件
- 理解 `wl_seat` 的能力广播机制

> **代码**: [module-04-input/](module-04-input/)

#### 模块 5 — 协议 XML 与 wayland-scanner

**目标**: 理解 Wayland 协议文件的语法、代码生成与版本协商机制。

- 阅读核心协议 `wayland.xml` 的关键接口定义
- 编写一个简单的自定义协议 XML
- 使用 `wayland-scanner` 生成 client-header 和 private-code
- 实现一个简单的自定义协议通信

> **代码**: [module-05-protocol/](module-05-protocol/)（与模块 8 联调）

---

### 第二阶段：客户端渲染进阶（6-7 天）

#### 模块 6 — 帧回调与垂直同步

**目标**: 理解 Wayland 的帧调度机制，实现无撕裂渲染。

- 使用 `wl_surface.frame()` 获取帧回调 (`wl_callback`)
- 实现"每帧渲染一次"的稳定动画循环
- 对比 busy-loop 与 frame-callback 的性能差异

> **代码**: [module-06-frame-cb/](module-06-frame-cb/)

#### 模块 7 — Cairo 软件渲染集成

**目标**: 将成熟的 2D 渲染库与 Wayland 缓冲区对接。

- 使用 Cairo 在 `wl_shm` 缓冲区上绘制
- 绘制抗锯齿文字、贝塞尔曲线、渐变
- 创建简单的 UI 组件（按钮、文本框）

> **代码**: [module-07-cairo/](module-07-cairo/)

---

### 第三阶段：Wayland 服务端入门（8-10 天）

#### 模块 8 — libwayland-server 骨架

**目标**: 理解服务端对象的生命周期：`wl_display_create` → `wl_global` → `wl_resource` → `wl_client`。

- 创建 `wl_display` 并绑定 Unix socket
- 注册一个自定义 global 接口
- 接受客户端连接并处理 request/event
- 实现一个纯协议的"echo server"

> **代码**: [module-08-server-skeleton/](module-08-server-skeleton/)

#### 模块 9 — DRM/KMS 后端（直接控制屏幕）

**目标**: 理解 Compositor 如何直接通过 DRM/KMS 控制显卡输出。

- 打开 `/dev/dri/card*` 设备、成为 DRM master
- 使用 `drmModeGetResources` 发现 CRTC/Connector/Encoder
- 设置显示模式 (`drmModeSetCrtc`)
- 通过 dumb buffer 直接在屏幕上绘制

> **前置阅读**: [docs/01 显卡的作用](docs/01-gpu-and-display.md) · **代码**: [module-09-drm/](module-09-drm/)

#### 模块 10 — libinput 集成

**目标**: 理解 Compositor 如何从内核 evdev 获取并处理输入事件。

- 初始化 libinput 上下文 (`libinput_udev_create_context`)
- 读取 `/dev/input/event*` 设备
- 处理键盘、鼠标、触摸板事件
- 将 libinput 事件循环集成到 Wayland 事件循环中

> **代码**: [module-10-libinput/](module-10-libinput/)

---

### 第四阶段：构建完整 Compositor（11-14 天）

#### 模块 11 — 最小化 TTY Compositor

**目标**: 将 DRM、libinput、libwayland-server 三者串联，创建一个可从 TTY 启动的最小 Compositor。

- DRM 模式设置 + 屏幕输出
- libinput 处理所有输入
- libwayland-server 接受客户端连接
- 客户端可通过 `wl_shm` 提交像素

> **代码**: [module-11-tty-compositor/](module-11-tty-compositor/)（可跑模块 3 的客户端！）

#### 模块 12 — 功能完善的 Compositor

**目标**: 在模块 11 基础上添加窗口管理、xdg-shell 支持和简单的合成。

- 实现完整的 `xdg_wm_base` 协议
- 窗口创建/销毁/焦点切换/交互移动
- 输入事件路由给客户端（模块 4 的对端）
- 简单的窗口重叠合成（画家算法）

> **代码**: [module-12-full-compositor/](module-12-full-compositor/)（毕业设计，附检验清单）

---

### 第五阶段：系统上下文（15-16 天）

#### 模块 13 — 音频栈实验（ALSA + PipeWire）

**目标**: 用同一段正弦波打通「PCM → ALSA → PipeWire → 扬声器」，理解声音服务器存在的理由。

- libasound 直连播放（帧/周期/参数协商的体感）
- 独占性实验：`hw:0,0` 冲突 vs PipeWire 混音
- 作为 node 接入 PipeWire 图（`pw-top` 观察自己）
- registry 枚举（对比模块 1 的 `wl_registry` 模式）

> **前置阅读**: [docs/03 音频栈](docs/03-audio-stack.md) · **代码**: [module-13-audio/](module-13-audio/)

#### 模块 14 — 登录与会话观察（systemd-logind）

**目标**: 把「登录后发生了什么」变成可观测数据，理解 compositor 的设备权力来源。

- sd-login 遍历 seat/session（找出 greeter 会话）
- 设备访问对照实验：图形会话 vs ssh（logind ACL）
- Wayland 服务发现三要素（`XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY`/socket）
- D-Bus TakeControl：免 sudo 接管显示的正道

> **前置阅读**: [docs/04 登录与会话](docs/04-login-and-session.md) · **代码**: [module-14-session/](module-14-session/)

---

## 快速开始

### 1. 配置虚拟机

```bash
# 在虚拟机中执行
sudo apt update
git clone <this-repo> ~/learn-wayland
cd ~/learn-wayland
bash setup-vm.sh
```

### 2. 编译运行（客户端模块）

```bash
# 在现有 Wayland 会话中（如 GNOME on Wayland）
cd module-02-hello-window
make
./hello-window
```

### 3. 编译运行（服务端模块，需 TTY）

```bash
# 切换到 tty3: Ctrl+Alt+F3
# 登录后执行
sudo systemctl stop gdm    # 或 sddm，停掉当前显示管理器
cd module-11-tty-compositor
make
sudo ./compositor
```

### 4. 编译运行（系统上下文模块）

```bash
make system               # 或进入各模块目录 make
cd module-13-audio && ./audio-lab pw-list
cd ../module-14-session && ./session-info me
```

## 关键参考资料

- [Wayland Protocol](https://wayland.freedesktop.org/docs/html/) — 官方协议文档
- [wayland-book.com](https://wayland-book.com/) — 社区最佳入门教程
- [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) — 扩展协议仓库
- [Weston](https://gitlab.freedesktop.org/wayland/weston) — 官方参考 Compositor 实现
- [tinywl](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/tinywl) — wlroots 最小化 Compositor 示例
- [libwayland 源码](https://gitlab.freedesktop.org/wayland/wayland) — libwayland 实现
- [PipeWire 文档](https://docs.pipewire.org/) — 音频/视频服务器
- [DRM/KMS 内核文档](https://docs.kernel.org/driver-api/drm.html) — 显示子系统

## 推荐学习路径

```
 第 0 天 : docs/00 全景地图 (+01/02 按需)     建立坐标
 第 1 天 : 模块 1-2  (建立连接 + 显示窗口)
 第 2 天 : 模块 3    (像素操作)
 第 3 天 : 模块 4    (输入处理)
 第 4 天 : 模块 5    (协议与代码生成, 配合模块 8 联调)
 第 5 天 : 模块 6    (帧同步)
 第 6 天 : 模块 7    (Cairo 渲染)
 第 7 天 : docs/03 音频 + 模块 13 (可并行穿插)
 第 8 天 : 模块 8    (服务端骨架)
 第 9 天 : 模块 9    (DRM/KMS; 前一晚读 docs/01)
 第10 天 : 模块 10   (libinput)
 第11-12天: 模块 11 (最小化 Compositor)
 第13-14天: 模块 12 (完善 Compositor)
 第15 天 : docs/04 + 模块 14 (登录与会话; 复盘启动链路 docs/02)
```

> 学习心法：**每个模块开始前先看它的「全景图定位」，结束后回 [docs/00](docs/00-big-picture.md) 确认自己走过的路**。三条主线（一帧像素 / 一次按键 / 一段声音）在模块 12/13 全部交汇。
