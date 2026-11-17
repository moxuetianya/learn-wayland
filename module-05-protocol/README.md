# 模块 5：自定义协议——XML、wayland-scanner 与 wire format

> 🗺️ **全景定位**: [docs/00 — Linux 现代 GUI 全景地图](../docs/00-big-picture.md)

> 模块 1-4 我们一直在"消费"现成的协议：wl_registry、wl_shm、xdg_shell 拿来就用。
> 本模块反过来问：**这些协议本身从哪来？** 答案是 XML 加代码生成器。你将亲手
> 定义一个 `hello` 协议，用 wayland-scanner 编译成 C API，并配合模块 8 的服务端
> 跑通完整的 request/event 往返——这也是模块 8 的前置一半。

## 0. 你将学到什么

- 协议 XML 语法：`protocol` / `interface` / `request` / `event` / `enum` / `arg` 与
  `version`、`since`、`allow-null` 属性；request 与 event 的**方向性**
- arg 类型系统：`int`/`uint`/`fixed`/`string`/`object_id`/`new_id`/`array`/`fd`
  在 wire 上各占多少字节、怎么编码
- wayland-scanner 的产物（client-header / server-header / private-code / public-code）
  各生成什么、给谁链接；Doxygen 文档也是同一份 XML 变换出来的
- wire format：对象 id + `size<<16 | opcode` 消息头；fd 走 SCM_RIGHTS
- 版本协商：`wl_registry_bind` 的 version 参数的精确语义

## 1. 背景：协议即 XML，API 即生成物

### 1.1 wayland.xml 是整个 Wayland 世界的"宪法"

核心协议住在 `/usr/share/wayland/wayland.xml`（扩展协议在 wayland-protocols 包，
`/usr/share/wayland-protocols/`）。libwayland 本身**不理解任何具体协议**——它只会
搬运"对象 id + opcode + payload"的字节壳；`wl_surface`、`wl_shm` 之所以存在，是因为
两端各自链接了**从同一份 XML 生成的编解码表**：

```
              hello.xml（唯一事实来源）
                      │
            wayland-scanner（代码生成器）
      ┌────────────────┼────────────────┐
      ▼                ▼                ▼
 client-header    server-header    private-code
 客户端声明:       服务端声明:       消息签名表/wl_interface
 ·struct hello代理 ·hello_interface  （编解码元数据，
 ·hello_listener   （dispatch vtable） 客户端与服务
 ·hello_say_hello() ·hello_send_*()   端都各链一份）
   等请求桩           等事件桩
      │                │                │
      ▼                ▼                ▼
  模块 5 客户端     模块 8 服务端      双方各链一份
```

### 1.2 XML 语法逐项拆解

本模块的 `hello.xml` 只有 49 行，但要素齐全：

| 元素/属性 | 含义 | 本模块中的例子 |
|-----------|------|---------------|
| `<protocol name>` | 协议（= 一个 XML 文件）的名字，也是产物里的宏前缀 | `hello` |
| `<interface name version>` | 一个对象类型；version 是该接口的当前版本 | `hello` v1 |
| `<request name>` | **客户端可发**的请求（client → server） | `say_hello`、`add_numbers` |
| `<event name>` | **服务端可发**的事件（server → client） | `hello_reply` 等 3 个 |
| `<arg name type>` | 参数；type 见 1.3 的类型表 | `message`（string）、`a`/`b`（int） |
| `<enum>` / `<entry>` | 常量表；entry 有 `value` 和 `summary` | `reply_level`（定义了但未接线） |
| `<description summary>` | 文档，**就是** Doxygen/官方文档的正文 | 各处 |
| `since="N"` / `allow-null="true"` | 条目从第 N 版才存在 / object、string 允许为空（wire 上 id=0、长度 0） | v1 未用到，见练习 3 |

方向口诀：**request 是"你能调对方的方法"，event 是"对方能推给你的通知"**。同一份
XML 两端共用，视角镜像。一个重要的推论：**request/event 在各自列表里的声明顺序就是
opcode**（0 起）。协议发布即冻结：只能向尾部追加（标 `since`），绝不能改顺序、改
语义、删条目——否则新老版本消息错位，连接直接被以协议错误断开。

