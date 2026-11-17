# 模块 12：全功能 Wayland Compositor —— xdg-shell、输入路由与窗口管理

> 模块 11 的最小合成器已经能把客户端像素送上屏幕，但在它眼里每个客户端只是一张
> 固定在 (0,0) 的"图片"：没有窗口、没有焦点、按键永远到不了客户端。本模块补齐
> 让普通应用真正能用的三块拼图：**xdg-shell 窗口协议**、**wl_seat 输入路由**、
> **窗口管理（点击抬升 / 拖动 / Alt+F4）**。约 1300 行 C，是客户端课程的"毕业合成器"。

## 0. 你将学到什么

- **xdg-shell 协议族**的三层角色：`wl_surface → xdg_surface → xdg_toplevel`，
  以及为什么裸 `wl_surface` 当不了窗口
- **configure / ack-configure 握手**：合成器与客户端如何就窗口尺寸达成一致，
  为什么客户端改尺寸前必须等 configure
- **键盘焦点是谁授予的**：`wl_keyboard` enter/leave 的语义、按键如何路由
- **指针命中测试**（hit testing）如何复用画家算法的 z 序链表
- **关闭窗口的协议路径**：close 是"请求"不是"命令"，服务端为什么不能强杀窗口
- **xkbcommon keymap**：为什么键位映射要用 fd 共享、为什么 keycode 要 +8

DRM/KMS 扫描输出、dumb buffer、libinput dispatch 模型、surface 的
`attach → commit → release` 状态机——这些底座在模块 11 已讲透，本文直接复用
其结论，不再展开。

## 1. 全景：相对模块 11 的增量

```
              模块 11                              模块 12
客户端 ──┐   wl_surface (attach/commit)         + xdg_surface / xdg_toplevel
         │                                     + wl_seat → wl_pointer / wl_keyboard
         ▼
合成器   ├─ 协议  wl_compositor + wl_shm        + wl_seat global
         │                                     + xdg_wm_base global（scanner 生成胶水）
         ├─ 输入  libinput → printf / Esc 退出  libinput → 命中测试 → 转发焦点客户端
         ├─ 窗口  无（全部堆在 (0,0)）           抬升 / 拖动 / Alt+F4 / 最大化
         └─ 合成  画家算法（链表序）             不变，但 z 序现在可交互、有意义
```

| 维度 | 模块 11 | 模块 12 |
|------|---------|---------|
| 窗口协议 | 无，只认裸 surface | xdg-shell（toplevel 一族） |
| 输入 | 本地消费（Esc 退出），printf 打印 | 转发给焦点客户端，本地只留组合键 |
| 焦点 | 无 | 键盘焦点 + 指针焦点，click-to-focus |
| 窗口位置 | 写死 (0,0) | 拖动（grab）、点击抬升 |
| 依赖 | libdrm / libinput / libwayland | + wayland-protocols / libxkbcommon |

一句话：模块 11 证明"像素能上屏"，模块 12 证明"协议能对话"。

## 2. 背景原理

### 2.1 xdg-shell：给 surface 一个"角色"

wl_core 协议（wl_surface）只回答"一块像素怎么提交"，所有窗口语义都由
**role 协议**叠在上面。xdg-shell 的三层分工：

```
wl_surface       像素载体：attach / commit / damage（模块 11 已学）
   │  xdg_wm_base.get_xdg_surface(wl_surface)
   ▼
xdg_surface      角色容器：给 surface 一个"身份"，掌管 configure 序号，
   │             并规定"首个 configure 到来前不得 commit"
   │  xdg_surface.get_toplevel()
   ▼
xdg_toplevel     窗口语义：标题、最大/最小化、全屏、close 事件、交互移动/缩放
```

**为什么裸 wl_surface 不能当窗口？**

- **谁定边界**：buffer 尺寸 ≠ 窗口尺寸。客户端会把阴影、圆角、客户端自绘的
  标题栏都画进 buffer，真实窗口几何要靠 `xdg_surface.set_window_geometry`
  声明；输入区域又是另一个量（`wl_surface.set_input_region`）。
- **谁管生命周期**：协议规定客户端必须先等第一个 configure 事件才能 commit。
  没有这层握手，合成器还没准备好管理这个窗口，像素就涌进来了。
