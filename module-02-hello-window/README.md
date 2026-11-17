# 模块 2：第一个 Wayland 窗口

> 模块 1 只是"看清单"：连上合成器、打印所有全局对象。本模块第一次
> **使用**这份清单——bind `wl_compositor` / `wl_shm` / `xdg_wm_base`，
> 走通"连接 → 角色协商 → 共享内存画布 → 首帧上屏"的完整最小闭环，
> 弹出一个 640×480 的渐变窗口。

## 0. 你将学到什么

- **窗口角色协商**：为什么光有 `wl_compositor.create_surface` 得不到窗口，
  必须再经 `xdg_wm_base → xdg_surface → xdg_toplevel` 三级赋角色
- **configure / ack_configure 握手**：它解决什么竞态，首次 commit 的规矩是什么
- **wl_shm 的 pool / buffer 生命周期**：一个 fd 如何变成两个进程共享的画布，
  pool、buffer、mmap、fd 各自何时可以释放
- **attach → commit 双阶段模型**首次登场：为什么"挂上"和"生效"要分开
- `xdg_wm_base` 的 ping/pong 存活探测

## 1. 背景：从一块画布到一个窗口

### 1.1 wl_surface 只是画布，角色才让它成为"窗口"

`wl_compositor.create_surface` 返回的 `wl_surface` 是一块**没有任何含义的
像素画布**：不提交就不可见，提交了也只是"某处有一块像素"。窗口的
语义——能不能被拖动、有没有标题栏、如何被任务栏识别——由**角色
（role）**赋予，而角色来自 xdg-shell：

```
wl_surface            纯画布: attach buffer / damage / commit
   │
   └─ xdg_surface      角色容器: 承载 configure/ack 握手
        │
        ├─ xdg_toplevel  "顶层窗口": 可移动/最大化/最小化, 有标题
        └─ xdg_popup     弹出菜单/tooltip (本模块不用)
```

三条设计理由值得咀嚼：

- **合成器是窗口管理的唯一权威**。X11 时代任何客户端都能自定窗口位置、
  改装饰、窥探其他窗口；Wayland 把这些权力全部收回服务端，客户端只能
  "申请一个角色"，剩下听指挥。安全模型由此而来。
- **角色是互斥且不可更换的**（一个 surface 只能有一个角色，赋了就不能换）。
  没有角色的 surface 直接 commit 是协议错误，合成器可以断连——这就是
  "先赋角色再提交"这条铁律的出处。
- **为什么要三级而不是一级**：`xdg_surface` 单独存在是为了承载与角色种类
  无关的握手协议（configure/ack），toplevel/popup 只扩展各自的语义。
  `xdg_wm_base` 则是这三者的工厂与入口。

### 1.2 configure / ack_configure：握手为什么必须存在

socket 上的消息是**异步**的：客户端刚说完"我要 640×480"，合成器可能
正因为用户在拖拽边缘而准备告诉你"实际是 800×600"。若没有同步机制，
两边对"当前窗口状态"的理解会永久错位，画面与输入区域对不上。

xdg-shell 的解法是一个强制回合：

```
客户端                                      合成器
──────                                      ──────
get_xdg_surface + get_toplevel
wl_surface.commit()  (空提交,无buffer) ──→  角色生效
                                           计算首次布局
xdg_toplevel.configure(w,h,states)  ←─事件─  "窗口尺寸/状态定了"
xdg_surface.configure(serial)       ←─事件─  同一回合的回执号
xdg_surface.ack_configure(serial)   ──请求→  "收到,按这个尺寸画"
wl_surface.attach(buffer)
wl_surface.commit()                 ──请求→  首帧像素(尺寸已对齐)
```

规矩（违反即协议错误）：

1. **首次 configure 应答之前，不得提交带 buffer 的 commit**——尺寸都没
  定，画了也是白画；
2. **ack 必须引用 configure 带来的 serial**，合成器凭 serial 知道你应答的
  是哪一次配置；
3. 窗口随后每次变化（用户拉伸、最大化）都会再来一轮 configure，客户端
  重新应答并按新尺寸提交。

`xdg_wm_base` 的 **ping/pong** 是同一哲学的存活探测：合成器周期性 ping，
客户端必须原样回 pong，否则窗口会被标"未响应"甚至强杀——它防的是
"客户端死循环卡住界面"这类场景。

### 1.3 wl_shm：pool 与 buffer 的生命周期

像素如何从客户端到达合成器？最朴素的方式是**共享内存**：

```
客户端进程                                   合成器进程
──────────                                  ──────────
mkstemp("/wayland-xxx") → fd ──┐
ftruncate(fd, size)            │ wl_shm_create_pool(shm, fd, size)
mmap(fd) ──────→ [像素内存] ←──┼──────────── mmap(同一 fd) ←──┐
   逐字节写渐变                │                              │
munmap()                       │ wl_shm_pool_create_buffer(   │ 服务端随后
close(fd)   (可以早关!)        │   pool, offset,w,h,stride,   │ 自己读像素
wl_shm_pool_destroy(pool)      │   FORMAT)                    │
                               └── fd 通过 SCM_RIGHTS 发过去 ─┘
```

