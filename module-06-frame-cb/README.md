# 模块 6：帧回调——把动画的"节拍器"交还合成器

> 模块 3 做了连续重绘动画，但节拍是 `usleep(16000)` 硬猜的 60Hz：CPU 自顾自地画，
> 合成器自顾自地取，两边各走各的。本模块引入 Wayland 的正确姿势——
> `wl_surface.frame()` 帧回调：**每一帧都等合成器说"该画下一帧了"再动笔**。
> 这是所有真实客户端（浏览器、游戏、GTK/Qt）做动画的基石。

## 0. 你将学到什么

- busy-loop 渲染的三宗罪：CPU 空转、与刷新率失同步、撕裂/跳帧
- `wl_surface.frame` → `wl_callback.done` 的**节流问答**模型，以及回调为何是一次性的
- 为什么 frame 请求必须放在 **commit 之前**（顺序错了退化回 busy-loop）
- done 事件在合成器侧的触发时机：合成/上屏之后；窗口不可见时可以永远不发
- 事件驱动主循环：没有一处 sleep，节奏完全由下行事件驱动
- `wl_buffer` 生命周期与 `wl_buffer.release`（本模块的简化 vs 正确做法）

## 1. 问题：busy-loop 渲染为什么是错的

回顾模块 3 的主循环：

```c
while (st.running && wl_display_dispatch(st.display) != -1) {
    struct wl_buffer *buf = create_frame(&st, st.width, st.height, st.frame_count);
    if (buf) { wl_surface_attach(...); wl_surface_damage_buffer(...);
               wl_surface_commit(st.surface); wl_buffer_destroy(buf); }
    usleep(16000);   /* ← 猜一个 60Hz */
    st.frame_count++;
}
```

| 罪状 | 机制 | 后果 |
|------|------|------|
| CPU 空转 | 无论画面是否变化，每 16ms 都 mkstemp+mmap+全量重绘+销毁 | 一颗核持续有占用；笔记本风扇起飞 |
| 失同步 | 16ms 只是对 60Hz 的**猜测**，且 usleep 之外还有渲染耗时，帧率随机漂移 | 120Hz 屏幕上浪费一半潜能；30Hz 内容上多画一倍废帧 |
| 撕裂/跳帧 | commit 的时机与合成器取帧的时机毫无关系——合成器可能取到没画完的 buffer，也可能一帧取两次 | 画面抖动、运动不连贯 |

根本矛盾：**只有合成器知道下一次合成/上屏何时发生**（它内部对齐 vblank 或 page
flip）。客户端猜，永远猜不准。所以节拍必须由合成器**下行**——frame callback 就是
这条指令。

## 2. 原理：节流问答模型

协议语义（意译自 wayland.xml 的 wl_surface.frame）：

> 请求一个 wl_callback；当合成器**准备好让你提交下一帧**时，它发送 `done`。
> done 的理想时机是上一次提交的内容已被送上输出（或被后续提交取代）之后。

```
   客户端                                        合成器
     │                                             │
     │ ① wl_surface.frame(surf)   → 生成 callback   │
     │ ② attach(buf) + damage_buffer + ──commit──→ │ pending → current
     │                                             │ 合成全部 surface
     │                                             │ 等 vblank / page flip 上屏
     │ ←────────────── ③ callback.done ─────────── │ "现在画下一帧"
     │ ④ 渲染新帧，回到 ①                           │
     │    ……循环，节奏完全由右栏决定 ……               │
```

这是一问一答的**节流阀**：客户端"问"的方式是 frame+commit，合成器"答"的方式是
done。问答不对齐，画笔就动不了。

几个决定成败的细节：

- **回调是一次性的**。wl_callback 在 done 之后协议对象即销毁，客户端每帧都要重新
  `wl_surface_frame()`。于是天然形成"问一次→画一帧→再问一次"的环。
- **frame 请求必须在 commit 之前**。callback 要和**本次提交的 surface 状态**绑在
  一起，done 才对准"这一帧被消费"的时刻。若在 commit 之后才请求，合成器会把它当成
  为"尚未提交的状态"排的队，可能**立刻**回 done——客户端退化成 busy-loop，还是那种
  自以为遵守了协议的 busy-loop。本模块代码里专门为此留了注释。
- **done 就是节流器**。合成器把各 surface 的回调攒到自己 repaint 之后统一发，于是
  客户端帧率自动 ≈ 合成器合成率 ≈ 输出刷新率。更进一步：窗口被完全遮挡、最小化、
  输出休眠时，合成器**可以一直不发 done**——动画自动暂停、功耗归零。这是协议设计
  出来的免费午餐，busy-loop 拿不到。
- **done 携带 time 参数**：合成器时钟（CLOCK_MONOTONIC，毫秒）的时间戳。做物理
  积分应该用它而不是"每帧当作 1/60 秒"——本模块的简化做法见练习 4。
