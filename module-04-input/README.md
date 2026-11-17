# 模块 4：键盘与鼠标输入

> 模块 2 学会"画出去"，模块 3 学会"画得像样"，但窗口仍是死的。
> 本模块接入 Wayland 的输入模型（`wl_seat → wl_keyboard / wl_pointer`），
> 把"输入 → 状态 → 重绘"的交互回路闭合起来：一个跟随鼠标的十字准星
> 窗口，加一个全量打印的输入事件调试台。

## 0. 你将学到什么

- **wl_seat 的能力广播**：seat 是什么、`capabilities` 事件如何声明
  "我有哪些设备"、热插拔时如何再次触发
- **wl_keyboard 的 keymap 事件与 fd 传递**：键位表为什么由服务端用
  **文件描述符**发给客户端，而不是让客户端自备
- **keycode → keysym → utf8 三级转换**：libxkbcommon 的角色、
  `+8` 历史偏移、修饰键掩码的灌入时机
- **wl_pointer 的相对坐标**：`wl_fixed_t` 定点数、surface 相对坐标系、
  enter/leave/motion/button/axis 事件族
- 事件里的 **serial** 是什么、为什么弹菜单/抢焦点都要回传它

## 1. 背景：Wayland 的输入模型

### 1.1 wl_seat：一组设备的能力广播

**seat（席位）= 一组"一起使用"的输入设备 + 它们服务的那个工作位**。
普通桌面就是一个 `seat0`（键盘+鼠标+触摸板一起服侍一块屏幕）；
机场信息亭可能有两个 seat，两套键盘鼠标互不干扰。`wl_seat` 是这些
设备在协议层的统一门面：

```
wl_registry.global("wl_seat") ──bind──→ wl_seat
                                          │ capabilities 事件(位掩码,可多次来)
                                          ├─ KEYBOARD 位 ── get_keyboard ──→ wl_keyboard
                                          ├─ POINTER  位 ── get_pointer  ──→ wl_pointer
                                          └─ TOUCH  位   ── get_touch    ──→ wl_touch (本模块不碰)
                                          └─ name 事件: "seat0"
```

关键设计：**能力是广播出来的，设备是按需领取的**。

- 服务端不直接把 `wl_keyboard` 塞给你，而是广播"现在有键盘能力"；
  客户端看到位才 `get_keyboard`。拔掉蓝牙键盘后能力位清零、
  `capabilities` 再次到来，客户端应 `release` 对应代理。
- 所以正确的写法是**边沿触发**式：位出现且未绑定才 get，位消失且已
  绑定才 release——本模块 `seat_capabilities()` 正是这个形状。
- `name` 事件告诉你 seat 的名字（多 seat 系统才有意义，桌面恒为 seat0）。

### 1.2 键盘：keymap 用 fd 传，解释权在服务端

`wl_keyboard` 绑定后**第一个事件**必然是 `keymap`。它有两种可能的
载体：一整段文本（no-keymap 兜底）或一个**可 mmap 的文件描述符**。
真实合成器（GNOME/KDE）全部用后者，这也是 Wayland 协议里最经典的
fd-passing 用例：

```
合成器                                     客户端
──────                                     ──────
把编译好的 XKB 键位表文本写入 memfd
keymap(format=XKB_V1, fd, size) ──SCM_RIGHTS──→  fd 被"复制"到客户端进程
                                                mmap(fd, size, PROT_READ)
                                                xkb_keymap_new_from_string(...)
                                                xkb_state_new(keymap)
```

为什么这样设计，三层理由：

1. **零拷贝**：键位表动辄几十上百 KB，走消息要过两次序列化，mmap
   一次映射两端共享页缓存。
2. **单一事实源**：键盘解释权在服务端——用户在 GNOME 设置里改了
   布局（比如切到 Dvorak），所有客户端**必须**立刻用新表解释按键，
   否则 A 窗口打出"q"、B 窗口打出"'"。客户端自备键位表会各自为政。
3. **安全边界**：客户端对键位表只读（`PROT_READ` + `MAP_PRIVATE`），
   不能篡改别人的输入解释。

随后的键盘事件序列：`enter`（拿到焦点，附带**当时按住的键**列表）→
（focus 期间）`modifiers` / `key` …→ `leave`（失去焦点）。焦点由
合成器决定，客户端只能被动接受——没有"请求焦点"的协议请求。

### 1.3 keycode → keysym → utf8：三级转换

内核 evdev 给的是**物理键编号**（keycode），跟"按出了什么字符"隔着
两层翻译，全部由 libxkbcommon 在客户端完成：

```
① keycode(Linux evdev 编号)              wl_keyboard 的 key 事件直接给
        │  +8  ←历史偏移: XKB 键码从 8 起, 0~7 留给 X11 遗留
        ▼
② keysym(逻辑键符号, 如 XKB_KEY_a/Escape)   xkb_state_key_get_one_sym()
        │  叠加修饰键状态(Caps/Shift/布局组)   ← modifiers 事件灌进 xkb_state
        ▼
③ utf8(真正要上屏的字符)                    xkb_state_key_get_utf8()
   "a" --Shift--> "A";  "1" --Shift--> "!"
```

