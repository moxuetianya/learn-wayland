# 模块 11：最小化 TTY Wayland Compositor

> 🗺️ **全景定位**: [docs/00 全景地图](../docs/00-big-picture.md) · [docs/01 显卡与显示](../docs/01-gpu-and-display.md)

> 把模块 8 的 `wl_display` 骨架、模块 9 的 DRM/KMS、模块 10 的 libinput 拧成一个
> 真正能从 TTY 启动、能接受客户端连接、能把客户端像素送上屏幕的最小合成器。
> 约 900 行 C，是整个课程的"会师之作"。

## 0. 你将学到什么

- Compositor 的三大职责：**输出**（KMS 模式设置）、**输入**（libinput 事件归一化）、
  **协议服务**（wl_display 对象分发）——三者如何挂进同一个事件循环
- GDM / GNOME Shell / Compositor 三者到底是什么关系，**谁才是 Wayland 服务端**
- DRM master 的排他性：为什么必须停掉图形会话、切到 TTY 才能运行
- dumb buffer 的扫描输出原理：为什么 CPU 往 mmap 区域写一个像素，屏幕上立刻就能看到
- Wayland surface 的核心状态机：`attach → commit → release`，以及 frame callback 的生命周期

## 1. 全景图：Compositor 在图形栈中的位置

```
┌─────────────────────────────────────────────────────┐
│            应用层（Wayland 客户端）                   │
│      weston-simple-shm、终端、浏览器……               │
└────────────────────┬────────────────────────────────┘
                     │  Unix domain socket
                     │  $XDG_RUNTIME_DIR/wayland-N
┌────────────────────▼────────────────────────────────┐
│   本模块：compositor == Wayland 服务端 == 合成器      │
│                                                     │
│  ┌──────────────┐ ┌───────────┐ ┌───────────────┐  │
│  │ wl_display    │ │ libinput  │ │ DRM/KMS       │  │
│  │ 协议解析/分发  │ │ 输入归一化 │ │ 输出模式设置   │  │
│  │ 事件循环主人   │ │ 按键/指针 │ │ 帧缓冲扫描输出 │  │
│  └──────────────┘ └───────────┘ └───────────────┘  │
└─────┬──────────────────┬──────────────────┬─────────┘
      │ socket           │ evdev            │ ioctl
      │                  │ /dev/input/*     │ /dev/dri/card0
┌─────▼──────────────────▼──────────────────▼─────────┐
│                    Linux 内核                        │
└─────────────────────────────────────────────────────┘
```

关键认知：**Wayland "服务端"不是一个独立于桌面的守护进程，而就是合成器进程本身**。
它同时扮演三个角色：

| 角色 | 对应本模块代码 | 对应 GNOME 里的实现 |
|------|---------------|-------------------|
| 协议服务端（监听 socket、分发请求） | `wl_display_*` 一族 | gnome-shell（内部用 mutter 库） |
| 窗口管理器（决定窗口位置/层级/焦点） | `compositor_create_surface` 里写死的 (0,0) | gnome-shell 的窗口管理部分 |
| 显示输出（直接驱动显卡） | `init_drm` / `mc_repaint` | gnome-shell 经 logind 拿 DRM master |

### 1.1 GDM、GNOME、本模块，谁是 Wayland 服务端？

这是初学最容易混淆的一点，用启动链条讲清楚：

```
systemd
 └─ gdm.service                    ← 显示管理器（Display Manager）
     │                               职责：登录界面、PAM 认证、拉起会话、协调 VT 切换
     │                               它不是 Wayland 服务端！
     ├─ 登录前：greeter（一个精简的 gnome-shell 实例，
     │          本身也是个 Wayland 合成器，但只服务"登录框"）
     │
     └─ 你输入密码后：在新 VT 上启动用户会话
         gnome-session
           └─ gnome-shell           ← 这才是日常使用的 Wayland 服务端
                ├─ 经 logind 授权成为 DRM master
                ├─ drmModeSetCrtc / page flip 直接驱动显卡
                └─ 监听 $XDG_RUNTIME_DIR/wayland-0
                     ↑ nautilus、firefox……全是它的客户端
```

