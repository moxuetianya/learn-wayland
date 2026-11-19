# 05 — KMS 对象、EDID 与链路协商：屏幕是怎么被点亮的

> 对应 [模块 9](../module-09-drm/)。模块 9 讲「**怎么调用**」（dumb buffer →
> `drmModeSetCrtc`），本篇讲「**为什么是这些调用**」以及调用之下硬件谈了什么：
> CRTC 在产生什么、EDID 在声明什么、DP 和 HDMI 各自怎么把这两样对上。
>
> 🗺️ **全景定位**: [docs/00 全景地图](00-big-picture.md) ·
> [docs/01 显卡与显示](01-gpu-and-display.md) ·
> [docs/02 从电源到像素](02-boot-to-pixels.md)

---

## 0. 一句话模型

**显卡是主，显示器是从。**

显卡按一个精确的像素时钟，不停地把像素流「推」到线缆上；显示器靠自己的 PLL
从信号里恢复这个时钟，跟着它逐点、逐行、逐帧地接收。所谓「协商」，实际是两件
事：

1. 显示器通过 **EDID / DPCD** 声明「我能接受什么」（能力集合，静态）；
2. 显卡的驱动选其中一组参数、把链路配好（链路训练），然后开始推流。

选择权始终在显卡这边。显示器**没有权力**要求显卡发什么。

| 角色 | 提供 | 产物 |
|------|------|------|
| 显卡（CRTC） | 像素时钟 + 时序 + 信号 | 有严格周期性的电信号流 |
| 显示器（TCON） | 面板原生点阵 + 接收能力 | 锁定时序后点亮面板 |
| 内核（KMS） | 把两者对上 | `connector->modes` 列表 |
| 用户态（合成器） | 从列表里挑一个 | 一次 atomic commit |

---

## 1. 对象模型：`card` 是显卡，不是显示器

最常见的误解：`/dev/dri/card0` 对应一台显示器。**不是。**

```
/dev/dri/by-path/pci-0000:00:02.0-card -> ../card0    ← card 绑在 PCI 设备上
/sys/class/drm/
├── card0                ← 设备（一块 GPU / 一个 DRM 实例）
├── card0-Virtual-1      ← connector（一个「口」）
├── card0-Virtual-2
├── card0-Virtual-3
└── card0-Virtual-4
```

两台实测机器：

| 机器 | card 数 | connector 数 | 实际接的显示器 | `edid` 字节 |
|------|---------|--------------|----------------|-------------|
| QEMU qxl | 1 | 4 | 1（Virtual-1） | **0** |
| QEMU virtio-gpu | 1 | 1 | 1 | **256** |

**一块卡、四个口**。`cardN` 的编号是 GPU 的探测顺序（笔记本双显卡就是
`card0` = i915、`card1` = nvidia-drm），与显示器数量无关。

一条完整的输出管线：

```
CRTC 36 ──▶ encoder 38 ──▶ connector 37 ──▶ 显示器
(扫描引擎)   (信号编码)      (物理口 + EDID)
```

| 对象 | 位置 | 数量 | 职责 |
|------|------|------|------|
| **connector** | 物理口 | 与插口同数 | 热插拔状态 + EDID 解析出的模式表 |
| **encoder** | 卡内 | 每个口 1~2 个 | 把像素流编码成 TMDS/DP/LVDS 信号 |
| **CRTC** | 卡内 | 1~6 个 | 产生时序，按像素时钟扫 framebuffer |
| **plane** | 挂在 CRTC 上 | 每个 CRTC 1~3+ | primary / cursor / overlay |
| **framebuffer** | 显存 | 任意 | 一块「按什么格式、多宽的 pitch」的像素 |

**能同时点几块屏 = min(connector 数, CRTC 数, `possible_crtcs` 交集)**。
4 个口配 3 个 CRTC 时，第四块屏点不亮。`encoder 38` 的
`possible_crtcs = 0x1` 意味着它只能被 CRTC index 0 驱动——组合不是任意的。

---

## 2. CRTC：显卡里唯一产生时序的部件

CRTC = **CRT Controller**，显像管时代留下的名字，今天就是**扫描引擎**。它的
每个像素时钟周期都在做：按 pitch 从 framebuffer DMA 取一个像素 → 叠加各
plane → 输出 = DE 有效时给像素、消隐时给同步信号。

