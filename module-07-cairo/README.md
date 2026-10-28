# 模块 7 — Cairo 软件渲染集成

> **全景图定位**: [docs/00 §2 第⑥层](../docs/00-big-picture.md) — 工具箱/渲染库如何落在 wl_shm buffer 上（GTK 的 2D 心脏就是 Cairo）。
> **前置**: [模块 3](../module-03-pixels/)（手写光栅化的苦）、[模块 6](../module-06-frame-cb/)（同步渲染的甜） · **产出**: 抗锯齿文字/曲线/渐变的成品 UI 窗口

## 目标

- 把成熟 2D 库 (Cairo) 对接到 Wayland 共享内存 buffer
- 理解 **软件渲染的分层**: 「画什么」(Cairo) 与「送到哪」(wl_shm) 正交分离
- 认识真实 UI 的绘制原语（路径/填充/混合/字体）

## 原理讲解

### 一次教科书级的分层协作

本模块的价值在于展示了 Linux 图形栈的**正交性**：Cairo 对 Wayland 一无所知，它只认「一块内存 + 格式 + 宽高」：

```
┌─────────────────────────────────────────────────┐
│ Cairo 世界 (只关心像素)                           │
│   cairo_image_surface_create_for_data(           │
│       pixels,          ← 你给的内存              │
│       CAIRO_FORMAT_ARGB32, ← 格式约定           │
│       w, h, stride)                              │
└───────────────────────┬─────────────────────────┘
                        │ 同一块 mmap 内存
┌───────────────────────▼─────────────────────────┐
│ Wayland 世界 (只关心传输)                         │
│   wl_shm_pool_create_buffer(...,                │
│       WL_SHM_FORMAT_ARGB8888)   ← 格式约定      │
└─────────────────────────────────────────────────┘
```

两个世界靠**格式契约**对齐：`CAIRO_FORMAT_ARGB32` ≡ `WL_SHM_FORMAT_ARGB8888`（字节序、premultiplied alpha 完全一致），所以 Cairo 写完的内存可以直接交给 compositor，**一行转换代码都不需要**。GTK 的 Wayland 后端本质上就是把这两层粘起来的工业版。

### Cairo 的渲染模型：路径 → 装备 → 绘制

Cairo（以及 SVG/PDF/Canvas）的统一三段式：

```c
cairo_rectangle(cr, x, y, w, h);          // 1. 描述路径 (几何)
cairo_set_source_rgb(cr, 0.3, 0.6, 1.0);  // 2. 装备 (颜色/渐变/图案/线宽)
cairo_fill(cr);                           // 3. 绘制 (fill/stroke/show_text)
```

它内部做的事（对照模块 3 的手写光栅化）：

| 模块 3 手写 | Cairo 内部 |
|--------------|-----------|
| `if ((x/32)^(y/32))` 区域判定 | 路径扫描转换 + **子像素采样** |
| `dist < r` 硬判定（锯齿） | 覆盖率计算 → alpha 混合（**抗锯齿**） |
| `r = 127+127*sinf(...)` 逐点算 | 渐变以 pattern 对象采样，可缓存 |
| 每像素 1 行代码 | SIMD + 行程合并优化 |

**抗锯齿的本质**：图元边缘像素不是非黑即白，而是按覆盖率与背景混合。模块 3 的圆边缘是楼梯，Cairo 的圆边缘是渐变过渡——这就是 `fill_pixels` 与 `cairo_fill` 的全部区别。

### Premultiplied Alpha：一个必须知道的细节

`ARGB8888` 里的 alpha 是**预乘**的：像素存 `(a·r, a·g, a·b, a)`。好处是合成公式从 4 次乘法变 1 次：`dst = src + dst·(1-src.a)`——compositor（模块 11/12 的 `paint_surface`）按此优化。本模块 Cairo 直接输出预乘格式，所以纯 Wayland 通路无需关心；但如果你手改 alpha 通道，记得保持预乘不变式。

## 代码走读