### 1.3 arg 类型系统：每种类型在 wire 上长什么样

| type | C 类型（客户端） | 线上编码 | 说明 |
|------|----------------|---------|------|
| `int` | `int32_t` | 4 字节 | |
| `uint` | `uint32_t` | 4 字节 | serial、version、name 都是它 |
| `fixed` | `wl_fixed_t` | 4 字节 | 24.8 有符号定点。坐标用它而非 float：消息体 32 位对齐，且定点跨架构逐位一致 |
| `string` | `const char *` | u32 长度（**含 NUL**）+ 字节 + NUL | UTF-8 |
| `object` | `struct wl_proxy *` | 4 字节对象 id | id=0 表示 null 对象 |
| `new_id` | 生成的代理指针 | 4 字节 id | 见下方"new_id 的特例" |
| `array` | `struct wl_array` | u32 长度 + 字节 | 长度 padding 到 4 的倍数 |
| `fd` | `int32_t` | **0 字节** | 不在字节流里！见 1.5 |

**new_id 的特例**：id 由**发送方分配**。若 new_id 声明了 interface 类型（如
`wl_surface.frame` 的 callback），wire 上只有 4 字节 id；若没声明类型（全协议唯一
一处：`wl_registry.bind`），要先传 interface 名 + version + id——接收方必须靠这条
消息才知道要造什么类型的对象。

### 1.4 wire format：一条消息的字节布局

```
 偏移   0          4                8
       ┌──────────┬───────────────┬────────────────────────┐
       │ 对象 id   │ size<<16      │  payload（按 arg 声明  │
       │  (u32)   │      |opcode  │  顺序，每个 arg 对齐 4） │
       │          │  (u32)        │                        │
       └──────────┴───────────────┴────────────────────────┘
```

- 低 16 位是 opcode，高 16 位是 **size**（含 8 字节消息头）；对象 id 约定：`1` 固定
  是 wl_display 单例，客户端新建对象从 2 递增，服务端创建的从 `0xff000000` 起。

### 1.5 fd 不走字节流：SCM_RIGHTS

`fd` 类型参数（如 `wl_shm.create_pool` 的 fd）从不出现在消息正文里。Unix domain
socket 允许在 `sendmsg()` 时附带 **ancillary data**，内核把发送方的一个打开文件描述
直接"塞"进接收进程的 fd 表——接收方拿到一个**新的 fd 号，指向同一个打开文件**：

```
客户端进程                                服务端进程
  msg 字节 ──────── write() ──────────→  read()
  fd 21 ─── sendmsg(SCM_RIGHTS, 21) ──→  recvmsg() 得到 fd 33
             （内核把同一个打开文件挂进对方 fd 表：两个 fd 号、同一份文件）
```

libwayland 在两端各维护一个 fd 队列保证顺序：消息里第 N 个 fd 参数对应队列里第 N
个 fd。模块 3/6/7 的 `wl_shm_pool` 全靠这个机制把 buffer 交给合成器。

### 1.6 版本协商

- interface 的 `version="N"` 经 `wl_registry.global` 广播出去；bind 时客户端请求的
  version 必须 `1 ≤ v ≤ 广播值`，服务端**按客户端请求的版本**对待这个 resource。
  模块 6/7 绑 `wl_compositor` 时广播 v6、请求 v4，就是协商。
- 带 `since="N"` 的条目：老版本绑定方绝不会发送/收到；scanner 生成的头文件还提供
  `*_SINCE_VERSION` 常量供防御性判断。

## 2. wayland-scanner 的产物各给谁用

`wayland-scanner --help` 列出五种模式（1.23）：

| 产物 | 生成内容 | 谁用 |
|------|---------|------|
| client-header | 不透明代理类型、`*_listener`、发送 request 的 static inline 桩、opcode 宏 | 客户端 include |
| server-header | dispatch vtable（`struct hello_interface`）、发送 event 的 static inline 桩 | 服务端 include |
| enum-header | 仅 enum 常量，无编解码表 | 需要共享常量但不链 libwayland 的场合 |
| private-code | 消息签名表、`wl_interface` 结构（符号不导出） | **客户端与服务端各链一份** |
| public-code | 同 private-code，但符号可见 | 仅当把协议符号打进共享库供外部链接；官方强烈建议非必要不用 |

