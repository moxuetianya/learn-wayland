# 模块 9：DRM/KMS——绕过一切抽象，直接点亮屏幕

> 🗺️ **全景定位**: [docs/00 全景地图](../docs/00-big-picture.md) · [docs/01 显卡与显示](../docs/01-gpu-and-display.md) · [docs/05 KMS/EDID/链路协商](../docs/05-kms-edid-and-link.md)

> 服务端三部曲之二。模块 8 解决了"协议怎么说"，本模块解决"像素怎么上屏"：
> 不经 X11、不经 Wayland、不经任何合成器，直接对 `/dev/dri/card0` 下 ioctl，
> 五分钟内在屏幕上画出一块渐变色。模块 11 合成器的整条输出通路（找
> connector、选模式、dumb buffer、`drmModeSetCrtc`）就是本模块的代码。

## 0. 你将学到什么

- DRM 子系统在内核里的位置，`/dev/dri/card0` 与 `/dev/dri/renderD128`
  两类设备节点的区别（card node vs render node）
- KMS 把"点亮一台显示器"抽象成 **framebuffer → CRTC → encoder → connector**
  对象管线的原因与约束（`possible_crtcs` 位图）
- `drmModeGetResources` 一族的枚举模型：资源先拿 ID 数组、再逐个取详情
- dumb buffer 三部曲：`CREATE_DUMB` → `AddFB2` → `MAP_DUMB + mmap`，
    以及为什么 CPU 往 mmap 区域写一个像素，屏幕立刻就能看到
- **DRM master 排他性**：为什么本模块必须在纯 TTY、停掉图形会话后运行
- 纯 TTY 文字控制台的真实身份：fbcon 在内核里光栅化上屏，与 DRM 共用
    同一条扫描通路（1.6）

## 1. 背景原理

### 1.1 DRM 是什么：内核里的显卡总管

DRM（Direct Rendering Manager）是内核的显卡子系统，用户态一切图形栈的
最底层：OpenGL/Vulkan 驱动靠它提交 GPU 命令，合成器靠它做模式设置
（Mode Setting，即 KMS）。它通过字符设备暴露接口：

| 设备节点 | 权限 | 用途 |
|----------|------|------|
| `/dev/dri/card0`（card node） | root 或 master 授权 | **全功能**：模式设置、页翻转、vblank 等"影响显示输出"的特权操作 |
| `/dev/dri/renderD128`（render node） | 普通用户可用 | 只能渲染（GPU 计算/纹理），**不能**碰显示输出 |

这个拆分（render node 由此得名）让无特权的 GPU 计算（如 OpenCL）不必
授予"能改你屏幕"的权力。本模块要点亮屏幕，必须走 card node。

### 1.2 KMS 对象管线：点亮一台显示器需要四个对象

内核把显示硬件抽象成一条流水线，任何"上屏"都是把这条管线接通：

```
 ┌──────────────┐   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐
 │ framebuffer  │──→│     CRTC     │──→│   encoder    │──→│  connector   │──→ 显示器
 ├──────────────┤   ├──────────────┤   ├──────────────┤   ├──────────────┤
 │ 像素从哪读    │   │ 何时/如何     │   │ 转成什么接口  │   │ 物理端口      │
 │ (一块显存)    │   │ 逐行扫描      │   │ 信号(TMDS/   │   │ (HDMI/DP/    │
 │              │   │ (模式时钟)    │   │ LVDS/…)      │   │ eDP/…)       │
 └──────────────┘   └──────────────┘   └──────────────┘   └──────────────┘
   数量任意，可复用   通常 1~6 个         每个端口若干个      与物理插口对应
```

- **framebuffer (FB)**：一块"按什么格式、多宽的 pitch"供扫描的像素存储。
  本模块用最朴素的 *dumb buffer*（见 1.4）；真实合成器用 GBM/EGL 分配的
  GPU buffer，但对 KMS 来说都只是一个 fb_id。
