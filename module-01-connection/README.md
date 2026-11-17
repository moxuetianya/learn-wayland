# 模块 1：建立连接与服务发现

> 课程的第一行 Wayland 代码：连上合成器，把它广播的所有全局对象打印出来。
> 只有约 60 行 C，但它是后续所有客户端模块的公共前奏——模块 2 到 7
> 做的事情，不过是把本模块打印出的这份清单里多 bind 几行而已。

## 0. 你将学到什么

- `wl_display_connect` 背后的 **socket 发现机制**：`$WAYLAND_DISPLAY`、
  `$XDG_RUNTIME_DIR` 如何拼出 socket 路径，找不到时发生什么
- Wayland 的对象模型：**proxy（代理）**、request（客户端→服务端）与
  event（服务端→客户端）在同一条 socket 上的双工流动
- `wl_registry` 的**全局对象广播**：interface / name / version 三个字段各是什么，
  为什么 bind 要拿 name 当"凭据"
- **版本协商**：`min(客户端请求, 服务端广播)` 规则，以及超出广播版本请求的后果
- `wl_display_roundtrip` / `wl_display_dispatch`：为什么 Wayland 客户端的
  事件循环是"拉"模型，回调在什么时机才真正执行

## 1. 背景：一条 socket 上的对象世界

### 1.1 wl_display_connect 到底做了什么

Wayland 没有 X11 那样的全局服务发现。客户端与合成器之间就是**一条
Unix domain socket**，connect 成功后 libwayland 返回一个 `wl_display`
代理对象——它代表"这条连接"本身，是所有其他对象的根。

socket 的定位规则（libwayland 按顺序尝试）：

```
wl_display_connect(NULL)
        │
        ├─ $WAYLAND_SOCKET 已设置?
        │     └─ 是: 值是父进程传来的 fd 编号(嵌套合成器/fork 场景), 直接用它
        │
        ├─ socket 名 = $WAYLAND_DISPLAY (如 "wayland-1")
        │     ├─ 含 '/' → 视为绝对路径, 直接 connect
        │     └─ 否则  → $XDG_RUNTIME_DIR/<名字>
        │
        └─ $WAYLAND_DISPLAY 未设置 → 回落到 "wayland-0"
                                     (最常见的默认会话 socket)
```

| 环境变量 | 作用 | 典型值 |
|----------|------|--------|
| `$WAYLAND_DISPLAY` | socket 名字（相对名或绝对路径） | `wayland-0`、`wayland-1` |
| `$XDG_RUNTIME_DIR` | 相对 socket 名的拼接目录（由 systemd/PAM 设置，权限 0700） | `/run/user/1000` |
| `$WAYLAND_SOCKET` | 直接给 fd（跳过路径解析） | 几乎只在嵌套合成器里出现 |

两个设计动机值得一提：

- **为什么用 `$XDG_RUNTIME_DIR`**：它保证 (a) 目录属主是当前用户，
  其他用户无法抢先创建同名 socket 冒充合成器；(b) 路径短——Unix socket
  路径上限 108 字节，放在 `/tmp` 里拼长名字容易撞墙。
- **为什么 connect 成功≠万事大吉**：连接后还没有任何对象存在，
  客户端连合成器支持什么都不知道。下一步就是"要清单"。

### 1.2 wl_registry：服务端的服务清单广播

Wayland 的对象模型极其朴素：**socket 上流动的只有 request 和 event**，
每个消息头带一个对象 id。`wl_display` 之外的一切对象（`wl_compositor`、
`wl_shm`、`xdg_wm_base`、`wl_seat`……）都是通过 `wl_registry` 这张
"总目录"发现的：

```
客户端                                      合成器
──────                                      ──────
wl_display_get_registry()          ──请求──→  创建 wl_registry 对象
                                              (id=2, 紧跟 display 的 id=1)
registry.global(name=1,            ←─事件──   对每个已注册的 global
  "wl_compositor", version=6)                 逐个广播
registry.global(name=2,
  "wl_shm", version=1)
registry.global(name=10,
  "xdg_wm_base", version=6)  …
wl_registry_bind(name=1,           ──请求──→  按名字找到 global,
  &wl_compositor_interface, 4)                new 一个 wl_compositor 对象
                                    ←─────    (此后客户端用新 id 与它对话)
```

三个字段必须分清：

- **interface**：字符串接口名，如 `"wl_compositor"`。这是"它是干什么的"，
  决定该对象上有哪些可用的 request/event——协议就是接口的集合。
- **name**：一个 `uint32_t` 临时编号，**仅在本连接内有效**，广播它、
  之后 bind 它、将来 `global_remove` 撤销它，用的都是这个号。
  它不是全局唯一的 id，重连之后编号可能完全不同。
- **version**：服务端为这个 global 实现的接口版本。

`bind` 是整个发现的"临门一脚"：客户端拿 name 作凭据，声明想要的接口与
版本，服务端在连接里 new 一个真正的对象，客户端拿到新 proxy。从这以后
`wl_registry` 就功成身退——多数客户端此后只关心 `global_remove`。