### 2.1 一个 mode 就是一组时序

```
pixel clock = 65.000 MHz          ← 每 15.4ns 吐一个像素
┌───────── 一行 = 1344 个像素时钟周期 ─────────┐
│ 1024 有效 │ 前肩 24 │ 同步 136 │ 后肩 160 │
│   DE=1    │  DE=0   │   DE=0   │   DE=0   │
└──────────────────────────────────────────┘
                 × 806 行 = 一帧
   768 有效行 + 3 前肩 + 6 同步 + 29 后肩
```

| `modetest` 列 | 含义 | 取自 `drmModeModeInfo` |
|---------------|------|------------------------|
| `hdisp / vdisp` | 有效像素 / 有效行 | `hdisplay / vdisplay` |
| `htot / vtot` | 一行 / 一帧的总周期数 | `htotal / vtotal` |
| `hss / hse` | 水平同步起点 / 终点 | `hsync_start / hsync_end` |
| `vss / vse` | 垂直同步起点 / 终点 | `vsync_start / vsync_end` |
| `clock` | 像素时钟（kHz） | `clock` |
| `flags` | 同步极性、隔行、倍扫 | `flags` |
| `type` | `preferred` / `driver` / `userdef`… | `type` |

刷新率不是独立参数，是算出来的：

```
refresh = clock / (htotal × vtotal)
        例：65,000,000 / (1344 × 806) = 60.00 Hz
            148,500,000 / (2200 × 1125) = 60.00 Hz   ← 正好整除
```

所以 `modetest` 打 clock 和 totals，不打「刷新率」。

### 2.2 为什么要消隐

CRT 时代是物理需要：电子束从行尾回扫到行首、从上到下回扫，必须留时间。
LCD 时代物理上不再需要，但协议保留了下来，而且消隐至今不是浪费：

- 编码器要靠它塞**同步信号**（TMDS 用控制周期承载 HS/VS）；
- DP 在消隐期插入 **MSA**（主数据流属性）和音频包；
- 链路也能借此喘息。

**CVT-RB**（Reduced Blanking）就是把消隐压到最小、换更大分辨率的折中方案。

### 2.3 vblank 与 plane

- 一帧扫完、下一帧开始前的那段垂直消隐就是 **vblank**。这是唯一能安全切换
  framebuffer 指针的时刻——page flip 就发生在这里，也是模块 6 帧回调的
  硬件源头。
- **plane** 让 CRTC 在一次扫描中从多块 buffer 采样叠加（硬件合成）：
  primary（桌面）、cursor（鼠标，不动 framebuffer）、overlay（视频独占一层）。
  模块 11/12 的软件合成是最原始的对照形态。

### 2.4 CRTC 不知道自己在驱动什么显示器

它只管按时序扫内存。分辨率、格式、该不该消隐，是**别人配给它的**。这个「别人」
就是内核（读 EDID 之后算出来的 mode）+ 用户态（选中哪个 mode）。

---

## 3. 显示器内部：它其实「很笨」

```
信号 → PHY/接收器 → TCON(时序控制器) ─┬→ source driver (列/数据线)
                                        └→ gate driver   (行/扫描线)
                                              ↓
                                           面板 (LCD/OLED)
```

- **TCON** 里有 PLL，锁定输入信号里的时钟，把像素流解成 RGB 写进面板；
- **原生分辨率**是面板物理点阵，改不了，也不由显卡决定；
- 「1920×1080 的屏显示 1280×720」靠显示器内部 **scaler** 插值（有画质损失）；
  要 1:1 就让 CRTC 直接输出 1280×720 再让显示器加黑边。

所以「设置分辨率」的本质是：**选择让显示器用哪组时序工作**，而不是改变面板。

---

## 4. EDID：显示器交出的能力清单

### 4.1 它物理上在哪、怎么被读到

EDID 是显示器主板上的一颗 **EEPROM**（或由 MCU 模拟），通过**边带通道**暴露，
从设备地址 `0x50`：