- **CRTC**（CRT controller，显像管时代的名字）：真正干活的扫描引擎，按
  模式时钟逐行读 FB、产生时序。一台显示器 60Hz 就是 CRTC 每秒把 FB 完整
  读 60 遍——这也是"CPU 写入即可见"的物理原因。
- **encoder**：把 CRTC 的数字输出翻译成具体接口的信号格式。一个 connector
  可能对应多个可选 encoder（HDMI 端口既能走 TMDS 也能走 DP-alt-mode）。
- **connector**：物理端口，携带热插拔状态和显示器 EDID 解析出的模式列表。

**约束：不是任意 CRTC 都能接任意 connector。** SoC 内部的显示路由是有限
的多路复用，内核把它表达成每个 encoder 的 `possible_crtcs` 位图（第 i 位
对应 `resources->crtcs[i]`）。代码里 `find_crtc()` 两步走：先复用 connector
当前 encoder 已绑定的 CRTC（BIOS/console 已验证过的最保守路由），不行再
按位图挑合法组合。

### 1.3 枚举模型：先拿 ID，再取详情

KMS 的 API 是两层结构：`drmModeGetResources()` 只返回四组**对象 ID 数组**
（CRTCs/connectors/encoders/fbs），要看某个对象的详情需再按 ID 调
`drmModeGetConnector()` 等，用完 `drmModeFree*` 归还。connector 详情里
最有两个字段：`connection`（`DRM_MODE_CONNECTED` = 插着线且读到 EDID）与
`modes[]`（显示器自己声明的模式表，`DRM_MODE_TYPE_PREFERRED` 标注推荐项，
通常是原生分辨率）。这种"句柄式"设计避免了巨型结构体，也是一切 KMS
工具（`drm_info`、`modetest`）的输出来源。

### 1.4 dumb buffer 三部曲

"dumb" 指不需要任何 GPU 知识：内核替你选一块**线性的、CPU 缓存一致的**
、可用于扫描的内存。三步缺一不可：

```
 ① DRM_IOCTL_MODE_CREATE_Dumb         ② drmModeAddFB2               ③ MAP_DUMB + mmap
 ┌───────────────┐                ┌────────────────┐            ┌──────────────────┐
 │ "给我 w×h×32" │                │ handle+pitch+  │            │ "给我这块的偏移"  │
 │ 内核分配内存   │──handle/pitch/─▶│ fourcc 格式    │─fb_id      │ 拿到伪 offset，   │
 │               │   size         │ 包装成 KMS 对象 │            │ mmap 到用户空间   │
 └───────────────┘                └────────────────┘            └──────────────────┘
```

为什么后两步必须存在：GEM buffer 不是文件，不能直接 `open`+`mmap`——
`MAP_DUMB` 返回的 offset 只在**对 DRM fd 本身做 mmap** 时有意义；而裸内存
也不能上屏——只有 fb_id 才能喂给 CRTC，AddFB2 同时登记了像素格式
（`DRM_FORMAT_XRGB8888`）和每行 pitch，驱动才知道怎么扫。pitch 由内核定，
可能大于 `width*4`（行对齐），所以填充像素必须按 stride 走行——`fill_buffer()`
里那个看起来多余的 `stride` 参数是 KMS 编程的第一课。

这句"第一课"背后是一对容易混淆的概念——**屏幕 stride 与 buffer stride
可能不同**：

- **屏幕 stride**：图像显示宽度对应的"逻辑"跨度，= 宽度 × 每像素字节数。
  本模块 XRGB8888 下即 `width * 4`。
- **buffer stride**：内存中从一行开头到下一行开头的"物理"字节距离，即
  `CREATE_DUMB` 返回的 pitch（此语境下 pitch 与 stride 同义）。它包含
  行尾填充，可以大于屏幕 stride。