### 1.3 版本协商：min(你要的, 它广播的)

每个接口随年份演化：新版本加 request/event、改语义。协商规则：

> **绑定后实际生效的版本 = min(客户端 bind 时请求的版本, 服务端广播的版本)**

推论与工程守则：

- 你请求 v4、服务端广播 v6 → 生效 v4。服务端承诺**不发送** v4 之后才有的
  事件——高版本服务端对低版本客户端是严格向后兼容的。
- 你请求的版本**高于**服务端广播的版本 → 不会"降级凑合"。现代
  libwayland 会以协议错误拒绝，且这个错误是**异步**送达的（bind 本身
  看似成功，下一轮 dispatch 才被踢）。下文练习 3 会让你亲手复现。
- 所以惯例是：**bind 自己代码真正支持的版本**，别盲目要最高版——
  高版本可能带来你没实现的新事件，程序会莫名其妙地断连。

### 1.4 dispatch：为什么事件循环是"拉"模型

libwayland 不会开线程、不会打断你的代码。事件静静躺在 socket 的接收
缓冲区里，**只有你主动交出控制权**，它们才会被解析、回调才会执行：

- `wl_display_dispatch(display)`：阻塞直到读回一批事件并逐个分发，返回
  分发的事件数（出错为 -1）。这是事件循环的标准骨架。
- `wl_display_roundtrip(display)`：先把本地挂起的请求全部冲发，然后发出一个
  内置的 `wl_display.sync` 请求，**阻塞到服务端处理完之前所有请求并回赠
  done 事件**。它是"问答式"代码的同步屏障：发出请求 → roundtrip →
  可以确定结果已经到达。

本模块只要一份清单，所以一个 roundtrip 就够；模块 2 开始你会看到
`while (st.running && wl_display_dispatch(...) != -1)` 这种常驻循环。

"拉"模型的好处：回调永远发生在**你自己线程的调用栈里**，不需要锁；
代价是渲染、定时的节奏要自己管理（模块 3 的 `usleep`、模块 6 的
frame callback 都是在解决"什么时候该主动拉一次"）。

## 2. 代码走读

全模块只有一个 `main.c`，按执行顺序：

**① 注册监听器（`registry_listener`）**。libwayland 的统一模式：listener
结构体的槽位与协议事件一一对应，不填的槽位默认 NULL、事件被忽略——
客户端永远只实现接口的子集：

```c
static const struct wl_registry_listener registry_listener = {
    .global        = global_handler,
    .global_remove = global_remove_handler,
};
```

**② `global_handler`——广播 arrives here**。每个 global 到达时打印一行。
注意签名里 `name` 是编号、`interface` 是字符串、`version` 是服务端版本，
和 1.2 节的字段一一对应：

```c
printf("  [%4u] %-40s version %2u\n", name, interface, version);
```

**③ `global_remove_handler`——留空的正确姿势**。服务端撤销 global
（显示器拔掉、协议扩展卸载）时通知客户端。正经客户端必须把对应代理
置空并停用相关功能，继续使用已失效代理会被断连；本模块只枚举不持有，
所以空实现加 `(void)` 转换压掉未用参数警告即可。

**④ `main` 的四步舞**：

```c
display  = wl_display_connect(NULL);          /* 1. 找 socket 并连接 */
registry = wl_display_get_registry(display);  /* 2. 本地构造 registry 代理 */
wl_registry_add_listener(registry, ...);      /* 3. 登记回调 */
wl_display_roundtrip(display);                /* 4. 冲发请求+等清单到齐 */
```

第 4 步返回后，`global_handler` 已经把清单打印完毕——这就是"打印所有
全局接口"的全部秘密。结尾 `wl_registry_destroy` + `wl_display_disconnect`
按申请的逆序释放（代理销毁请求也会真正发给服务端）。

## 3. 编译与运行

前置：**在桌面 Wayland 会话内的终端**运行（不是 TTY！TTY 下没有
`$WAYLAND_DISPLAY`）。先自检：

```bash
echo $XDG_SESSION_TYPE          # 应输出 wayland
echo $WAYLAND_DISPLAY           # 应输出 wayland-0 之类的名字
```

编译与运行：

```bash
cd module-01-connection
make
./list-globals
```

在 Ubuntu 24.04 的 GNOME (mutter) 会话里，真实输出（节选）：

```
=== Wayland 模块 1: 建立连接与发现全局对象 ===

已连接到 display: OK

--- 等待服务器广播全局对象... ---
  [   1] wl_compositor                            version  6
  [   2] wl_drm                                   version  2
  [   3] wl_shm                                   version  1
  [   5] zxdg_output_manager_v1                   version  3
  [   9] wl_subcompositor                         version  1
  [  10] xdg_wm_base                              version  6
  [  11] gtk_shell1                               version  6
  [  16] wl_seat                                  version  8
  [  23] zwp_linux_dmabuf_v1                      version  4
  [  27] wp_presentation                          version  2
  [ 37] wp_cursor_shape_manager_v1                version  2
  [206] wl_output                                 version  4
  …

--- 协议版本协商示例 ---
客户端连接后, 通过 MIN(requested_version, server_version) 协商版本.
例如: wl_registry_bind(registry, name, &wl_compositor_interface, 4)
如果服务器只支持 v5, 协商后使用 v4; 如果服务器只支持 v2, 协商后使用 v2.

=== 模块 1 完成 ===
```