"private" 指符号私有（不污染动态符号表），不是"内部专用"——它是默认正确选择，
本仓库所有 Makefile 都用它。另外 **Doxygen 文档不是 scanner 的模式**：wayland 源码树
的 `doxygen.xsl` 从同一份 XML 变换出 HTML——官方协议手册网页就是这么生成的，
`<description>` 里的每个字都会成为 API 文档。

## 3. 代码走读

### 3.1 hello.xml：一份完整的协议

```xml
<interface name="hello" version="1">
    <request name="say_hello">
      <arg name="message" type="string"/>
    </request>
    <event name="sum_result">
      <arg name="a" type="int"/> <arg name="b" type="int"/> <arg name="sum" type="int"/>
    </event>
```

设计动机：`say_hello` 演示 string 编解码；`add_numbers` 演示多参数往返；三个 event
让服务端能主动推数据；`enum reply_level` 故意不接线（练习 2 接上）。

### 3.2 Makefile：两条生成规则

```make
hello-client-protocol.h: hello.xml
	wayland-scanner client-header $< $@
hello-protocol.c: hello.xml
	wayland-scanner private-code $< $@
```

XML 比产物新时 make 自动重新生成，**改了 XML 忘记重编**因此很难发生。生成物是否
入库是团队约定：本模块现场生成，模块 6/7 则把 xdg-shell 生成物提交进了仓库。

### 3.3 main.c：客户端全流程

1. **发现**：`wl_display_get_registry` + `wl_display_roundtrip`——roundtrip 即
   "发一个 sync 请求并等到它的 done"，保证所有 global 广播完、回调全部跑完才继续。
2. **绑定**：回调里 `strcmp` 命中后 `wl_registry_bind(registry, name,
   &hello_interface, 1)`——最后一参就是 1.6 的版本协商。
3. **监听**：`hello_add_listener`。生成的 `struct hello_listener` 字段与 XML 的
   event 一一对应，scanner 保证字段序 = event opcode 序。
4. **发请求**：`hello_say_hello(st.hello, "Hello from client!")` 等。生成的桩只有
   两行实质——查 private-code 里的签名表，把参数编码进**本地缓冲**：

   ```c
   wl_proxy_marshal_flags((struct wl_proxy *)hello,
            HELLO_SAY_HELLO, NULL, wl_proxy_get_version(...), 0, message);
   ```

   此刻还没有一个字节写进 socket。
5. **`wl_display_flush`** 才把缓冲写进 socket（忘掉它请求就滞留本地）。
6. **收事件**：`while (!st.done && wl_display_dispatch(...))`——dispatch 阻塞等事件，
   listener 回调在本线程执行；`sum_result` 里核对 `sum == 3+5` 后置 `done` 退出。

### 3.4 对面：模块 8 的服务端

模块 8 用**同一份 hello.xml** 生成的 server-header + 同一份 private-code：
`hello_bind()` 里 `wl_resource_create` + `wl_resource_set_implementation` 挂上
vtable，请求落进两个实现函数，再用 `hello_send_*` 桩回事件。完整时序：

```
客户端                              模块 8 服务端
  │ bind hello(v1) ────────────────→ hello_bind()
  │                                     └ hello_send_server_info()  ← bind 时立刻推
  │ say_hello(...) ───────────────→ hello_say_hello() → send_hello_reply()
  │ add_numbers(3,5) ─────────────→ hello_add_numbers() → send_sum_result()
  │ ←── server_info / hello_reply / sum_result ── flush 后陆续到达
  │ hello_destroy ─────────────────→ （resource 销毁回调）
```

留意真实输出里 `server_info` 是**第一个**到的事件，但它由 bind 触发：bind 在 wire
上晚于 roundtrip 的 sync 请求，回来时 roundtrip 已结束，只能在主循环的 dispatch 里
被听见——事件与请求在两条方向上各自保序。想看"API 糖"之下的字节流：
`WAYLAND_DEBUG=1 WAYLAND_DISPLAY=wayland-0 ./hello-client`。

## 4. 编译与运行

