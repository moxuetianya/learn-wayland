# 模块 3：手动像素缓冲区与图形绘制

> 模块 2 的渐变是"一次性填好"的；本模块把像素当成一等公民逐帧操纵：
> 不用任何绘图库，直接往 `wl_shm` buffer 的内存里逐像素写值——
> 正弦渐变背景 + 棋盘格 + 中心圆，每帧全量重算，连成动画。
> 顺手建立"像素格式、stride、软件渲染"的底层世界观。

## 0. 你将学到什么

- `WL_SHM_FORMAT_XRGB8888` 的**内存字节序**：小端机器上为什么是 B,G,R,X，
  与 `0xFF000000 | r<<16 | g<<8 | b` 的对应关系
- **stride（行距）与对齐**：stride 是什么、为什么可以大于 `width*4`、
  寻址通式 `base + y*stride + x*4` 及 stride 弄错的经典"斜图"症状
- 逐像素图形算法要点：**归一化坐标 + 正弦渐变**、棋盘格的**异或奇偶**、
  圆的**距离判断**，以及整数除法这个高频坑
- **连续重绘动画**的最朴素实现：每帧新建 pool/buffer → attach →
  damage → commit，`usleep` 压节奏及其与 vsync 的差距

## 1. 背景：像素格式与软件渲染

### 1.1 XRGB8888：4 字节一个像素，内存里是 B,G,R,X

wl_shm 的像素格式沿用 Linux 内核的 **fourcc** 编码，名字从左到右描述
**字节在内存中的顺序（小端）**，而字母含义是"该字节装什么通道"：

```
WL_SHM_FORMAT_XRGB8888 —— 每像素 32 位

  作为 uint32（写代码时的视角）:
  ┌────────┬────────┬────────┬────────┐
  │ 31..24 │ 23..16 │ 15..8  │  7..0  │
  │ X=0xFF │   R    │   G    │   B    │     0xFF<<24 | r<<16 | g<<8 | b
  └────────┴────────┴────────┴────────┘

  内存字节序（小端机器, 硬件/合成器读的视角）:
  offset:   +0      +1      +2      +3
  ┌────────┬────────┬────────┬────────┐
  │   B    │   G    │   R    │   X    │     ← "XRGB" 的字面意思
  └────────┴────────┴────────┴────────┘
```

- **X = 不用的填充字节**。XRGB8888 不透明，合成器忽略 X；要透明度就用
  `ARGB8888`（X 的位置变成真正的 alpha），本课程模块 7 的 Cairo 默认用它。
- 为什么"名字按字节序、代码按位序"？因为 fourcc 描述的是**线性内存**，
  而 C 里我们习惯把 4 个字节合成一个 `uint32_t` 再赋值——小端机器上
  低位字节落在低地址，于是字面 XRGB 与位序 XRGB 相同，但**大端机**上
  两者互换。x86/ARM 桌面全是小端，课程内可以放心。
- 同族格式还有 `XBGR8888/ARGB8888`（R、B 位置对调，RGB"通道序"相反）。
  症状速查：颜色"红蓝互换"或"惨绿"→ 十有八九格式与位打包不匹配。

### 1.2 stride：一行的字节数，不只是 width*4

**stride（行距）= 从某行首像素到下一行同列像素的字节数。** 它由创建
buffer 的人声明、合成器照此解释，允许大于 `width*4`：

```
通用寻址:  pixel(x, y) = *(uint32_t *)((uint8_t *)base + y*stride + x*4)

本模块 stride == width*4, 于是退化成最顺手的写法:

  uint32_t *pixels → pixels[y * width + x]
```

为什么会有对齐填充？GPU/显示控制器按行突发读取，行首对齐到 16/64/
256 字节能显著提速；某些格式每像素不足 1 字节（如 C8）也靠 stride
对齐。所以"看见别人代码里 `stride/4` 不要惊讶——那才是通用写法"。

stride 弄错的长相非常典型：**图像整体斜着撕裂**——每行起点偏移固定
字节数，行越多偏移越大，画面像被推倒的多米诺。

### 1.3 逐像素算法：三个图案的数学

`fill_pixels()` 每帧对全部 `w*h` 个像素重算一次颜色，图案全靠数学：

1. **归一化先行**：`u=(float)x/w, v=(float)y/h` 把坐标压到 0..1，
   图案比例与分辨率解耦。**必须先转 float 再除**——`x/w` 是整数除法，
   恒为 0，渐变瞬间塌成纯色（编译器不报错的高频坑）。
