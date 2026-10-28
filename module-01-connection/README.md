# 模块 1 — 建立连接与服务发现

> **全景图定位**: [docs/00 §2 第③→⑤层](../docs/00-big-picture.md) — 用 libwayland 客户端库连上 ⑤ 层的 compositor。
> **前置**: 无（本课程起点） · **产出**: 能列出当前桌面支持的所有 Wayland 接口

## 目标

- 理解 **`wl_display` / `wl_registry` / 事件循环** 三大核心概念
- 弄清「app 如何找到 compositor」（socket 服务发现）
- 第一次亲眼看到 compositor 广播的 globals 列表

## 原理讲解

### 连接的本质：一个 Unix domain socket

Wayland 没有注册中心、没有配置文件。compositor 启动时在用户运行时目录里创建一个 socket 文件，客户端连接它：

```
compositor 进程                      你的客户端
  监听 /run/user/1000/wayland-0       wl_display_connect(NULL)
       ▲                                   │
       └───────── AF_UNIX 连接 ────────────┘
```

路径由两个环境变量拼出：`$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`（默认 `wayland-0`）。这两个变量来自哪里？——来自 [登录过程](../docs/04-login-and-session.md#61-第一步找到-compositor-的-socket)（logind 建会话时创建运行时目录、DM 启动会话时导出变量）。**所以「连不上 Wayland」几乎总是登录环境的问题**（sudo/ssh/cron 里丢了环境变量）。

### 服务发现：wl_registry 的 globals 广播

连上后第一件事是拿 `wl_registry`，compositor 会把自己支持的每个「全局接口」（interface）广播给你：

```
客户端                                    compositor
  │ get_registry()                          │
  │◀── global(name=1, "wl_compositor", v6) ──│
  │◀── global(name=2, "wl_shm", v1) ────────│
  │◀── global(name=3, "xdg_wm_base", v6) ───│
  │◀── global(name=4, "wl_seat", v8) ───────│
  │      ... (几十个, 桌面越丰富列表越长)      │
  │   bind(name=1, wl_compositor, v4) ─────▶│   ← 挑需要的绑定, 版本协商 min(c,s)
```

- `name` 是这次连接里的临时编号（不是稳定 ID，重连会变）；
- **版本协商**: bind 时请求 `min(客户端想要的, 服务器支持的)`；
- `global_remove` 事件：服务器能力变化（如拔掉 Tablet）时通知你。

### roundtrip：最原始的同步

`wl_display_roundtrip()` = 「把请求全部发出去，等服务器把已排队的事件全部处理回来」。本模块用它等 globals 到齐——这是 Wayland 编程里最基本的同步原语。

## 代码走读

[`main.c`](main.c) 只有 59 行，四个看点：

| 位置 | 代码 | 说明 |
|------|------|------|
| [L35](main.c#L35) | `wl_display_connect(NULL)` | 读环境变量连 socket，失败即返回 NULL（检查这个返回值就是本模块的排错练习） |
| [L43](main.c#L43) | `wl_display_get_registry()` | 创建 registry 代理对象 |
| [L6-13](main.c#L6-L13) | `global_handler` | 每收到一个 global 打印一行——**这个回调就是本模块的主角** |
| [L47](main.c#L47) | `wl_display_roundtrip()` | 阻塞等广播到齐 |

## 动手实验

### 步骤

```bash
cd module-01-connection
make
./list-globals
```

### 预期输出（GNOME/KDE 下约 30-60 行）

```
=== Wayland 模块 1: 建立连接与发现全局对象 ===
--- 等待服务器广播全局对象... ---
  [   1] wl_compositor                             version  6
  [   2] wl_subcompositor                          version  1
  [   3] wl_shm                                    version  1
  [   7] zxdg_output_manager_v1                    version  2
  ...
```

### 观察点（原理照进现实）

1. **对照 socket**：另开终端 `ls -l $XDG_RUNTIME_DIR/wayland-0`——你现在知道这个文件是谁创建的；
2. **找出桌面用的扩展协议**：输出里带 `zwp_`/`zxdg_` 前缀的是实验/扩展协议（如 `zwp_pointer_gestures`）；查 [wayland-protocols 仓库](https://gitlab.freedesktop.org/wayland/wayland-protocols) 对号入座；
3. **故意破坏**：`env -u WAYLAND_DISPLAY ./list-globals` → 观察 libwayland 的回退行为（它会尝试 `wayland-1`…直到失败）；
4. **换一个服务器**：如果做了模块 8，`WAYLAND_DISPLAY=<模块8的socket> ./list-globals` 对比两份 globals 列表——差异就是「真实桌面」与「500 行骨架」的距离。

## 进阶练习

1. 只 bind `wl_compositor` 并打印协商后的版本：`wl_registry_bind(registry, name, &wl_compositor_interface, 100)`（请求超过服务器支持的版本会怎样？动手验证 libwayland 的报错方式）；
2. 统计 globals 里 request/event 总数最多的接口（读 `wayland.xml`）；
3. 把 `global_remove` 从空实现改成打印，然后热插拔一个输入设备观察（桌面会增删 seat 相关 global）。

## 常见坑

| 现象 | 原因 |
|------|------|
| `ERROR: 无法连接` | 不在 Wayland 会话（`echo $XDG_SESSION_TYPE` 应为 `wayland`），或环境变量丢失 |
| `sudo ./list-globals` 失败 | sudo 重置了环境变量，`XDG_RUNTIME_DIR` 丢失——见 [docs/04 §6.1](../docs/04-login-and-session.md) |
| globals 只打印了一半 | 没有做 roundtrip 就开始读结果 |

## 延伸阅读

- [docs/00 全景地图](../docs/00-big-picture.md) — 建立层坐标
- [wayland.xml](https://gitlab.freedesktop.org/wayland/wayland/-/blob/main/protocol/wayland.xml) — 对照输出读协议源文件

下一模块: [模块 2 — 第一个 Wayland 窗口](../module-02-hello-window/)