对象关系与释放时机（容易踩坑，逐条记）：

- **pool** = "一整块共享内存"，**buffer** = "这块内存上的一扇窗"
  （offset + 宽高 + stride + 格式）。一个 pool 可以切多个 buffer。
- fd 在 `create_pool` 时就通过 Unix socket 的 `SCM_RIGHTS` 机制**复制**
  到了服务端，所以客户端随后 `close(fd)` 完全合法，两边的内核映射不受影响。
- buffer 协议层持有 pool 的引用，因此 `wl_shm_pool_destroy` 之后 buffer
  依然有效——销毁的只是"pool 代理对象"，不是背后的内存。
- buffer 一旦 attach + commit，**写权就交给了合成器**，直到它发来
  `wl_buffer.release` 才能复用（本模块一次性画完不回收，暂不需要处理；
  模块 3 的"每帧新 buffer"是另一种绕开方式）。

### 1.4 attach → commit：双阶段提交首次登场

```c
wl_surface_attach(surface, buffer, 0, 0);   /* 暂存: 只记录到 pending */
wl_surface_commit(surface);                 /* 生效: pending → current */
```

为什么不一次完成？因为 Wayland 用 **pending/current 双状态**实现帧的
**原子更新**：一帧里可能同时换 buffer、挪子 surface、改 damage——
attach 一批、commit 一次，合成器要么看到全部新状态，要么全部旧状态，
永远不会观察到半新半旧的帧。这是贯穿模块 3/6/7 和服务端模块 11/12 的
核心机制，本模块先记住最简形态。

## 2. 代码走读

### 2.1 registry_global()：清单变工具

模块 1 的广播在模块 2 变成实际动作——按接口名 bind 三个全局对象：

```c
if (strcmp(interface, wl_compositor_interface.name) == 0) {
    st->compositor = wl_registry_bind(registry, name,
                                      &wl_compositor_interface, 4);
```

版本参数遵守模块 1 的规则：请求"自己代码支持的版本"（compositor v4、
shm v1、xdg_wm_base v2），在 GNOME（广播 v6）上生效为请求值。

### 2.2 create_buffer()：一块画布的诞生

四步，注释里都有，这里讲顺序的必然性：

```c
fd = mkstemp(name); unlink(name);      /* 匿名文件: 名字没了, fd 还在 */
ftruncate(fd, size);                    /* 0 字节的文件 mmap 不出像素 */
data = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
```

- `mkstemp + unlink` 组合等于匿名文件：路径立即消失、fd 依然有效，
  进程退出内核自动回收——既安全又不留垃圾；
- **必须 `MAP_SHARED`**：`MAP_PRIVATE` 是写时复制，合成器将永远看不到
  你写入的像素（新手高频 bug）；
- 填完像素即可 `munmap`——本模块从此不再碰这块内存；
- 最后 `create_pool → create_buffer → 立刻 destroy pool + close(fd)`，
  依据 1.3 的引用关系，这不会影响 buffer。

### 2.3 三个事件回调：握手的一来一回

- `xdg_wm_base_ping`：一行 `xdg_wm_base_pong(wm_base, serial)`，必回。
- `xdg_surface_configure`：先 ack、后作画，严格按 1.2 的顺序：

```c
xdg_surface_ack_configure(xdg_surface, serial);   /* ① 应答 */
if (st->buffer == NULL)
    st->buffer = create_buffer(st, st->width, st->height);  /* ② 首帧才造 */
wl_surface_attach(st->surface, st->buffer, 0, 0); /* ③ 暂存 */
wl_surface_commit(st->surface);                   /* ④ 生效 */
```

- `xdg_toplevel_configure`：`width/height` 为 0 表示"客户端自己定"
  （首次映射常见），所以代码只在 `> 0` 时覆盖默认的 640×480；
  `xdg_toplevel_close`：用户点关闭 → 置 `running = 0` 让主循环退出。

### 2.4 main()：标准起手式

连接 → 枚举 roundtrip → 检查必备 global → 挂监听 → 建角色链 →
**空提交**换首次 configure → 进入 dispatch 循环。空提交值得再看一眼：

```c
wl_surface_commit(st.surface);      /* 不带 buffer, 只为让角色生效 */
wl_display_roundtrip(st.display);   /* 等首次 configure 送达 */
```

之后程序就静止在 `while (st.running && wl_display_dispatch(...) != -1)`
里，一切窗口行为都由回调驱动。退出时按依赖反序销毁（toplevel →
xdg_surface → surface → 全局代理 → display）。

## 3. 编译与运行

前置同模块 1：桌面 Wayland 会话内的终端。本模块多了一步**协议代码生成**：
xdg-shell 不是核心协议，libwayland 里没有它的实现，Makefile 用
`wayland-scanner` 从系统安装的 `wayland-protocols` XML 生成头文件
（`xdg-shell-client-protocol.h`）与协议实现（`xdg-shell-protocol.c`），
再与 `main.o` 一起链接：

```bash
cd module-02-hello-window
make                # 生成 protocol 头/实现 + 编译 + 链接
./hello-window
```