2. **正弦渐变**：`127 + 127*sin(...)` 把 [-1,1] 映射到 [0,255]；
   三个通道用不同频率/相位（含随帧推进的 `t`），得到平滑流动的彩色
   波——比线性插值的好处是天然周期、无带状边界。
3. **棋盘格**：`(x/32) ^ (y/32)` 的结果按位看奇偶，整数除法先分格、
   异或让"同行同列奇偶相同"的格同色。棋盘格是检验缩放/对齐/alpha
   的标准参考图案（GNOME 截图工具、合成器调试都在用）。
4. **圆**：到中心 `(0.5,0.5)` 的归一化距离 `sqrt(dx²+dy²) < 0.2`。
   "对每个像素问'你在不在圆里'"——软件渲染的暴力美学，代价是
   像素数×每像素浮点运算，分辨率翻倍开销翻四倍。

### 1.4 连续重绘：最朴素的动画循环

```
while (running && dispatch() != -1)     ① 先处理积压事件(resize/close)
    buf = create_frame(...)             ② 每帧新建匿名文件+pool+buffer
    attach(buf)                         ③ 挂 pending
    damage_buffer(整块)                  ④ 声明"全图都变了"
    commit()                            ⑤ 生效
    destroy(buf); usleep(16000)         ⑥ ~60fps 节拍
```

三个值得记住的设计选择与代价：

- **每帧新 buffer**：彻底绕开"上一块 buffer 是否已被合成器放手"的
  状态跟踪（buffer 提交后写权归合成器，直到 `wl_buffer.release`）。
  代价是每帧一次临时文件创建 + mmap + 全量像素写入，60fps 下 640×480
  就是每秒 ~74MB 的分配与拷贝。生产客户端用 2~3 块 buffer 轮转，等
  release 再复用。
- **damage_buffer**：告诉合成器"这块矩形我改过"，它只需重合成这块。
  报整块是保守但正确的起步姿势。
- **usleep(16000) 是拍脑袋的 60fps**：与屏幕刷新率毫无对齐——可能
  在扫描中段提交造成撕裂，也可能合成器一帧内收到两次提交白白丢弃
  一帧。正统解法是 frame callback（模块 6 的主题）。

## 2. 代码走读

### 2.1 fill_pixels()：一个像素的诞生

```c
float u = (float)x / w;              /* 先转 float! 整数除是新手坟场 */
...
uint8_t r = (uint8_t)(127 + 127 * sinf(u * 6.28f + t));
int cx = (x / 32) ^ (y / 32);        /* 整数除法在这里才是故意的 */
if (cx) { r *= 0.7f; ... }           /* 棋盘格暗格: 乘法比减法柔和 */
...
pixels[y * w + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
```

注意三处除法的双重身份：`x/w` 要 float（归一化），`x/32` 要整数
（分格），同文件两种意图并存，写错方向都不会有编译警告。

### 2.2 create_frame()：buffer 的流水线

与模块 2 的 `create_buffer` 相同的 mkstemp→unlink→ftruncate→mmap→
create_pool→create_buffer→立刻销毁 pool 流水线（1.3 节模块 2 README
详解过引用关系），差别只在每帧调用一次、`fill_pixels` 之后再 munmap。

### 2.3 main()：动画主循环

窗口搭建部分与模块 2 完全一致（bind 三个 global、角色链、空提交换
configure），不再重复。新的部分就是 1.4 图中的循环体——`dispatch`
在前、渲染在后：先把窗口事件（尺寸变化、关闭）处理完，再按最新
`st.width/height` 画下一帧，避免用过期尺寸作画。

## 3. 编译与运行

前置：桌面 Wayland 会话内的终端。Makefile 会用 `wayland-scanner` 自动生成
xdg-shell 协议胶水（client-header + private-code）并一并编译链接：

```bash
cd module-03-pixels
make
./pixel-anim
```

预期输出（窗口弹出前）与画面：

```
=== 模块 3: 手动像素绘制 ===
理解: WL_SHM_FORMAT_XRGB8888 = 每像素 4 字节 (B,G,R,A)
      像素数据通过共享内存 (wl_shm_pool) 传递给 compositor.

动画循环开始 (每帧使用新 buffer, 演示 Attach-Commit 模型)...
```