- **角色唯一**：一个 surface 同时只能扮演一个角色。给已有角色的 surface 再包
  一层是协议错误，代码里用 `wl_resource_post_error(XDG_WM_BASE_ERROR_ROLE)`
  回应（见 `xdg_wm_base_real_get_xdg_surface`）。

`mc_toplevel` 结构体就是这个角色对象的服务端记账：标题、几何、
configure 序号、mapped/maximized/fullscreen/minimized 标志。

### 2.2 configure / ack-configure：尺寸协商握手

Wayland 里**没有人能直接命令别人改窗口大小**——合成器不碰客户端的 buffer，
客户端也不该自作主张。尺寸变化是一次双向握手：

```
合成器                                              客户端
──────                                              ──────
xdg_toplevel.configure(w, h, states) ────────────→  "我建议你变成 w×h，
xdg_surface.configure(serial)       ────────────→   这次协商的编号是 serial"
                                                    按新尺寸重新布局、渲染
xdg_surface.ack_configure(serial)   ←────────────  "收到，serial 对得上"
wl_surface.attach(new_size_buffer)  ←────────────
wl_surface.commit()                 ←────────────  这次 commit 才携带新尺寸生效
```

要点：

- **两个事件成对出现**：`xdg_toplevel.configure` 携带内容（尺寸 + 状态位，
  如 MAXIMIZED），紧随其后的 `xdg_surface.configure` 携带本次协商的 serial。
  `toplevel_send_configure()` 严格按这个顺序发。
- **客户端改窗口大小前必须等 configure**——attach 新尺寸的 buffer 必须落在
  对应 ack 之后的 commit 里，否则协议错误。
- **serial 是对账单号**：客户端 ack 时带回同一个 serial，合成器据此知道
  "客户端现在应用的是哪一次协商"。

本模块的诚实简化：`ack_configure` 只比对 serial 就丢弃；`mapped` 改由
"第一次带 buffer 的 commit"判定（见 `surface_commit`）；resize 请求是空实现。
握手只实现了"通知"方向，没有实现完整的"协商"方向。

### 2.3 ping / pong：客户端假死检测（本模块留白）

协议还提供了 `xdg_wm_base.ping` / `pong`：合成器定期发 ping（带 serial），
客户端事件循环活着就必须尽快回 pong；超时不回即可判定假死，此时合成器才
**有权**弹"应用未响应"或强制断开——这是"服务端不能强杀窗口"原则的唯一例外
出口。本模块 `xdg_wm_base_pong` 是空实现且从不发 ping，列入 6 节差距。

### 2.4 窗口管理：close 是"请求"不是"命令"

```
用户按 Alt+F4
  → handle_key_event()
      → xdg_toplevel_send_close()          ← 只发一个事件，仅此而已
          → 客户端自己决定：通常销毁 toplevel/surface
                          也可能弹"文件还没保存，退出吗？"
                          甚至假装没听见（bug 或恶意）
              → wl_surface 销毁 → surface_destroy() → 合成器重绘
```

Wayland 的协议哲学：**窗口的生命周期归客户端**。合成器不替客户端释放资源，
只表达"用户想关你"。这保证了对话框、"保存吗"提示等 UX 成立。真正需要
强杀时走的也不是协议，而是 `wl_client_destroy` / 断 socket——本模块
`cleanup()` 里 `wl_display_destroy_clients` 就是在退出时干这个。

其余窗口管理动作的落点：`set_title`（打印窗口名）、`set_maximized /
set_fullscreen`（置标志后发屏幕尺寸的 configure）、`set_minimized`（只置
标志，无还原路径）、`xdg_toplevel.move`（见 2.5 的 grab）。

### 2.5 焦点与输入路由：enter/leave 是合成器授予的

键盘焦点不是客户端"抢"来的，而是合成器**授予**的：

```
seat_set_keyboard_focus(c, new)
  ├─ 旧焦点客户端: wl_keyboard_send_leave(serial, old_surface)
  └─ 新焦点客户端: （首次）先发 keymap fd
                   wl_keyboard_send_enter(serial, surface, 按下的键集合)
                   wl_keyboard_send_modifiers(...)   ← 同步修饰键状态
```

- 之后 `handle_key_event` 里所有非组合键的按键都 `wl_keyboard_send_key`
  给焦点客户端；客户端拿 keymap 自己把 keycode 翻译成字符。