不相等的根源是**内存对齐与硬件限制**：GPU、显示控制器通常要求每行数据
的起始地址或字节数满足特定对齐规则（16 字节、32 像素、256 字节等，因
硬件而异），驱动就在每行末尾垫上填充字节（padding）。例如一个 533 像素
宽的 RGB565 图像，屏幕 stride = 533×2 = 1066 字节，实际 buffer stride
却可能是 544×2 = 1088 字节——多出的 11 个像素（22 字节）就是填充：

```
 ┌──────────────────────────────────┬─────────────┐
 │         533 个有效像素            │ 11 像素填充 │
 │          （1066 字节）            │ （22 字节） │
 └──────────────────────────────────┴─────────────┘
 ←────────── 屏幕 stride ──────────→
 ←───────────── buffer stride（pitch = 1088 字节）─────────────→
```

程序若按屏幕宽度走行（`width*4`），从第二行起行首逐行错位，图像便斜着
拖出"平行四边形"或花屏——第 4 节坑表与练习 3 的"stride 实验室"就是它。
这个坑远不止 dumb buffer 有：微软文档明确指出**两个尺寸相同的视频帧
缓冲区可以有完全不同的 stride**，处理视频图像必须把 stride 纳入考虑；
Linux 内核也修过同类 bug——驱动计算映射 stride 时误用了 framebuffer
宽度而非用户指定的 stride，导致硬件读错数据位置甚至触发缺页错误。

一句话理解：stride 是一行数据在内存中实际占用的"车位宽度"，屏幕显示
只用其中"有效宽度"那段，但对齐要求每行停够整数个"车位"，多出的空位
就是 padding。定位每行起始地址永远以 buffer 实际返回的 pitch 为准——
`fill_buffer()` 里 `ptr + y * stride` 走的就是这个值。

### 1.5 DRM master：为什么必须 TTY + 停图形会话

模式设置会**改变所有人看到的屏幕**，所以内核把它设为特权操作：对 card
node 调用 `drmSetMaster()` 成为 master 后才允许 `drmModeSetCrtc` 等，而
**同一时刻一个 DRM 设备只能有一个 master**。GNOME 会话在跑时，gnome-shell
就是现任 master；此时本模块的 `drmSetMaster()` 会失败（代码打了警告继续跑，
后面的 SetCrtc 才真正被拒）。所以运行前要 `sudo systemctl stop gdm`，放倒
`gdm → gnome-session → gnome-shell` 整条链、释放 master 和图形 VT——这与
模块 11 完全相同。root 直接 `drmSetMaster` 是学习项目的偷懒；正规姿势是
普通用户经 logind 的 `TakeDevice` 授权成为 master（gnome-shell、wlroots
都这么干）。退出时 `drmDropMaster` 归还身份。

> `TakeDevice` 如何把 fd 交出来、切 VT 时如何收回（revoke / drop master）
> 以及 seat 与 VT 的关系，见
> [docs/06 会话设备管理](../docs/06-seat-devices-and-revoke.md)。

### 1.6 彩蛋：纯 TTY 的文字界面是谁画的

`systemctl stop gdm` 后登录的文字 TTY 并非"屏幕天生的样子"——那些字符
同样是像素，同样有人往 scanout buffer 里写。两个时代两种画法：

- **VGA 文本模式（老 PC）**：显卡进 text mode，VRAM 里存的不是像素而是
  "字符码 + 颜色属性"（每字符 2 字节），字形点阵烧在显卡 ROM 里由硬件
  字符发生器实时合成——内核的 `vgacon` 只往 VRAM 写字符码，从不碰像素。
- **UEFI/KMS 机器（本模块环境）**：固件 GOP 与 DRM 驱动早已把屏幕置于
  图形模式，文字得内核自己**光栅化**成像素：

```
VT 子系统 (tty1~ttyN：键盘、转义序列、切屏)
   ↓
fbcon（用内核内置点阵字体把字符流渲染成像素；光标、滚动也归它管）
   ↓ fbdev 抽象层
DRM 驱动的 fbdev emulation（simpledrm / i915 / amdgpu…）
   ↓
scanout buffer——与 drm-demo 的 dumb buffer 同一条扫描通路
```

