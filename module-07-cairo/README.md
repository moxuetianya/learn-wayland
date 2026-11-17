# 模块 7：Cairo 软件渲染——把成熟 2D 库"零拷贝"焊到 wl_shm 上

> 模块 3 手写像素太原始，模块 6 学会了帧节奏；本模块解决"画得好看"——引入工业级
> 2D 库 cairo。关键在于：**不换渲染通路、不加一次拷贝**，让 cairo 直接在 wl_shm
> buffer 的 mmap 内存上作画。"第三方渲染库 + wl_shm" 是 Wayland 最低门槛也最通用
> 的对接范式，本模块是这个范式的最小样板。

## 0. 你将学到什么

- `cairo_image_surface_create_for_data`：让 cairo 在**别人的内存**上作画
- 为什么 wl_shm 天然适配一切 CPU 渲染库（cairo/pixman/Skia/ffmpeg……）
- 格式与 stride 两个契约：`CAIRO_FORMAT_ARGB32` ↔ `WL_SHM_FORMAT_ARGB8888`，
  以及**预乘 alpha** 是什么、为什么恰好字节级兼容
- cairo 的 path/fill/stroke 模型、渐变 pattern、三次贝塞尔、toy 文字 API
- 所有权顺序：`cairo_destroy` → `munmap` → 包装成 wl_buffer → commit
- 抗锯齿原理：coverage 光栅化为什么能让圆的边缘平滑

## 1. 背景：wl_shm 是字节搬运工，cairo 是字节生产者

回顾模块 3 的 wl_shm 通路：mkstemp + ftruncate + mmap 得到一块匿名内存，
`wl_shm_create_pool` 把 fd 经 SCM_RIGHTS 交给合成器，**两个进程各自 mmap 同一物理
页**。合成器对这块内存的全部要求只有一行：按你声明 width/height/stride/format
排布字节。

这意味着：**字节怎么写出来的，协议毫不在乎**。手写循环可以，任何能向 CPU 内存按
约定格式写字节的库都可以——cairo（矢量 2D）、pixman（光栅化）、Skia 的 CPU 后端、
ffmpeg 解码出的视频帧……这就是 wl_shm 作为"最低门槛渲染通路"的本质。

### 1.1 零拷贝链条

```
 ┌──────────────── 客户端进程 ─────────────────┐      ┌──── 合成器进程 ────┐
 │ cairo_t ──画──→ 像素内存（mmap, MAP_SHARED）│      │                    │
 │    ↑ cairo_image_surface_create_for_data    │ 同一 │ wl_shm_pool        │
 │    （不分配内存！直接把画布指向 shm 的页）    │─────│ ← mmap 同一个 fd   │
 │                                             │ 物理页│                    │
 └─────────────────────────────────────────────┘      └────────────────────┘
        整条链路 0 次像素拷贝：cairo 直写，合成器直读
```

反例是"有拷贝"的接法：先渲到 cairo 自建的 surface，再 memcpy 进 shm——每帧多一次
整屏拷贝，且把 cairo 限制在固定分配的内存上。本模块的 `render_frame()` 用
`create_for_data` 把两条路并成一条。

### 1.2 两个必须严守的契约

```c
cairo_surface_t *csurf = cairo_image_surface_create_for_data(
    pixels, CAIRO_FORMAT_ARGB32, w, h, stride);
```

**契约一：格式。**

| cairo 格式 | wl_shm 格式 | 内存排布（小端机器） | 说明 |
|-----------|-------------|--------------------|------|
| ARGB32 | ARGB8888 | 4 字节/像素，[B,G,R,A]，**预乘 alpha** | 完全一致，直接透传 |
| RGB24 | XRGB8888 | 4 字节/像素，[B,G,R,×] | alpha 被忽略，位兼容 |

"预乘 alpha"指存储的 RGB 已乘过 alpha（R' = R×a/255）。Wayland 合成做
`dst = src + dst×(1-a)` 的前提是 src 是预乘的；cairo 的 ARGB32 输出恰好就是预乘
的——所以字节可以原样进 wl_buffer，无需任何转换。手写像素（模块 3）不用管它，
一旦写半透明就得自己乘，否则透明区域发白/发黑。

