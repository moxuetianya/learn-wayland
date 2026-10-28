# 模块 3 — 手动像素缓冲区与图形绘制

> **全景图定位**: [docs/00 §3.1](../docs/00-big-picture.md) ⑥ 层——亲手当一次「渲染器」，理解 toolkit 替你做的事。
> **前置**: [模块 2](../module-02-hello-window/) · **产出**: 渐变+棋盘格+圆形的动画窗口

## 目标

- 吃透 **像素格式**（XRGB8888 的内存布局）与 **stride**
- 手写图形 primitive（渐变/棋盘/圆），理解光栅化的本质
- 用最笨的动画循环暴露 buffer 管理问题（为模块 6 铺垫）

## 原理讲解

### 像素格式：显存里的一维字节数组

窗口是二维的，内存是一维的。`XRGB8888` 把每个像素编码为 4 字节，**行优先**排列：

```
地址:    0        4        8                    stride(行跨度)
      ┌────────┬────────┬────────┬─ ─ ─ ─ ─ ┐
y=0   │ B G R X│ B G R X│ B G R X│  ...     │  ← 一行 width×4 字节
y=1   │ B G R X│ B G R X│ B G R X│  ...     │  ← 紧跟其后
      └────────┴────────┴────────┴─ ─ ─ ─ ─ ┘
pixels[y * (stride/4) + x] = 一个像素 (注意除以4!)
```

- 内存字节序是小端 `B,G,R,X`，但写代码时按**逻辑位序** `(0xFF<<24)|(r<<16)|(g<<8)|b` 组装 32 位整数；
- `X` 是 padding，合成时被忽略；`ARGB8888` 才有真正的 alpha（模块 7 用到）；
- **stride ≥ width×4**，可能因硬件对齐更大——正确代码永远用 `wl_shm_buffer_get_stride()` 取（模块 11 服务端就会遇到 stride 不等的情况）。

### 手写光栅化：每个像素都要自己算

本模块的 `fill_pixels()`（[L27-58](main.c#L27-L58)）就是一台最小光栅化机器：

```c
for (y...) for (x...) {          // 遍历屏幕每个采样点
    float u = x/w, v = y/h;      // 归一化坐标
    r = 127 + 127*sinf(...);     // 渐变 = 连续函数采样
    if ((x/32) ^ (y/32)) ...     // 棋盘格 = 整数域周期函数
    dist = sqrt(...); if (<r)   // 圆 = 距离场判定
}
```

渐变、棋盘、圆分别对应**函数采样、区域判定、距离场**——这三招组合起来就是所有 2D 渲染的骨架。Cairo（模块 7）不过是把这些做成抗锯齿+向量化+硬件加速的工业版本：圆的边缘在这里是锯齿状的（硬判定），Cairo 会按覆盖率混合边缘像素。

### 动画 = 每帧一块新 buffer

本模块的动画循环（[L170-180](main.c#L170-L180)）故意用「每帧新建 buffer→attach→commit→销毁」的朴素方式：

```
帧N:   mkstemp→mmap→fill→pool→buffer→attach→commit→destroy
帧N+1: 再来一遍...
```

三宗罪，留待模块 6 修复：
1. **CPU 浪费**：每帧 mmap/munmap + 全量重绘；
2. **节奏失控**：`usleep(16000)` 盲猜 60fps，和显示器刷新率毫无同步（快了浪费、慢了卡顿、还会撕裂）；
3. **无 frame callback**：compositor 忙时照样疯狂提交。

> 但它有一个优点：**每帧独立 buffer**——这是 Wayland 推荐做法（buffer 复用需要处理 release 事件，模块 6 讲）。

## 代码走读

| 位置 | 内容 |
|------|------|
| [L27-58](main.c#L27-L58) | `fill_pixels()`：三 primitive 的采样实现（本模块灵魂） |
| [L60-85](main.c#L60-L85) | `create_frame()`：每帧 buffer 工厂（对照模块 2 的 create_buffer，多了 frame 参数） |
| [L170-180](main.c#L170-L180) | 朴素动画主循环：dispatch 处理输入事件 + 重绘提交 |

其余（registry/xdg 回调）与模块 2 相同——从本模块起这部分算「已掌握的样板」。

## 动手实验

```bash
cd module-03-pixels
make && ./pixels
```

### 观察点

1. **FPS 感知**：数一下渐变呼吸速度，再 `time ./pixels` 跑 5 秒退出（Esc 或关窗）对比——usleep 版本实际帧率受调度抖动影响很大；
2. **棋盘格锯齿**：盯着棋盘格边界看锯齿，然后想象 Cairo 会怎么处理（模块 7 见证）；
3. **性能 profiling**：`perf record -g ./pixels` 10 秒后 `perf report`——热点几乎全在 `fill_pixels`，这就是「CPU 软件渲染」的含义，也是 GPU 存在的理由（[docs/01 §2](../docs/01-gpu-and-display.md#2-cpu-vs-gpu为什么图形需要专门的处理器)）；
4. **改分辨率感知工作量**：把窗口拖到全屏，`perf` 里 fill_pixels 占比上升——像素数量线性放大 CPU 成本。

## 进阶练习

1. 画一条 **抗锯齿直线**（Bresenham + 距离场混合），和棋盘格的硬边对比；
2. 把棋盘格改成 16px 并加 `xdg_toplevel_configure` 响应 resize 后仍保持密度；
3. 实现两 buffer 轮换复用（处理 `wl_buffer.release` 后再复用），对比 mkstemp 调用次数（`strace -f -e trace=memfd_create,openat ./pixels 2>&1 | grep -c wayland-mod3`）；
4. 用 `damage_buffer` 只标脏矩形（代码已调用但全量），改为真正局部更新，观察 CPU 占用变化。

## 常见坑

| 现象 | 原因 |
|------|------|
| 画面斜着撕裂 | 就是 usleep 盲跑的后果——不同步 vblank（模块 6 的存在意义） |
| 圆变成椭圆 | 窗口非正方形时距离场没除以宽高比 |
| 颜色通道错乱 | 位序组装错（对照模块 2 的格式说明） |
| 窗口 resize 后花屏 | 用了旧尺寸的 width/height 计算 |

## 延伸阅读

- [FourCC 像素格式注册表](https://drmdb.eu/formats) — XRGB8888/ARGB8888 的权威定义
- [docs/01 §5.2](../docs/01-gpu-and-display.md#52-vblank-与-page-flip为什么会有撕裂) — 撕裂的硬件原理

下一模块: [模块 4 — 键盘与鼠标输入](../module-04-input/)