**光栅化（rasterization）＝把"形状的抽象描述"换算成"像素网格上该点亮
哪些点"**。屏幕是离散像素阵列（raster），而字符 `A` 在内存里只是一个
字节 `0x41`；中间必须有人按字体查表，把字形位图逐位铺进 framebuffer：

```
'A'(0x41) ──查字体表──▶ 字形位图（1=填前景色, 0=填背景色）──逐位写入──▶ 像素

 00011000
 00111100
 01100110
 01111110
 01100110
 01100110
```

fbcon 用的是编译进内核的点阵字体（VGA 8x16 一类），查表即得、按位
拷贝，零开销但丑：无抗锯齿、字号死板、连中文都显示不了。桌面矢量字体
（TrueType/OTF）存的则是贝塞尔曲线轮廓，得靠用户态的 FreeType 运行时
计算轮廓盖住了哪些像素——那是用户态库的活，内核不干。用户态替代方案
（kmscon）曾出现过，已废弃，所以今天默认仍是 fbcon。

这段链路顺带解释了本模块的三个细节：

- 退出前恢复 CRTC，本质是把 scanout 交还给 fbcon 的 console fb——
  不恢复就是坑表里的"退出后 TTY 黑屏/花屏"；
- fbcon 在内核里直接走 fbdev，**不经过 DRM master 机制**——master
  排他约束的是用户态（gnome-shell 与 drm-demo 之争），所以文字控制台
  从来不需要"当 master"；
- `Ctrl+Alt+F3` 能在图形/文字间来回切换，就是 VT 切换时内核在图形
  客户端与 fbcon 之间交接 scanout 的结果（交接协议见 docs/06）。

## 2. 代码走读

单文件 `main.c`，按 `main()` 执行顺序：

1. **打开设备 + 抢 master**：`open("/dev/dri/card0")` 后 `drmSetMaster()`。
   注意失败只警告不退出——枚举类 ioctl 不需要 master，能让程序先打印
   出资源情况再死在 SetCrtc，方便排查。
2. **枚举**：`drmModeGetResources()` → 遍历 connector，找第一个
   `DRM_MODE_CONNECTED && count_modes > 0` 的（有热插拔信号但没有 EDID
   模式的端口是编不了任何东西的）。
3. **选模式**：优先 `DRM_MODE_TYPE_PREFERRED`（显示器 EDID 标注的原生
   推荐），否则退而求第一个。
4. **`find_crtc()`**：见 1.2——先复用现成绑定，再查 `possible_crtcs`。
5. **保存现场**：`drmModeGetCrtc()` 存下当前 CRTC 配置。退出时恢复它，
   TTY 文字控制台才不会黑屏/花屏——这是对系统最基本的礼貌。
6. **`create_dumb_fb()`**：1.4 的三部曲，失败路径逐步回滚（RmFB →
   DESTROY_DUMB）。
7. **`fill_buffer()` + `drmModeSetCrtc()`**：填充渐变图案，一条 ioctl 把
   "这个 CRTC、从这个 FB 读、用这个模式、扫到这个 connector"一次接通：

```c
if (drmModeSetCrtc(fd, crtc_id, fb.fb_id, 0, 0, &conn_id, 1, mode) < 0) {
```

此后屏幕扫描的就是我们的 dumb buffer——`fill_buffer` 写入的每个像素最迟
在下一遍扫描（≤1/60 秒）出现在屏幕上。注意这里**没有 vsync 同步**：写入
撞上扫描位置会撕裂。生产合成器用双缓冲 + `drmModePageFlip` 在 vblank 时刻
换帧（见第 5 节）。

## 3. 编译与运行

前置：真实机器或 VM 的**纯 TTY**，图形会话已停（原因见 1.5）。依赖
`libdrm-dev`（`sudo apt install libdrm-dev`；或一次跑 `bash setup-vm.sh`
装齐全部模块依赖）。

