# 02 — 从按下电源到看到桌面：显示链路的完整启动史

> 对应诉求：**Linux 启动后如何一步一步通过显卡显示内容**。本文按时间线讲每一个「屏幕上出现新画面」的时刻，背后是谁在驱动显卡。读完你就能回答：「开机 logo、GRUB 菜单、滚动的内核日志、登录界面、桌面，分别是哪个程序画的？」
>
> 前置阅读：[01-gpu-and-display.md](01-gpu-and-display.md)（CRTC/ framebuffer / vblank 的概念）。

---

## 0. 总时间线

```
 时刻    谁在控制显卡                 屏幕上显示什么
 ────────────────────────────────────────────────────────────────────────────
 t0   UEFI/BIOS 固件          ──▶ 厂商 logo（固件用 GOP 画在 framebuffer 上）
 t1   GRUB (bootloader)       ──▶ 启动菜单（继续用固件留下的 framebuffer）
 t2   内核 + simpledrm/efifb   ──▶ 滚动的内核日志（fbcon 把文本画进同一块内存）
 t3   内核 + 真 GPU 驱动       ──▶ 无缝接管（同一块显存换个驱动管，肉眼无感）
 t4   内核 (KMS) + 用户态      ──▶ 切到图形 VT：显示管理器 (GDM/SDDM) 的登录界面
 t5   compositor (DRM master) ──▶ 登录成功，桌面会话（Wayland compositor 接管）
 ────────────────────────────────────────────────────────────────────────────
```

**核心线索只有一条：一块 framebuffer（显存里的像素数组）+ 一个扫描它的 CRTC。** 从 t0 到 t5，变的是「谁在写这块内存、谁有权配置 CRTC」，不变的是硬件扫描机制本身。

## 1. t0 — 固件阶段：UEFI GOP

按下电源，CPU 从固件（UEFI，老机器是 BIOS）开始执行。固件要做一件和显示有关的事：**初始化显卡，拿到一块能用的 framebuffer**。

- UEFI 提供 **GOP**（Graphics Output Protocol）标准接口：固件在启动早期让 GPU 进入最简单的线性 framebuffer 模式，并把这块显存的**物理地址、宽度、高度、像素格式**记下来；
- 你看到的厂商 logo（那只企鹅/ROG 眼/Dell 圆环）就是固件通过 GOP 画上去的；
- 老式 BIOS 时代对应的是 **VESA VBE** 模式，思想完全一样。

固件把 framebuffer 信息作为 **UEFI 变量/内存表** 传给下一棒——操作系统引导器。

> 观察：`journalctl -b 0 | grep -iE 'efi|gop'`，或看 `/sys/firmware/efi/` 是否存在（存在即 UEFI 启动）。

## 2. t1 — Bootloader：GRUB 菜单

GRUB 接手时**不重新初始化显卡**，直接沿用固件留下的 framebuffer 画它的菜单（gfxterm 图形终端）。这就是为什么 GRUB 菜单的分辨率往往和固件 logo 一致：

```
# /etc/default/grub 里控制这一阶段的分辨率：
GRUB_GFXMODE=1024x768x32      # GRUB 菜单分辨率
GRUB_GFXPAYLOAD_LINUX=keep    # 关键：把这块 framebuffer "keep" 下来交给内核
```

GRUB 通过 **boot protocol** 把 framebuffer 的地址和尺寸塞进内核的启动参数（`boot_params`），然后跳进内核。

> 观察：`sudo cat /proc/cmdline`，若有 `video=efifb:` / `quiet splash` 等参数，就是这条传递链的痕迹。

## 3. t2 — 内核早期：simpledrm/efifb + fbcon

内核启动最初几秒，**GPU 驱动还没加载**，但屏幕必须有输出（否则出问题时是黑箱）。内核的做法：

1. **早期 framebuffer 驱动**直接把 GRUB 传下来的显存注册成 framebuffer 设备：
   - 老方案 `efifb`（以及 `vesafb`）——传统 fbdev 设备；
   - 新方案 **`simpledrm`**（内核 5.15+，Ubuntu 24.04 默认）——把固件 framebuffer 包装成一个**只有 primary plane 的迷你 DRM 设备**，统一走 DRM 接口；
2. **fbcon**（framebuffer console）把内核的文本控制台「画」进这块内存——你看到滚动的 `[    0.000000] Linux version ...` 日志，就是 fbcon 在逐行把字符点阵写进 framebuffer，CRTC 照常扫描。

