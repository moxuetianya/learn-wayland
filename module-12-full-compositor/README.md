# 模块 12 — 功能完善的 Compositor

> **全景图定位**: [docs/00 §2 第⑤层](../docs/00-big-picture.md) — 一个「桌面级最小集」：窗口管理 + xdg-shell + 焦点路由 + 交互移动。这是课程的毕业设计。
> **前置**: [模块 11](../module-11-tty-compositor/) · **产出**: 能用真实 GTK/Qt 应用（部分）的 TTY compositor

## 目标

- 实现完整 **xdg-shell**（wm_base/xdg_surface/toplevel/popup）
- 输入事件**路由给客户端**（键盘焦点 + 指针焦点 + 光标绘制）
- 窗口管理：Alt 拖动移动、Alt+F4 关闭、焦点切换（点击/环切）
- xkbcommon 在**服务端**的正确用法（keymap 下发 + 修饰键维护）

## 原理讲解

### 补全的协议版图

模块 11 只支持 `wl_compositor + wl_shm`。本模块加上：

| 接口 | 实现位置 | 核心语义 |
|------|----------|----------|
| `xdg_wm_base` | [L716-734](compositor.c#L716-L734) | ping/pong 存活探测（模块 2 客户端回 pong 的对端） |
| `xdg_surface` | [L758-820](compositor.c#L758-L820) | configure/ack 状态机（首次 configure 后才许 commit——模块 2 排过的坑在这里被实现） |
| `xdg_toplevel` | [L823-915](compositor.c#L823-L915) | 标题/app_id/move/resize/最大化/全屏/关闭 |
| `xdg_positioner/popup` | 后半部分 | 弹窗定位（菜单的原语） |
| `wl_seat` + keyboard/pointer | [L384-566](compositor.c#L384-L566) | **输入路由**（模块 4 收到的事件的发送端！） |

### 焦点模型：两套独立焦点

```
键盘焦点 (keyboard focus)          指针焦点 (pointer focus)
  · 全局唯一                        · 光标下最顶层的 surface
  · 点击/Alt+Tab 切换               · 随移动实时变化
  · enter/leave 事件通知            · enter/leave 事件通知
  · 修饰键状态按焦点客户端维护        · 光标图案由指针焦点客户端提供
```

`seat_set_keyboard_focus`（[L511](compositor.c#L511)）/`seat_set_pointer_focus`（[L487](compositor.c#L487)）是两条路的实现：leave 旧的 → enter 新的 → 重新广播修饰键掩码。**这就是模块 4「点别的窗口就收不到按键」的实现现场**。

### 输入路由完整链（对照模块 4/10 闭环）

```
evdev ─▶ libinput(模块10) ─▶ 本模块 seat ─▶ 焦点客户端的 wl_keyboard/wl_pointer(模块4)
```

- **keycode 原样转发**（+偏移归一），客户端自己用 xkb 解释；
- **keymap 下发**：compositor 编译一份 keymap 文本（`create_keymap_fd` [L343-353](compositor.c#L343-L353)），mmap 成 fd 推给每个键盘 resource——模块 4 的 `keyboard_keymap` 回调接收的就是它；
- **修饰键状态机在服务端**：xkb_state 更新 → 掩码以 `modifiers` 事件广播（[L549-566](compositor.c#L549-L566)）。

### 交互式窗口操作：grab 机制

拖动窗口的本质是一个**输入独占期**（grab）：

```
Alt+左键按下 ─▶ xdg_toplevel.move(serial) 请求 [客户端发来]
              ─▶ 开始 GRAB_MOVE: 记录起点, 独占后续指针事件
Alt 按住移动  ─▶ dx/dy 累加到窗口 x,y ─▶ repaint
Alt+左键释放  ─▶ 结束 grab
```

`mc_grab_type { GRAB_MOVE, GRAB_RESIZE }`（[L52](compositor.c#L52)）+ `toplevel_move`（[L848](compositor.c#L848)）实现这套流程。**注意 move 是客户端请求、compositor 执行**——客户端交出控制权换取一致的拖拽体验。

### 软件光标：Wayland 光标协议的服务端

模块 4 埋的梗（客户端 set_cursor）在此回收：指针焦点客户端通过 `wl_pointer.set_cursor` 提交一个 surface 当光标图案，compositor 在 repaint 时**最后合成它**（永远顶层）。没有客户端设置时画一个默认箭头。

### Alt+F4 与 close 事件

`xdg_toplevel.close`（event，compositor→客户端）：请求关闭窗口的礼貌方式——客户端收尾后销毁 surface。Alt+F4 → 找键盘焦点 toplevel → 发 close（对照模块 2 客户端 `xdg_toplevel_close` 回调置 running=0）。

## 代码走读

在模块 11 骨架上新增的板块（按文件顺序）：

| 板块 | 位置 | 内容 |
|------|------|------|
| xkb 服务端 | [L343-382](compositor.c#L343-L382) | keymap 编译/下发/状态机 |
| wl_seat 协议族 | [L384-566](compositor.c#L384-L566) | get_keyboard/get_pointer + 双焦点切换 + 掩码广播 |
| xdg-shell 协议族 | [L716-1010+](compositor.c#L716) | wm_base/xdg_surface/toplevel/popup 全语义 |
| 窗口结构 `mc_toplevel` | [L55-70](compositor.c#L55-L70) | 最大化/全屏状态、surface 包装 |
| 交互 grab | 主循环输入分发处 | move/resize 状态机 |
| 合成增强 | `mc_repaint` | z-order 链表 + 焦点窗口高亮边框 + 光标叠加 |

## 动手实验

```bash
sudo systemctl stop gdm
cd module-12-full-compositor
make && sudo ./compositor
# 记下 WAYLAND_DISPLAY
```

测试客户端矩阵（另开 tty2）：

```bash
export WAYLAND_DISPLAY=wayland-1

weston-simple-shm      # 基础窗口
cd ../module-02-hello-window && ./hello-window    # ★ 模块 2 需要的 xdg_wm_base 现在有了
cd ../module-04-input && ./input-demo             # ★ 输入调试器: 事件真的到了!
cd ../module-06-frame-cb && ./frame-cb            # ★ 帧回调动画
GTK_BACKEND=wayland gtk3-demo                     # 工业级客户端尝试 (GTK3)
```

### 观察点

1. **模块 2 复活**：模块 11 连不上的 `hello-window` 现在正常显示——你补全了它依赖的协议面；
2. **输入闭环**：模块 4 的 `input-demo` 在你的 compositor 上打印键名/坐标——模块 4/10/12 三视角在此对齐；注意它收到的 keymap 就是你 compositor 下发的；
3. **焦点切换**：开两个窗口，点击切换键盘焦点（观察边框高亮/`keyboard enter` 日志），Alt+Tab 环切；
4. **拖动窗口**：Alt+左键拖动——跟踪日志里 grab 状态的建立与释放；
5. **Alt+F4**：对 `hello-window` 按 Alt+F4 → 客户端收到 close 正常退出（对照它源码的 close 回调）；
6. **光标归属**：`input-demo` 没设光标时显示 compositor 默认箭头；weston-simple-shm 会 set_cursor——观察光标图案变化；
7. **GTK 兼容性摸底**：`gtk3-demo` 大概率能显示（GTK 的 shm 路径）——**能跑工业客户端是 compositor 正确性的试金石**；若失败用 `WAYLAND_DEBUG=1` 找出它 bind 了哪个你没实现的接口（如 `wl_drm`/`wl_subcompositor`），那就是你的下一个 TODO。

## 进阶练习

1. **resize 实现**：Alt+右键拖动调整窗口尺寸（complete `toplevel_resize` 的 edges 语义 + 给客户端发 configure 新尺寸）；
2. **最大化/全屏语义**：实现 `set_maximized` 的 configure 往返（先发 configure(屏幕尺寸) → 等 ack → 上屏）；
3. **popup**：实现 xdg_popup 的锚点定位（positioner 数学），跑一个带菜单的 GTK 应用；
4. **键盘导航**：Alt+Tab 用 wl_list 环切 + 预览高亮；
5. **稳定帧率**：接 `drmModePageFlip` + vblank 事件驱动 repaint（和模块 11 练习同款，此处收益更大：动画+光标流畅度质变）；
6. **多 seat**：第二个键盘只路由给「跟随它的焦点」——深入 [docs/04 §3](../docs/04-login-and-session.md#3-systemd-logind设备权的中央管家)。

## 常见坑（含真实调试案例）

> 2026-10 在真实 VM 上的系统验证中，本模块与模块 12 一起修出了 4 类教科书级 bug，均已修复并纳入代码：
> ① **buffer 悬挂指针**：客户端 commit 后销毁 buffer，compositor 仍持旧指针调 `wl_buffer_send_release()` → use-after-free 崩溃（libwayland 内 NULL+0x13）。修复：`mc_buffer_watch` 挂 `wl_resource` destroy 监听，销毁即清指针。**教训：服务端引用客户端资源必须跟踪其生命周期。**
> ② **watch 链表未初始化**：`calloc` 的 `wl_list` 零值直接 `wl_list_remove` → 向 NULL+8 写入。修复：创建时 `wl_list_init`。
> ③ **seat name 事件版本违规**：给绑定 v1 的客户端发 v4+ 的 `name` 事件，简化客户端 listener 槽位为 NULL 直接 abort。修复：`WL_SEAT_NAME_SINCE_VERSION` 守卫。
> ④ **seat 资源 user_data 错传**：把栈上的 compositor 结构当堆对象在析构中 `free()` → 踩坏整个 compositor。修复：user_data 传 `mc_seat_resource` + 回指针。
> 调试手法（值得复用）：`WAYLAND_DEBUG=1` 看协议流、journalctl 看内核段错误地址、stderr 面包屑二分定位、内核日志偏移 + `addr2line` 符号解析。

| 现象 | 原因 |
|------|------|
| 客户端停在「等待 configure」 | 首个 configure 没发（map 请求后要主动 configure 一次）或 serial 不匹配 |
| 键盘乱码 | keymap 下发的 fd 内容与声明的 size 不符；或 keycode 没做归一 |
| 拖动窗口时画面撕裂 | repaint 与 grab 移动没合并成一次重绘（每事件全量 repaint 即可，别双缓冲 dumb buffer） |
| gtk3-demo 黑屏/秒退 | `WAYLAND_DEBUG=1` 找缺失接口（常见 wl_subcompositor、fractional-scale） |
| 光标消失 | 指针焦点切换时 set_cursor 的 surface 生命周期管理错（客户端可能已销毁） |
| Alt 组合键触发不了 | xkb 修饰键掩码没维护（seat_send_modifiers 的调用时机） |

## 毕业检验清单

- [ ] 能解释一条按键从键盘到 GTK 应用回调的**全链路**（模块 4/10/12 三层）
- [ ] 能画出一帧像素从 shm buffer 到屏幕的**全路径**（模块 2/3/9/11）
- [ ] 能说出 configure/ack、ping/pong、move-grab 各自解决的**同步问题**
- [ ] 读 [tinywl](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/tinywl) 源码不再有陌生符号 → 你已具备读 wlroots/weston 的资格

## 延伸阅读

- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) — sway/Hyprland 的地基，本模块概念的工业全集
- [Weston](https://gitlab.freedesktop.org/wayland/weston) — 参考实现
- [xdg-shell 协议](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/stable/xdg-shell/xdg-shell.xml) — 通读一遍，找你实现里偷懒的语义

下一步: [模块 13 — 音频栈实验](../module-13-audio/) · [模块 14 — 登录与会话观察](../module-14-session/)