```bash
# 1. 切到 tty3（物理机/VM 通常 Ctrl+Alt+F3），登录后停掉图形会话
sudo systemctl stop gdm        # 或 sddm / lightdm，取决于发行版

# 2. 编译并运行
cd module-09-drm
make
sudo ./drm-demo
```

成功时的输出大致如下（**数值因机器而异**，这是在一台 1080p 机器上的示例）：

```
=== 模块 9: DRM/KMS 直接屏幕控制 ===
从 TTY 运行: Ctrl+Alt+F3 登录后执行 sudo ./drm-demo

资源: CRTCs=3, connectors=5, encoders=6, fbs=0
已连接的 connector id=61，模式数=27
选中的模式: "1920x1080" 1920x1080 @ 60Hz
选中的 CRTC id=76
dumb buffer: fb_id=68 handle=3 pitch=7680 size=8294400
已显示测试图案，5 秒后恢复...
```

同时屏幕显示出"红渐变横向 × 绿渐变纵向"的测试图案，5 秒后程序恢复 CRTC
并退出，回到 TTY 文字界面。看完把图形会话拉回来：

```bash
sudo systemctl start gdm
```

旁路验证（不开图形界面也能玩 KMS 枚举）：装上 `libdrm-tests` 后，用**本机
驱动名**点开设备，打印与本程序对应的 connector/CRTC 全表，用来对照自己程序
的枚举结果：

```bash
drv=$(sudo awk '{print $1}' /sys/kernel/debug/dri/0/name)   # qxl / virtio_gpu / i915 …
sudo modetest -M "$drv" -c
```

为什么不直接 `sudo modetest -c`：老版本 libdrm 在不带 `-M/-D` 时**不会扫描
`/dev/dri`**，而是拿一份硬编码的驱动名清单（i915/amdgpu/…/simpledrm）逐个
尝试，`qxl`、`bochs`、`cirrus`、`vboxvideo`、`nvidia-drm`、`xe`、`vkms` 都不在
清单里，于是刷一屏 `trying to open device '…'...failed` + `no device found`。
而且这些版本里 `-D` 是 `drmOpen()` 的 busid，**不是设备路径**（2.4.123 之后
才是），所以 `-D /dev/dri/card0` 同样无效。

### 3.1 旁路工具：`drm-info`

同目录还有一个**只读**工具 `drm-info`（`make` 会一起构建），把本模块用到的
`drmModeGetResources()` 那一族 API 的全部结果摊开：connector → encoder →
CRTC → fb 的绑定、每个 connector 的模式表、EDID blob 解码、属性表。

```bash
make
sudo ./drm-info                 # 自动探测 /dev/dri/card0…card9
sudo ./drm-info -c 37           # 只看一个 connector
./drm-info -e /sys/class/drm/card0-DP-1/edid   # 只解码 EDID，不需要 root
```

qxl VM 上的真实输出（节选）：

```
=== /dev/dri/card0 ===
驱动   qxl 0.1.0 (20120117) — RH QXL
资源   CRTC×4  connector×4  encoder×4  fb×0

[37] Virtual-1  connected  size=0x0mm  modes=35  encoders=1
      encoder 38 (possible_crtcs=0x1) -> crtc 36 (fb 54, 1024x768@60.00Hz)
      #0  1024x768  60.00Hz  65000kHz  1024x768  totals 1344x806  nhsync,nvsync  preferred,driver
      ...
      EDID: 无此属性（VIRTUAL/WRITEBACK 类 connector 默认不挂）
      属性:
      DPMS                 = On(0)
      link-status          = Good(0)
      non-desktop          = 0  [只读]

CRTC:
  [36] fb 54  1024x768@60.00Hz  pos=(0,0)  已启用
  [41] 未启用（fb 0）
```

有真 EDID 的机器（virtio-gpu / 真显卡）会多出一段解码：

