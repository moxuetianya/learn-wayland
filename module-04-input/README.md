# 模块 4 — 键盘与鼠标输入处理

> **全景图定位**: [docs/00 §3.2](../docs/00-big-picture.md) 「一次按键的旅程」的终点——输入到达应用。
> **前置**: [模块 3](../module-03-pixels/) · **产出**: 交互式输入调试窗口（键名/坐标实时打印）

## 目标

- 掌握 Wayland 输入模型: **`wl_seat` → `wl_keyboard` / `wl_pointer`**
- 用 **libxkbcommon** 把 keycode 翻译成人类可读键名（键盘布局的正确处理方式）
- 理解焦点 (focus) 与 serial 的含义

## 原理讲解

### 输入的反向通路

显示是「应用 → compositor」，输入是「内核 → compositor → **焦点应用**」。compositor 是唯一的路由器：

```
键盘 ──▶ evdev(/dev/input/event*) ──▶ libinput ──▶ compositor
                                                    │ wl_seat: 只有焦点窗口收到
                                                    ▼
                                              wl_keyboard.key
                                              (你的窗口)
```

模块 10 你会站到 compositor 一侧看同一次按键；模块 4 先站在客户端视角。**关键结论：非焦点窗口收不到键盘事件**——这就是 Wayland 无「全局按键监听」的原因（需要 [portal](../docs/04-login-and-session.md#64-桌面集成的最后一公里portal)）。

### wl_seat：一组输入设备的抽象

`wl_seat` 不是「一个键盘」或「一只鼠标」，而是「一整套人机交互能力」，来自 logind 的 seat 概念（[docs/04 §3.1](../docs/04-login-and-session.md#31-seat席位一组完整-workstation)）：

```
wl_seat (v5+)
 ├── capabilities 事件: KEYBOARD | POINTER | TOUCH (位掩码, 热插拔会变!)
 ├── get_keyboard() ──▶ wl_keyboard
 ├── get_pointer()  ──▶ wl_pointer
 └── get_touch()    ──▶ wl_touch
```

能力是**动态广播**的——拔掉蓝牙鼠标，compositor 重发 capabilities，客户端要跟随绑定/解绑（[main.c L203-227](main.c#L203-L227) 演示了这个双向处理）。

### keycode ≠ 字符：xkbcommon 登场

`wl_keyboard.key` 给的 `key` 是 **evdev keycode**（如 28=Enter, 1=Esc），只对应「物理键位」。同一个物理键在不同布局（US/Dvorak/拼音）下含义完全不同。翻译链：

```
keycode (28)
   │ xkb_state_key_get_one_sym(state, key+8)      ← +8 是 evdev→xkb 的历史偏移!
   ▼
keysym (XKB_KEY_Return)
   │ xkb_keysym_get_name()
   ▼
"Return" (可读键名)
   │ (再进一步: xkb_state_key_get_utf8 → 实际字符, 处理组合键)
   ▼
"\r"
```

keymap 从哪来？**compositor 通过 `keymap` 事件把整套布局推给客户端**（一个 mmap 的文本文件, [L38-64](main.c#L38-L64) 接收并编译）。布局由 compositor 统一管理、客户端只管解释——这与 X11「客户端各自猜」截然不同。修饰键状态同理：compositor 发 `modifiers` 事件（depressed/latched/locked 掩码），客户端 `xkb_state_update_mask()` 更新（[L111-122](main.c#L111-L122)）。

### serial：事件序列号

注意 `enter/leave/key` 都带 `serial`——它是「本次交互动作的编号」。以后你会遇到需要「回执」的请求（如 `xdg_toplevel.move` 要传引发移动的那个按键 serial），compositor 用它确认请求确实源于一次真实用户交互，防止应用伪造输入。

### 指针：表面坐标 + 按钮 + 轴

`wl_pointer` 的事件以**表面局部坐标**给出（`wl_fixed_t` 定点数）：

| 事件 | 含义 |
|------|------|
| `enter/leave` | 光标进入/离开你的 surface（含坐标） |
| `motion` | 移动（表面坐标） |
| `button` | 按下/释放（button 用 **linux input code**: 0x110 左键 0x111 中键 0x112 右键） |
| `axis` | 滚轮/触摸板滚动（值是定点数, 高分辨率滚轮有 `axis_value120`） |

光标图案由**客户端自己绘制**（`set_cursor` 请求）——本模块没画，所以光标在你的窗口上「消失」（实为透明），这是理解「Wayland 光标归客户端」的直观一课。

## 代码走读

| 位置 | 内容 |
|------|------|
| [L38-64](main.c#L38-L64) | `keyboard_keymap`: mmap 接收 keymap 文件 → 编译成 xkb_keymap/state |
| [L84-109](main.c#L84-L109) | `keyboard_key`: keycode → keysym → 键名打印; Esc 退出 |
| [L111-122](main.c#L111-L122) | `keyboard_modifiers`: 修饰键掩码更新 xkb_state |
| [L203-227](main.c#L203-L227) | `seat_capabilities`: 能力变化时绑定/解绑 keyboard/pointer |
| [L241-272](main.c#L241-L272) | `create_frame`: 鼠标位置可视化（绿十字+红中心点） |

## 动手实验

```bash
cd module-04-input
make && ./input-demo
```

### 观察点

1. **焦点实验**：点另一个窗口再打字——本窗口完全静默；点回来出现 `keyboard enter`。输入的独占性亲眼所见；
2. **布局实验**：`setxkbmap -layout dvorak`（或桌面设置里加输入源），再按 `hjkl`——键名打印跟着变，因为 compositor 推了新 keymap；
3. **修饰键链路**：按住 Shift 再按 a → 打印 `A`；观察 modifiers 事件在 key 事件之前到达；
4. **定点数**：滚轮打印的 `%.2f` 值来自 `wl_fixed_to_double`——触摸板惯性滚动时数值是连续小数，机械滚轮是离散整数（通常 ±1 或 ±120/120）；
5. **serial 观察**：记录几次按键的 serial，应该是递增的全局序号（不只是键盘序号）。

## 进阶练习

1. 实现 `xkb_state_key_get_utf8` 输出实际字符，支持中文输入法场景下的 commit 文本（深入: [zwp_text_input](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/unstable/text-input/text-input-unstable-v3.xml)）；
2. 用 `wl_pointer_set_cursor` + 自绘位图，让光标在你的窗口上变成自定义图案（提示: 创建一个 surface 当光标）；
3. 加一个「按住 Ctrl 时显示坐标十字线」的修饰键状态可视化；
4. 记录按键的 `time`（毫秒戳）计算两次按键间隔，观察系统按键重复是 compositor 模拟的（`repeat_info` 事件, [L126-130](main.c#L126-L130)）。

## 常见坑

| 现象 | 原因 |
|------|------|
| 键名全是 `keycode=N` | keymap 事件没处理对（或 compositor 没发），xkb_state 为空 |
| keysym 偏移 8 的诡异错位 | 忘了 `key + 8`（evdev→xkb 偏移），按 A 打印成反引号之类 |
| 光标在窗口里消失 | 没调 set_cursor（本模块故意如此），见观察点 5 |
| 中键变左键 | button code 用错：0x110/0x111/0x112 是左/中/右 |
| Esc 退出后窗口残留 | xdg_toplevel close 与键盘退出没统一清理路径 |

## 延伸阅读

- [libxkbcommon 文档](https://xkbcommon.org/doc/current/) — keymap 编译与 state 机器
- [linux/input-event-codes.h](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/linux/input-event-codes.h) — keycode/button 权威表

下一模块: [模块 5 — 协议 XML 与 wayland-scanner](../module-05-protocol/)
