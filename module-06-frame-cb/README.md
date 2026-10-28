# 模块 6 — 帧回调与垂直同步

> **全景图定位**: [docs/01 §5.2](../docs/01-gpu-and-display.md#52-vblank-与-page-flip为什么会有撕裂) — 客户端如何与显示器的刷新节拍对齐。
> **前置**: [模块 3](../module-03-pixels/)（它的三宗罪在这里清算） · **产出**: 与刷新率同步的弹跳球动画 + FPS 统计

## 目标

- 掌握 **`wl_surface.frame()` 帧回调**：Wayland 的垂直同步机制
- 建立正确的动画循环结构（回调里渲染、回调前挂下一个回调）
- 理解「compositor 节流」的意义：不浪费 CPU/GPU

## 原理讲解

### 为什么模块 3 的动画是错的

模块 3 用 `usleep(16000)` 盲跑 60fps。但屏幕刷新是[硬件节拍](../docs/01-gpu-and-display.md#51-扫描线crtc-是一台不知疲倦的读稿机)（60Hz = 每 16.67ms 一次 vblank），两者各跑各的后果：

```
usleep 节拍:  |──渲染A──|──渲染B──|──渲染C──|──渲染D──|...
vblank 节拍:      ▲v1        ▲v2        ▲v3       ▲v4
                  │          │          │          │
上屏内容:        A的上半+B的下半  B或C   C或D ...   ← 不可预测 → 撕裂/跳帧
```

- 提交太快 → 中间帧被丢弃（白白渲染）+ 可能撕裂；
- 提交太慢 → 掉帧卡顿；
- usleep 根本感知不到真实刷新率（144Hz 屏上浪费一倍，30Hz TV 上撕裂）。

### 帧回调：把节拍交给硬件

```
客户端                              compositor
  │ frame() ─────────────▶ 注册一个 wl_callback
  │ attach(bufN) + commit() ──────▶ 提交第 N 帧
  │                                   ... 等待本帧真正合成的 vblank ...
  │◀── callback.done(time) ────────── "第 N 帧上屏了, 现在画 N+1"
  │ frame() ─────────────▶ ★ 立刻再挂下一个回调
  │ 渲染 N+1 帧, commit
  ...
```

**done 事件的语义**：compositor 在「这帧内容被合成/送出」的时刻触发它。对客户端来说 done ≈ vblank 对齐点。正确循环三要素：

1. **渲染发生在 done 里**（而不是自顾自定时器）；
2. **下一个 frame() 在 commit 之前挂上**——让它和这一帧的状态一起排队（[main.c L150-155](main.c#L150-L155) 的注释强调了这个顺序）；
3. **不渲染就不挂**：窗口被遮挡/最小化时 compositor 不发 done，循环自动休眠——**省电就藏在这细节里**（这就是笔记本合盖不打折续航的原因之一）。

### 高刷屏与节流策略

144Hz 屏幕的 done 每 6.9ms 一次，你的循环自动跟上；compositor 还可能「跳拍」（每 2 个 vblank 回调一次）以节流过快的客户端。**帧回调让动画速率自适应，永远与显示同步**。

> 本模块的弹跳球用固定步长更新物理（`update_ball` 每回调一次走一步）——在 144Hz 屏上球速会快 2.4 倍。正确做法是用 done 的 `time` 参数算 dt；进阶练习 1 修复它。

### FPS 正确的测法

`update_fps()`（[L118-133](main.c#L118-L133)）统计「每秒 done 次数」——这才是**实际上屏帧率**。模块 3 统计的只是「每秒渲染次数」，两者在丢帧时会分道扬镳（观察点 2）。

## 代码走读

| 位置 | 内容 |
|------|------|
| [L137-167](main.c#L137-L167) | `frame_done`: 本模块灵魂——销毁回调→渲染→挂下一个→commit |
| [L174-196](main.c#L174-L196) | `xdg_surface_configure`: 首个 configure 时启动循环（第一次挂回调的合法时机） |
| [L90-116](main.c#L90-L116) | `update_ball`: 重力+反弹物理 |
| [L118-133](main.c#L118-L133) | `update_fps`: 基于 done 次数的正确 FPS |

对照模块 3 的主循环：那里 `while(dispatch) { render; usleep; }`，这里 `while(dispatch);` **渲染全部搬进回调**——结构反转正是同步模型的体现。

## 动手实验

```bash
cd module-06-frame-cb
make && ./frame-cb
```

### 观察点

1. **FPS 读数**：正常应稳定等于屏幕刷新率（60/120/144）。`top` 里 CPU 占用对比模块 3——同步后大幅下降；
2. **遮挡休眠**：用另一个窗口完全盖住它 → FPS 打印停止（或骤降为 0）；移开恢复。compositor 的节流可见；
3. **不撕裂**：球快速移动时边缘不再有模块 3 那种水平错位；
4. **裸看协议**：`WAYLAND_DEBUG=1 ./frame-cb 2>&1 | grep -E 'frame|callback'`——你会看到 frame 请求与 done 事件严格交替；
5. **协议层验证回调顺序**：debug 输出里确认 `wl_surface@N.frame(new id callback@M)` 总在 `commit` 之前的同一批请求里。

## 进阶练习

1. **dt 修正**：用 done 的 `time` 参数计算真实帧间隔，让球在任何刷新率下速度一致（`v += g*dt`）；
2. **buffer 复用**：改成双 buffer 轮换（收到 `wl_buffer.release` 才复用该 buffer），用 strace 验证 mkstemp 从「每秒 60 次」降到 2 次；
3. **故意撕裂**（教学）：同时开两个本模块实例 + 桌面特效全开，观察 compositor 过载时 done 抖动（帧间隔方差变大）；
4. **降频渲染**：每 2 个 done 才重绘一次（半刷新率动画），FPS 与 CPU 同步减半。

## 常见坑

| 现象 | 原因 |
|------|------|
| 只播一帧就停 | 回调链断了：done 里忘了挂下一个 frame()，或首个回调挂早（configure 前） |
| FPS 递减到 0 | 每次新建回调但没销毁旧对象（泄漏），或 done 处理里 commit 顺序颠倒 |
| 144Hz 屏上球飞快 | 固定步长物理没除以刷新率（进阶练习 1） |
| 帧回调与 commit 乱序断连 | frame() 必须和本轮状态一起提交（在 commit 前调用） |

## 延伸阅读

- [wl_surface.frame 协议注释](https://gitlab.freedesktop.org/wayland/wayland/-/blob/main/protocol/wayland.xml#L764) — done 时机的官方定义
- [docs/01 §5.2-5.3](../docs/01-gpu-and-display.md#52-vblank-与-page-flip为什么会有撕裂) — vblank/page-flip 硬件侧

下一模块: [模块 7 — Cairo 软件渲染集成](../module-07-cairo/)