| 接口 | 传输 | 引脚 |
|------|------|------|
| VGA | DDC2B（真 I2C） | 15=SCL, 12=SDA, 9=+5V |
| DVI | DDC（真 I2C） | 6=SCL, 7=SDA, 14=+5V, 16=HPD |
| HDMI | DDC（真 I2C） | 15=SCL, 16=SDA, 18=+5V, 19=HPD |
| DP / eDP | **AUX**（不是 I2C） | 15=AUX+, 17=AUX−, 18=HPD, 20=DP_PWR |

内核里统一走 `drm_do_get_edid()`，唯一的差别是「i2c adapter 是谁」：

- HDMI：GPU 的 DDC 控制器（Intel 叫 gmbus），每个口一个 adapter；
- DP：`drm_dp_aux` 在 AUX 上实现的 **I2C-over-AUX**（把一次 I2C 事务封装进
  AUX 包转发）；
- 所以四个口都用 `0x50` 不冲突——它们在不同的 adapter 上。

读失败的处理：校验和错、header 错、I2C NAK → 重试几次 → 放弃。此时
connector 可能仍是 `connected`（HPD 电平在），但模式表极少——这就是
「插着线只能 640×480」的经典故障。

### 4.2 128 字节基础块

| 偏移 | 长度 | 内容 |
|------|------|------|
| 0x00 | 8 | 魔数 `00 FF FF FF FF FF FF 00` |
| 0x08 | 10 | 厂商 ID（3 字母 × 5bit）、产品码、序列号、生产周/年 |
| 0x12 | 2 | EDID 版本（1.3 / 1.4） |
| 0x14 | 5 | 基础显示参数：gamma、色度坐标 |
| 0x19 | 2 | 特性位：支持 RGB/YCbCr、sRGB、DTD#1 是否原生 |
| 0x1B | 3 | established timings 位图（VGA 时代的遗产） |
| 0x26 | 16 | standard timings：8 条，`h=(b+31)×8`，`refresh=b+60` |
| 0x36 | 72 | **4 个 18 字节描述符**（见下） |
| 0x7E | 1 | 扩展块数量 |
| 0x7F | 1 | 校验和（128 字节求和 ≡ 0 mod 256） |

### 4.3 DTD：18 字节装下一整套时序

Detailed Timing Descriptor 是 EDID 的核心，字段与 `modetest` 一一对应：

| 字节 | 字段 | 对应输出 |
|------|------|----------|
| 0–1 | pixel clock（10 kHz 单位） | `clock` |
| 2–4 | h active / h blanking | `hdisp` / (`htot-hdisp`) |
| 5–7 | v active / v blanking | `vdisp` / (`vtot-vdisp`) |
| 8–10 | h front porch / sync width | `hss-hdisp` / (`hse-hss`) |
| 11–12 | v front porch / sync width | `vss-vdisp` / (`vse-vss`) |
| 13–14 | 图像尺寸（mm） | `size (mm)` |
| 15 | 边框 | — |
| 17 | 隔行 / 同步类型 / **同步极性** | `flags: nhsync, pvsync…` |

flags 里 bits[4:3] = `11` 表示「数字分离同步」，此时 bit2 = vsync 极性、
bit1 = hsync 极性。

前两字节为 0 的是**描述符**（不是时序）：

| tag | 含义 | 用途 |
|-----|------|------|
| `0xFC` | monitor name | `"QEMU Monitor"` |
| `0xFD` | **range limits** | `50-125Hz, max clock`，自算 CVT 模式的边界 |
| `0xFF` | serial number | |
| `0xFE` | ASCII string | |
| `0xF7` | established timings III | 更多老分辨率 |

### 4.4 扩展块

基础块只放得下 4 个模式，远不够。第 126 字节声明后面还有几个 128 字节块：

- **`0x02` CEA-861**：电视路数。VIC 列表（`VIC 97` = 3840×2160@60）、音频、
  **HDMI VSDB（OUI `0x000C03`）**——里面写着支持哪些色彩格式、深色、HDR；
- **`0x70` DisplayID**：现代高分屏用它承载更多 DTD、平铺信息（`TILE` 属性）、
  自适应同步能力。

### 4.5 解剖一份真 EDID

QEMU virtio-gpu 的 `/sys/class/drm/card0-Virtual-1/edid`（256 字节）：