```
      EDID:
      EDID connector 属性（256 字节）
        厂商 RHT  产品 0x1234  序列 0x00000000  生产 2014-W42  EDID 1.4  2 块
        DTD1  1280x800@74.99Hz  clock=107.30MHz  htot=1728 vtot=828  hfp=320 hsw=38  ...
        size=325x203mm  nhsync,nvsync
        描述符3 支持范围 50-125Hz, 行频 30-160kHz, 最大时钟 2550MHz
        描述符4 显示器名 "QEMU Monitor"
        CEA-861 rev3: 1 个数据块, 6 个 VIC
```

和 `modetest` 的分工：`modetest` 是 libdrm 的通用瑞士军刀（能改模式、关屏）；
`drm-info` 是本仓库的教学版，多打印管线绑定和 EDID 解码，少了一堆暂时用不到
的选项。两者字段的硬件含义见 [docs/05](../docs/05-kms-edid-and-link.md)。

## 4. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| `Package libdrm was not found in the pkg-config search path` + `fatal error: xf86drm.h` | 缺 `libdrm-dev`。典型报错链：pkg-config 找不到 `libdrm.pc` → Makefile 的 `$(shell pkg-config ...)` 展开为**空** → 编译器没有任何 `-I` 路径 → 头文件找不到 | `sudo apt install libdrm-dev`；一次装齐所有模块依赖用 `bash setup-vm.sh` |
| `drmSetMaster` 警告 + `drmModeSetCrtc: Permission denied` | 图形会话仍是 DRM master | 确认 `sudo systemctl stop gdm`；`pgrep -a gnome-shell` 应为空 |
| `open /dev/dri/card0: Permission denied` | 非 root 且不在 video 组 | `sudo` 运行（学习项目）；render node 无此问题但也做不了模式设置 |
| `找不到已连接且带有效模式的 connector` | VM 显卡未暴露模式/显示器未直通 | 检查 VM 显示设置（QEMU 建议 virtio-gpu）；用 `modetest -M <驱动名> -c` 对照 |
| `modetest -c` 刷一屏 `trying to open device '…'...failed` + `no device found` | 不带 `-M/-D` 时 modetest 只按**内置驱动名清单**探测，`qxl`/`bochs`/`cirrus`/`vboxvideo`/`nvidia-drm`/`xe`/`vkms` 都不在清单里；且 libdrm ≤2.4.11x 的 `-D` 是 busid 而非设备路径 | `sudo modetest -M "$(sudo awk '{print $1}' /sys/kernel/debug/dri/0/name)" -c`；libdrm ≥2.4.123 可用 `-D /dev/dri/card0` |
| 图案整体斜着拖出"平行四边形" | 填像素时按 `width*4` 而不是 pitch 走行 | 一律用返回的 `pitch/stride`（见 `fill_buffer`，原理见 1.4） |
| 颜色不对（红蓝互换之类） | fourcc 格式与填充顺序不匹配 | `DRM_FORMAT_XRGB8888` 低字节是 X、随后 R；对照 fourcc 定义 |
| 退出后 TTY 黑屏/花屏 | 没恢复 CRTC 就销毁了 FB | 本模块已 `drmModeGetCrtc` 保存并恢复；玩脱了就切 VT 或 `start gdm`；TTY 由谁画出来见 1.6 |
| 在 SSH 里跑，一切成功但"屏幕没反应" | 你看的物理屏与程序驱动的 connector 不是同一个 | 用 `modetest` 核对 connector 编号，多屏机器上选对目标 |

## 5. 与真实实现的差距 / 深入话题

