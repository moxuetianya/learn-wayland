# 模块 10 — libinput 集成：输入的内核侧来源

> **全景图定位**: [docs/00 §3.2](../docs/00-big-picture.md) 「一次按键的旅程」的前半段——从 evdev 到 compositor 的输入加工线。
> **前置**: [模块 4](../module-04-input/)（客户端视角的同一条路）· **产出**: 打印所有原始输入事件的 TTY 调试器

## 目标

- 理解 **evdev → libinput → compositor** 的输入加工管线
- 掌握 libinput 上下文创建（udev 后端）与事件循环（poll）
- 亲眼看触摸板/鼠标事件的「原始形态」

## 原理讲解

### 输入栈分工：为什么需要 libinput

```
┌─ 内核 (evdev) ─────────────────────────────┐
│ /dev/input/event12: 每次硬件中断产出一个     │
│ struct input_event { time, type, code, val }│  ← 原始、机械、设备各自为政
└────────────────────────────────────────────┘
                 │ 读字符设备
┌─ libinput (用户态库, 跑在 compositor 里) ────┐
│ · 合并/排序多设备事件                         │
│ · 触摸板: 手势识别 (双指滚动/缩放/轻点)        │
│ · 加速度曲线, 去抖, 按键重复                  │
│ · 统一成结构化事件 (LIBINPUT_EVENT_*)         │
└────────────────────────────────────────────┘
                 │ compositor 路由给焦点客户端
┌─ Wayland 协议 ──────────────────────────────┐
│ wl_keyboard.key / wl_pointer.motion ...     │  ← 模块 4 收到的
└─────────────────────────────────────────────┘
```

X11 时代每个桌面/窗口管理器自己处理 evdev，手势/加速度实现五花八门；libinput（2013~）把它统一成一处——**每个 Wayland compositor 的输入引擎都是它**（Mutter/KWin/wlroots/sway）。

### 打开设备的方式：udev 后端 + seat

libinput 不直接扫 `/dev/input`，而是问 udev「这个 seat 有哪些设备」（[docs/04 §3.1](../docs/04-login-and-session.md#31-seat席位一组完整-workstation)）：

```c
udev = udev_new();
li = libinput_udev_create_context(&interface, NULL, udev);
libinput_udev_assign_seat(li, "seat0");      // ← 按座位查询: 显示+输入是成套的
```

`open_restricted` 回调（[L11-17](main.c#L11-L17)）是 libinput 把「打开设备的权限」交还给你——compositor 场景里这里会走 logind 的 TakeDevice，课程用 root 直开。

### 事件模型：类型 → 子事件 → 字段

```
libinput_get_event(li)                 取出一个事件
libinput_event_get_type(ev)            判类型
libinput_event_get_keyboard_event(ev)  向下转型到具体事件
libinput_event_keyboard_get_key(kev)   取字段
libinput_event_destroy(ev)             ★ 必须销毁
```

事件按设备分组、时间有序；`DEVICE_ADDED/REMOVED` 让设备热插拔成为一等事件——插鼠标的瞬间你会看到 `[dev] added`。

### poll 集成：为什么 compositor 能同时等输入和客户端

`libinput_get_fd()` 返回一个可 poll 的 fd（它管理着一堆 event 设备 fd）。本模块用裸 poll 等（[L195-210](main.c#L195-L210)）；**模块 11 的下一步就是把这个 fd 挂进 wl_event_loop**——那时 DRM + 协议 + 输入三路 IO 就汇进同一个 epoll，一个「事件驱动 compositor」的骨架就立起来了。

## 代码走读

| 位置 | 内容 |
|------|------|
| [L11-27](main.c#L11-L27) | `open_restricted/close_restricted`: 设备打开权的回调（生产版在此对接 logind） |
| [L45-168](main.c#L45-L168) | `handle_events`: 事件大分发——键盘/指针/触摸/热插拔全类型打印 |
| [L64-73](main.c#L64-L73) | 键盘事件: keycode + 状态（对比模块 4: 这里没有键名——**libinput 不管键盘布局**，那是客户端 xkbcommon 的职责） |
| [L75-81](main.c#L75-L81) | 相对运动 `dx/dy`: 触摸板/鼠标的增量（模块 4 收到的绝对坐标是 compositor 累加后的结果） |
| [L180-193](main.c#L180-L193) | udev 上下文 + seat0 指派 |
| [L195-210](main.c#L195-L210) | poll 主循环: fd 可读 → dispatch → 排空事件队列 |

## 动手实验

```bash
# TTY 下 (root 或 input 组)
sudo ./libinput-demo
```

### 观察点

1. **设备清单**：启动立刻打印一串 `added`——对照 `cat /proc/bus/input/devices`，理解 udev 按 seat 过滤了什么；
2. **触摸板 vs 鼠标**：分别移动两种设备——触摸板产生**高频小 dx/dy**（120Hz 采样），滚轮鼠标的 motion 事件稀疏而大；
3. **双指手势的合成本质**：触摸板双指滚动时看到的是 `POINTER_AXIS` 事件（不是「手指事件」）——手势在 libinput 里被合成了；`LIBINPUT_EVENT_TOUCH_*` 只有在禁用手势的设备上才直接暴露；
4. **按键三连**：按一个键产生 `KEY` DOWN → （延迟后高频 DOWN——重复）→ UP；重复不是内核的（evdev 只报真实按放），是 libinput 合成的（对照模块 4 的 `repeat_info`）；
5. **权限对照**：不 sudo 直接跑 → `assign_seat 失败`——libinput 打不开 `/dev/input/event*`。桌面上为什么应用不需要这个权限？因为它根本不碰 libinput（compositor 独占输入，见 [docs/04 §3.3](../docs/04-login-and-session.md#33-设备访问控制为什么你的-compositor-要-sudo)）；
6. **热插拔**：运行中插拔 USB 键盘，看 `DEVICE_REMOVED/ADDED` 实时打印。

## 进阶练习

1. **加速度感知**：`libinput_device_config_accel_set_speed()` 调到 ±1，观察同一手势的 dx 变化；
2. **指针状态机**：用 dx/dy 累加出绝对坐标并画到模块 9 的 dumb buffer 上——恭喜，你做出了「软件光标」（模块 12 的 cursor plane 知识点对照）；
3. **把手势关掉**：`libinput_device_config_tap_set_enabled` / scroll method 切换后对比事件流；
4. **seat 感知**：`libinput_device_get_seat_logical_name()` 打印每个设备的逻辑 seat（多 seat 实验的入口）。

## 常见坑

| 现象 | 原因 |
|------|------|
| `assign_seat 失败` | 非 root 且不在 input 组——课程用 sudo；桌面会话内则会和 compositor 抢设备 |
| 事件延迟成簇到达 | 忘了先 `libinput_dispatch()` 再取事件（代码 L47 的顺序就是为此） |
| 键名打不出来 | 正常！libinput 只给 keycode（布局归客户端管），对照模块 4 |
| 事件泄漏内存 | 每个 `libinput_get_event` 后必须 `libinput_event_destroy` |
| 桌面里跑设备列表为空 | udev seat 匹配到了但设备被 master 占用/ACL 不在当前会话 |

## 延伸阅读

- [libinput 文档](https://wayland.freedesktop.org/libinput/doc/latest/) — 特别是 [事件处理](https://wayland.freedesktop.org/libinput/doc/latest/event-handling.html) 一章
- [evdev 手册](https://www.kernel.org/doc/html/latest/input/input.html) — 内核侧

下一模块: [模块 11 — 最小化 TTY Compositor](../module-11-tty-compositor/)
