# 模块 8 — libwayland-server 骨架

> **全景图定位**: [docs/00 §2 第⑤层](../docs/00-big-picture.md) — 你第一次站到 compositor 的位置：实现协议的**服务端**。
> **前置**: [模块 5](../module-05-protocol/)（同一协议的客户端视角） · **产出**: 一个注册了自定义 `hello` 全局接口的协议服务器

## 目标

- 理解服务端对象模型: **`wl_display` → `wl_global` → `wl_client` → `wl_resource`**
- 实现 request 处理器（客户端调用的方法在服务器侧长什么样）
- 掌握 bind / 事件发送 / 生命周期管理的最小闭环

## 原理讲解

### 视角反转：同一个接口的两侧

模块 5 里你是「调用方」，现在是「被调用方」。自定义协议 `hello` 的完整分工：

```
                hello 接口
客户端实现 (模块5)              服务器实现 (本模块)
────────────────               ─────────────────
hello_say_hello() 发出请求  ──▶ hello_say_hello() 被调用    [request]
hello_reply 回调被触发    ◀── hello_send_hello_reply()      [event]
```

scanner 生成的同一份 private-code 服务两边；client-header 给模块 5，server-header 给本模块。

### 服务端四大对象

```
wl_display                       服务器的根对象: 事件循环 + socket + 客户端管理
 ├── wl_global × N               广播给所有客户端的能力 ("我支持 hello 接口")
 │     └── bind 回调              客户端 bind 时触发 → 为它创建 resource
 ├── wl_client × N               每个连接的客户端 (一条 unix socket)
 │     └── wl_resource × N       该客户端持有的每个协议对象
 │           ├── implementation  你写的 request 处理器表
 │           └── destroy 回调     资源销毁时的清理钩子
 └── wl_event_loop               epoll 封装 (模块 11 会往里加 libinput 的 fd)
```

对照客户端侧（模块 1-2）记忆：客户端的「代理对象」(`struct hello*`) ↔ 服务端的「资源」(`wl_resource`)——同一枚硬币的两面。

### 一个 request 的服务端旅程

```
客户端: hello_say_hello(hello, "msg")
   │ socket
   ▼
libwayland-server 解码: 对象id → wl_resource → 查 implementation 表
   │
   ▼
你的 hello_say_hello(client, resource, "msg")     ← [hello-server.c L12-18]
   │
   ├── printf 收到消息
   └── hello_send_hello_reply(resource, "...")     ← 事件打包回发
```

**这就是全部机制**。wl_compositor.commit、wl_surface.attach 在模块 11/12 里的处理方式与 `say_hello` 没有任何本质区别——只是实现更复杂。

### socket 与多客户端

`wl_display_add_socket_auto()` 在 `XDG_RUNTIME_DIR` 下自动找空闲名（`wayland-1`, `wayland-2`...）。服务器可以同时服务多个客户端——每个连接一个 `wl_client`，各自的 resource 空间独立（id 只在连接内唯一）。

## 代码走读

[`hello-server.c`](hello-server.c) 仅 93 行：

| 位置 | 内容 |
|------|------|
| [L12-27](hello-server.c#L12-L27) | 两个 request 处理器: `say_hello` / `add_numbers`（注意签名是 scanner 规定的） |
| [L37-40](hello-server.c#L37-L40) | `hello_interface` 实现表——**把函数指针填进 scanner 生成的接口结构体** |
| [L42-59](hello-server.c#L42-L59) | `hello_bind`: global 被绑定时创建 resource、挂实现表、主动推送 `server_info` 事件 |
| [L29-35](hello-server.c#L29-L35) | resource 销毁回调（客户端 destroy 或断连时触发，清理 user data） |
| [L62-93](hello-server.c#L62-L93) | main: create display → socket → global create → `wl_display_run` 主循环 |

## 动手实验

```bash
cd module-08-server-skeleton
make && ./hello-server
# 输出: 监听 socket: wayland-1
#       设置环境变量: export WAYLAND_DISPLAY=wayland-1
```

另开终端联调（详见 [模块 5](../module-05-protocol/)）：

```bash
cd module-05-protocol
WAYLAND_DISPLAY=wayland-1 ./hello-client
```

### 观察点

1. **多客户端**：同时跑两个 `hello-client`——服务器为每个 bind 打印一次（每客户端独立 resource）；`ss -x | grep wayland` 看到两条连接；
2. **裸看协议双向流**：`WAYLAND_DEBUG=1 ./hello-server 2>&1 | grep -v wl_registry`——对照输出理解「服务器视角的 request 进 / event 出」；
3. **客户端暴毙**：跑一个客户端 Ctrl+C 杀掉——服务器收到 destroy 回调打印 `client resource destroyed`，但服务器不死（对比模块 11 要处理的更复杂场景）；
4. **权限实验**：`ls -l $XDG_RUNTIME_DIR/wayland-1` 看权限；另一个用户能连吗？（服务器可 `chmod` 精确控制谁能接入——安全边界在文件系统层）；
5. **和真桌面对比**：`WAYLAND_DISPLAY= ./hello-client`（连桌面）时 registry 里没有 hello——自定义协议的意义就是 compositor 私有扩展（生产例: wlroots 的 `wlr-layer-shell`）。

## 进阶练习

1. 加 `hello.subscribe(event_mask)` request + 定时推送事件（`wl_event_loop_add_timer`），实现「服务器主动广播」模型；
2. 用 `wl_client_get_credentials()` 打印连接对端的 UID/PID——思考 compositor 如何识别「谁在连接我」（portal 权限的基础）；
3. 把 `reply_level` 枚举用起来：verbose 模式回显带时间戳；
4. 限制最大并发客户端数（数 wl_client），超了 `wl_client_destroy`——实现一个「协议层的限流」。

## 常见坑

| 现象 | 原因 |
|------|------|
| 客户端连不上 | `WAYLAND_DISPLAY` 没指向本服务器 socket（默认连了桌面） |
| 段错误 on bind | resource 创建失败没检查；实现表函数签名与 scanner 生成的不一致 |
| event 发出但客户端没反应 | 事件排进了缓冲，主循环没 flush（`wl_display_run` 会自动，但自己写循环时要 `wl_display_flush_clients`） |
| 改了 XML 后链接断 | server-header 过期，`touch hello.xml && make` 重新生成 |

## 延伸阅读

- [wayland-book: Server](https://wayland-book.com/server-side/index.html) — 本模块的展开版
- [wl_display API](https://wayland.freedesktop.org/docs/html/apib.html#Client-and-Server-Functions)

下一模块: [模块 9 — DRM/KMS 直接控制屏幕](../module-09-drm/)
