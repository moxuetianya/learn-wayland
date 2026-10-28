# 模块 2 — 第一个 Wayland 窗口

> **全景图定位**: [docs/00 §3.1](../docs/00-big-picture.md) 「一帧像素的旅程」的前两步——buffer 与 attach/commit。
> **前置**: [模块 1](../module-01-connection/) · **产出**: 一个 640×480 渐变窗口

## 目标

- 掌握 `wl_compositor → wl_surface → xdg-shell` 的窗口创建全流程
- 理解 **wl_shm 共享内存 buffer**：像素如何跨进程传递
- 建立 **attach → commit** 双阶段提交模型的心智（Wayland 最重要的协议设计）

## 原理讲解

### 从 surface 到「窗口」：为什么需要 xdg-shell

`wl_surface` 只是一块「可显示的像素矩形」，没有任何窗口语义（没有标题、不能关闭、桌面不知道拿它怎么办）。要成为真正的窗口，必须给它一个**角色 (role)**——桌面角色就是 `xdg_toplevel`：

```
wl_surface          (像素的载体, 课程的"画布")
   │ xdg_wm_base.get_xdg_surface()
   ▼
xdg_surface         (提交协议: configure/ack 配对)
   │ get_toplevel()
   ▼
xdg_toplevel        (窗口角色: 标题/最大化/关闭...)
```

> 为什么要绕两层？因为还有别的角色（popup 弹窗、图层 shell 等）。**一个 surface 一生只能绑一个角色**。

### configure/ack：谁说了算

窗口尺寸**不是客户端单方面定的**。协议规定了一个回合制握手：

```
客户端                                compositor
  │ commit(初始空提交) ──────────────▶ │ "你想要窗口了?"
  │◀── configure(serial, 640x480) ───── │ "按这个尺寸画" (桌面可能改你的请求)
  │ ack_configure(serial) ────────────▶ │ "收到"
  │ attach(buffer) + commit() ─────────▶ │ 首帧像素上屏
```

在 configure 之前 attach buffer 是协议错误（服务器可能直接断连）——[main.c](main.c) 里首次绘制放在 `xdg_surface_configure` 回调里就是这个原因。

### wl_shm：跨进程传像素的最朴素方式

```
你的进程                                  compositor
1. mkstemp 创建临时文件, unlink(匿名)
2. ftruncate 扩到 stride×height
3. mmap ──▶ 得到可写内存, 填像素
4. wl_shm_create_pool(fd, size) ─────fd 通过 unix socket 传递────▶ 5. compositor mmap 同一文件
6. wl_shm_pool_create_buffer(...)                                  7. 两边看到同一块物理内存
```

- **stride**（行跨度）= `width × 4`。像素格式 `XRGB8888`：每像素 4 字节，内存序 B,G,R,X（x86 小端）；
- 传的是 **文件描述符** 而非数据——Unix socket 的 SCM_RIGHTS 机制，零拷贝；
- 这是 CPU 渲染路径；GPU 渲染对应 [linux-dmabuf 协议](../docs/01-gpu-and-display.md#4-buffer-的跨进程旅行wl_shm-与-dmabuf)，思想相同（传句柄不传数据）。

### attach/commit：双缓冲的提交语义

```
attach(buffer)   ──▶ "下次生效的 buffer 换成它"   (pending 状态)
set_title(...)   ──▶ "下次生效的标题..."          (pending 状态)
commit()         ──▶ ★ 原子地把全部 pending 变为 current
```

一次 commit 就是一次「事务提交」。compositor 在自己的刷新周期里取走 current 状态合成——**客户端永远不知道也不需要知道合成发生的精确时刻**（要同步就用模块 6 的帧回调）。

## 代码走读

| 位置 | 内容 |
|------|------|
| [L26-80](main.c#L26-L80) | `create_buffer()`: 匿名文件 → mmap → 填渐变像素 → `wl_shm_pool` → `wl_buffer` 全流程 |
| [L84-89](main.c#L84-L89) | `xdg_wm_base_ping` 回调：服务器存活探测，**必须回 pong**，否则窗口被判定死亡 |
| [L93-104](main.c#L93-L104) | `xdg_surface_configure`：ack 后首次绘制提交 |
| [L129-145](main.c#L129-L145) | registry 过滤：本模块需要 `wl_compositor` + `wl_shm` + `xdg_wm_base` 三个 global |
| [L177-236](main.c#L177-L236) | main: 标准化的连接→建窗→事件循环→销毁清理 |

## 动手实验

```bash
cd module-02-hello-window
make && ./hello-window
```

### 观察点

1. **协议错误实验**：把 [L99-101](main.c#L99-L101) 的首次绘制移到 configure 回调之外（main 里 commit 后立刻 attach），重新编译运行——观察 compositor 的报错（GNOME 下 `journalctl` 或运行输出会提示 invalid state）。体验「协议是契约」；
2. **像素格式实验**：把 [L69](main.c#L69) 的 `r<<16 | g<<8 | b` 改成 `b<<16 | r<<8 | g`，直观看 XRGB8888 的字节序；
3. **stride 实验**：故意把 stride 改成 `width*4+64`（每行尾 64 字节 padding），`wl_shm_pool_create_buffer` 照样传这个 stride——画面应该不变，理解 stride 的意义（合法且常见：对齐加速）；
4. **对比图形栈**：`xwininfo`/`xlsclients` 看不到这个窗口（它是纯 Wayland 窗口），用 `wayland-info` 或桌面的开发工具能看到。

## 进阶练习

1. 响应窗口 resize：`xdg_toplevel_configure` 给了新尺寸（代码已保存），让 buffer 跟着重建（提示：每次 configure 检查尺寸变化）；
2. 窗口标题改成你的名字，加 `xdg_toplevel_set_app_id`，观察任务栏图标归类变化；
3. 用 `wl_display_get_error(display)` 在断连时打出协议错误码。

## 常见坑

| 现象 | 原因 |
|------|------|
| 窗口一闪就消失/被杀 | 没回 pong（ping 超时）；或缺 ack_configure |
| 窗口黑屏 | buffer 全零 + X 通道恰好为 0 之外的颜色损坏；检查 `0xFF<<24` |
| `invalid commit` 断连 | configure 前 attach/commit 了 buffer |
| 编译缺头文件 | `xdg-shell-client-protocol.h` 由 Makefile 从 wayland-protocols 生成，先 `make` |

## 延伸阅读

- [xdg-shell 协议原文](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/stable/xdg-shell/xdg-shell.xml) — configure 语义的权威定义
- [wayland-book: Surfaces](https://wayland-book.com/protocol-design/surfaces.html)

下一模块: [模块 3 — 手动像素绘制](../module-03-pixels/)
