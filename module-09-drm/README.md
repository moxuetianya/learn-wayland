# 模块 9 — DRM/KMS 后端：直接控制屏幕

> **全景图定位**: [docs/01 §5](../docs/01-gpu-and-display.md#5-显示输出通路像素如何离开显卡) — 绕过一切中间层，亲手指挥显卡的显示引擎。
> **前置**: 建议先读 [docs/01 显卡的作用](../docs/01-gpu-and-display.md) · **产出**: 在纯 TTY 下把测试图案直接写到屏幕

## 目标

- 打开 DRM 设备（自动扫描 `card[0-9]*`，或 `DRM_CARD=/dev/dri/cardX` 指定）并成为 **DRM master**
- 用 `drmModeGetResources` 探索显卡拓扑: **Connector → Encoder → CRTC**
- 创建 dumb buffer、编程 CRTC 扫描它（`drmModeSetCrtc`）

## 原理讲解

### 本模块在整门课的位置

模块 1-8 都活在「协议世界」；本模块直接掉到[内核层](../docs/00-big-picture.md#2-总分层图)——**这是 compositor 权力的硬件来源**。没有 KMS 的知识，「compositor 控制屏幕」就只是咒语。

### 显卡拓扑：三个要找的部件

（详细原理见 [docs/01 §5](../docs/01-gpu-and-display.md#5-显示输出通路像素如何离开显卡)，此处是操作视角）

```
你的任务: 为「已连接的显示器」找到「能驱动它的扫描引擎」，并把 buffer 挂上去

drmModeGetResources(fd)
 └─▶ connectors[]        遍历: connection == DRM_MODE_CONNECTED 且有 modes
       └─▶ modes[]       选分辨率: 优先 DRM_MODE_TYPE_PREFERRED
       └─▶ encoders[]    查 possible_crtcs 位掩码
             └─▶ crtcs[] 找一个可用的 CRTC ──▶ drmModeSetCrtc(挂 buffer)
```

代码里的 `find_crtc()`（[L27-58](main.c#L27-L58)）完整实现这条查找链：先偷懒复用 connector 当前绑定的 encoder→CRTC，不行再遍历 possible_crtcs。

### DRM master：内核里的互斥锁

`drmSetMaster()` 申请**唯一的显示控制权**（[docs/01 §6](../docs/01-gpu-and-display.md#6-gpu-怎么被指挥mesaegl/openglvulkan)）：

- 同时只能有一个 master——已有桌面会话时你会失败（EACCES/EBUSY），这正是模块 9 必须停掉 gdm、在纯 TTY 跑的原因；
- 生产级 compositor 通过 logind 拿这个权限（[docs/04 §3.3](../docs/04-login-and-session.md#33-设备访问控制为什么你的-compositor-要-sudo)），课程用 sudo 直通。

### dumb buffer：教学版 framebuffer

```
DRM_IOCTL_MODE_CREATE_DUMB  ──▶ 内核分配一块线性内存, 返回 handle
drmModeAddFB2               ──▶ 包装成 framebuffer 对象 (fb_id), 绑定格式/尺寸
DRM_IOCTL_MODE_MAP_DUMB     ──▶ 拿到 mmap 偏移
mmap                        ──▶ CPU 可写
drmModeSetCrtc              ──▶ 命令 CRTC: "从现在起扫描这块内存"
```

写入 mmap 内存的每个字节，下一个扫描周期直接上屏（无撕裂保障，教学可接受；生产用 page flip——见 [docs/01 §5.2](../docs/01-gpu-and-display.md#52-vblank-与-page-flip为什么会有撕裂)）。

### 退出时的礼仪：恢复 CRTC

`drmModeGetCrtc` 保存现场 → 退出时 `drmModeSetCrtc` 还原（[L233-234](main.c#L233-L234) / [L259-266](main.c#L259-L266)）。不做的话退出后 TTY 控制台可能黑屏/花屏——**写 KMS 程序的第一美德是还原现场**。

## 代码走读

| 位置 | 内容 |
|------|------|
| [L27-58](main.c#L27-L58) | `find_crtc`: encoder→CRTC 查找链（含 possible_crtcs 位掩码的用法） |
| [L60-71](main.c#L60-L71) | `fill_buffer`: 测试图案（红渐变×、绿渐变Y——注意按 pitch 取行） |
| [L73-131](main.c#L73-L131) | `create_dumb_fb`: 四个 ioctl 的完整生命周期（创建→AddFB→map→mmap） |
| [L162-179](main.c#L162-L179) | 打开 card0 + drmSetMaster（含失败提示） |
| [L191-205](main.c#L191-L205) | connector 遍历与选择 |
| [L247-250](main.c#L247-L250) | `drmModeSetCrtc`: 一锤定音——屏幕从此扫描你的 buffer |
| [L257-266](main.c#L257-L266) | 清理路径：恢复 CRTC → 销毁 fb/buffer → 交还 master |

## 动手实验

```bash
# Ctrl+Alt+F3 进 tty3 登录
sudo systemctl stop gdm        # 停图形会话 (Ubuntu; KDE 用 sddm)
cd module-09-drm
make && sudo ./drm-demo
```

### 预期输出

```
资源: CRTCs=3, connectors=5, encoders=5, fbs=0
已连接的 connector id=85，模式数=24
选中的模式: 1920x1080 1920x1080 @ 60Hz
选中的 CRTC id=72
dumb buffer: fb_id=88 handle=3 pitch=7680 size=8294400
已显示测试图案，5 秒后恢复...
```

（屏幕出现 5 秒红绿渐变图案，然后恢复文本控制台。）

### 观察点

1. **看拓扑**：对比 `drm-info` 输出与你程序打印的 connector/CRTC 数量；找到你显示器接的那个 connector 名（HDMI-A-1/DP-1/eDP-1）；
2. **master 抢占实验**：回到图形会话（`sudo systemctl start gdm`）后再跑 `./drm-demo`（不 sudo）——观察 `drmSetMaster` 失败的报错信息，体会互斥；
3. **改模式**：把 preferred 换成 `conn->modes[1]`（通常是次高分辨率）——屏幕以新分辨率显示图案（真实切换显示模式）；
4. **画点东西**：改 `fill_buffer` 画一个十字准线/棋盘格，验证你对 pitch/像素格式的掌握（stride ≠ width×4 时按 `ptr + y*pitch` 取行）；
5. **`/sys` 对照**：`cat /sys/class/drm/card0-*/status`（connected/disconnected）与程序遍历结果一致；
6. **VM 虚拟显卡**：`dmesg | grep -iE 'virtio_gpu|vmwgfx'`——在 QEMU/VMware 里同样跑通，KMS 抽象屏蔽了虚拟/物理差异。

## 进阶练习

1. **page flip**：创建两块 dumb buffer，`drmModePageFlip(..., DRM_MODE_PAGE_FLIP_EVENT, ...)` + `drmHandleEvent` 轮换显示，画平滑动画——补上「无撕裂」的最后一块拼图；
2. **多显示器**：找第二个 connected connector，用另一个 CRTC 同时输出不同图案（注意 possible_crtcs 约束）；
3. **atomic API**：用 `drmModeAtomicCommit` 一次提交 connector+CRTC+plane 的完整状态（现代接口，weston/wlroots 全用它）；
4. **restore 兜底**：注册 `atexit` + 信号 handler，确保 Ctrl+C 也能恢复 CRTC。

## 常见坑

| 现象 | 原因 |
|------|------|
| 找不到任何 card 设备 | 无显卡/容器；或设备不是 card0——程序会自动扫描 `card[0-9]*`，也可 `DRM_CARD=/dev/dri/card1` 显式指定（virtio-gpu 常注册为 card1） |
| drmSetMaster 失败 | 图形会话还在跑——必须 stop DM 后从纯 TTY 运行；注意即使 `sudo` 也抢不走别人的 master（互斥与 UID 无关） |
| 打不开 card 设备 | 权限：非 root 需要 video 组 + 活跃会话 ACL（课程直接 sudo） |
| 找不到 connector | 服务器无显示器（headless）→ VM 加显卡设备；或用 `modeset=0`? 不，检查 `drm-info` |
| SetCrtc 返回 -22 (EINVAL) | CRTC 与 connector 不兼容（possible_crtcs 没对上）、模式不被支持 |
| 退出后黑屏 | 没恢复 saved_crtc（代码已处理，别删那段） |

## 延伸阅读

- [DRM/KMS 内核文档](https://docs.kernel.org/driver-api/drm.html)
- [modesetting helper: modetest](https://gitlab.freedesktop.org/mesa/drm/-/blob/main/tests/modetest/modetest.c) — libdrm 自带瑞士军刀（`sudo modetest -M virtio_gpu -c`）

下一模块: [模块 10 — libinput 集成](../module-10-libinput/)
