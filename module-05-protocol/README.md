# 模块 5 — 协议 XML 与 wayland-scanner

> **全景图定位**: [docs/00 §3](../docs/00-big-picture.md) ③ 层——Wayland 协议本身的语法与代码生成机制。
> **前置**: [模块 1-4](../module-01-connection/) · **产出**: 自定义 `hello` 协议 + 客户端（需配合 [模块 8](../module-08-server-skeleton/) 的服务器联调）

## 目标

- 读懂 Wayland 协议 XML 的语法（interface/request/event/enum）
- 掌握 **wayland-scanner** 三种产物（client-header / server-header / private-code）
- 理解版本协商与「协议即 API」的思想

## 原理讲解

### Wayland 的 RPC 本质

剥掉所有图形语义，Wayland 是一个**双向 RPC 协议**：

```
客户端 ── request ──▶ 服务器    (客户端能主动调用的"方法")
客户端 ◀── event ─── 服务器    (服务器能主动推送的"通知"
```

- **接口 (interface)** = 一组 request+event 的集合（类比一个类/服务）；
- **对象 (object)** = 接口的实例，用 32 位 id 引用；
- **方向性是相对的**：同一个接口，客户端实现 event 的处理，服务器实现 request 的处理。

图形功能全部建立在这个 RPC 之上：`wl_surface.commit` 是 request，`wl_keyboard.key` 是 event——你在模块 1-4 用的一切都能这样拆解。

### XML：协议的单一事实来源

[hello.xml](hello.xml) 定义了一个最小但完整的接口（对照阅读）：

```xml
<interface name="hello" version="1">
  <request name="say_hello">              ← 客户端可调
    <arg name="message" type="string"/>
  </request>
  <event name="hello_reply">              ← 服务器可推
    <arg name="reply" type="string"/>
  </event>
  <enum name="reply_level">               ← 枚举常量
    <entry name="normal" value="0"/>
  </enum>
</interface>
```

参数类型系统（比 C 类型少得多）：

| type | 对应 C | 说明 |
|------|--------|------|
| `int` / `uint` | int32_t / uint32_t | 标量 |
| `fixed` | wl_fixed_t | 24.8 定点数（坐标用） |
| `string` | const char* | UTF-8，NUL 结尾 |
| `object` | struct wl_* * | 另一接口的实例 |
| `new_id` | 返回值 | **请求创建新对象**（如 get_registry） |
| `array` | struct wl_array | 长度前缀的字节数组 |
| `fd` | int32_t | **文件描述符**，走 SCM_RIGHTS 旁路（wl_shm 靠它） |

版本 `version="1"` 意味着：**只能追加，不能修改已有语义**（向后兼容的铁律）。升级版本的套路：加新 request/event、给旧行为加枚举开关。

### wayland-scanner：三种产物

```bash
wayland-scanner client-header  hello.xml hello-client-protocol.h    # 客户端 API 声明
wayland-scanner server-header  hello.xml hello-server-protocol.h    # 服务端 API 声明
wayland-scanner private-code   hello.xml hello-protocol.c           # 消息编解码（共用）
```

生成的客户端 API 形如（见 Makefile 自动生成规则）：

```c
hello_say_hello(struct hello *, const char *message);   // request → 打包发送
hello_send_hello_reply(struct wl_resource *, ...);      // (服务端) 发 event
// 以及 struct hello_listener { void (*hello_reply)(...); };  // 收 event 的回调表
```

**没有魔法**：scanner 只是把 XML 翻译成「按签名序列化消息」的样板代码。模块 8 你会看到这些函数如何被服务器逐个实现。

### 传输格式（选读）

每条消息 = 两个 32 位字（对象 id + 大小+opcode）+ 参数区；fd 不占参数区而是随 SCM_RIGHTS 附带。这就是 wl_shm 传 fd 的底层通道——协议设计者用「fd 是一等公民」换来了零拷贝 buffer 传递。

## 代码走读

客户端 [`main.c`](main.c)：

| 位置 | 内容 |
|------|------|
| [L14-42](main.c#L14-L42) | 三个 event 回调（hello_reply/sum_result/server_info）——「实现 event 侧」 |
| [L44-54](main.c#L44-L54) | registry 里识别自定义接口 `hello` 并 bind |
| [L88-97](main.c#L88-L97) | 没找到 global 时的引导文案（正常情况——先跑模块 8 的 server） |
| [L101-108](main.c#L101-L108) | 发两个 request → flush → dispatch 等 event：一次完整 RPC 往返 |

## 动手实验

本模块需要「两方」：先起服务器再跑客户端。

```bash
# 终端 1: 模块 8 的服务器
cd module-08-server-skeleton && make && ./hello-server
# 输出: 监听 socket: wayland-1

# 终端 2: 模块 5 的客户端（指向上面打印的 socket）
cd module-05-protocol && make
WAYLAND_DISPLAY=wayland-1 ./hello-client
```

预期输出（客户端）：

```
[registry] found global: hello v1 (id=1)
[registry] bound hello interface v1
[event] server_info: protocol_version=1  server=learn-wayland-module8
[event] hello_reply: Hello back from server!
[event] sum_result: 3 + 5 = 8
```

### 观察点

1. **裸看字节流**：`WAYLAND_DEBUG=1 WAYLAND_DISPLAY=wayland-1 ./hello-client 2>&1 | head -20`——libwayland 会打印每条收发的消息（对象id.方法名(参数)），**这是调试一切 Wayland 程序的神器**，把输出与 XML 对照逐字段理解；
2. **改协议再联调**：给 hello.xml 加一个 `ping` request + `pong` event，重新 make（scanner 重跑），在模块 8 的 server 里实现它——体验「协议→代码→双方实现」的完整迭代闭环；
3. **版本协商失败实验**：把客户端 bind 版本改成 2（XML 里只有 1），观察错误；
4. **socket 即协议**：`ss -x | grep wayland-1` 看到这对连接——Wayland 的 RPC 就是一条 unix socket。

## 进阶练习

1. 在 XML 里加 `new_id` 返回的 request（如 `get_stats` 返回一个 stats 对象），实现双向对象创建；
2. 加 `array` 类型参数（一次传多个数求和）；
3. 读 [wayland.xml](https://gitlab.freedesktop.org/wayland/wayland/-/blob/main/protocol/wayland.xml) 里 `wl_surface` 的定义，找出 commit/damage/frame 的完整签名——验证你对模块 2/3/6 的理解；
4. 用 `WAYLAND_DEBUG=1` 重跑模块 2 的窗口程序，找出 configure/ack 的完整往返。

## 常见坑

| 现象 | 原因 |
|------|------|
| 客户端提示「不提供 hello 接口」 | 服务器没先跑，或 `WAYLAND_DISPLAY` 没指向它（默认连的是桌面） |
| 链接错误 undefined reference | private-code 没编译进二进制（Makefile 已处理，注意别手动删生成文件） |
| event 一次没到 | request 后没 flush 就阻塞 dispatch——本代码 [L104](main.c#L104) 特意 flush 了 |
| 修改 XML 后行为没变 | scanner 产物没重新生成（`touch hello.xml && make`） |

## 延伸阅读

- [Wayland 协议文档: XML 规范](https://wayland.freedesktop.org/docs/html/ch05.html)
- [wayland-scanner 手册](https://man.archlinux.org/man/wayland-scanner.1.en)

下一模块: [模块 6 — 帧回调与垂直同步](../module-06-frame-cb/)
