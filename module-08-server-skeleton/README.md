# 模块 8：libwayland-server 骨架——第一个 Wayland 服务端

> 模块 1-7 你一直坐在客户端这一侧：连接、开窗、交像素、收输入。从本模块起
> 换到桌子对面——用 libwayland-server 从零写一个协议服务端，把模块 5 定义
> 的 `hello` 协议变成真正能通信的 echo server。约 90 行 C，是模块 11 合成器
> 的协议地基：`wl_display` / `wl_global` / `wl_resource` 这套对象模型，每个
> Wayland 合成器都在用。

## 0. 你将学到什么

- 服务端四大对象 **`wl_display` / `wl_client` / `wl_global` / `wl_resource`**
  各自代表什么、谁拥有谁
- "global 是广告，resource 才是服务"：`wl_global_create` → registry 广播 →
  bind 回调 → `wl_resource_set_implementation` 挂虚表的完整链路
- 请求（client→server）与事件（server→client）在代码层面分别怎么落地：
  vtable 函数与生成的 `*_send_*` 内联函数
- **`wl_event_loop`**：`wl_display_run` 的心脏，也是模块 11 把 DRM/libinput
  fd 挂进同一个主循环的挂载点
- wayland-scanner 三种产物（server-header / client-header / private-code）
  各自的角色，以及 wire format 里签名（`"s"`、`"ii"`）的用途

## 1. 背景原理

### 1.1 服务端的四个对象

| 对象 | 数量 | 生命周期 | 一句话 |
|------|------|----------|--------|
| `wl_display` | 每个服务端进程 1 个 | 进程全程 | 服务端世界的根：内含事件循环、global 列表、客户端列表 |
| `wl_client` | 每个连接 1 个 | 连接建立到断开 | 一个连上 socket 的客户端；libwayland accept 后自动创建 |
| `wl_global` | 每个全局接口 1 个 | 注册到销毁 | 广告牌："本服务端支持 hello v1"，面向**所有**客户端 |
| `wl_resource` | 每客户端×每接口 1 个 | bind 到客户端销毁 | 该客户端手里那个对象的**服务端代理人**，虚表挂在它身上 |

```
                    wl_display（服务端进程唯一）
                    │ owns                        │ owns
      ┌─────────────┴──────────┐          ┌───────┴────────┐
      │ wl_global "hello" v1   │          │ wl_client A    │──┐
      │ （广告，共享一个）       │          │  └ wl_resource │  │ Unix socket
      │                        │          │     (hello)    │  │ $XDG_RUNTIME_DIR/
      │      bind 发生时，     │          │    + hello_impl│◄─┘ wayland-N
      │      每客户端各建一个   │          │ wl_client B    │
      │      resource ─────────┼─────────►│  └ wl_resource │
      └────────────────────────┘          │     (hello)    │
                                          └────────────────┘
```

关键认知：**global 与 resource 是一对多的关系**。global 只是"能力公告"，
本身不存任何客户端状态；客户端 `bind` 之后，服务端才为这个客户端创建一个
resource，业务状态（本例的 `struct hello_data`）作为 user_data 挂在 resource
上。GNOME 上的 `wl_compositor` global 只有一个，但 50 个客户端就有 50 个
`wl_compositor` resource。

### 1.2 完整链路：从注册到请求进虚表

```
 服务端                                      客户端（模块 5）
 ────────                                    ──────────────
 wl_display_create()
 wl_display_add_socket_auto()
 wl_global_create(hello, v1)
 wl_display_run() ──┐
                    │                     wl_display_connect()
                    │                     → 自动拿到 wl_registry（协议内建）
                    ├─────────────────── registry.global(id=1, "hello", v1) ──→
                    │                     wl_registry_bind(id=1, "hello", v1)
                    │◄────────────────── bind ──────────────────────────────┤
 hello_bind(client, version, id)
   ├ wl_resource_create()
   ├ wl_resource_set_implementation()
   └ hello_send_server_info() ──────────────────────────────────────────────→
                    │                     hello_say_hello("Hello from client!")
                    │◄────────────────── say_hello ──────────────────────────┤
 hello_impl.say_hello() 被调用             （客户端视角：请求已发出）
   └ hello_send_hello_reply() ──────────────────────────────────────────────→
                                          listener.hello_reply() 被调用
```

分三步理解：

1. **连接即拿 registry**：`wl_display_connect` 后客户端不需要任何请求就拥有
   `wl_registry`（wire 协议规定它的对象 id 固定为 2）。服务端把 global 列表
   逐个用 `registry.global` 事件播给它——模块 1 里你打印过这张清单，现在你
   知道它是怎么来的了。
2. **bind 是分水岭**：global 广播里只有接口名和版本，客户端手里还没有可调
   用的对象。`wl_registry.bind` 带着"我要哪个 global、用几版、新对象叫什么
   id"三个要素发过来，服务端的 `hello_bind` 被调用，`wl_resource_create` +
   `wl_resource_set_implementation` 之后，这个客户端的后续请求才有落点。