依赖 libwayland（含 wayland-scanner，`which wayland-scanner` 可确认）：

```bash
cd module-05-protocol
make
```

**跑法 A：桌面 Wayland 会话内直接跑**（观察 registry、确认"没有 hello"）：

```bash
./hello-client
```

预期输出（global 列表因合成器而异）：

```
=== 模块 5: 自定义 Wayland 协议 ===
本模块演示: ...（4 行自述）
[registry] found global: wl_compositor v6 (id=1)
...（桌面合成器广告的全部 global，每个一行）

当前 compositor 不提供 'hello' 全局接口.
这是正常的——hello 是我们自己定义的协议.
请先运行模块 8 的 server, 它注册了这个接口: ...
```

**跑法 B：配合模块 8 服务端跑通全流程**（两个终端；server 无窗口，不需要 TTY）：

```bash
# 终端 1：启动纯协议 echo server
cd module-08-server-skeleton && make && ./hello-server

# 终端 2（socket 名以 server 打印为准，从 wayland-0 起找第一个空名，被占则顺延）
cd module-05-protocol
WAYLAND_DISPLAY=wayland-0 ./hello-client
```

客户端预期输出（服务端同期打印 `client says / computing 3 + 5 / resource destroyed`）：

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

## 5. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| 服务端直接断连、客户端 dispatch 返回 -1 | 两端生成物版本不一致（改了 XML 只重编一端），opcode 错位 | 两端都重新 make；生成物入库时务必提交新版 |
| bind 后被踢，报 invalid object | 请求版本 > 广播版本 | bind 的 version 取 `min(广播值, 所需)` |
| 服务端毫无反应，随后一次性收到 | 忘了 `wl_display_flush`，请求滞留本地缓冲 | 发完请求 flush，或进入 dispatch（会先 flush） |
| 收 event 崩/乱；加了新 request 后两端对不上 | 手改了生成的 listener 结构体；或新 request 插在列表中间 | 永远别改生成物；新条目只能追加到尾部并标 `since` |

## 6. 与真实实现的差距 / 深入话题

真实的协议演进远比"写个 XML"谨慎。核心协议与 wayland-protocols 都遵循**发布即
冻结**的纪律：一个 interface 版本发布后，request/event 集合与语义永久固定，演进只能
靠追加（`since` 更高的条目）或另起新 interface——所以模块 1 里你能看到 `zwp_*_v1`、
`zwp_*_v2` 并存的 global；进 wayland-protocols 的 stable 目录前，协议要在
staging/test 里被至少两个实现采纳验证。另一个值得细品的细节：`wl_registry.bind`
的无类型 new_id 是历史包袱——早期设计没有类型信息，`wl_registry_bind` 至今要手传
`wl_interface` 指针，后来的协议都带类型。

## 7. 练习

1. **加一对 request/event**：给 hello 加 `shout(string)` 请求和 `echo(string)` 事件，
   重新生成、改两端实现。体会"XML 是唯一需要先改的地方"。
2. **接上 enum**：给 `say_hello` 加 `enum="reply_level"` 的参数，服务端按
   normal/verbose 回不同文案，观察生成的常量名。
3. **版本实验**：interface 升 v2、新 event 标 `since="2"`；bind v1 验证收不到，
   再 bind v2 看服务端日志的版本号。
4. **读字节流**：`WAYLAND_DEBUG=1` 跑跑法 B，找出 `add_numbers(3,5)` 的 opcode，
   对照 1.4 手算它的 size。
5. **fd 传阅**：给协议加一个 `fd` 参数并在两端打印收到的 fd 号——预习模块 3 的
   `wl_shm_create_pool`，现在你知道那个 fd 是怎么过进程的了。

## 8. 延伸阅读

- [Wayland 协议文档（含 wire format 完整规范）](https://wayland.freedesktop.org/docs/html/)
- 本机 `/usr/share/wayland/wayland.xml` —— 最好的教材就是宪法本身，先读 wl_registry
- [wayland-book: Protocol Design](https://wayland-book.com/protocol-design/index.html)
- `man wayland-scanner`；[wayland-protocols 仓库](https://gitlab.freedesktop.org/wayland/wayland-protocols)——stable/staging/test 的分层本身就是教材