三个工程要点：

- **`+8` 不加全错位**：keycode 30 本是 A 键，不加 8 会被解释成完全
  不同的键，且不报任何错——最隐蔽的 bug 之一。
- **修饰键状态也是服务端广播的**（`modifiers` 事件的四个掩码：
  depressed 按住 / latched 粘滞 / locked 大写锁定 / group 布局组）。
  客户端不跟踪按下史，收到掩码就 `xkb_state_update_mask()` 灌进去；
  此后的 key 事件才能正确解释（Shift+a → "A"）。
- **连发是客户端自己的事**：`repeat_info` 事件只告知 rate/delay 参数，
  服务端**不替你重复** key 事件——文本编辑器类客户端要自己起定时器。

本模块代码走到第②级并把 keysym 转成可读名打印（`xkb_keysym_get_name`），
第③级 utf8 是练习 2 的作业——三级管线在模块 7 的 Cairo 文本渲染里
会真正用上。

### 1.4 指针：surface 相对坐标 + 定点数

指针事件族：`enter`（进入窗口，服务端把全局指针换算成本窗口坐标）→
`motion`（高频移动）→ `button` / `axis`（滚轮）→ `leave`。

两个协议级细节：

- **坐标系是 surface 相对**的，原点在窗口左上角。客户端**永远不知道**
  指针在屏幕上的绝对位置——这是 Wayland 隐私模型的体现（窗口不能
  打探别处发生的事），也意味着拖拽出窗口要靠 `wl_pointer` 的隐式
  grab（按下期间 motion 持续发给原窗口）或 explicit-grab 协议。
- **`wl_fixed_t` 是 24.8 定点数**：24 位整数 + 8 位小数。整数像素在
  高 DPI 缩放（1.25x、1.5x）和触控板细腻移动下精度不够，协议里所有
  指针坐标因此都是定点数，`wl_fixed_to_int/double` 负责换算。

**serial** 首次在输入事件里出现：它是服务端给每个输入事件编的
递增序号。客户端做"需要证明由真实用户触发"的事——创建弹出菜单
（模块 12 服务端要做校验）、启动拖拽、用 `xdg_activation` 请求焦点——
都必须**回传引发它的那个 serial**，服务端据此防止伪造输入的应用
抢焦点。

## 2. 代码走读

### 2.1 keyboard_keymap()：收到 fd 的标准处理

```c
if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
struct xkb_keymap *km = xkb_keymap_new_from_string(
        st->xkb_ctx, map_str, XKB_KEYMAP_FORMAT_TEXT_V1, ...);
...
xkb_keymap_unref(st->xkb_keymap);   /* 支持热切换: 新表替换旧表 */
st->xkb_keymap = km; st->xkb_state = ks;
```

流程：校验格式 → 只读映射 → 编译成 keymap → 派生 state → 整体替换
旧表。fd 映射完就 close（内容已在页缓存里，字符串已交给 xkb）。

### 2.2 keyboard_key()：一次按键的翻译

```c
sym = xkb_state_key_get_one_sym(st->xkb_state, key + 8);  /* ② +8! */
xkb_keysym_get_name(sym, sym_name, sizeof(sym_name));     /* 打印用 */
if (sym == XKB_KEY_Escape && state == PRESSED) st->running = 0;
```

回调只做"翻译 + 改状态"，不做渲染——渲染统一在主循环，避免高频
事件把画面节奏搅乱（`pointer_motion` 同理，连 printf 都省了）。

### 2.3 seat_capabilities()：边沿触发的绑定管理

两个 if/else 各管一种能力：位在且未绑定 → `get_*_pointer + add_listener`；
位没了且还绑着 → `release + 置 NULL`。重复的 capabilities 事件到来时
不会重复建对象，热插拔安全。

### 2.4 create_frame() 与主循环：回路的另一半

每帧全量重绘棋盘格背景，在 `pointer_x/y` 处画十字准星（8 像素绿框 +
红点）。主循环 `dispatch → create_frame → attach/damage/commit →
usleep`，与模块 3 相同的朴素节奏。窗口搭建部分与模块 2 一致，仅
多 bind 了一个 `wl_seat`（v5，为了用 `release` 系列请求）。

## 3. 编译与运行

前置：桌面 Wayland 会话内的终端。Makefile 会自动生成 xdg-shell 协议
胶水并一并链接（编译时有两个无害的既有警告：`xdg_tcfg` 里两个 if 写在
同一行触发 `-Wmisleading-indentation`、`reg_global` 的 `ver` 参数未用）：

```bash
cd module-04-input
make
./input-demo
```

窗口获得焦点后，典型交互输出（serial/time 为示例值）：