3. **虚表分发**：此后客户端每发一个 request，libwayland 解码后按 opcode 查
   虚表（本例 `hello_impl`）调用对应函数。事件方向则用生成的
   `hello_send_hello_reply()` 一类的内联函数，本质是
   `wl_resource_post_event(resource, opcode, ...)`。

### 1.3 wire format 一瞥：签名不是摆设

打开生成的 `hello-protocol.c`，能看到每个 request/event 的签名字符串：

```c
static const struct wl_message hello_requests[] = {
	{ "say_hello", "s", hello_types + 0 },
	{ "add_numbers", "ii", hello_types + 0 },
};
```

线上消息只是 `[对象 id][opcode][长度][参数...]` 的裸字节流，`"s"` = 一个
string（长度 + 字节），`"ii"` = 两个 int。libwayland 靠这张表在**运行时**
编解码两端的消息——所以 server-header（虚表、`*_send_*`）和 private-code
（`wl_interface` 元数据）必须和 XML 严格同源，接口定义就是两端之间的契约。

### 1.4 wl_event_loop：服务端的心脏

`wl_display_create` 内部创建了一个 `wl_event_loop`；`wl_display_run` 就是
在循环执行 `wl_event_loop_dispatch`：

- 每个已连接客户端的 socket 是循环上的一个 fd source（可读→收请求→进虚表）；
- 监听 socket 上来新连接→创建 `wl_client`；
- 每轮结束把各客户端连接缓冲区里排队的事件冲刷出去。

重要的是这个循环**对外开放**：`wl_display_get_event_loop()` +
`wl_event_loop_add_fd / add_timer / add_signal` 可以把任意 fd 挂进来。模块 11
正是把 libinput 的 fd 挂到 Wayland 事件循环上，让"协议 + 输入 + 输出"共用
一个 `epoll`——这是合成器单主循环架构的根基。`wl_display_terminate` 是唯一
让 `wl_display_run` 返回的开关。

## 2. 代码走读

文件只有 `hello-server.c`（93 行）加两个生成物。按执行顺序：

### 2.1 骨架搭建（main）

```c
display = wl_display_create();
const char *socket = wl_display_add_socket_auto(display);
...
wl_global_create(display, &hello_interface, 1, NULL, hello_bind);
wl_display_run(display);
```

`wl_display_add_socket_auto` 在 `$XDG_RUNTIME_DIR` 下选第一个空闲的
`wayland-N` 并监听：桌面会话内通常是 `wayland-1`（`wayland-0` 已被会话
占用），纯 TTY 下是 `wayland-0`。注意 `wl_global_create` 的第 3 个参数是
**广播出去的版本上限**，不得超过 XML 里 `interface` 的 `version`，否则
libwayland 直接 abort——版本协商的第一道闸在服务端注册时就把守。

### 2.2 hello_bind：global → resource 的转变

```c
hd->resource = wl_resource_create(client, &hello_interface, version, id);
...
wl_resource_set_implementation(hd->resource, &hello_impl, hd, hello_destroy);
```

`id` 必须原样传回——它是客户端在 bind 请求里自选的新对象 id，客户端靠它把
服务端 resource 和自己刚拿到的 proxy 对上号。`wl_resource_set_implementation`
一次挂上三样东西：虚表、user_data、析构函数。析构在客户端 destroy proxy
**或连接断开**时触发，是释放 `hello_data` 的唯一时机。

### 2.3 虚表与生成代码的分工

```c
static const struct hello_interface hello_impl = {
    .say_hello   = hello_say_hello,
    .add_numbers = hello_add_numbers,
};
...
hello_send_hello_reply(resource, "Hello back from server!");
```

`hello-server-protocol.h`（`wayland-scanner server-header` 生成）给出虚表
结构体和三个 `hello_send_*` 事件发送内联函数；`hello-protocol.c`
（`private-code` 生成）给运行时编解码元数据。手写的只有业务：收到什么、
回什么。缺省 NULL 的虚表槽位一旦被客户端调到，libwayland 自动抛 protocol
error——服务端的"未实现"是安全的。

### 2.4 Makefile：scanner 产物如何进构建

`hello-server-protocol.h`、`hello-protocol.c` 都由 `hello.xml` 经
`wayland-scanner` 生成（server-header / private-code 两个子命令），与模块 5
客户端用的 client-header 同源。**同一份 private-code 两端通用**——模块 5 和
模块 8 各自编译一次 `hello-protocol.c`，wire 元数据在两边严丝合缝。改协议
XML 后两端都要重新生成编译，这就是"协议即契约"的工程形态。

## 3. 编译与运行

本模块**不碰屏幕**，在桌面会话内的终端即可运行（Wayland socket 只是个
Unix domain socket，echo 不需要任何图形权限）。

```bash
# 终端 1：编译并启动服务端
cd module-08-server-skeleton
make
./hello-server

# 终端 2：跑模块 5 的客户端（需先在 module-05-protocol 里 make）
cd module-05-protocol
WAYLAND_DISPLAY=wayland-1 ./hello-client    # 名字以服务端打印的为准
```

服务端输出（本仓库实测转录）：