```
   内核日志 (printk)
        │
        ▼
   fbcon (把字符画成像素点阵)
        │
        ▼
   simpledrm 的 framebuffer (固件留下的那块显存)
        │
        ▼
   CRTC 扫描 ──▶ 屏幕
```

> 观察（这是本篇最有价值的一条）：
> ```bash
> dmesg | grep -iE 'simpledrm|efifb|fbcon|drm' | head -20
> # 典型输出（Ubuntu 24.04 虚拟机）：
> # [    0.000000] efifb: probing GPU...
> # [    0.523] simple-framebuffer ... >> setting mode ... via DRM
> # [    0.844] fbcon: ... (fb0) is primary device
> ```

## 4. t3 — 真 GPU 驱动接管：无缝迁移

稍后（通常同一秒内），真正的 GPU 驱动加载：Intel 是 `i915`、AMD 是 `amdgpu`、QEMU/KVM 虚拟机是 `virtio_gpu`、VMware 是 `vmwgfx`。它做三件事：

1. 完整初始化 GPU（渲染引擎 + 显示引擎 + 全部 CRTC/Connector/plane）；
2. **从 simpledrm/efifb 手里无缝接管 framebuffer**：内核保证切换瞬间 CRTC 仍扫描同一块内存，屏幕**一个像素都不闪**；
3. 从此 `/dev/dri/card0` 是一个功能齐全的 DRM 设备，fbcon 的内容改由新驱动继续供着。

```
simpledrm (固件显存, 只读式接管)          i915/amdgpu/virtio_gpu (完整 DRM)
      │  ─ ─ ─ ─ 无缝交接 (fbcon 内容不丢) ─ ─ ─ ─ ▶
      └─ 阶段使命结束，被卸载/旁路           /dev/dri/card0 + renderD128
```

> 观察：
> ```bash
> dmesg | grep -E 'i915|amdgpu|virtio_gpu|vmwgfx' | head    # 谁接管了
> ls /sys/class/drm/        # card0, card0-eDP-1/DP-1/HDMI-A-1, renderD128
> lsmod | grep -E 'drm|gpu'
> ```

**此刻整台机器处于「内核独占显示」状态**：任何用户进程都还没碰过 KMS。这也是本课程模块 9-12 实验窗口的「原点状态」——停掉显示管理器后的纯 TTY，就回到这里。

## 5. t4 — 用户空间启动：systemd → 显示管理器

内核启动 PID 1（systemd），开始拉起用户空间。与显示相关的两条线：

```
systemd
 ├─▶ getty@tty2,tty3...        文本登录线：Ctrl+Alt+F3 看到的登录提示符
 │     └─ login → PAM → shell   （fbcon 内容，仍在内核 framebuffer 上）
 │
 └─▶ display-manager.service    图形登录线：GDM (GNOME) / SDDM (KDE) / greetd
       └─▶ 切换到专用 VT (通常是 tty1/tty7)
       └─▶ 启动 greeter（登录界面）
```

**显示管理器（DM）是图形世界的第一个 compositor**。以 GDM 为例：

1. GDM 让 logind 把 VT 切到图形终端（`chvt`），fbcon 让位；
2. GDM 启动一个** headless 的 Mutter 实例**渲染登录界面（greeter），它通过 logind 授权成为 **DRM master**，用自己的 framebuffer 接管 CRTC——登录界面本质上是一个「迷你 Wayland 会话」；
3. 你输入密码 → GDM 交给 **PAM** 校验（[04 号文档](04-login-and-session.md) 详述）；
4. 认证成功 → logind 创建用户会话 → 准备交接棒。

## 6. t5 — 桌面会话：compositor 接管

登录成功的瞬间发生了一次**显示权力的移交**：

```
 GDM 的 greeter (DRM master)                用户会话的 compositor (新 master)
        │                                          │
        ├─ logind 结束 greeter 会话                  │
        ├─ 切换到用户会话的 VT ─────────────────────▶│
        └─ DRM master 权限撤销 ────────────────────▶ │ drmSetMaster() 成功
                                                   │ drmModeSetCrtc() 配置显示模式
                                                   │ 循环: 合成 → page flip
                                                   ▼
                                              桌面出现
```

