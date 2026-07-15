# Learn Wayland — 从零到 Compositor 开发

> 基于 Wayland 协议的渐进式学习课程，从客户端到服务端，覆盖协议核心、扩展协议、DRM/KMS、libinput 等关键技术栈。

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
├── README.md                     # 本文件
├── setup-vm.sh                   # 一键安装所有依赖
├── Makefile                      # 顶层构建
├── common/                       # 共享工具
│   ├── wayland-util.h
│   └── wayland-util.c
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
└── module-12-full-compositor/
```

## 课程大纲

---

### 第一阶段：Wayland 客户端开发（1-5 天）

#### 模块 1 — 建立连接与服务发现

**目标**: 理解 `wl_display`、`wl_registry`、事件循环三大核心概念的运作方式。

- 连接到 Wayland Compositor 并获取 `wl_display`
- 通过 `wl_registry` 发现服务器支持的全局对象
- 实现事件循环 (`wl_display_dispatch`)
- 打印所有可用的全局接口及版本

> **代码**: `module-01-connection/main.c` — 连接、发现、列出所有全局接口

#### 模块 2 — 第一个 Wayland 窗口

**目标**: 掌握 `wl_compositor` → `wl_surface` → `xdg-shell` 的完整窗口创建流程。

- 使用 `xdg_wm_base` 创建 `xdg_toplevel`（真正的窗口角色）
- 使用 `wl_shm` 创建共享内存缓冲区
- 填充像素数据并提交到屏幕
- 理解 `attach → commit` 模型

> **代码**: `module-02-hello-window/main.c` — 显示 640×480 彩色窗口

#### 模块 3 — 手动像素缓冲区与图形绘制

**目标**: 深入理解像素格式、颜色空间与帧缓冲区结构。

- 理解 `WL_SHM_FORMAT_XRGB8888` 等像素格式
- 手写简单的图形 primitive（画线、填充矩形、渐变）
- 实现简单的动画帧（连续重绘）

> **代码**: `module-03-pixels/main.c` — 绘制几何图案与渐变背景

#### 模块 4 — 键盘与鼠标输入处理

**目标**: 掌握 Wayland 的输入模型（`wl_seat` → `wl_keyboard` → `wl_pointer`）。

- 监听 `wl_keyboard` 按键事件并打印 keycode
- 使用 `libxkbcommon` 将 keycode 转换为可读键名
- 监听 `wl_pointer` 移动/点击事件
- 理解 `wl_seat` 的能力广播机制

> **代码**: `module-04-input/main.c` — 交互式输入调试窗口

#### 模块 5 — 协议 XML 与 wayland-scanner

**目标**: 理解 Wayland 协议文件的语法、代码生成与版本协商机制。

- 阅读核心协议 `wayland.xml` 的关键接口定义
- 编写一个简单的自定义协议 XML
- 使用 `wayland-scanner` 生成 client-header 和 private-code
- 实现一个简单的自定义协议通信

> **代码**: `module-05-protocol/` — 自定义协议 + 客户端实现

---

### 第二阶段：客户端渲染进阶（6-7 天）

#### 模块 6 — 帧回调与垂直同步

**目标**: 理解 Wayland 的帧调度机制，实现无撕裂渲染。

- 使用 `wl_surface.frame()` 获取帧回调 (`wl_callback`)
- 实现"每帧渲染一次"的稳定动画循环
- 对比 busy-loop 与 frame-callback 的性能差异

> **代码**: `module-06-frame-cb/main.c` — 60fps 动画窗口

#### 模块 7 — Cairo 软件渲染集成

**目标**: 将成熟的 2D 渲染库与 Wayland 缓冲区对接。

- 使用 Cairo 在 `wl_shm` 缓冲区上绘制
- 绘制抗锯齿文字、贝塞尔曲线、渐变
- 创建简单的 UI 组件（按钮、文本框）

> **代码**: `module-07-cairo/main.c` — 使用 Cairo 渲染的窗口应用

---

### 第三阶段：Wayland 服务端入门（8-10 天）

#### 模块 8 — libwayland-server 骨架

**目标**: 理解服务端对象的生命周期：`wl_display_create` → `wl_global` → `wl_resource` → `wl_client`。

- 创建 `wl_display` 并绑定 Unix socket
- 注册一个自定义 global 接口
- 接受客户端连接并处理 request/event
- 实现一个纯协议的"echo server"

> **代码**: `module-08-server-skeleton/` — 客户端+服务器完整通信示例

#### 模块 9 — DRM/KMS 后端（直接控制屏幕）

**目标**: 理解 Compositor 如何直接通过 DRM/KMS 控制显卡输出。

- 打开 `/dev/dri/card*` 设备
- 使用 `drmModeGetResources` 发现 CRTC/Connector/Encoder
- 设置显示模式 (`drmModeSetCrtc`)
- 通过 dumb buffer 直接在屏幕上绘制

> **代码**: `module-09-drm/main.c` — 从 TTY 打印纯色到屏幕

#### 模块 10 — libinput 集成

**目标**: 理解 Compositor 如何从内核 evdev 获取并处理输入事件。

- 初始化 libinput 上下文 (`libinput_udev_create_context`)
- 读取 `/dev/input/event*` 设备
- 处理键盘、鼠标、触摸板事件
- 将 libinput 事件循环集成到 Wayland 事件循环中

> **代码**: `module-10-libinput/main.c` — 从 TTY 打印所有输入事件

---

### 第四阶段：构建完整 Compositor（11-14 天）

#### 模块 11 — 最小化 TTY Compositor

**目标**: 将 DRM、libinput、libwayland-server 三者串联，创建一个可从 TTY 启动的最小 Compositor。

- DRM 模式设置 + 屏幕输出
- libinput 处理所有输入
- libwayland-server 接受客户端连接
- 客户端可通过 `wl_shm` 提交像素

> **代码**: `module-11-tty-compositor/` — ~500 行最小化 Compositor

#### 模块 12 — 功能完善的 Compositor

**目标**: 在模块 11 基础上添加窗口管理、xdg-shell 支持和简单的合成。

- 实现完整的 `xdg_wm_base` 协议
- 窗口创建/销毁管理
- 输入焦点切换
- 简单的窗口重叠合成（画家算法）

> **代码**: `module-12-full-compositor/` — 功能完善的 TTY Compositor

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

## 关键参考资料

- [Wayland Protocol](https://wayland.freedesktop.org/docs/html/) — 官方协议文档
- [wayland-book.com](https://wayland-book.com/) — 社区最佳入门教程
- [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) — 扩展协议仓库
- [Weston](https://gitlab.freedesktop.org/wayland/weston) — 官方参考 Compositor 实现
- [tinywl](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/tinywl) — wlroots 最小化 Compositor 示例
- [libwayland 源码](https://gitlab.freedesktop.org/wayland/wayland) — libwayland 实现

## 推荐学习路径

```
第 1 天: 模块 1-2  (建立连接 + 显示窗口)
第 2 天: 模块 3    (像素操作)
第 3 天: 模块 4    (输入处理)
第 4 天: 模块 5    (协议与代码生成)
第 5 天: 模块 6    (帧同步)
第 6 天: 模块 7    (Cairo 渲染)
第 7 天: 模块 8    (服务端骨架)
第 8 天: 模块 9    (DRM/KMS)
第 9 天: 模块 10   (libinput)
第10-11 天: 模块 11 (最小化 Compositor)
第12-14 天: 模块 12 (完善 Compositor)
```