- **多输出**：surface 显示在多个输出上时，每个输出会各触发一次 done，帧回调天然按
  输出节流；要更精确的呈现反馈另有 presentation-time 协议（见第 6 节）。

### 与模块 3 的正面对比

| | 模块 3 busy-loop | 模块 6 frame-cb |
|---|---|---|
| 节拍来源 | `usleep(16000)` 猜 | 合成器的 done 事件 |
| 帧率 | 漂移、不可控 | 锁定在输出刷新率（60Hz 屏≈60fps） |
| 被遮挡/最小化时 | 照画不误 | done 不来，自动停帧 |
| CPU | 持续占用 | 每帧只在 done→commit 的窗口里干活 |
| 与合成器取帧的关系 | 无关，可能撕裂/跳帧 | 每帧对齐一次合成 |

## 3. 代码走读（main.c，291 行）

### 3.1 两个入口：configure 点火，done 续命

xdg-shell 有条硬规定：**收到首个 configure 并 ack 之前，不得 attach buffer**。所以
`main()` 里那次无 buffer 的空 `wl_surface_commit`（main.c:274）只为宣告"surface 角色
已就绪"，骗出第一个 configure；真正的首帧在 `xdg_surface_configure()` 里点火：

```c
if (!st->configured) {
    st->configured = 1;
    struct wl_buffer *buf = create_frame(st);
    ...
    st->frame_cb = wl_surface_frame(st->surface);   /* ① 先要 callback */
    wl_callback_add_listener(st->frame_cb, &frame_listener, st);
    wl_surface_commit(st->surface);                 /* ② 再 commit */
}
```

从此主循环再没有"主动渲染"——一切由 `frame_done` 驱动。

### 3.2 frame_done：顺序敏感的 8 行

```c
static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    struct client_state *st = data;
    wl_callback_destroy(cb);            /* 一次性回调：done 即销毁 */
    st->frame_cb = NULL;                /* 清指针，防 double-free/悬挂 */

    struct wl_buffer *buf = create_frame(st);
    if (buf) {
        wl_surface_attach(st->surface, buf, 0, 0);
        wl_surface_damage_buffer(st->surface, 0, 0, st->width, st->height);

        st->frame_cb = wl_surface_frame(st->surface);  /* 下一帧的问 */
        wl_callback_add_listener(st->frame_cb, &frame_listener, st);

        wl_surface_commit(st->surface);                /* 本次状态发布 */
        wl_buffer_destroy(buf);
    }
    update_ball(st);                    /* 物理更新放在渲染之后 */
    ...
}
```

逐条讲 why：

- `damage_buffer(0,0,w,h)`：告诉合成器"整个 buffer 都是新的"。全量重绘就全量
  damage；生产客户端只 damage 变化的区域，合成器就能跳过干净区域的合成。
- **先 frame 后 commit**：2 节反复强调的顺序，和 3.1 的点火完全同构——帧循环的
  每一圈都是"问（frame）→ 交（commit）"成对出现。
- `wl_buffer_destroy` 紧跟 commit 是**简化而非错误**：请求按序到达服务端，commit
  先于 destroy 被处理；且服务端的 wl_shm 实现按"未销毁 buffer 数"对 pool 持有
  引用，像素内存不会在合成器读它之前失效。真正的生产做法是留 2-3 个 buffer 轮转，
  等 `wl_buffer.release` 事件再复用（练习 3）。
- `update_ball` 放在渲染后：本帧画面用的是更新前的状态，一帧延迟换逻辑清晰。

### 3.3 动画与 FPS：帧率锁定后的物理

`update_ball()` 里 `ball_vy += 0.6f` 是**每帧**加一次重力——隐含"一帧 = 1/60 秒"的
假设。frame callback 让这个假设在 60Hz 屏上恰好成立，但这只是巧合而非契约：换成
144Hz 屏球会下落得快 2.4 倍。正确做法是用 done 的 `time` 参数做时间积分（练习 4）。

`update_fps()` 每秒打印一次帧计数。它是模块 3 对比实验的读数表：本模块应稳定读出
显示器刷新率（60Hz 屏 `FPS: 60`），而模块 3 的读数会带着渲染耗时一起漂移。

### 3.4 main：一个没有 sleep 的动画程序

```c
while (st.running && wl_display_dispatch(st.display) != -1)
    ;
```

`wl_display_dispatch` 在无事件时阻塞。整个程序的节奏 = done 事件的节奏 =
合成器合成的节奏 = 屏幕刷新的节奏。这就是"事件驱动"四个字在 Wayland 里的具体形状。

## 4. 编译与运行

在桌面 Wayland 会话内的终端运行：

```bash
cd module-06-frame-cb
make
./frame-cb
```

预期输出（FPS 数值 = 你的显示器刷新率）：