```
=== 模块 8: libwayland-server 骨架 ===
启动 Wayland 服务器, 注册 'hello' 全局接口...

监听 socket: wayland-1
设置环境变量: export WAYLAND_DISPLAY=wayland-1

服务器运行中... (Ctrl+C 退出)
在另一个终端运行: cd module-05-protocol && ./hello-client

[server] client bound hello interface v1
[server] client says: Hello from client!
[server] computing 3 + 5
[server] client resource destroyed
```

客户端输出：

```
[registry] found global: hello v1 (id=1)
[registry] bound hello interface v1

--- 发送请求 ---
--- 等待事件 ---
[event] server_info: protocol_version=1  server=learn-wayland-module8
[event] hello_reply: Hello back from server!
[event] sum_result: 3 + 5 = 8

=== 模块 5 完成 ===
```

注意 `server_info` 在 bind 时就被服务端推出、先于两个应答到达，正好验证
1.2 图中的时序。

## 4. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| 客户端"无法连接 Wayland compositor" | `WAYLAND_DISPLAY` 没指向服务端的 socket，或 SSH 环境 `XDG_RUNTIME_DIR` 与服务端不一致 | 按服务端打印的 `export WAYLAND_DISPLAY=...` 设置；确认 `$XDG_RUNTIME_DIR` 相同 |
| 客户端连上了但"当前 compositor 不提供 hello 全局接口" | 连到了桌面会话（GNOME 等）而不是本服务端 | 检查 `WAYLAND_DISPLAY` 指向哪个 `wayland-N` |
| `wl_global_create` 一调用就 abort | 广播版本 > XML 里 interface 的 version | 版本参数不超过 `hello.xml` 声明的版本 |
| Ctrl+C 后 `$XDG_RUNTIME_DIR` 残留 `wayland-N` 文件 | 信号硬退没走 `wl_display_destroy`（正常退出会 unlink socket） | 无害；auto 会跳到下一个 N，也可手动删 |
| 两个服务端同时跑 | socket 名自动错开，互不干扰 | 各自的客户端用各自的 `WAYLAND_DISPLAY` |

## 5. 与真实实现的差距 / 深入话题

真实合成器（mutter、wlroots）在同一套对象模型上还要处理：成百上千个
global（wl_output、wl_seat、xdg_wm_base、linux-dmabuf……）的注册与
`wl_global_remove` 撤下；`wl_display_init_shm` 这类**libwayland 内建实现**
（模块 11 直接用，一行注册 wl_shm）；`wl_client` 级的记账（连接/断开监听、
按客户端限流）；以及版本协商的完整语义——客户端 bind 的版本会被钳制在
"global 广告的版本"与"客户端支持的版本"之间，bind 回调收到的 `version`
参数才是这个 resource 的实际版本，虚表函数里据此区分行为。

另一个值得深挖的方向：`wl_event_loop` 的定时器源（`wl_event_loop_add_timer`）
是合成器里"游标闪烁""双击判定""动画帧"这类时间驱动的标准载体；空闲回调
（`wl_event_loop_add_idle`）则用来把"本轮 dispatch 里顺带想做"的事推迟到
安全时点。理解了 event loop 的源模型，模块 11 的三线合一就是水到渠成。

## 6. 练习

1. **看裸协议流**：用 `WAYLAND_DEBUG=1 ./hello-server`（和客户端同样加）
   重跑一遍，把 stderr 里的每条消息与 1.2 的时序图逐条对上——这是把对象
   模型落到字节流的最快途径。
2. **加一个 request**：给 `hello.xml` 增加 `shout` request（消息原样回大写），
   `make` 重新生成代码，更新服务端虚表与模块 5 客户端——体会改协议要动
   几处、漏一处会怎样（虚表槽位为 NULL 时客户端会被抛 error）。
3. **制造版本事故**：把 `wl_global_create` 的版本参数改成 2 再跑，观察
   libwayland 的 abort 断言；再把 XML 里 interface 升到 2、广播 1，验证
   "广告版本 ≤ 接口版本"的约束方向。
4. **心跳事件**：不再用 `wl_display_run`，改用手动循环 +
   `wl_event_loop_add_timer` 每秒向已 bind 的客户端推一条 `hello_reply`——
   预习模块 11"单主循环多事件源"的写法。
5. **客户端记账**：用 `wl_display_add_client_created_listener` 统计连接数，
   在每个客户端 bind 时打印序号，验证"一个连接一个 wl_client"。

## 7. 延伸阅读

- [Wayland 服务端 API 文档](https://wayland.freedesktop.org/docs/html/apb.html) —
  wl_display / wl_global / wl_resource / wl_event_loop 的权威说明
- [wayland-book: Compositor 章节合集](https://wayland-book.com/) — 对象模型与
  事件循环的最佳英文教材
- [wayland.xml（核心协议）](https://gitlab.freedesktop.org/wayland/wayland/-/blob/main/wayland.xml) —
  对照 wl_registry/wl_display 的接口定义，读本模块相当于读了它的最小子集
- [libwayland 源码](https://gitlab.freedesktop.org/wayland/wayland) —
  `src/wayland-server.c` 里 `wl_display_run` 与事件循环的实现只有几百行