```
=== 模块 4: 键盘与鼠标输入 ===
理解: wl_seat 代表一组输入设备 (键盘+鼠标+触摸).
      capability 事件广播当前可用的输入设备类型.
      libxkbcommon 将 raw keycode 转为可读键名.
提示: 移动鼠标/按键查看事件. 按 Esc 退出.

seat name: seat0
seat: 键盘已绑定
seat: 鼠标已绑定
⌨  keyboard enter (窗口获得键盘焦点)
⌨  keyboard repeat: rate=33  delay=500 ms
⌨  key DOWN  ser=124  time=531024  a
⌨  key UP    ser=125  time=531132  a
⌨  key DOWN  ser=126  time=531560  Escape
🖰  pointer enter at (320, 240)
🖰  button 272 DOWN  ser=127  time=532100
🖰  axis vertical = -1.00  time=533400
```

读输出：`272` = `0x110` = `BTN_LEFT`；窗口里绿框跟着鼠标走、命中点
是红色；按 Esc（或关窗）退出，打印 `=== 模块 4 完成 ===`。

## 4. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| 按键打印的键名全错/错位 | keymap 事件没处理（`xkb_state` 为 NULL，只剩裸 keycode），或忘了 `+8` 偏移 | 对照 2.2；keymap 到达前先别解释键 |
| 完全收不到 key 事件 | 窗口没有键盘焦点（`keyboard enter` 未来） | 先点一下窗口拿焦点；enter 事件是"门票" |
| 收不到 keymap / xkb 相关函数拿不到东西 | xkb 上下文在 keymap 事件之后才创建 | `xkb_context_new` 必须在连接前（本模块 main 的顺序是刻意示范） |
| motion 一动就刷爆终端 | 在 motion 回调里 printf | 学本模块：motion 只更新状态，渲染在主循环 |
| 坐标当整数丢精度 | 直接把 `wl_fixed_t` 当 int 用 | 一律 `wl_fixed_to_int/double` 转换 |
| 拔掉键盘后使用旧代理断连 | capabilities 位清零后没 release | 边沿触发式管理（2.3） |
| 链接失败 `undefined reference to xdg_*` | Makefile 未编入 wayland-scanner 生成的协议实现（本仓库 Makefile 已含正确规则） | 确认 `wayland-scanner` 已安装后重跑 `make`（同模块 3） |

## 5. 与真实实现的差距 / 深入话题

按当前代码如实列四条边界：**只走到 keysym**——文本输入还需要第③级
`xkb_state_key_get_utf8()`（练习 2），而真正的输入法（拼音/日文）走
`zwp_text_input_v3` 协议，由合成器内嵌 IM 模块处理，客户端收到的已是
commit 好的字符串；**无按键连发**——repeat_info 只打印不实现，编辑器
要按 rate/delay 自行起定时器；**指针只用了 v5 之前的语义**——
`frame`（把一批 axis/motion 圈成一个原子手势）、`axis_value120`
（高分辨率滚轮）、discrete 事件都没碰；**没有触摸/手势**——
`wl_touch`、`zwp_pointer_gestures_v1`（双指缩放）留作扩展。

另一条值得记住的分界线：libxkbcommon 只活在**客户端**，libinput 只活
在**服务端**。合成器用 libinput 从 `/dev/input/*` 读原始 evdev（模块
10/11），翻译成 Wayland 事件广播给客户端；客户端再用 xkb 把 keycode
翻译成字符。两级翻译、各管一半，这就是 Wayland 输入栈的全景。

## 6. 练习

1. **修饰键观察**：按住 Shift 再按 a，对照打印 `A`；再按 CapsLock 后
   释放 Shift，观察差异——体会 modifiers 事件与 xkb_state 的配合。
2. **补全第三级**：在 `keyboard_key` 里加 `xkb_state_key_get_utf8()`，
   把 utf8 字符也打印出来；配合练习 1 验证 Shift+1 → "!"。
3. **热切换键位表**：`gsettings set org.gnome.desktop.input-sources
   sources "[('xkb','us')]"` 改成 `dvorak`，观察 keymap 事件再次到来、
   键名立即变化——单一事实源的直观体验。
4. **画轨迹**：把最近 N 个 motion 坐标存成数组，`create_frame` 里画成
   渐隐轨迹线，得到一个"鼠标尾迹"应用。
5. **连发实现**：按 `repeat_info` 的 rate/delay 用 `timerfd` 或帧循环
   计时，按住某键时自己生成重复的 key 处理，打印重复字符。

## 7. 延伸阅读

- [wl_seat / wl_keyboard / wl_pointer 协议定义](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_seat) — 事件语义的权威出处
- [libxkbcommon 文档](https://xkbcommon.org/doc/current/) — keymap/state API
- [Wayland 书 · Seats, input handling](https://wayland-book.com/xkb/introduction.html) — 三级转换教程
- [linux/input-event-codes.h](https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h) — BTN_* 键码定义
- [text-input 协议](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/stable/text-input/text-input-unstable-v3.xml) — 输入法接入