```
=== 模块 6: 帧回调 (wl_callback / frame callback) ===
理解: wl_surface_frame() 请求一个 wl_callback,
      compositor 在准备好绘制下一帧时发送 done 事件.
      在 done 中渲染并再次请求回调 -> 与刷新率同步, 无撕裂.

帧回调驱动的动画循环开始 (弹跳球 + FPS 计数)...
FPS: 60
FPS: 60
FPS: 60
（关闭窗口后）
=== 模块 6 完成 ===
```

窗口内容：渐变背景上一个弹跳的黄球，触底会衰减反弹。两个对照实验：

```bash
# 实验 1：看 CPU 占用对比（frame-cb 应明显低于模块 3 的 busy-loop）
top -p $(pgrep frame-cb)      # 再跑 module-03-pixels/pixel-anim 对比

# 实验 2：看协议流里的问答循环
WAYLAND_DEBUG=1 ./frame-cb 2>&1 | grep -E "frame|callback" | head -20
```

## 5. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| FPS 远超刷新率（几百上千） | frame 请求放在 commit **之后**，合成器立即回 done | frame 必须在 commit 前，与该次状态绑定 |
| done 永远不来，窗口静止 | surface 未真正映射（没等首个 configure 就画）；或窗口被完全遮挡/合成器停发 | 先 ack configure 再画；遮挡属正常节流 |
| FPS 只有十几帧 | 每帧 mkstemp+ftruncate+mmap+munmap 的系统调用开销 | buffer 池复用（练习 3） |
| FPS 正确但动画忽快忽慢 | 物理按"每帧"积分，帧间隔不稳定 | 用 done 的 time 参数做 dt 积分 |
| 关闭窗口时崩溃 | done 里 `wl_callback_destroy` 后未清 `frame_cb`，退出路径二次销毁 | 置 NULL 后退出前判空（本模块 main 末尾的做法） |
| 帧率锁在 60 但屏幕是 120Hz | 绑定的合成器/输出按最低刷新节流（多输出取最小公倍数等） | presentation-time 或 per-output 观察，本课程不展开 |

## 6. 与真实实现的差距 / 深入话题

本模块每帧新建 wl_shm pool + buffer、用完即毁，正确但浪费：一次 mkstemp、一次
ftruncate、两次 mmap（双方各一）、一串协议对象创建销毁，都摊在每帧头上。生产客户端
（包括 GTK、Qt 内部）用 **buffer 池**：预分配 2-3 块 buffer 轮转，commit 后不销毁，
而是等合成器发 `wl_buffer.release`（"我不再需要它了"）再复用。release 语义是模块
11 服务端 `surface_commit` 里那句"对上一帧 current buffer 发 release"的另一端视角。

帧调度的更深层问题 Wayland 还有专门协议：`wp_presentation`（presentation-time）给
你每帧**实际上屏的时间戳和序列号**（done 只给"该画了"，不给"上屏了"），适合视频
播放的音画同步；`wp_commit_timing` / fifo 系列则允许客户端精确指定某帧该在第几个
vblank 呈现。浏览器和视频播放器靠这些把 jitter 压到毫秒级。最后别忘了 done 节流最
漂亮的一面——**遮挡即暂停**：合成器不 repaint，就没人发 done，客户端的渲染线程安静
地睡在 dispatch 里。这个模型把"何时画画"的决定权彻底交给了唯一掌握全局信息的那个
进程。

## 7. 练习

1. **测量节拍**：把 `frame_done` 的 `time` 打印出来，观察相邻两次 done 的差值——
   60Hz 屏上应恒为 ~16-17ms。再开一个 120Hz 外接屏把窗口拖过去对比。
2. **制造反例**：把 `wl_surface_frame` 那两行挪到 `wl_surface_commit` 之后，重编译，
   观察 FPS 飙升——亲手踩一次第 2 节说的退化 busy-loop。
3. **buffer 池**：改成两块 wl_buffer 轮转，监听 `wl_buffer.release` 后才复用；
   用 `strace -c -p <pid>` 对比每帧系统调用数量的变化。
4. **帧率无关的物理**：用 done 的 `time` 计算真实 dt，替换 `ball_vy += 0.6f` 的
   按帧积分；验证在 60/144Hz 下球速一致。
5. **遮挡实验**：把窗口完全移出屏幕（或最小化），观察 FPS 输出停止；再移回来。
   用 `WAYLAND_DEBUG=1` 确认 done 确实不来了。

## 8. 延伸阅读

- wayland.xml 中 `wl_surface.frame` 与 `wl_callback.done` 的原文
  （`/usr/share/wayland/wayland.xml`）——关于 done 时机与一次性语义的权威表述
- [wayland-book: Frame Callbacks](https://wayland-book.com/surfaces-in-depth/frame-callbacks.html)
- [presentation-time 协议](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/stable/presentation-time/presentation-time.xml)
- [wlroots 的帧调度讨论](https://gitlab.freedesktop.org/wlroots/wlroots) ——
  搜索 frame callback 相关 issue，能看到合成器作者们对 done 时机的纠结