本模块画的是**被 CRTC 正在扫描的单缓冲**：简单、立即可见，但没有 vsync
同步（撕裂），也把"渲染"和"扫描"绑死在同一块内存上。生产合成器
（mutter / wlroots / Weston）的输出栈是：GBM 从显存分配双/三缓冲，GPU
合成完一帧后 `drmModePageFlip` 提交换帧请求，内核**等到下一个 vblank** 才
原子地把 CRTC 的 FB 指针换成新帧，并通过事件通知合成器"旧帧已不再扫描、
可以复用"——这正是 Wayland frame callback 与 `wl_buffer.release` 语义的
硬件落点（模块 6 客户端视角学过的机制，在服务端就这么实现）。

更现代的路线是 **atomic API**（`drmModeAtomicCommit`）：把 FB、模式、平面
位置、缩放等几十项属性打包成一个"要么全成功要么全不生效"的事务提交，
还能先 `DRM_MODE_ATOMIC_TEST_ONLY` 干跑验证。此外真实硬件还有 overlay
plane / sprite（让视频帧不进合成队列直接叠上屏幕）、多 CRTC 多显示器
独立模式、fence 与显存同步等话题。本模块的 legacy `drmModeSetCrtc` 路线
没有消失——模块 11 就原样复用它，先跑通再谈优雅。

继续做模块 11/12 要注意 **render node**：GBM/EGL 需要
`/dev/dri/renderD128`，而 qxl 这类纯 2D 虚拟显卡只暴露 `card0`。如果你在
qxl 的 VM 里看到 `gbm_create_device` / `eglInitialize` 失败，先换成
virtio-gpu（QEMU 开 `gl=on`，即 virgl/venus）再排查代码。

## 6. 练习

1. **验证"扫描输出"**：把 `fill_buffer` 的蓝色分量从 128 改成按 `(x+y)`
   变化，重编译运行——确认每处颜色变化都来自你写的那个像素循环。
2. **模式清单**：把 connector 的 `modes[]` 全部打印（名称、分辨率、刷新率、
   是否 PREFERRED），再故意选一个非 PREFERRED 的模式（如 1280x720）跑
   `drmModeSetCrtc`，观察显示器重新同步。
3. **stride 实验室**：故意把 `fill_buffer` 里的 `stride` 换成 `w * 4` 编译
   运行（如果你的机器 pitch == w*4 就借 `modetest` 找台不对齐的机器），
   亲眼看看"斜掉的图"，理解行对齐。
4. **多屏**：如果机器有两个以上 connected connector，把第二个也点亮并
   显示不同图案（各配一个 CRTC）——这就是多显示器合成器的雏形。
5. **观察 master 排他**：不停 gdm，先在图形会话里 `sudo ./drm-demo`，
   阅读警告内容并确认它死在哪一步；再对照 1.5 解释为什么。
6. **对照枚举**：用 `sudo ./drm-info` 与自己程序的输出逐项对比（connector id、
   模式数、encoder/CRTC 绑定），再用 `drm-info -e` 解码 `/sys/class/drm/*/edid`，
   确认「模式表不是显示器能力，而是 EDID ∩ 驱动限制」。

## 7. 延伸阅读

- [docs/05 KMS 对象、EDID 与链路协商](../docs/05-kms-edid-and-link.md) —
  本模块背后的硬件原理：时序参数、EDID 字节结构、DP/HDMI 各自怎么协商
- [DRM/KMS 内核文档（Mode Setting 章节）](https://docs.kernel.org/gpu/drm-kms.html) —
  dumb buffer、KMS 对象、master 语义的权威出处
- [内核 fbcon 文档](https://docs.kernel.org/fb/fbcon.html) —
  1.6 的权威出处：文字控制台如何渲染、如何与 VT 交接
- [libdrm 仓库](https://gitlab.freedesktop.org/mesa/drm) — 本模块用的
  `xf86drm.h` / `xf86drmMode.h` 与 `modetest` 示例工具
- [kmscube](https://gitlab.freedesktop.org/mesa/kmscube) — 在本模块基础上
  加 GBM/EGL 用 GPU 渲染上屏的下一步示例
- [tinywl](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/tinywl) —
  模块 11/12 的"工业版近亲"，输出通路与本模块几乎一致但带 page flip