- 指针焦点则**每次 motion 都重算**：`surface_at()` 从链表尾（最顶层）向头
  做矩形命中测试，命中结果变了就发 pointer leave/enter——这就是"指针悬停
  到哪个窗口"的实现。鼠标点击（press）同时做 click-to-focus：抬升窗口
  （`raise_surface`）并把键盘焦点交给它。
- **交互式移动（grab）**：客户端（通常是自己画的标题栏）调
  `xdg_toplevel.move`，合成器记录 grab 偏移并进入 GRAB_MOVE 状态；此后
  所有 motion 事件被合成器**截获**用于搬窗口（`surface->x/y = pointer -
  grab_offset`），直到按键释放。这就是窗口拖拽的全部秘密。

### 2.6 键位映射：keymap 走 fd、keycode 要 +8

- 合成器用 xkbcommon 按 "evdev/pc105/us" 编译 keymap，序列化成文本后写进
  **memfd**，通过 `wl_keyboard_send_keymap(fd, len)` 发给客户端——keymap
  文本几十 KB，协议用"传 fd + 长度"让客户端 mmap 共享，而不是走 socket
  逐字节搬。真实合成器在创建 keyboard 资源时就发，本模块延迟到首次聚焦前
  补发（`keyboard_keymap_sent` 标志保证只发一次）。
- 每个按键经 `xkb_state_update_key` 更新服务端 xkb 状态，再把修饰键掩码
  `wl_keyboard_send_modifiers` 出去——客户端靠它区分 `c` 和 `Ctrl+c`。
- `key + 8`：libinput 给的是 evdev keycode（从 0 计），xkbcommon 的 keycode
  从 8 计（历史遗留的 8 字节偏移），两套编号差 8。

### 2.7 合成与 z 序：一条链表两种走法

模块 11 的 surfaces 链表只用来当绘制顺序，本模块让它身兼二职：

| 遍历方向 | 用途 | 代码 |
|----------|------|------|
| 头 → 尾 | 绘制（画家算法，后画的盖住先画的，尾 = 最顶层） | `mc_repaint` |
| 尾 → 头 | 命中测试（先问最顶层的窗口） | `surface_at` |

`raise_surface` 把 surface 摘下来插到链表尾 = 置顶。移动窗口只是改
`surface->x/y` 后全量重绘——绘制本体（`paint_surface`）与模块 11 完全相同。

## 3. 代码走读

### 3.1 Makefile：wayland-scanner 首次进场

模块 11 只用 wayland 核心协议，libwayland 自带头文件与实现；xdg-shell 是
核心之外的协议，胶水代码要从 XML 现场生成：

```make
xdg-shell-server-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner server-header $< $@      # 接口/事件/枚举的声明
xdg-shell-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@       # 请求分发/事件编码的实现
```

`xdg_wm_base_interface` 等符号都来自这两个生成物，所以最终二进制要链
两个 .o。

### 3.2 数据结构增量

- `mc_toplevel`（全新）：窗口角色的记账，挂在 compositor 的 `toplevels` 链表；
  `mc_surface` 增加 `toplevel` 反向指针。
- `mc_client` 增加 `pointer / keyboard / keyboard_keymap_sent`：每个客户端
  持有的 seat 资源——事件路由时按客户端找它们。
- `mc_compositor` 增加 seat 域（`seat_resources`、`pointer_focus`、
  `keyboard_focus`、坐标）、xkb 域、grab 域（`grabbed_toplevel`、
  `grab_type`、`grab_x/y`）。

### 3.3 xdg-shell 实现链

`xdg_wm_base_real_bind`（版本钳到 3）→ `get_xdg_surface`（角色检查）→
`get_toplevel`（挂实现 + 发首个 configure）：

```c
    wl_resource_set_implementation(resource, &xdg_surface_impl_init, t,
                                    xdg_surface_resource_destroy);
    toplevel_send_configure(t, 0, 0);
```

这行 `configure(0, 0)` 不是可有可无的礼貌——协议规定客户端在收到首个
configure 前不得 commit，不发它规矩的客户端会永远卡住不画。

### 3.4 seat 与焦点管理