窗口内是持续流动的彩色正弦渐变，叠着 32 像素一格的暗色棋盘格，
中央一个白色实心圆。拉伸窗口：图案按新尺寸重算（这部分代码处理了
尺寸变化——每帧都按 `st.width/height` 新建 buffer）。关闭窗口或
Ctrl+C 退出，打印 `=== 模块 3 完成 ===`。

## 4. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| 颜色红蓝互换/惨绿 | 位打包与格式不匹配（XRGB vs XBGR） | 核对 `(r<<16)\|(g<<8)\|b` 与格式名的字节序（1.1 图） |
| 图像整体斜着撕裂 | stride 与实际寻址不一致 | 用通式 `base + y*stride + x*4`；别假设 stride 恒等于 w*4 |
| 渐变塌成纯色/生硬分层 | `x/w` 整数除法恒为 0 | 先 `(float)x` 再除 |
| 窗口不刷新 | 忘了 `wl_surface_damage_buffer`，合成器按需重绘直接跳过 | commit 前上报变化区域 |
| 帧率远低/远高于预期 | usleep 节拍与合成器实际节奏脱节（1.4） | 模块 6 换 frame callback |
| 链接失败 `undefined reference to xdg_*` | Makefile 未编入 wayland-scanner 生成的协议实现（本仓库 Makefile 已含正确规则） | 确认 `wayland-scanner` 已安装后重跑 `make`；规则写法可对照 module-02 的 Makefile |
| CPU 占用刺高 | 每帧全量重算 + 每帧新建 pool | 只重算 damage 区域；改用 buffer 池轮转 |

## 5. 与真实实现的差距 / 深入话题

本模块是"裸像素"的教学极限，真实渲染栈至少有四级台阶：**缓冲池**
（2~3 块 buffer 轮转，配合 `wl_buffer.release`，weston-simple-shm 可当
参考实现）；**成熟 2D 库**（Cairo/Skia 直接画在 shm buffer 上，模块 7
的 Cairo 就是把本模块的 `fill_pixels` 换成抗锯齿的矢量绘制）；
**硬件渲染**（EGL 在 GPU 上渲染，经 linux-dmabuf 或 EGLStream 提交，
零拷贝、无 CPU 像素搬运）；**帧调度**（frame callback + presentation
反馈，模块 6）。另一个现代细节：`mkstemp+unlink` 的正确替代是
`memfd_create()`——一步得到匿名 fd，还带 sealing（F_SEAL_*）防
"客户端建池后偷偷缩文件坑服务端"的攻击面。

## 6. 练习

1. **斜图实验**：把 `create_frame` 里传给 `create_buffer` 的 stride 改成
   `width*4 + 64`（像素照 `y*w+x` 写），重跑看"多米诺"斜图；再用通式
   `y*(stride/4)+x` 修正。理解 stride 是协议参数而非派生值。
2. **画线**：实现 Bresenham 直线，在渐变背景上画一条从 (50,50) 到
   (600,400) 的白线——整数增量、无浮点无除法，体会 1960 年代的算法美学。
3. **透明度**：格式换 `WL_SHM_FORMAT_ARGB8888`，把棋盘格暗格的 alpha
   从 0xFF 改 0x40，观察合成器对半透明 buffer 的混合效果。
4. **帧率计**：用 `clock_gettime(CLOCK_MONOTONIC)` 每 60 帧打印一次
   实际 fps；再对比 `WAYLAND_DEBUG=1` 下的协议流量，思考差距来自哪。
5. **局部重绘**：让圆心随时间做圆周运动，damage 只报圆的包围盒，
   用 `WAYLAND_DEBUG=1` 验证 damage 矩形确实变小了。

## 7. 延伸阅读

- [drm_fourcc.h](https://github.com/torvalds/linux/blob/master/include/uapi/drm/drm_fourcc.h) — 格式命名与字节序的权威定义
- [wl_shm 协议定义](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_shm) — pool/buffer 与格式列表
- [wl_surface.damage_buffer](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_surface) — damage 的语义
- [memfd_create(2)](https://man7.org/linux/man-pages/man2/memfd_create.2.html) — 匿名共享内存的现代做法
- [weston-simple-shm 源码](https://gitlab.freedesktop.org/wayland/weston/-/blob/clients/simple-shm.c) — buffer 池轮转的参考实现