- **GDM 之于 gnome-shell**，就像 SDDM 之于 kwin_wayland：门卫与被拉起的进程。
- gnome-shell 以**普通用户**身份运行，它拿 DRM master 走的是 logind 的 `TakeDevice` 授权；
- 本模块用 `sudo` 以 root 直接跑，绕过 logind——学习项目的合理偷懒。

**为什么 `sudo systemctl stop gdm` 是必须的**：内核规定一个 DRM 设备同一时刻只能有
一个 master。GNOME 会话在跑时，gnome-shell 就是现任 master，本模块 `init_drm()` 里的
`drmSetMaster()` 会失败（代码里专门为此打了警告），后续 `drmModeSetCrtc` 也会被拒绝。
停掉 gdm 会连带放倒 `gdm → gnome-session → gnome-shell` 整条链，释放 DRM master
和图形 VT，本模块才有机会接管显卡。

## 2. 三条通路的原理

### 2.1 输出通路：DRM/KMS 扫描管线

内核把"显卡怎么点亮屏幕"抽象成一条对象管线：

```
framebuffer ──→ CRTC ──→ encoder ──→ connector ──→ 显示器
(像素从哪读)   (何时/如何    (转成接口     (物理端口)
               扫描每一行)   信号)
```

- **framebuffer (FB)**：一块供扫描的像素存储。本模块用 *dumb buffer*——
  `DRM_IOCTL_MODE_CREATE_DUMB` 让内核分配一块**线性的、CPU 可映射**的显存/内存，
  再 `drmModeAddFB2` 把它包装成 FB，`DRM_IOCTL_MODE_MAP_DUMB` + `mmap` 拿到 CPU
  可直接读写的指针。
- **CRTC**（CRT controller 的历史名字）：真正按模式时钟（如 60Hz）逐行读取 FB、
  产生扫描时序的硬件。`drmModeSetCrtc` 一次性完成"这个 CRTC 从这个 FB 读、
  用这个模式、扫到这个 connector"的绑定。
- **encoder/connector**：encoder 把 CRTC 输出转换成 TMDS/DP/LVDS 等接口信号，
  connector 是物理端口。不是任意 CRTC 都能接任意 connector，`find_crtc()` 里
  encoder 的 `possible_crtcs` 位图描述了合法组合。

**为什么 CPU 写入即可见**：dumb buffer 是被 CRTC *正在扫描*的单缓冲。扫描光栅每
1/60 秒把整块 FB 读一遍，你写入的像素最迟在下一遍扫描时就会出现在屏幕上。代价是
**没有 vsync 同步**：写入如果撞上扫描位置会产生撕裂。生产级合成器（mutter、wlroots）
用双缓冲 + `drmModePageFlip` 在 vblank 时刻原子换帧——本模块为了简单牺牲了这一点，
`drmModeDirtyFB` 只是对某些需要提示的驱动打个招呼。

**DRM master**：模式设置类 ioctl 是特权操作，只有 master 能调用。同一时刻一个
`/dev/dri/card0` 只能有一个 master（root 直接 `drmSetMaster` 即可；普通用户需要
logind 代为授权——这正是上一节 gnome-shell 的做法）。退出时 `drmDropMaster` 归还。

### 2.2 输入通路：libinput

内核 evdev 只给你原始的 `/dev/input/event*` 字节流；libinput 负责归一化：
合并键盘/指针/触摸板差异、触摸板手指→指针运动映射、去抖、加速曲线。要点：

- **udev 后端**：`libinput_udev_create_context` + `libinput_udev_assign_seat("seat0")`
  让 libinput 通过 udev 枚举出属于该 seat 的所有输入设备并自动处理热插拔。
- **open_restricted 回调**：libinput *不自己* open 设备节点——它要求使用者在
  `struct libinput_interface` 里提供 open/close 回调。这样特权处理（root 或 logind
  授权）留给合成器，libinput 保持无特权。本模块以 root 运行，回调里直接 `open()`。