**契约二：stride。** 每行字节数。cairo 按你传入的 stride 落笔，合成器按
`wl_shm_pool_create_buffer` 声明的 stride 读行——**两者必须同一个值**。本模块两处
都是 `w * 4`，同源于一个变量，这是最安全的写法。生产代码常用
`cairo_format_stride_for_width()` 取对齐后的 stride（4/16/64 的倍数利于 SIMD），
此时 wl_buffer 声明必须用同一个返回值。两边不一致的经典症状：图像整体斜着撕裂、
底部出现彩条。

### 1.3 cairo 十五分钟速览（只讲本模块用到的）

- **surface 与 context**：surface 是画布（像素+格式+stride），`cairo_t` 是画笔——
  一个状态机，记住当前颜色/线宽/字体/变换矩阵。所有绘制调用都发给 context。
- **path 模型**（血统来自 PDF/PostScript）：`move_to`/`line_to`/`arc`/`curve_to`
  只是把路径铺在纸上，`cairo_fill`/`cairo_stroke` 才真正落墨。本模块的圆角矩形、
  圆、饼图、贝塞尔曲线全是"铺路径 → fill/stroke"。
- **pattern 是画刷**：纯色只是最简单的 pattern；`create_linear` 造线性渐变，两端点
  之间插值若干 color stop。
- **三次贝塞尔**：`cairo_curve_to(x1,y1, x2,y2, x3,y3)` 从当前点出发，经两个控制点
  到达终点——路径模型里"任意光滑曲线"的标准表达。
- **文字**：`select_font_face` + `show_text` 是 cairo 自嘲的 **toy API**——只做简单
  的从左到右排版，不做复杂文字 shaping（连字、双向文、合字）。真实文字栈是
  fontconfig（找字体）+ HarfBuzz（shaping）+ Pango（布局）+ cairo（glyph 光栅化）。
  演示英文够用；中文/阿拉伯文请直接上 Pango（练习 5）。
- **抗锯齿**：cairo（pixman 后端）光栅化时计算每个像素被几何**覆盖的比例**
  （coverage），按比例与底色混合。所以圆的边缘是一圈灰阶过渡——对比模块 3 手写
  `dx*dx+dy*dy < r*r` 判定的硬边锯齿，这是本模块"好看"的全部秘密。

## 2. 代码走读（main.c，309 行）

### 2.1 draw_rounded_rect：没有原语的图形怎么拼

cairo 没有"圆角矩形"原语，标准做法是四段 90° 圆弧拼角：

```c
cairo_new_sub_path(cr);
cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);        /* 右上角 */
cairo_arc(cr, x + w - r, y + h - r, r, 0,          M_PI / 2); /* 右下角 */
cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2,   M_PI);    /* 左下角 */
cairo_arc(cr, x + r,     y + r,     r, M_PI,       3*M_PI/2);/* 左上角 */
cairo_close_path(cr);                                       /* 直线封口 */
```

arc 自带 line_to 到弧起点，所以四段自然连成轮廓——理解这个技巧后 UI 里一切
"胶囊按钮"都不再神秘。

### 2.2 render_frame：从画布到饼图

- 开头 `create_for_data(pixels, ARGB32, w, h, stride)` + `cairo_create`：零拷贝的关键
  一跃，cairo 从此直写 shm 内存。
- 渐变背景：`cairo_pattern_create_linear` → 两个 color stop → `set_source` →
  `fill`。pattern 是引用计数对象，用完必须 `cairo_pattern_destroy`（泄漏自查可用
  `CAIRO_DEBUG=refcount` 或 valgrind）。
- 文字两行：`select_font_face("Sans", ...)` 经 fontconfig 解析成系统实际字体；
  `show_text` 内部走上面说的 toy 路径。
