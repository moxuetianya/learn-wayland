# 模块 10：libinput——把形形色色的输入设备读成一种事件

> 🗺️ **全景定位**: [docs/00 — Linux 现代 GUI 全景地图](../docs/00-big-picture.md)

> 服务端三部曲之三。模块 9 解决"出"（像素上屏），本模块解决"入"：键盘、
> 鼠标、触摸板、触摸屏，底层设备千差万别，libinput 把它们归一化成一套统一
> 的事件流，并解释了合成器为什么不直接读 `/dev/input/event*`。模块 11 会
> 把本模块的代码原样搬进合成器，再把它的 fd 挂进 Wayland 事件循环。

## 0. 你将学到什么

- Linux 输入栈分层：内核 evdev → **libinput** → 合成器 → 客户端 `wl_seat`，
  以及 libinput 收编的到底是哪些"脏活"
- **seat** 概念与 udev 枚举：`libinput_udev_create_context` +
  `assign_seat("seat0")` 如何发现设备并处理热插拔
- `struct libinput_interface` 的 `open_restricted` 回调为什么存在——
  特权留给合成器、库保持无特权的设计动机
- **两段式取事件模型**：`libinput_dispatch()` → `libinput_get_event()`，
  以及"fd 可读 ≠ 事件就绪"这个新手第一坑
- libinput 事件类型树与事件生命周期的管理（destroy 由调用者负责）

## 1. 背景原理

### 1.1 输入栈全景：libinput 站在哪一层

```
 ┌────────────────────────────────────────────────────────────┐
 │   Wayland 客户端（模块 4 的 wl_keyboard / wl_pointer）      │
 └──────────────────────────▲─────────────────────────────────┘
                            │ wl_seat 协议事件（合成器转发）
 ┌──────────────────────────┴─────────────────────────────────┐
 │   合成器（模块 11/12）＋ 本模块                              │
 │   ┌──────────────────────────────────────────────────┐    │
 │   │ libinput：归一化 · 加速 · 去抖 · 手势 · 热插拔      │    │
 │   └──────────────┬───────────────────────────────────┘    │
 └──────────────────┼─────────────────────────────────────────┘
                    │ read() struct input_event 字节流
 ┌──────────────────▼─────────────────────────────────────────┐
 │   内核 evdev：/dev/input/event3 …（每设备一个字符设备）       │
 │   更底层：i2c-touchpad、USB HID、PS/2、virtio-input…        │
 └────────────────────────────────────────────────────────────┘
```

内核把"多种物理总线上的输入设备"统一成 evdev 字节流已经是一层抽象，但那
只是**传输层**的统一：字节流里仍是各设备自己的坐标原点、量纲和怪癖。X11
时代每台合成器/桌面自己消化这些差异，结果同一块触摸板在不同桌面手感不同、
bug 修八遍。libinput（由 Wayland/Weston 社区发起）把这层"语义归一化"收编
为一个系统级库：所有合成器（GNOME/KDE/wlroots/Sway）共享同一套输入行为。

### 1.2 为什么不直接读 evdev

假如合成器直接 `read()` `/dev/input/event*`，要自己处理至少这些事：

| 问题域 | evdev 给你的原始样子 | libinput 替你做的 |
|--------|---------------------|-------------------|
| 设备差异 | 触摸板有的报绝对坐标、有的报相对位移，单位不一 | 统一的 `POINTER_MOTION` dx/dy（毫米级归一） |
| 指针加速 | 原始计数，快慢完全取决于设备分辨率 | 内置加速曲线，且可按设备/用户配置 |
| 触摸板手势 | 只有零散的 ABS_MT 位置流 | 双指/三指滚动、捏合缩放、swipe 手势事件 |
| 去抖与手掌检测 | 指尖抖动、打字时手掌蹭到的原始数据全都会来 | 软件去抖、palm detection、tap-to-click |
| 滚轮 | 滚轮一格是 REL_WHEEL 的整数 `1` | 归一化 axis 事件：滚轮离散 1.0/格，触摸板连续像素 |
| 设备发现 | 自己扫 `/dev/input` + 处理热插拔 | udev 枚举 + 监视，回放 ADDED/REMOVED 事件 |
| 多设备同 seat | 两个鼠标两个键盘各是各的 fd | 同一 seat 统一状态（如 `seat_button_count`） |

模块 11 只用其中一小部分（按键+移动+滚轮），但只要它是"合成器"，这层
就绕不开——区别只在"自己写"还是"用 libinput"。

### 1.3 seat 与 udev 枚举

**seat** 是"一组可同时使用的输入输出设备"：单用户台式机就是一个
`seat0`；双座位车站查询机是 seat0+seat1，各配一套键鼠屏。libinput 的设备
发现走 udev：内核给每个输入设备打上 `ID_INPUT=1` 等属性标签，udev 规则
再把键盘/指针/触摸屏分类，`libinput_udev_assign_seat(li, "seat0")` 借这些
标签筛出属于该 seat 的全部设备。之后 udev 监视器自动接管热插拔——插上一个
USB 鼠标，热插拔会以 `DEVICE_ADDED` 事件出现，拔掉则是 `DEVICE_REMOVED`，
**枚举和热插拔是同一套事件流**（assign_seat 时把存量设备"回放"成一批
ADDED）。

