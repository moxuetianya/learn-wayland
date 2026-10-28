# 模块 11 — 最小化 TTY Compositor

> **全景图定位**: [docs/00 §3.1 + §3.2](../docs/00-big-picture.md) — **三条道路在此汇合**：DRM/KMS（模块 9）+ libinput（模块 10）+ libwayland-server（模块 8）= 一个真正的 compositor。
> **产出**: ~900 行、可从 TTY 启动、能跑 `weston-simple-shm` 的最小合成器

## 目标

- 把显示、输入、协议三路 IO 汇入**同一个事件循环**
- 实现 `wl_compositor`/`wl_surface`/`wl_shm` 的服务端语义（attach/commit 双缓冲）
- 用「画家算法」在 dumb buffer 上完成第一次窗口合成

## 原理讲解

### compositor 的本体：一个事件驱动的多路复用器

剥掉所有细节，compositor 的主循环就是：

```
                 wl_event_loop (epoll 封装)
                ┌─────────────────────────────┐
   客户端 socket ─┤                             ├─▶ wl_display 的 dispatch
   libinput fd  ─┤        一个 poll            ├─▶ on_input_data() (模块 10 的循环)
   (将来: vblank ─┤                             ├─▶ page flip 完成 → 发帧回调
    /timer/...)   └─────────────────────────────┘
```