- 饼图三个扇形，每个都是同一招：**move_to(圆心) + arc(起止角) + close_path** 得到
  "弧 + 两条半径"围成的扇形，三个扇形首尾角度相接拼满整圆；最后叠一个半透明圆心
  点当装饰——半透明能成立，靠的就是 1.2 的预乘 alpha 契约。
- 结尾两句 `cairo_destroy(cr); cairo_surface_destroy(csurf);` 是**所有权顺序**的
  起点，见 2.3。

### 2.3 create_buffer：顺序即正确性

整个函数是一条严格的时间线：

```
mkstemp → unlink          匿名文件惯用法：fd 留下，文件名抹掉，退出自动回收
ftruncate(size)           定尺寸：stride×h
mmap(MAP_SHARED)          ← cairo 直写这块内存
render_frame(...)         cairo 全部画完并 destroy——此后代码不再触碰 pixels
munmap                    客户端映射可以撤了（fd 还在，合成器另有映射）
wl_shm_create_pool(fd)    把 fd 交给合成器
wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888)
                          格式/stride 与 cairo 那边同源（w*4）
commit 之后 wl_buffer_destroy（在 configure 回调里）
```

两个"为什么"：

- **`cairo_surface_destroy` 必须先于 `munmap`**：surface 还活着就 munmap，cairo
  内部再碰 pixels 就是 use-after-unmap，段错误。
- **所有写入必须完成于 commit 之前**：commit 是这帧像素的"发布"点，之后合成器
  随时可能读这块内存。commit 后紧跟的 `wl_buffer_destroy` 是安全的简化——请求按序
  到达服务端，commit 先被处理；服务端 wl_shm 实现按未销毁 buffer 数对 pool 持有
  引用，像素不会在读取中途失效（与模块 6 同款简化，正确做法见练习 3）。

### 2.4 事件模型：静态画面只要一帧

对比模块 6：本模块没有动画，于是**完全没有 frame callback**——只在
`xdg_surface_configure()`（首次 configure 和每次 resize）时画一帧。窗口拖拽缩放时
`xdg_toplevel_configure` 先更新 w/h，紧随的 `xdg_surface.configure` 触发按新尺寸
重绘。这是"按需渲染"的最小形态：画面不变，一个字节都不多画。

至于那个 "Click Me" 按钮——纯装饰。命中检测和重绘留给输入事件，知识在模块 4
（练习 4 把它们串起来）。

## 3. 编译与运行

在桌面 Wayland 会话内的终端运行：

```bash
cd module-07-cairo
make        # pkg-config 拉进 cairo 的头文件与链接参数
./cairo-demo
```

预期输出：

```
=== 模块 7: Cairo 软件渲染 ===
理解: Cairo 在 wl_shm buffer 之上绘制反锯齿图形.
      WL_SHM_FORMAT_ARGB8888 与 Cairo 的 ARGB32 格式兼容.
      mmap 的像素内存直接传给 cairo_image_surface_create_for_data().

Cairo 渲染窗口已启动 (关闭窗口以退出)...

=== 模块 7 完成 ===
```

窗口（640×480 起，可自由缩放）从上到下：蓝紫对角渐变背景；"Cairo on Wayland" 标题
与半透明副标题；蓝色圆角矩形按钮写着 "Click Me"；橙色的圆；一条绿色贝塞尔曲线；
右侧一个三色饼图加半透明圆心。放大窗口到 200%：**边缘依旧平滑**——矢量描述在
每次 resize 时按新尺寸重新光栅化，这是手写像素给不了的。关闭窗口即退出。