```
header 00ffffffffffff00      ver 1.4, ext blocks 1
mfg "RHT", prod 4660, 2014-w42
DTD#1  clock 107.3MHz  1280x800  htot 1728  vtot 828
       hss 1600 hse 1638  vss 804 vse 808
       size 325x203mm   flags 0x18
desc#2 tag 0xF7 (established timings III)
desc#3 range 50-125 Hz
desc#4 name "QEMU Monitor"
```

换算一下就和 `modetest` 对上了：

```
107,300,000 / (1728 × 828) = 75.0 Hz        → refresh 列
flags 0x18 = 0b0001_1000 → 数字分离同步, 双负极性 → nhsync, nvsync
size 325×203 mm                              → size (mm) 列
```

`preferred` 那个模式通常就是 DTD#1（EDID 特性字节里有「DTD#1 是原生」一位）。
**显示器能「推荐」，但最终选哪个由显卡这边决定。**

---

## 5. 边带通道：DDC、AUX 与 HPD

### 5.1 DP 的 AUX 不是 I2C

DP 没有 I2C 线。它的边带是 **AUX**：差分、半双工、Manchester 编码、1 Mbps。
AUX 上跑两种语义完全不同的东西：

1. **原生 AUX 事务** → 读写 **DPCD**（链路能力、训练状态、下游拓扑…）；
2. **I2C-over-AUX** → 把一次 I2C 事务封装进 AUX 包转发，专门读 EDID。

所以对 DP 说「走 I2C」只在**逻辑层**成立。`drm_dp_aux` 在 AUX 上实现了
`i2c_adapter`，于是 `drm_do_get_edid()` 用同一套代码读 HDMI 和 DP 的 EDID。

### 5.2 HPD 的两种语义

| 接口 | HPD | 含义 |
|------|-----|------|
| HDMI / DVI | **电平** | 线插着就一直高；源据此读 EDID |
| DP | **事件**（脉冲 IRQ_HPD） | 「状态变了，去读 DPCD 的 SINK_COUNT 看有几个屏」 |

DP 的 HPD 是事件，这才是它能用一条线挂多屏（**MST**）的入口。

---

## 6. 协商：三层与两条路线

### 6.1 三个层各自谈什么

| 层 | 内容 | 谁决定 | 载体 |
|----|------|--------|------|
| **链路层** | 接口类型、lane 数、速率、均衡 | 驱动（DP 要闭环训练） | DPCD / EDID+试错 |
| **像素格式** | RGB / YCbCr444/422/420、6~12bpc、HDR | 驱动在能力交集里选 | EDID VSDB / DPCD / MSA |
| **时序** | 分辨率、刷新率、消隐、同步极性 | 用户态选，驱动校验带宽 | EDID DTD / CVT 计算 |

### 6.2 DisplayPort：真协商

```
1. 插线 → HPD 脉冲
2. 读 DPCD 0x00001 MAX_LINK_RATE / 0x00002 MAX_LANE_COUNT
3. 用 I2C-over-AUX 读 EDID(0x50) → 拿 DTD 列表
4. 用户态选 2560x1440@60
     带宽估算: 241.5 MHz × 24 bit ≈ 5.8 Gbps
     HBR2 每 lane 5.4 Gbps × 8b/10b(80%) = 4.32 Gbps 有效
     → 2 lane 就够（8.64 Gbps）
5. Link training: 试 VS(0-3) × PE(0-3)，
   读 0x00202~0x00204 的 CR_DONE / EQ_DONE / SYMBOL_LOCKED，
   直到全部通过或放弃
6. 写 0x00100 LINK_BW_SET / 0x00101 LANE_COUNT_SET
7. 开始发流：MSA(时序+格式) + 像素 + 音频包；sink 锁定并显示
```

关键 DPCD 地址（写这类代码时按规范核对）：

| 地址 | 内容 |
|------|------|
| `0x00001` | MAX_LINK_RATE |
| `0x00002` | MAX_LANE_COUNT |
| `0x00005` | DOWN_STREAM_PORT_PRESENT（DP++/转接器） |
| `0x00100` / `0x00101` | LINK_BW_SET / LANE_COUNT_SET |
| `0x00200` | SINK_COUNT（MST 多屏） |
| `0x00202`–`0x00204` | 训练状态（CR/EQ/SYMBOL_LOCK/INTERLANE_ALIGN） |

失败的表现很具体：`connector` 仍 `connected`，但 `link-status` 属性变
**Bad**，黑屏。这就是那个属性的意义——用户态能看见链路坏了。