`seat_bind` 发 capabilities(pointer|keyboard) 和名字；`seat_get_keyboard`
里有个容易忽略的细节——客户端可能在焦点已经存在之后才 bind seat：

```c
    if (c->keyboard_focus && wl_resource_get_client(c->keyboard_focus->resource) == client)
        seat_set_keyboard_focus(c, c->keyboard_focus);
```

迟到的 keyboard 资源要**补发一次 enter**，否则这个客户端永远收不到按键。

### 3.5 输入路由：从 printf 到转发

模块 11 的 `handle_keyboard`/`handle_pointer_*` 只 printf；模块 12 的
`handle_key_event` 变成三段式：组合键本地消费（Esc / Ctrl+Alt+Backspace
退出、Alt+F4 发 close）→ 其余转发焦点客户端 → 更新 xkb 并广播 modifiers：

```c
        if (alt && key == KEY_F4 && c->keyboard_focus && c->keyboard_focus->toplevel) {
            xdg_toplevel_send_close(c->keyboard_focus->toplevel->xdg_toplevel_resource);
            return;
        }
```

`handle_pointer_motion` 同样三分支：grab 期间截获移动去搬窗口；否则命中
测试 + 维护指针焦点；最后转发 motion。注意一个已知简化：转发的坐标是**屏幕
全局坐标**，协议要求的是 surface 局部坐标（应减去 `s->x/y`）。

### 3.6 生命周期防御：三处悬空指针清理

窗口可以被客户端随时销毁，所以凡是存了 `mc_surface *` 的地方都要防悬空：

- `surface_destroy`：清 `pointer_focus` / `keyboard_focus` / grab / toplevel
  反向指针，再 repaint；
- `client_destroy`：客户端整个断开时同样清一遍，否则焦点指向已释放内存；
- `toplevel_resource_destroy`：xdg_surface 与 xdg_toplevel 两个资源的
  析构函数都可能触发 free(t)，互相把对方的 `user_data` 置 NULL 防双 free。

## 4. 编译与运行

前置与模块 11 相同：真实机器或 VM 的**纯 TTY**、停掉图形会话，另外需要
`wayland-protocols` 和 `libxkbcommon` 的开发包。

```bash
# 1. tty3 登录（VM 里 Ctrl+Alt+F3），停掉图形会话
sudo systemctl stop gdm          # 或 sddm / lightdm

# 2. 编译并启动（首次 make 会先生成 xdg-shell 胶水）
cd module-12-full-compositor
make
sudo ./compositor

# 3. 另一个 TTY（Ctrl+Alt+F4）或 SSH 里连客户端
WAYLAND_DISPLAY=wayland-1 weston-simple-shm    # 模块 11 的老朋友，仍然可用
WAYLAND_DISPLAY=wayland-1 weston-terminal      # xdg-shell 客户端，能打字了！
```

有 xdg-shell 之后，**自绘装饰的 shm 客户端**（如 weston-terminal：自己画
边框、自己渲染文字、只要 keymap + 按键转发）第一次真正可用——在里面敲键盘，
字符经过 libinput → 合成器 → wl_keyboard → 客户端 xkb 翻译，走完了输入
协议的完整闭环。

操作速查：**点击**窗口 = 抬升 + 获得键盘焦点；**按住标题栏拖动** = 移动
（客户端发起 move，合成器 grab）；**Alt+F4** = 请求关闭；**Esc /
Ctrl+Alt+Backspace** = 退出合成器（断开所有客户端）。

预期内的局限：屏幕上**看不到鼠标指针**（`pointer_set_cursor` 是空实现，也不
画默认箭头），但指针位置在合成器内部是准确的，移动/点击/拖拽都有效；重量级
应用（GTK/Qt/EGL 系，如 gedit、alacritty）大概率起不来——它们还需要
wl_output、data-device-manager、linux-dmabuf 等一整串本模块没有的协议。

退出与恢复同模块 11：合成器内 Esc 退出后 `sudo systemctl start gdm`。