之后的每一帧都走 [00 号文档 §3.1](00-big-picture.md) 的路径：客户端 commit buffer → compositor 合成 → vblank 时 page flip → CRTC 扫描。**从固件到桌面，这条显示链路的「最后一棒」永远定格在 compositor 手里。**

## 7. 特殊路径：从 TTY 手动启动（本课程模块 11/12）

理解了标准流程，就明白课程实验走的是「跳过 t4/t5」的野路子：

```
 标准:  UEFI → GRUB → simpledrm → GPU驱动 → GDM(PAM登录) → Mutter 桌面
 课程:  UEFI → GRUB → simpledrm → GPU驱动 → getty(文本登录) → sudo ./compositor
                                                      └── 你自己就是"显示管理器"
```

在 tty3 上 `sudo systemctl stop gdm` 之后，机器回到 t3/t4 之间的状态：内核 KMS 在位、没有用户进程是 DRM master。此时你的 compositor 调 `drmSetMaster()` 就能拿到钥匙——这正是模块 9/11/12 的开场动作。生产级的等价物是 `logind` 的 TakeControl 接口（不用 sudo），[04 号文档 §5](04-login-and-session.md) 和 [模块 14](../module-14-session/) 会展开。

## 8. 动手观察：复盘一次完整启动

在课程 VM（或任何 Linux）上按顺序执行，把本文每个阶段在日志里找出来：

```bash
# ① 固件阶段
journalctl -b 0 -o short-monotonic | head -5     # 时间从 0 开始
[ -d /sys/firmware/efi ] && echo UEFI || echo "Legacy BIOS"
sudo cat /proc/cmdline                            # 内核启动参数

# ② bootloader
sudo grub-editenv list 2>/dev/null || true        # GRUB 环境
journalctl -b 0 | grep -i grub | head -3

# ③ 内核早期显示
dmesg | grep -iE 'simpledrm|efifb|vesafb' | head
dmesg | grep -i 'fbcon' | head -3

# ④ GPU 驱动接管
dmesg | grep -E 'i915|amdgpu|virtio_gpu|vmwgfx|\[drm\]' | head -15
ls /sys/class/drm/                                # 看到完整 connector 列表

# ⑤ 显示管理器与登录
systemctl status display-manager --no-pager | head -5
loginctl list-sessions                            # 当前会话（详见模块 14）

# ⑥ 当前谁在扫描
cat /sys/class/drm/card0/device/uevent            # 绑定的驱动
fuser -v /dev/dri/card0 2>&1 | head               # 哪个进程开着它（=compositor）
```

**综合实验**（把整条时间线亲眼看一遍）：

1. 重启 VM，在 GRUB 菜单按 `e`，把 `quiet splash` 删掉，`Ctrl+X` 启动 → 你会看到 t2 阶段滚动的内核日志（fbcon）；
2. 到达桌面后 `dmesg | grep -E 'simpledrm|virtio_gpu'`，对照刚才看到的画面确认两阶段的存在；
3. `sudo systemctl stop gdm`（TTY 下）→ 屏幕回到 fbcon 控制台——**这就是没有 DRM master 时的世界**；
4. 运行模块 11 的 `sudo ./compositor` → 你的程序成为 t5 时刻的「桌面」。

## 9. 与课程模块的关联

| 本文章节 | 对应模块 |
|----------|----------|
| §4 GPU 驱动接管后的 `/dev/dri` | [模块 9](../module-09-drm/) 打开 card0 |
| §7 TTY 手动启动路径 | [模块 11](../module-11-tty-compositor/)、[模块 12](../module-12-full-compositor/) |
| §5/§6 DM 与会话移交 | [模块 14](../module-14-session/)（用 loginctl 复盘这次移交） |
| §6 最后一棒的渲染循环 | [模块 6](../module-06-frame-cb/)（客户端侧的 vblank 同步） |

## 10. 延伸阅读

- [内核文档：simpledrm](https://docs.kernel.org/fb/simpledrm.html) — 固件 framebuffer 的 DRM 包装
- [内核文档：DRM 内存管理](https://docs.kernel.org/driver-api/drm-mm.html)
- systemd 文档中关于 `graphical.target` / `getty.target` 的启动顺序
- 延伸思考：为什么 Windows/Linux 都坚持「固件 → 内核 → 显示服务器」逐级移交 framebuffer，而不是一开始就上完整驱动？（提示：可用性与故障可见性）

---

下一篇：[03-audio-stack.md — Linux 音频栈](03-audio-stack.md)