读清单的三个练习点：`z` 前缀（如 `zwp_linux_dmabuf_v1`）= 尚在
wayland-protocols 仓库 unstable 阶段的协议；`gtk_shell1` 是 GNOME 私有
扩展（模块 2 起要用的 `xdg_wm_base` 就是它的标准化前身）；`wl_output`
的 name 高达 206，说明 name 只是连接内的流水号，**不保证连续**。

> 注意：结尾那段"版本协商示例"文字是示例程序的简化说法。结合 1.3 节：
> "bind v4、服务端只有 v2"在现代 libwayland 上**不是**降级使用 v2，
> 而是协议错误——练习 3 可验证。

## 4. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| `无法连接到 Wayland compositor` | 在 X11 会话或纯 TTY 里运行；或 `$WAYLAND_DISPLAY` 指向陈旧 socket（文件还在、无人监听，常见于合成器崩溃后） | `echo $XDG_SESSION_TYPE` 确认是 `wayland`；`ls $XDG_RUNTIME_DIR` 里找活着的 socket |
| 连接成功但清单是空的 | 用了 `wl_display_dispatch` 但服务端广播还没到就退出 | 枚举类代码用 `wl_display_roundtrip`（本模块做法），保证清单到齐 |
| `WAYLAND_DISPLAY` 设置了却 still 失败 | 相对名拼错，或 `$XDG_RUNTIME_DIR` 未设置（常见于 cron/sudo 环境，PAM 没跑） | 用绝对路径：`WAYLAND_DISPLAY=$XDG_RUNTIME_DIR/wayland-0 ./list-globals` |
| bind 之后程序莫名断连 | 请求版本高于服务端广播版本，服务端异步发来协议错误 | bind 自己代码支持的版本（见 1.3） |
| 复制别人机器上的输出找不到某接口 | global 清单因合成器、加载的扩展而异 | 清单本来就是"每台机器一份"，没有标准答案 |

## 5. 与真实实现的差距 / 深入话题

真实的枚举工具是 `wayland-info`（wayland-utils 包）——它对每个 global
额外 bind 并打印详细信息（seat 的名字与能力、输出的模式列表等），
排查"合成器到底支不支持某协议"时几乎总是先跑它。

工程化客户端与本模块的三点差距：其一，`global_remove` 被认真对待，
对象生命周期全程跟踪（窗口系统里显示器热插拔是常态）；其二，事件循环
不会裸用 `wl_display_dispatch`，而是 `wl_display_get_fd()` 拿到 fd 后
交给 poll/epoll（或 GLib 主循环）统一调度，让 Wayland 事件与定时器、
其他 IPC 共存——模块 11 会在服务端看到同一哲学；其三，多线程客户端
需要代理队列（`wl_proxy_create_wrapper`、`wl_event_queue`）把事件分发
到指定线程，本课程为清晰起见全部单线程。

## 6. 练习

1. **换一个 socket**：`WAYLAND_DISPLAY=<别的名字> ./list-globals` 连到
   另一个合成器（比如你在 TTY 里跑模块 11 时创建的 `wayland-1`），
   对比两份清单的差异。
2. **观察 bind 的效果**：在 `global_handler` 里对 `wl_shm` 调用
   `wl_registry_bind(registry, name, &wl_shm_interface, 1)`，再
   `wl_display_roundtrip`，用 `WAYLAND_DEBUG=1 ./list-globals` 对照
   协议流里多出的 bind 请求。
3. **复现版本错误**：对广播 `version 1` 的 `wl_shm` 请求版本 99，
   观察异步到达的 `invalid version for global` 错误——直观体会 1.3 的规则。
4. **使用官方工具对照**：`sudo apt install wayland-utils && wayland-info`，
   找到它输出的 `wl_seat` 能力位与本模块打印的版本号。
5. **试试 WAYLAND_SOCKET**：写个程序 `fork` 后通过环境变量传 fd 给
   子进程连接（提示：`dup` 后 `snprintf` fd 号到 `WAYLAND_SOCKET`）。

## 7. 延伸阅读

- [Wayland 书 · The wl_display and event handling](https://wayland-book.com/protocols/wire-format.html) — 协议线格式与对象模型
- [wl_registry 协议定义](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_registry) — 官方 protocol 文档
- [wayland-client(3)](https://man.archlinux.org/man/wayland-client.3.en) — libwayland-client API 手册
- [XDG Runtime Dir 规范](https://specifications.freedesktop.org/basedir-spec/latest/) — `$XDG_RUNTIME_DIR` 的由来