### 6.3 HDMI：单向配置

```
源加 +5V → sink 拉 HPD → 读 EDID(DDC/I2C) → 选 DTD
→ 按 HDMI VSDB 选 RGB888 还是 YCbCr444/420、8/10/12bpc
→ 配 TMDS 时钟与 PLL → 发流（消隐期插 InfoFrame）
```

没有链路训练，所以线材不够好时**不会自动降级**，只会闪黑/花屏。带宽边界是硬的：
4K60 8bit RGB 需要 594 MHz TMDS（约 17.8 Gbps），正好卡在 HDMI 2.0 上限；
再高就得 YCbCr420 或 DSC。

### 6.4 EDID 与 DPCD 的分工

| | EDID | DPCD |
|---|---|---|
| 性质 | 静态能力表（说明书） | 动态状态 + 链路参数（对讲机） |
| 传输 | HDMI: DDC；DP: I2C-over-AUX | DP 原生 AUX |
| 内容 | 时序、格式、音频、名字、尺寸、范围 | lane/rate、训练结果、下游拓扑、DSC、PSR、背光 |
| 谁写 | 厂商烧录，只读 | 源写配置，sink 写状态 |
| HDMI 有吗 | 有 | **没有**（等价信息在 CEA/VSDB 里） |

一句话：**EDID 回答「你是谁、能吃什么」，DPCD 回答「这条链路现在能跑多快、怎么调」。**

### 6.5 内核与用户态各干什么

```
内核驱动:  HPD → detect() → get_modes()（EDID/DPCD → DTD + CVT + 兜底）
           → mode_valid() 裁剪（带宽、vram、时钟上限）
           → 填 connector->modes → hotplug uevent

用户态:    从列表挑一个 → atomic commit
           → 驱动做 link training / 重配 PLL / 校验带宽
```

所以「协商」发生在两个时间尺度：**热插拔时**（能力查询）和**每次 modeset**
（链路训练、PLL 重配）。DRM 的抽象价值就在于：合成器不需要懂 link training。

---

## 7. CRTC 与 EDID：区别和联系

### 7.1 区别

| 维度 | **CRTC** | **EDID** |
|------|----------|----------|
| 位置 | 显卡里 | 显示器里 |
| 本质 | 硬件扫描引擎（一组寄存器） | 128B+ 只读数据（EEPROM） |
| 数量 | 每卡 1~6 个，每块屏占一个 | 每台显示器 1 份 |
| 谁写 | 内核/驱动在 modeset 时写 | 厂商出厂烧录 |
| 方向 | **产生**时序（主动） | **声明**时序（被动） |
| 生命周期 | 每次 modeset 重新配置 | 插拔/开机读一次 |
| 失配后果 | 黑屏、花屏、显示器报 out of range | 读不到 → 只能兜底模式 |

### 7.2 联系：菜单与厨房

```
   EDID ──解析──▶ connector->modes ──用户态挑选──▶ atomic commit ──▶ CRTC 寄存器
  (菜单)              (模式清单)              (点菜)                  (厨房照单做菜)
```

- EDID 里的一个 **DTD 就是一份可供 CRTC 执行的时序配方**；
- 「亮屏」的定义是：**CRTC 实际输出的时序 ∈ EDID 声明可接受的集合**；
- 但两者**物理上完全解耦**：CRTC 不读 EDID，EDID 也不知道 CRTC 在哪。中间的
  搬运全靠内核（生成 modes）+ 用户态（选一个）。

### 7.3 解耦的证据

- `xrandr --newmode` / `modetest -s 37:1280x720` 可以设置一个 **EDID 里根本没有
  的时序**，照样能亮（前提是没超出 `range limits` 的物理能力）；
- 反过来，EDID 声明了 4K，但如果链路带宽不够或 CRTC PLL 到不了，驱动会在
  `atomic_check` 里直接拒绝（`-EINVAL` / `-ENOSPC`），根本不会尝试。

**没有任何硬件能阻止你配一个显示器不支持的时序**——它只能黑屏或提示
「out of range」。这也是排障时要分清「我配错了」还是「它不支持」的原因。

---

## 8. 没有 EDID 的世界