## 5. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| make 报找不到 `xdg-shell.xml` | 未装 wayland-protocols 或 pkg-config 探测不到 | 安装后确认 `pkg-config --variable=pkgdatadir wayland-protocols` 有输出 |
| 链接报 `undefined reference to xdg_wm_base_interface` | 手动 `cc compositor.c` 漏了生成物 | 用 make，`xdg-shell-protocol.o` 必须一起链 |
| 客户端连上后秒退 / 协议错误 | 客户端需要本模块未实现的 global（wl_output、data-device-manager…）或 EGL | 换 weston-terminal / weston-simple-shm 验证 |
| 屏幕上找不到鼠标指针 | `pointer_set_cursor` 空实现，也没画默认光标 | 预期行为，指针追踪仍在工作；补画光标见练习 4 |
| 点窗口内控件位置偏移/不灵 | motion 转发的是屏幕全局坐标，客户端按 surface 局部坐标解释 | 已知简化，见练习 3 |
| Alt+F4 没反应 | 键盘焦点不在目标窗口（没点击过）或该 surface 没有 toplevel | 先点击目标窗口再按 |
| 窗口拖出屏幕找不回来 | 拖动没做边界约束，窗口失去可点击区域 | 重启客户端；加约束见练习 |
| 菜单/右键弹不出来 | `get_popup` 是空实现，本模块只有 toplevel | 预期行为，popup 在差距清单里 |
| Esc 一按整个合成器退出 | Esc 是设计好的全局退出键，会断开所有客户端 | 预期行为；改键位在 `handle_key_event` |
| 黑屏 / drmSetMaster 失败 | 同模块 11：gnome-shell 仍是 DRM master | `systemctl stop gdm`，`pgrep -a gnome-shell` 应为空 |

## 6. 与真实实现的差距

对照 mutter / wlroots / tinywl（tinywl 几乎就是"本模块的完全体"）：

| 本模块 | 真实实现 |
|--------|----------|
| 从不发 ping，pong 空实现 | 周期 ping，超时未 pong 判假死（"未响应"对话框/灰化） |
| resize 空实现，GRAB_RESIZE 枚举从未使用 | 完整 resize 握手 + 边缘检测 + min/max 尺寸钳制 |
| popup / positioner / set_parent 全是 stub | 菜单、 tooltip、模态对话框 |
| cursor 不渲染，set_cursor 忽略 | 主题光标：硬件 cursor plane 或软件叠加，hotspot 处理 |
| motion/enter 坐标为屏幕全局 | 换算成 surface 局部坐标（减 surface 偏移） |
| modifiers 广播给所有客户端 | 只发给键盘焦点客户端 |
| move 不校验 serial | 校验引发交互的 button serial，防过期请求抢 grab |
| damage 仍全忽略、全屏重绘（同模块 11） | damage tracking + 局部重绘，叠加 page flip |
| 无 wl_output、无多输出 | 完整输出管理、热插拔、scale |
| 无 data-device（剪贴板/拖放）、xdg-decoration、layer-shell、linux-dmabuf | 桌面级全家桶 |
| 手工 user_data=NULL 防双 free | wlroots 用 wlr_* 封装 + signal 生命周期 |

## 7. 练习

1. **实现 resize**：在 `xdg_toplevel_resize` 记录 edges，motion 里按边改尺寸
   并 `toplevel_send_configure`，观察客户端 ack 后提交新尺寸 buffer 的完整握手。
2. **真正的 ping/pong**：用事件循环定时器每 5 秒 `xdg_wm_base_send_ping`，
   超时未 pong 打印"客户端假死"——你就实现了 2.3 的完整逻辑。
3. **surface 局部坐标**：转发 motion/enter 前减去 `s->x / s->y`，用
   weston-terminal 验证点击位置与文字光标是否对齐。
4. **软件光标**：在 `pointer_set_cursor` 里记下客户端光标 surface 和
   hotspot，`mc_repaint` 最后叠加绘制；没设置过就画一个默认箭头。
5. **Alt+Tab**：在 `toplevels` 链表里轮换键盘焦点并抬升，注意焦点切换要走
   enter/leave 而不是只改指针。

## 8. 延伸阅读

- [xdg-shell 协议正文](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/stable/xdg-shell/xdg-shell.xml) — XML 里的注释本身就是最权威的教材
- [tinywl](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/tinywl) — 本模块的"完全体"：同款功能 + resize + page flip
- [The Wayland Book](https://wayland-book.com/) — seat/输入协议与 xkbcommon 章节
- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) / [mutter](https://gitlab.gnome.org/GNOME/mutter) — 生产级实现对照