### 1.4 open_restricted：特权留给合成器

libinput 需要 fd 才能读设备，但它**不自己 open**——`open()` `/dev/input/*`
需要 root 或 input 组，这是策略问题而非机制问题：

- 以 root 跑的学习项目：回调里直接 `open()`（本模块、模块 11 的做法）；
- 生产合成器（普通用户身份）：回调里走 **logind** 授权接口拿 fd，
  会话激活期间设备归你，VT 切走时自动收回——多座位、多会话不打架。

把打开设备的责任通过 `struct libinput_interface` 交回给使用者，libinput
自身就与特权模型解耦：库管机制（怎么读、怎么解析），合成器管策略（凭什么
读）。这个"回调注入特权操作"的模式与模块 9 的 DRM master/logind 授权是
同一个问题的两种面貌。

```c
static const struct libinput_interface interface = {
    .open_restricted  = open_restricted,    /* 返回 fd 或 -errno */
    .close_restricted = close_restricted,
};
```

### 1.5 两段式取事件：fd 可读 ≠ 事件就绪

`libinput_get_fd()` 返回的 fd 只代表"**底层有新数据可读**"，把它挂进
poll/epoll 后唤醒了，还必须走两步：

```
 poll 返回 POLLIN ──→ ① libinput_dispatch()   读原始 evdev → 跑状态机
                                        │      （去抖判定/手势合成/加速…）
                                        ▼
                        ② libinput_get_event() 循环取空事件队列
```

为什么不能省①：一次 `read` 拿到的原始样本可能还不足以形成事件——去抖要
攒几个样本、双指手势要等第二根手指落下、滚轮合成要累计刻度。dispatch 就是
"喂数据进状态机"的动作，事件只有等状态机拍板后才入队。顺序弄反（或漏掉
dispatch）的典型症状就是"鼠标明明动了，程序却打不出事件"。

### 1.6 事件类型树与生命周期

所有事件都是 `struct libinput_event *` 基类：`libinput_event_get_type()`
判型后再用 `libinput_event_get_keyboard_event()` 等下转型取具体载荷——
一个 C 语言写的"多态"。注意**每个事件由调用者负责
`libinput_event_destroy()`**，libinput 不替你释放；类型不认识就跳过
（default 分支），这样新版本 libinput 加事件类型也不会破坏老程序。

## 2. 代码走读

文件只有 `main.c`，两块结构：

### 2.1 上下文搭建（main）

```c
li = libinput_udev_create_context(&interface, NULL, udev);
...
libinput_udev_assign_seat(li, "seat0");
```

三样东西依次入场：udev 上下文（只管发现）、`libinput_interface` 回调表
（见 1.4）、seat 名。assign_seat 成功的瞬间设备枚举就开始了——所以第一个
`DEVICE_ADDED` 事件在你进入 poll 之前就已躺在队列里，第一次 dispatch 就能
打出来。失败提示"run as root or member of 'input' group"正是
open_restricted 连番 `-EACCES` 的后果。

### 2.2 主循环与事件泵（main + handle_events）

```c
while (1) {
    poll(fds, 1, -1);                    /* 只等 libinput 一个 fd */
    if (fds[0].revents & POLLIN)
        if (handle_events(li)) break;    /* Esc → 退出 */
}
```

`handle_events()` 是标准的"两段式"（1.5 的①②），随后按 1.6 的类型树
switch：设备增删、按键、相对/绝对移动、按钮（带 `seat_button_count`，多
设备同按一目了然）、滚轮 axis、触摸四件套。值得留意的是它拿到的是
**linux keycode**（`KEY_ESC == 1`），不是键名——keycode→keysym 的翻译是
xkb 的职责，模块 4 客户端侧见过，模块 12 会在合成器侧把它接回来。

## 3. 编译与运行

依赖：`sudo apt install libinput-dev`（udev 开发文件随之一并就位）；
或一次跑 `bash setup-vm.sh` 装齐全部模块依赖。
注意：**本模块目录下的 Makefile 是空占位文件**，直接用下面的命令编译：

```bash
cd module-10-libinput
gcc -Wall -Wextra -g -O2 \
    $(pkg-config --cflags libinput libudev) \
    -o libinput-demo main.c \
    $(pkg-config --libs libinput libudev)
```

运行：需要**纯 TTY**（图形会话内也可以跑，但键盘/指针事件会被桌面会话
同时消费，现象混乱；课程统一在 TTY 实验），root 或 input 组权限：

```bash
sudo systemctl stop gdm          # 与模块 9/11 相同的净场流程
sudo ./libinput-demo             # 在 TTY 里（Ctrl+Alt+F3）
```

启动后先是一批设备回放，随后动一动鼠标、按几个键、滚一下滚轮
（**数值与设备名因机器而异**）：