| 场景 | EDID | 结果 |
|------|------|------|
| 普通 HDMI/DP 显示器 | 256~512B | 模式表 = DTD + CEA |
| **eDP 笔记本内屏** | 常常 **0B** | 时序来自驱动里的面板表 + DPCD |
| **虚拟显卡（qxl/bochs）** | **0B**（连 EDID 属性都没有） | 内核灌 DMT 表兜底 |
| virtio-gpu | 256B（host 伪造） | 例如 `"QEMU Monitor"` 1280×800 |
| 劣质线 / 转接头 | 读失败 | connected 但模式极少 |

qxl 这台（模块 9 的实测环境）具体发生了什么：

```
edid = 0 字节       → size (mm) = 0x0，无 DTD
→ drm_add_modes_noedid(8192, 8192)  灌 DMT 表（滤掉 >61Hz）
→ qxl_check_mode() 按 vram 裁掉 4K
→ qxl_add_extra_modes() 补 3 个
→ drm_set_preferred_mode(pwidth=1024)
→ drm_mode_sort()：preferred 第一，其余按面积降序
= 32 + 3 = 35 个模式，最大 2560x1600
```

所以 `modetest` 里那 35 个模式**不是显示器能力**，是 qxl 的默认剧本；`preferred
1024×768` 也只是 host 窗口/控制台的尺寸。`size (mm) = 0x0` 是同一件事的证据。

还有一层细节：qxl 的 connector 类型是 `VIRTUAL`，而内核
`drm_connector_init()` 对 `VIRTUAL`/`WRITEBACK` **默认不挂 EDID 属性**
（`if (connector_type != DRM_MODE_CONNECTOR_VIRTUAL && …)
drm_connector_attach_edid_property()`）。所以 `modetest -c` 的属性表里也看不到
`EDID` 那一项——不是读失败，是压根没有这个属性。virtio-gpu 想要 EDID 就得自己
显式 attach，这也是它能给出 256 字节 `"QEMU Monitor"` 的原因。

---

## 9. 排障对照表

| 现象 | 最可能的原因 | 查什么 |
|------|--------------|--------|
| `connected` 但黑屏 | 链路训练失败 / 带宽不够 / CRTC 没配 | `link-status` 属性、`dmesg` |
| `connected` 但只有 640×480 | EDID 读失败 | `wc -c .../edid`、换线 |
| connector 列表里没有你的口 | 驱动没接管（fallback 到 simpledrm） | `dmesg \| grep drm`、`lsmod` |
| 模式被"吞掉"（EDID 有 4K 但列表里没有） | `mode_valid` 裁剪：带宽/显存/时钟 | `dmesg \| grep -i mode` |
| `modetest -c` 刷一屏 `trying to open … failed` | 驱动名不在 modetest 内置清单里 | 见 §10 |
| 设置后花屏、换回就好 | 时序超出显示器能力（EDID 没这么声明） | 对照 DTD / range limits |
| 多屏只能亮一块 | CRTC 数不够或 `possible_crtcs` 冲突 | `modetest -p` / `-e` |

---

## 10. 动手观察

```bash
# ① 分清 card 与 connector
ls /sys/class/drm/
cat /sys/class/drm/card0-DP-1/status        # connected / disconnected
wc -c /sys/class/drm/card0-DP-1/edid        # 0 → 没有 EDID

# ② 解剖 EDID（找 DTD、range limits、VSDB）
sudo apt install edid-decode
sudo cat /sys/class/drm/card0-DP-1/edid | edid-decode

# ③ 取本机 DRM 驱动名，再让 modetest 枚举
drv=$(sudo awk '{print $1}' /sys/kernel/debug/dri/0/name)
sudo modetest -M "$drv" -c

# ④ 直接改 CRTC 的时序：证明 EDID 与 CRTC 解耦
sudo modetest -M "$drv" -s 37:1280x720

# ⑤ 看链路训练（DP 机器）
sudo cat /sys/kernel/debug/dri/0/DP-1/link_settings 2>/dev/null
sudo dmesg | grep -iE 'link training|edid|mode_valid'

# ⑥ 本仓库的教学版工具（模块 9 目录，make 时一起构建）
cd ../module-09-drm && make && sudo ./drm-info
./drm-info -e /sys/class/drm/card0-DP-1/edid    # 只解码 EDID，不需要 root
```