| 位置 | 内容 |
|------|------|
| [L42-48](main.c#L42-L48) | 对接点：`cairo_image_surface_create_for_data` 包住 mmap 内存——**全模块最关键 5 行** |
| [L51-57](main.c#L51-L57) | 线性渐变 pattern（对照模块 3 手写渐变） |
| [L60-65](main.c#L60-L65) | 字体渲染：`select_font_face` + `show_text`（光栅化+hinting 全由 Cairo/字体栈处理） |
| [L29-38](main.c#L29-L38) | `draw_rounded_rect`: 圆角矩形路径（4 段圆弧）——UI 组件的通用形状 |
| [L87-99](main.c#L87-L99) | 贝塞尔曲线 + 饼图扇形（`arc` + `close_path`） |
| [L124-160](main.c#L124-L160) | buffer 创建：与模块 3 相同的管线，只是填充换成了 `render_frame` |

## 动手实验

```bash
cd module-07-cairo
make && ./cairo-demo
```

### 观察点

1. **放大看抗锯齿**：截图（桌面截图工具）放大圆和曲线边缘，与模块 3 的硬边对比；
2. **对照 CPU 占用**：静态画面下 Cairo 版几乎 0%（只在 configure 绘制一次），模块 3 是持续满载——「按需重绘」的价值；
3. **格式实验**：把格式换成 `XRGB8888`（去掉 alpha）——注意 Cairo 侧也要换 `CAIRO_FORMAT_RGB24`，半透明元素（`set_source_rgba(...,0.6)` 的副标题）会出现什么？（提示：没有 alpha 通道时混合对象是背景色本身）；
4. **stride 实验**：故意给 Cairo 和 wl_buffer 传不同 stride（一个 `w*4` 一个 `w*4+128`）——画面斜切撕裂，直观理解「两个世界必须约定同一 stride」；
5. **字体栈**：`FC_DEBUG=1 ./cairo-demo 2>&1 | grep -i family | head` 看 fontconfig 如何把 `"Sans"` 解析成实际字体。

## 进阶练习

1. **动画化**：把模块 6 的帧回调结构套上 Cairo 渲染（done 里 `render_frame`），画一个旋转的饼图；
2. **组件雏形**：实现「按钮」：检测指针位置（模块 4 的 pointer 事件）→ hover 变色 → 点击回调——你等于写了一个 1/1000 的 GTK；
3. **文字进阶**：`cairo_text_extents` 居中一段文字；用 Pango（`pango_cairo`）做自动换行（GTK 的真实文字方案）；
4. **性能对比**：全屏 4K 下用 `perf` 对比模块 3 与本模块的重绘耗时，思考 GPU 渲染（[docs/01 §7](../docs/01-gpu-and-display.md#7-把整条路连起来)）要解决什么。

## 常见坑

| 现象 | 原因 |
|------|------|
| 编译找不到 cairo.h | `libcairo2-dev` 未装（setup-vm.sh 已含） |
| 画面斜切/错位 | Cairo surface 的 stride 与 wl_buffer 的 stride 不一致 |
| 半透明画成黑块 | 用了 XRGB 格式但按 ARGB 混合 |
| 文字不显示 | 字体环境缺（容器/最小系统装 `fonts-dejavu-core`） |
| munmap 后 Cairo 崩溃 | Cairo surface 未 destroy 就 munmap 了底层内存（顺序: cairo_destroy → surface_destroy → munmap） |

## 嘉宾对照：本课程 Cairo 模块 = GTK 的最小内核

```
GTK 应用        widget 树/信号/主题/输入路由
  └─ Cairo      2D 绘制 (本模块)
       └─ wl_shm buffer 传输 (模块 3 管线)
            └─ compositor 合成 (模块 11/12)
```

## 延伸阅读

- [Cairo 官方教程](https://www.cairographics.org/tutorial/)
- [Cairo API 手册](https://www.cairographics.org/manual/)

下一阶段: [模块 8 — libwayland-server 骨架](../module-08-server-skeleton/)