[L893-898](compositor.c#L893-L898) 把 libinput 的 fd 挂进 `wl_event_loop`——这一行是本模块的「合龙时刻」。对照模块 10 的裸 poll：现在输入事件和协议消息在同一循环里排队等待处理，互不阻塞。

### wl_surface 的服务端实现：pending → current

模块 2 客户端用的 `attach/commit`，在这里被实现：

```
客户端 attach(buf) ──▶ surface_attach(): s->pending_buffer = buf   [L422-430]
客户端 commit()   ──▶ surface_commit():                          [L472-494]
                        ① release 旧 buffer (让客户端可复用)
                        ② pending → current
                        ③ mc_repaint() 合成上屏
                        ④ 触发 frame callback (done 事件)
```

`damage`/`set_input_region` 等请求收下但忽略（最小实现）——**接口必须应答，行为可以打折**，这是渐进实现协议的合法姿势。

### 合成：画家算法 + 裁剪拷贝

`mc_repaint()`（[L376-397](compositor.c#L376-L397)）：

```
清屏黑 → for 每个 surface (按插入顺序): 逐行拷贝像素 (带边界裁剪)
```

- **画家算法**：后画的盖先画的——wl_list 的插入顺序天然是 z-order；
- `paint_surface()`（[L326-374](compositor.c#L326-L374)）处理两个现实问题：**stride 不同**（客户端 buffer 的行跨度 ≠ 屏幕 stride，按字节偏移取行）与 **格式检查**（只接受 XRGB8888/ARGB8888 的 shm buffer）；
- `wl_shm_buffer_begin_access/end_access`：防止和客户端并发写同一块内存（futex 锁）。

> 这是纯 CPU 合成。生产 compositor 把这一步交给 GPU（[docs/01 §7](../docs/01-gpu-and-display.md#7-把整条路连起来)），但「收集可见 surface → 按序叠加 → 产出一帧」的逻辑骨架完全相同。

### wl_shm：白嫖 libwayland 的内建实现

`wl_display_init_shm()`（[L886-890](compositor.c#L886-L890)，extern 声明因为头文件没导出）一行注册 `wl_shm` 全局——pool/buffer 的解析、fd 接收、mmap 全由 libwayland-server 代劳，你只管 `wl_shm_buffer_get_data()` 读像素。**协议库替你干脏活的典型样本**。

### 输入路由的最小版

`handle_keyboard`（[L693-720](compositor.c#L693-L720)）目前只处理全局快捷键（Esc/Ctrl+Alt+Backspace 退出）——事件**没有**转发给客户端（那是模块 12 的活）。指针同理只打印。此刻你拥有的是一个「输入孤儿院」：收得到、送不出。

## 代码走读

按启动顺序（`main` [L831-915](compositor.c#L831-L915)）：

| 步骤 | 函数 | 要点 |
|------|------|------|
| 1 | `init_drm` [L195-294](compositor.c#L195-L294) | 模块 9 全套：master→connector→CRTC→dumb buffer→SetCrtc |
| 2 | `init_libinput` [L773-800](compositor.c#L773-L800) | 模块 10 全套：udev ctx + seat0 |
| 3 | `wl_display_create` + socket | 模块 8 的骨架 |
| 4 | `wl_global_create(wl_compositor)` | bind 时挂 surface_impl |
| 5 | `wl_display_init_shm` | 内建 wl_shm |
| 6 | `wl_event_loop_add_fd(libinput_fd)` | ★ 三路合流 |
| 7 | `wl_display_run` | 进入主循环 |

数据结构三个：`mc_compositor`（全局状态）、`mc_surface`（每个窗口：pending/current buffer + 帧回调 + 坐标）、`mc_client`（连接跟踪）。

## 动手实验

```bash
# tty3, 停 DM
sudo systemctl stop gdm
cd module-11-tty-compositor
make && sudo ./compositor
# 记下打印的 WAYLAND_DISPLAY (如 wayland-1)
```

屏幕变黑（compositor 接管）。另开 tty2 登录，连一个客户端：

```bash
# 安装演示客户端: sudo apt install weston   (或 libweston 随附的 simple-shm)
WAYLAND_DISPLAY=wayland-1 weston-simple-shm
# 或用你自己的模块 3!
cd ../module-03-pixels && make
WAYLAND_DISPLAY=wayland-1 ./pixels
```

### 观察点

1. **你自己课程的客户端**：模块 3 的动画窗口在你的 compositor 上跑起来——**1-7 模块的客户端 × 8-11 模块的服务端，全课程打通**；
2. **多窗口合成**：再开一个 `weston-simple-egl`（不行，本版只支持 shm；用两个 simple-shm）——后启动的盖住先前的（画家算法）；关掉一个，下层重绘可见；
3. **没有 xdg-shell**：模块 2 的 `hello-window` 连不上（缺 `xdg_wm_base` global，bind 检查失败退出）——体会「compositor 决定支持什么」；模块 12 补全它；
4. **输入侧**：动鼠标/打字只在 compositor 的 TTY 打印日志，客户端收不到（对照观察点 3 的原因）；
5. **frame callback 生效**：模块 6 的 `frame-cb` 能跑且动画节拍正常——`surface_commit` 里发的 done（[L487-493](compositor.c#L487-L493)）在驱动它；
6. **退出恢复**：Esc 退出 → CRTC 恢复 → tty3 控制台完好（saved_crtc 的功劳）；
7. **协议调试**：`WAYLAND_DEBUG=1 weston-simple-shm 2>&1 | head -40` 看客户端视角的完整握手。

## 进阶练习

1. **窗口平铺**：surface 创建时按 `x += 40, y += 40` 级联排布，避免全叠在 (0,0)；
2. **输入转发**：把 `handle_pointer_motion` 的 dx/dy 累加成光标坐标，画一个软件光标（模块 12 有完整实现可对照）；
3. **vblank 同步**：把 repaint 改成 `drmModePageFlip` + DRM 事件驱动（模块 9 进阶练习的延续），done 事件改在 flip 完成后发——更接近真实 compositor 的时序；
4. **damage 优化**：记录 `surface_damage` 的脏矩形，repaint 时只重绘脏区（全量合成的 CPU 开销在 4K 下会暴露）。

## 常见坑（含真实调试案例）

> 2026-10 实机验证中，本模块修复了与模块 12 同源的 **buffer 悬挂指针** bug：客户端在 commit 后销毁 `wl_buffer`，而 compositor 的 `surface_commit` 仍对旧 buffer 调 `wl_buffer_send_release()` → use-after-free 崩溃。现以 `mc_buffer_watch`（`wl_resource` destroy 监听）跟踪 buffer 生命周期，销毁即清空 `s->buffer / s->pending_buffer`。详见 [模块 12 README](../module-12-full-compositor/) 的完整案例分析。
> 另：`sudo` 运行时 `XDG_RUNTIME_DIR` 被清空，现自动回退到 `/tmp/wayland-runtime`；socket 权限放开到 0777 以便普通用户客户端连接（生产环境应走 logind 授权而非 root）。

| 现象 | 原因 |
|------|------|
| 客户端连上但窗口不显示 | 客户端用了非 shm 的 buffer（EGL），或格式不是 XRGB/ARGB8888 |
| 屏幕花屏/斜切 | `paint_surface` 的 stride 处理被改坏——回去看 L355-371 的逐行拷贝 |
| 启动即 DRM master 失败 | gdm 没停干净（`systemctl status gdm`）或另一个实例还在；设备节点不是 card0 时程序会自动扫描（也可 `DRM_CARD=` 指定） |
| 键盘狂触发 | `handle_keyboard` 的 DOWN/UP 状态判断被去掉（连发） |
| 退出后 tty 花屏 | 退出路径没走 cleanup（被信号杀死）→ `sudo systemctl restart gdm` 或重切 VT 恢复 |
| 客户端 commit 后卡死 | repaint 在持锁时调了会回锁的 wl 函数；或 frame callback 重复发送 |

## 延伸阅读

- [tinywl](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/tinywl) — wlroots 的教学 compositor（本模块的工业亲戚）
- [wayland-book: Compositor 设计](https://wayland-book.com/server-side/index.html)

下一模块: [模块 12 — 功能完善的 Compositor](../module-12-full-compositor/)