> **`modetest -c` 的老坑**：libdrm ≤ 2.4.11x 在不带 `-M/-D` 时**不会扫描
> `/dev/dri`**，而是拿一份硬编码的驱动名清单（i915/amdgpu/…/simpledrm）逐个
> `drmOpen()`，全不匹配就打印 `no device found`。`qxl`、`bochs`、`cirrus`、
> `vboxvideo`、`nvidia-drm`、`xe`、`vkms` 都不在清单里。
> 另外这些版本的 `-D` 是 `drmOpen()` 的 **busid**，**不是设备路径**；
> 直到 2.4.123 重构后才变成 `-D /dev/dri/card0`。

---

## 11. 输出对照速查表

同一件事在三个工具里的看法（`drv` 的取法见 §10）：

| 你想知道 | `modetest -M $drv` | `edid-decode` | `drm-info`（本仓库） |
|----------|--------------------|---------------|----------------------|
| 有几个 DRM 设备 | 不显示 | — | 首行 `=== /dev/dri/card0 ===` |
| 有几个口、插没插 | `-c` 的 `status` 列 | — | `[37] Virtual-1 connected` |
| 口 → encoder → CRTC → fb | `-e` + `-p` 组合着看 | — | 一行：`encoder 38 -> crtc 36 (fb 54, …)` |
| 模式表 | `-c` 的 modes 段 | — | 每个模式一行，带 totals 与极性 |
| 模式来源 | `type` 列 | — | 同 modetest |
| 各 CRTC 是否在扫 | `-p` | — | 末尾 `CRTC:` 段 |
| EDID 原始字节 | 只给 blob 长度 | 全解码 | `-e`，或连接时自动解码 |
| 支持范围 | — | range limits | `支持范围 50-125Hz …` |
| 色彩/音频/HDR 能力 | — | CEA / VSDB | `CEA-861 … HDMI VSDB` |
| 属性（DPMS/link-status） | `-c` 的 props | — | 每行一个，只读项标 `[只读]` |

一句话分工：**`modetest` 能改（设模式/关屏），`edid-decode` 只看 EDID，
`drm-info` 把「对象关系 + 模式 + EDID + 属性」放在一屏里。**

---

## 12. 与课程模块的关联

| 模块 | 本篇的哪一部分是它的原理 |
|------|--------------------------|
| [9 DRM/KMS](../module-09-drm/) | CRTC/connector 枚举、dumb buffer、`drmModeSetCrtc` |
| [10 libinput](../module-10-libinput/) | 无关（输入侧），但共用「内核对象 + 用户态」模式 |
| [11 最小合成器](../module-11-tty-compositor/) | 一条管线的完整接管：选 mode → page flip |
| [12 完整合成器](../module-12-full-compositor/) | 多输出、plane、atomic commit |
| [02 从电源到像素](02-boot-to-pixels.md) | simpledrm → 真驱动接管时的整条链路迁移 |

> **qxl 的提醒**：只有 `card0`、没有 `renderD128`。模块 9 完全够用，但模块
> 11/12 要用 GBM/EGL 时会卡在找 render node。做那两章建议把 QEMU 换成
> **virtio-gpu**（`gl=on`，即 virgl/venus）。

---

## 13. 延伸阅读

- [DRM/KMS 内核文档（Mode Setting 章节）](https://docs.kernel.org/gpu/drm-kms.html) —
  KMS 对象模型、atomic API、connector 语义的权威出处
- [VESA EDID 标准](https://www.vesa.org/vesa-standards/) 与
  [E-EDID 1.4 文本](https://glenwing.github.io/docs/VESA-EEDID-A2.pdf) —
  逐字节定义；`edid-decode` 源码是最好的对照
- [VESA DisplayPort 规范](https://www.vesa.org/vesa-standards/) —
  DPCD、AUX、link training 的原始定义
- [HDMI 2.0 规范（含 SCDC）](https://www.hdmi.org/spec/hdmi2_0) —
  TMDS、VSDB、InfoFrame
- [libdrm `tests/modetest`](https://gitlab.freedesktop.org/mesa/drm) —
  §2/§4 所有输出字段的实现
- [kernel: `drivers/gpu/drm/qxl/qxl_display.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/qxl/qxl_display.c) —
  §8 那套兜底逻辑的源码