- **dispatch 模型**：`libinput_get_fd()` 返回一个可 epoll 的 fd。fd 可读 **不代表**
  事件已经就绪——必须先调 `libinput_dispatch()` 让 libinput 读原始数据并解析成事件
  队列，再用 `libinput_get_event()` 循环取空。顺序反了就会"有事件却取不到"。

### 2.3 协议通路：wl_display / wl_global / wl_resource

模块 8 已经搭过 echo server 骨架，本模块是它的"真业务"版本：

```
wl_display_create()
   │
   ├─ wl_global_create(wl_compositor_interface, ...)
   │     → 客户端 wl_registry 广播里出现 wl_compositor
   │     → 客户端 bind → compositor_bind() → wl_resource_create()
   │                     + wl_resource_set_implementation(compositor_impl)
   │
   ├─ wl_display_init_shm()     ← libwayland 内建的 wl_shm 实现！
   │     → 自动注册 wl_shm global，自动处理 wl_shm_pool/wl_buffer 请求，
   │       并提供服务端取像素的 wl_shm_buffer_get_* 接口
   │
   └─ wl_display_add_socket_auto()
         → 创建 $XDG_RUNTIME_DIR/wayland-N 并监听
```

服务端的一切都是"global 注册 → 客户端 bind → resource 上挂实现 → 请求进 vtable
函数"这一套。注意 `wl_shm` 不用手写：`wl_display_init_shm` 一个调用搞定，这是
libwayland 给合成器作者的礼物（头文件没声明，所以代码里 `extern` 了一下）。

### 2.4 surface 状态机：attach → commit → release

Wayland 客户端渲染的核心协议只有两个动作，本模块在 `mc_surface` 里如实还原：

```
客户端                                合成器（本模块）
────────                              ──────────────
wl_surface.attach(buffer)   ──────→   surface_attach(): 只记到 pending_buffer
wl_surface.commit()         ──────→   surface_commit():
                                         1. 对上一帧 current buffer 发
                                            wl_buffer.release()  ← 允许客户端复用
                                         2. pending → current
                                         3. mc_repaint() 全量重绘
                                         4. 触发 frame callback (若客户端请求过)
```

- **pending/current 双态**是 Wayland 的原子更新模型：attach 只是"暂存"，commit 才
  "生效"。本模块没有 damage/offset 等其它 pending 状态，但机制相同。
- **release 语义**：客户端提交 buffer 后就失去了它的写权，直到合成器发 release。
  本模块还用 `watch_buffer()` 给 pending/current 各挂一个销毁监听：客户端提前销毁
  buffer 时，监听器把指针安全置 NULL，避免 use-after-free。
  没有这一步，客户端的双缓冲池会立刻"饿死"——这是新手合成器最常见的 bug。
- **frame callback 是一次性的**：协议规定 done 之后资源即销毁，客户端每帧重新
  `wl_surface.frame()` 请求新的。这是"请问下一帧什么时候画"的节流问答，
  没有它客户端会 busy-loop 渲染（模块 6 在客户端视角学过）。

本模块的简化：commit 后**立即**触发 frame callback（真实合成器应等该帧真正上屏，
即 vblank/page-flip 完成后），所以客户端的帧率与屏幕刷新并未真正同步。

## 3. 代码走读

文件只有一个 `compositor.c`，按 `main()` 的执行顺序：

### 3.1 init_drm() —— 接管输出

1. `open_drm_card()`：优先环境变量 `$DRM_CARD` 指定的设备，否则自动扫描
   第一个可打开的 `/dev/dri/card[0-9]*`（多显卡/命名不是 card0 的机器友好），
   然后 `drmSetMaster()`（root 下必成；失败说明有别的会话）
2. `drmModeGetResources()` 枚举全部 connector/CRTC/encoder
3. 挑第一个 `DRM_MODE_CONNECTED` 且有模式的 connector；模式优先
   `DRM_MODE_TYPE_PREFERRED`（显示器 EDID 里标注的推荐分辨率）