预期终端输出（窗口弹出前）：

```
=== Wayland 模块 2: Hello Wayland 窗口 ===
创建一个 640x480 的渐变窗口 (xdg-shell)

窗口已创建. 运行事件循环...
理解: attach() 附加像素缓冲区, commit() 原子提交给 compositor.
Compositor 收到 commit 后在下一刷新周期显示.
关闭窗口或按 Ctrl+C 退出.
```

屏幕出现 640×480 窗口：从上到下绿→红渐变、从左到右混入越来越多的蓝
（`r=255*y/h, g=255*(1-y/h), b=255*x/w`），标题"模块2 - Hello Wayland"。
用鼠标拉伸窗口：画面会保持原尺寸被**平铺/留白**而不是跟随变大——
原因见第 5 节。点窗口关闭按钮或 Ctrl+C 退出，终端打印"清理资源..."
与"=== 模块 2 完成 ==="。

## 4. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| `缺少必要的全局接口 (compositor=…, wm_base=(nil))` | 在 X11 会话里运行，没有 `xdg_wm_base` | 确认 `$XDG_SESSION_TYPE` 为 `wayland`；WSL2/X11 下无解 |
| 窗口一闪而过 / 根本不显示 | 在首次 configure 之前就 commit 了带 buffer 的帧（协议错误被断连）；或忘了赋 xdg 角色 | 严格遵守"空提交 → 等 configure → ack → 带像素 commit"的顺序 |
| 窗口秒被置灰/消失 | 忘了回 `xdg_wm_base_pong` | ping 回调里原样回 serial |
| 窗口全黑/内容错乱 | mmap 用了 `MAP_PRIVATE`；或忘了 `ftruncate` | 用 `MAP_SHARED`；先扩容再映射 |
| 红蓝互换 | 像素打包字节序写反（XRGB8888 小端内存序是 B,G,R,X） | 检查 `(0xFF<<24)\|(r<<16)\|(g<<8)\|b` 的位序 |
| 链接错误 `undefined reference to xdg_wm_base_*` | Makefile 没把 wayland-scanner 生成的 `xdg-shell-protocol.c` 编入 | 对照本模块 Makefile 的 `private-code` 规则（模块 3/4 正卡在这，见各自 README） |
| bind 后莫名断连 | 请求版本高于服务端广播版本（模块 1 的协商规则） | 请求自己支持的版本 |

## 5. 与真实实现的差距

按当前代码如实列三条最大的简化：

1. **窗口拉伸后 buffer 不跟随**。`xdg_toplevel_configure` 更新了
   `st->width/height`，但 buffer 只在首次 configure 时创建一次，之后
   重复提交的还是旧尺寸——真实客户端应在尺寸变化时重建 buffer
   （练习 2 会补上）。另外因为没有处理 `wl_buffer.release`，本模块
   也不复用旧 buffer。
2. **单帧静态画面，无重绘节奏**。没有 frame callback，configure 之外
   不会主动再画（模块 3 引入连续重绘、模块 6 引入 vsync 对齐）。
3. **没有 damage 与透明格式**：颜色用不透明的 XRGB8888，也没有上报
   damage 区域让合成器优化重绘。

更宏观的差距：真实工具包（GTK/Qt）在这套裸协议上还要叠加
`wp_fractional_scale`（分数缩放）、`xdg_toplevel.wm_capabilities`
（询问合成器支持哪些窗口操作）、EGL/linux-dmabuf 硬件渲染路径等。

## 6. 练习

1. **观察协议流**：`WAYLAND_DEBUG=1 ./hello-window 2> log`，对照 1.2 的
   时序图找出 ping/pong、configure/ack、attach/commit 各自的消息。
2. **让窗口跟随拉伸**：在 `xdg_surface_configure` 里比较尺寸是否变化，
   变了就销毁旧 buffer、按新尺寸重建（提示：需要保存尺寸并在回调里
   判断"非首次"）。完成后拉伸窗口，渐变应铺满新尺寸。
3. **回应 ping 迟到试试**：把 `xdg_wm_base_ping` 里的 pong 删掉，窗口
   交互时观察 GNOME 标记"未响应"的过程（记得改回来）。
4. **换格式**：把格式换成 `WL_SHM_FORMAT_ARGB8888`，把左半屏的 alpha
   字节改成 0x80，观察半透明效果（需要合成器支持，GNOME 支持）。
5. **统计 configure**：拖动/最大化窗口，在 `xdg_toplevel_configure`
   里打印 states 数组，识别 maximized/activated 位的出现时机。

## 7. 延伸阅读

- [xdg-shell 协议定义](https://wayland-protocols.freedesktop.org/specs/xdg-shell/) — configure/ack、角色规则的权威出处
- [Wayland 书 · Surfaces and frames](https://wayland-book.com/surfaces-and-frames/buffers-in-depth/shm.html) — shm buffer 深入
- [wayland-scanner(1)](https://man.archlinux.org/man/wayland-scanner.1.en) — 协议代码生成器手册
- [wl_shm 协议定义](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_shm) — pool/buffer 关系的官方描述