```
=== 模块 10: libinput 输入处理 ===
Listening for input events ... (press Esc to exit)
[dev] added:   AT Translated Set 2 keyboard
[dev] added:   SynPS/2 Synaptics TouchPad
[dev] added:   USB Optical Mouse
[key] keycode= 46  DOWN
[key] keycode= 46  UP
[ptr]  motion  dx=+3.75  dy=-1.25
[ptr]  button  btn=272  DOWN  seat_count=1     ← 272 = BTN_LEFT
[ptr]  axis    VERT  1.00                      ← 滚轮向下滚了一格
Esc（keycode=1）退出
```

对照着把每个输出映射回 1.5/1.6：ADDED 回放发生在 assign_seat 之后；
motion 的 dx/dy 已是加速后的毫米量；滚轮一格正好 1.0。

## 4. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| `Package libinput was not found ...` + `fatal error: libinput.h` | 缺 `libinput-dev`，pkg-config 失败使编译命令展开为空（报错链同模块 9） | `sudo apt install libinput-dev` 或 `bash setup-vm.sh` |
| `Failed to assign seat` / 一串 `Failed to open` | 无权打开 `/dev/input/event*` | sudo 运行，或把用户加入 input 组 |
| 编译报 `udev_new` 隐式声明（GCC≥14 默认报错） | `main.c` 未 `#include <libudev.h>`（Ubuntu 24.04/GCC13 下仅警告） | 在文件头补一行 `#include <libudev.h>` |
| 鼠标动了但打印不出事件 | 漏了先 `libinput_dispatch()` 就 `get_event` | 严格两段式（见 1.5） |
| 收到 `DEVICE_ADDED` 后没有任何事件 | 事件是给"seat"的，但设备被会话独占/或设备不属于该 seat | 用 `sudo libinput list-devices` 核对设备与 seat |
| 想知道按的是什么键 | libinput 只给 keycode，不给字符 | 用 libxkbcommon 翻译（模块 4 / 模块 12 的做法） |
| 在图形会话内跑，终端里打字全被自己的程序吃掉打印 | evdev 是广播式读取：会话和本程序同时收到 | 这正是课程统一用纯 TTY 的原因，不是 bug |
| 触摸板只有 `abs` 事件没有 `motion` | 那是触摸屏（绝对坐标设备），不是触摸板 | 对照 `libinput list-devices` 的设备分类 |

## 5. 与真实实现的差距 / 深入话题

本模块只**读**事件，真实合成器还要做三件事。其一，**配置**：libinput 暴露
一整套 `libinput_device_config_*` 接口（加速曲线、tap-to-click、自然滚动、
左右手模式…），桌面环境的"鼠标设置"面板最终落到这些调用上。其二，**翻译
与转发**：keycode 经 xkbcommon 变 keysym/字符串，指针事件按焦点路由到对应
客户端的 `wl_seat`/`wl_pointer`——模块 11 只用 Esc 退出，模块 12 补全
焦点与转发。其三，**集成进 Wayland 事件循环**：把 `libinput_get_fd()`
挂到 `wl_event_loop_add_fd`，让协议 socket、输入、DRM 共用一个主循环——
这正是模块 11 `init_libinput()` 的最后一行。

更深的话题还有：手势事件（swipe/pinch，Wayland 侧对应 pointer-gestures
协议）、图形输入板（tablet 协议）、中间键模拟与 trackpoint、以及
libinput 本身的测试基建（`libinput-debug-events`、replay 框架）——它把
"输入设备的手感"从玄学变成了可回归测试的工程问题。

## 6. 练习

1. **数一数 open_restricted**：在回调里打印每次被打开的 `path`，对照
   `sudo libinput list-devices` 与本程序开头的 ADDED 回放——验证"枚举即
   回放"与 1.3 的说法。
2. **制造权限事故**：把 `open_restricted` 改成无条件 `return -EACCES`，
   观察程序仍能启动、只是收不到任何设备事件——体会"上下文活着但设备
   全灭"的降级行为。
3. **两段式反例**：把 `handle_events` 里的 `libinput_dispatch(li)` 注释
   掉再跑，亲手复现"动了鼠标却没事件"；然后在 dispatch 前后各加一次
   `libinput_get_event` 计数对比，理解队列何时被填。
4. **滚轮vs触摸板**：分别用滚轮和触摸板双指滚动，观察 axis 事件的取值
   特征（离散 1.0 vs 连续小数），再读 `libinput_event_pointer_get_axis_source`
   把来源打出来。
5. **调速实验**：用 `libinput_device_config_accel_set_speed`（ADDED 时对
  指针设备调用）分别设 -1/0/+1，感受 dx/dy 数值与手感的变化——这是
   桌面"鼠标速度"滑杆的全部秘密。

## 7. 延伸阅读

- [libinput 官方文档](https://wayland.freedesktop.org/libinput/doc/latest/) —
  设备处理语义、配置项、seat 模型的权威说明
- [libinput API 参考](https://wayland.freedesktop.org/libinput/doc/latest/api/) —
  本模块每个调用的出处
- [内核输入子系统文档](https://www.kernel.org/doc/html/latest/input/) —
  evdev 原始事件、`input-event-codes.h` 的键值定义
- [wl_seat 协议定义](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_seat) —
  libinput 的事件最终如何变成发给客户端的 Wayland 输入事件（模块 11/12）