4. `find_crtc()`：优先复用 connector 当前 encoder 绑定的 CRTC（最保守），
   否则查 encoder 的 `possible_crtcs` 位图找合法组合
5. `drmModeGetCrtc()` **保存现场**——退出时恢复，TTY 才不会黑屏/花屏
6. `create_screen_fb()`：CREATE_DUMB → AddFB2 → MAP_DUMB → mmap（见 2.1）
7. `drmModeSetCrtc()`：点亮，从此屏幕扫描我们的 FB

### 3.2 init_libinput() —— 归一化输入

udev 上下文 → libinput 上下文 → 绑定 seat0。之后
`wl_event_loop_add_fd(loop, libinput_get_fd(...), WL_EVENT_READABLE, on_input_data, ...)`
把输入 fd 挂进 Wayland 事件循环——合成器只需要一个主循环。

### 3.3 wl_display 建立

`wl_display_create` → client_created 监听（记账用）→ `wl_display_add_socket_auto`
（自动选 wayland-1、wayland-2……不与运行中的会话冲突，并把选中的名字打印出来）
→ 注册 `wl_compositor` global（版本 4）→ `wl_display_init_shm`。

### 3.4 surface 实现与绘制

- `compositor_create_surface()`：新 surface 固定摆放在 (0,0)，插到链表尾——
  链表顺序即绘制顺序（画家算法，后画的盖住先画的）。
- `mc_repaint()`：清屏黑 → 按链表序 `paint_surface()` → `drmModeDirtyFB`。
- `paint_surface()`：只认 wl_shm buffer（`wl_shm_buffer_get` 拿不到就忽略），
  只认 XRGB8888/ARGB8888；`begin/end_access` 包住取数据的区间（池的引用计数）；
  逐像素拷贝并做屏幕边界裁剪（注意屏幕 stride 与客户端 buffer stride 是两个量）。

### 3.5 输入处理与退出

`on_input_data()`：dispatch → 取空事件队列，按类型分发。键盘路径里用静态变量
跟踪 Ctrl/Alt 修饰键状态，`Esc` 或 `Ctrl+Alt+Backspace` 调 `wl_display_terminate()`
让主循环退出。`cleanup()` 按**依赖关系的反序**拆解：libinput → wl_display（先
`destroy_clients` 再销毁）→ 恢复 CRTC → munmap/RmFB/销毁 dumb buffer → DropMaster。

## 4. 编译与运行

前置：真实机器或 VM 的**纯 TTY**，且已停掉图形会话。依赖
`libdrm-dev`、`libinput-dev`、`libudev-dev`（wayland-server 随客户端
阶段的 `libwayland-dev` 已就位）；一次装齐可跑 `bash setup-vm.sh`。

```bash
# 1. 切到 tty3（VM 里通常是 Ctrl+Alt+F3），登录
sudo systemctl stop gdm        # 或 sddm / lightdm，取决于发行版

# 2. 编译并启动
cd module-11-tty-compositor
make
sudo ./compositor
```

启动成功的输出大致是：

```
=== 模块 11: 最小化 TTY Wayland Compositor ===
connector id=61  模式 "preferred" 1920x1080 @ 60Hz
CRTC id=60
screen fb_id=68 handle=1 stride=7680 size=8294400
Compositor 已启动。
  WAYLAND_DISPLAY=wayland-1
  连接客户端示例: WAYLAND_DISPLAY=wayland-1 weston-simple-shm
  退出: Esc 或 Ctrl+Alt+Backspace
```

在另一个 TTY（Ctrl+Alt+F4）或 SSH 里连接客户端验证：

```bash
WAYLAND_DISPLAY=wayland-1 weston-simple-shm   # 需安装 weston，只依赖 wl_surface
```

能移动的彩色渐进方块出现在屏幕左上角 = 你的合成器完成了第一次客户组合成。

退出与恢复：

```bash
# 合成器内按 Esc（或 Ctrl+Alt+Backspace），它会恢复 CRTC 后退出
# 回到图形登录界面：
sudo systemctl start gdm
```