## 4. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| 图像斜着撕裂、底部彩条 | cairo 写入的 stride 与 wl_buffer 声明的不一致 | 两处 stride 用同一个变量；或都用 `cairo_format_stride_for_width` 的返回值 |
| 半透明图形发白/发黑 | 手写像素没做预乘（cairo 路径不受此坑） | 要么全交给 cairo，要么自己乘 alpha |
| 文字显示为方块（豆腐块） | 系统缺字体或 fontconfig 找不到 "Sans" | 安装 `fonts-dejavu`；`fc-list | grep -i sans` 检查 |
| 半透明区域不透 | 声明了 XRGB8888（alpha 被忽略） | 需要透明就声明 ARGB8888，且 surface 用 ARGB32 |
| 偶发段错误 | surface 未 destroy 就 munmap，或 resize 时用旧尺寸越界写 | 守住 2.3 的顺序；resize 后等 configure 再画 |
| 窗口全黑 | 没 ack configure / 没 commit / damage 忘了发 | 对照模块 2 的首帧流程检查 |
| 换了格式花屏 | ARGB/XRGB 两处声明不配套（cairo 格式 vs shm 格式） | 按 1.2 的表成对替换 |

## 5. 与真实实现的差距 / 深入话题

**cairo 在 2026 年的图形栈里处于什么位置？** GPU 时代的旗舰客户端已转向 GPU 渲染：
GTK4 的 GSK 用 OpenGL/Vulkan 直接合成场景图，cairo 退居二线（打印、录制 surface、
部分工具链）。但在 Wayland 世界里，"CPU 渲染库 + wl_shm" 这条通路毫发未损——
Weston 自身的 UI 就是 cairo 画的，无数轻量工具（状态栏、通知器、截图工具）走的
正是本模块这条路。把 cairo 换成任何能填字节的库，对接代码一行都不用改。

往深走还有两步：其一，**文字栈补全**——`show_text` 之上是 Pango（布局）+
HarfBuzz（shaping），中文、emoji、双向文才正确；其二，**动画化**——把模块 6 的
frame callback 嫁接到本模块的渲染函数上（练习 4），再换成 2-3 块 buffer 轮转等
`wl_buffer.release`，就得到了一个结构上与真实工具库无异的小型 GUI 程序。再往后
是 GPU 通路：EGL 直接渲染到 dmabuf（模块 11 之外的另一条战线），那正是
`zwp_linux_dmabuf` 协议存在的理由——wl_shm 的"字节契约"换成了"显存对象契约"，
对接的思想一脉相承。

## 6. 练习

1. **故意踩 stride 坑**：给 `render_frame` 传 `stride + 16`（cairo 用它），wl_buffer
   仍声明 `w*4`——观察斜撕；再让两处同用 `cairo_format_stride_for_width` 的返回值
   修好。体会"契约二"。
2. **透明实验**：把背景 fill 改成 `set_source_rgba(cr,0,0,0,0)` 的全透明（记得用
   `CAIRO_OPERATOR_SOURCE`），wl_buffer 保持 ARGB8888——窗口变成只剩图形的悬浮
   贴纸；换 XRGB8888 对比 alpha 被忽略的效果。
3. **变换**：画饼图前 `cairo_translate(cr, 500, 400); cairo_rotate(cr, t);` 把坐标系
   搬到圆心再旋转——cairo 的变换只影响落笔坐标，不动 shm 内存布局。
4. **动画 + 输入合体**：把模块 6 的 frame callback 嫁进来让饼图旋转，再用模块 4 的
   wl_pointer 事件做 "Click Me" 的命中检测与变色——三个模块的知识点闭环。
5. **真文字栈**：链接 Pango，用 `pango_layout` 渲染一行中文与 emoji，对比 toy
   `show_text` 的效果（可能缺字或排版错误）。

## 7. 延伸阅读

- [cairo 官方文档与教程](https://www.cairographics.org/documentation/) ——
  "Cairo Tutorial" 的 PDF 绘图模型一节与本模块 1.3 对应
- [wayland-book: Rendering](https://wayland-book.com/rendering.html) ——
  "Third-party libraries" 一节正是本模块范式的出处
- `man cairo_image_surface_create_for_data`、`man cairo_format_stride_for_width`
- [Pango](https://docs.gtk.org/Pango/) —— 真正的文字排版层
- [pixman](https://gitlab.freedesktop.org/pixman/pixman) —— cairo 背后的光栅化引擎，
  抗锯齿 coverage 的实现现场