## 5. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| `Package libdrm/libinput was not found ...` + `fatal error: xf86drm.h` 等 | 缺服务端开发包，pkg-config 失败 → `CFLAGS`/`LDFLAGS` 展开为空（报错链同模块 9） | `bash setup-vm.sh`，或 `sudo apt install libdrm-dev libinput-dev libudev-dev` |
| `drmSetMaster 失败` 警告后黑屏/无输出 | gnome-shell 仍是 DRM master | 确认 `systemctl stop gdm` 已执行；`pgrep -a gnome-shell` 应为空 |
| `找不到已连接且带有效模式的 connector` | VM 显卡是 virtio-gpu 且未暴露模式 / 无显示器直通 | 检查 VM 显示设置；QEMU 建议用 `virtio-gpu` 并指定分辨率 |
| 客户端能连上但"什么都没显示" | 该客户端需要 xdg-shell（本模块未实现）或非 shm buffer | 换纯 wl_surface 客户端，如 `weston-simple-shm`；xdg-shell 见模块 12 |
| 合成器退出后 TTY 花屏 | CRTC 恢复失败/被跳过 | 切到其它 TTY 再切回；一般 `start gdm` 即可恢复 |
| 客户端只画了一帧就卡住 | 合成器忘了 `wl_buffer_send_release`（本模块已处理，见 surface_commit） | 对照 2.4 的 release 语义排查 |
| 非 root 运行打开 card0 成功但模式设置全失败 | 非 master 无权模式设置 | `sudo`；或走 logind 授权（本课程不展开） |

## 6. 与真实实现的差距

| 本模块 | 生产合成器（mutter / wlroots / Weston） |
|--------|----------------------------------------|
| dumb buffer 单缓冲，写入即可见，可能撕裂 | GBM 分配 + 双/三缓冲 + `drmModePageFlip` 于 vblank 原子换帧 |
| commit 后立即触发 frame callback | page flip 完成后触发，真正对齐刷新率 |
| 每次 commit 全屏重绘 | damage tracking + 局部重绘/仅 dirty 区域合成 |
| 客户端永远在 (0,0)，无输入转发 | xdg-shell 窗口管理、焦点、指针/键盘路由（模块 12 迈出第一步） |
| root 直跑，不处理 VT 切换 | logind 集成：VT 切走时 DropMaster、切回时恢复；支持多 seat |
| 只支持 wl_shm 客户端 | EGL/GPU 直渲染（linux-dmabuf 协议） |

## 7. 练习

1. **验证扫描输出**：把 `mc_repaint` 的清屏色从黑改成深蓝，重编译运行，观察"代码 →
   像素"的通路。
2. **移动的窗口**：在 `handle_pointer_motion` 里累计 dx/dy 更新第一个 surface 的
   x/y 并 repaint——你就实现了最原始的"拖拽"。
3. **观察协议流**：用 `WAYLAND_DEBUG=1 WAYLAND_DISPLAY=wayland-1 weston-simple-shm`
   重跑客户端，对照 2.4 的状态机解读 attach/commit/release/frame 序列。
4. **制造撕裂**：写一个全屏渐变动画（每帧 memset 整屏），大概率能看到撕裂线——
   这就是单缓冲没有 vsync 的直观代价，也是 page flip 存在的理由。
5. **帧率测量**：让客户端每帧计数打印，对比 60Hz 屏幕——本模块立即回 frame
   callback，客户端会远超 60fps；思考为什么这"不健康"。

## 8. 延伸阅读

- [DRM/KMS 用户态 API 文档](https://drm.pages.freedesktop.org/drm-docs/drm/) — 内核官方
- [tinywl](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/tinywl) — wlroots 的最小合成器，可视为"本模块 + xdg-shell + page flip"
- [Weston](https://gitlab.freedesktop.org/wayland/weston) — 官方参考合成器
- [mutter](https://gitlab.gnome.org/GNOME/mutter) — gnome-shell 背后的合成器库
