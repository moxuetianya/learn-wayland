# 04 — 登录、会话与应用如何使用 Wayland

> 对应诉求：**登录部分如何管理；其他 app 又是如何使用 Wayland 的**。本文回答两个问题：「输入密码之后系统到底做了什么」和「一个普通应用（GTK/Qt/Electron）从启动到画出窗口经历了什么」。动手部分对应 [模块 14](../module-14-session/)。
>
> 前置阅读：[02-boot-to-pixels.md](02-boot-to-pixels.md) §5-§7（显示管理器与权力移交的时间线）。

---

## 1. 「登录」拆开是两件事

平时说的「登录」其实是两个独立机制的组合：

```
 ① 身份认证 (你是谁?)              ② 会话建立 (给你什么?)
 ─────────────────────             ─────────────────────────
 getty/GDM 弹出提示                systemd-logind 创建 session
        │                                │
        ▼                                ▼
      PAM                            分配 seat/VT
 (Pluggable Authentication           授予设备访问权 (DRM/输入设备)
  Modules: 密码/指纹/…校验)               │
        │                                ▼
        ▼                          设置环境 (XDG_RUNTIME_DIR)
 认证成功/失败                     记录会话状态 (谁在前台?)
```

PAM 只回答「密码对不对」；logind 才回答「这个用户现在拥有这台机器的图形/输入设备」。理解了这个拆分，GDM 的行为就不再神秘：它先做 ①，成功后请求 logind 做 ②。

## 2. 文本世界的登录：getty → login → shell

最古老的形态（`Ctrl+Alt+F3` 看到的就是这个）：

```
systemd
 └─▶ agetty /dev/tty3          # ① systemd 拉起, 监视一个 VT
      └─▶ login -p             # ② 你输入用户名, login 接管
           └─▶ PAM stack       # ③ /etc/pam.d/login 里的规则链
                │  pam_unix.so ──▶ 对照 /etc/shadow 校验密码
                │  pam_env.so  ──▶ 加载环境变量
                │  pam_systemd.so ──▶ ★通知 logind: 注册一个 tty 会话
                ▼
           shell (bash/zsh)    # ④ 你拿到提示符
```

第 ③ 步里的 `pam_systemd.so` 是关键一环：正是它在 PAM 校验通过后调用 logind 创建会话。**这就是「登录」和「随便起个 shell」的本质区别**——后者没有会话，拿不到设备授权。

> 观察：tty3 登录后 `loginctl` 能看到 `session = c3, type=tty, class=user`；而在桌面终端里敲 `bash` 不会产生新会话。

## 3. systemd-logind：设备权的中央管家

logind 通过 D-Bus (`org.freedesktop.login1`) 提供四类服务，每一类都能在课程实验里摸到：

### 3.1 seat（席位）：一组「完整 workstation」

```
seat0 ──┬── drm/card0          (显示)
        ├── input/event*       (键盘/鼠标)
        ├── snd/*              (音频)
        └── tty1~tty63         (虚拟终端)
```

seat 是「一套人机交互设备」的抽象。多数机器只有一个 `seat0`；多 seat 系统（两个显示器两套键鼠两人同时用）会派生出 seat1。**libinput 的 `libinput_udev_assign_seat(li, "seat0")`（模块 10/11）就是在向 udev 查询「这个 seat 有哪些输入设备」**。

### 3.2 session（会话）：一次登录的生命周期

```
$ loginctl list-sessions
 SESSION  UID USER  SEAT  TTY
      c2  1000 peter seat0 tty2     ← 图形会话 (GDM 登录产生)
      c3  1000 peter seat0 tty3     ← 你在 tty3 的文本会话

$ loginctl show-session c2 -p Type -p Class -p State -p Active
Type=wayland
Class=user
State=active          ← 拥有 seat 的前台会话 (同一时刻一个 seat 只有一个 active)
```

**同一 seat 上多个会话通过 VT 切换轮换 `active` 状态**——这正是「Ctrl+Alt+F3 时桌面冻结、回来时恢复」的实现机制：compositor 观察 VT/会话信号，失活时停止渲染和输入（模块 11 的教学版不做这一步，生产 compositor 必须做）。

### 3.3 设备访问控制：为什么你的 compositor 要 sudo

`/dev/dri/card0` 默认只有 root 能读写。logind 的授权模型：

- **会话激活时**：logind 通过 udev ACL (`uaccess`) 把活跃会话的用户加进设备 ACL——这就是为什么你在**图形会话的终端里** `ls -l /dev/dri/card0` 会看到 `+peter`；
- **DRM master 语义**：即使能打开 card0，`drmSetMaster()` 仍要求「当前活动的 DRM master 已退出」——抢不走别人的；
- **compositor 的正道**：通过 logind D-Bus 接口 `TakeControl()`/`TakeDevice()` 拿设备（GDM 登录的 Mutter、以及 wlroots 系 compositor 都这么干），**不需要 root**；
- **课程野路子**：TTY 上 `sudo ./compositor`——root 直接无视所有授权（模块 11/12 的做法）。模块 14 的 `take-control` 实验演示正道。

> `TakeDevice` 交出的 fd 到底是什么（dup 语义、revoke、master 交接）、
> 以及 seat 与 VT 的隔离关系，见
> [06 会话设备管理：TakeDevice、revoke 与多座席](06-seat-devices-and-revoke.md)。

### 3.4 会话/电源管理

`loginctl lock-session`、`suspend`、`SwitchTo`（VT 切换）、会话结束清理……全部是 logind 的 D-Bus 方法。

## 4. 显示管理器：图形登录的完整编排

以 GDM 为例（SDDM/greetd 同理，细节略异），从开机到桌面的编排：

```
gdm.service (root)
 ├─▶ 向 logind 要一个 greeter 会话 (自己的 VT, 通常是 tty1)
 ├─▶ 以用户 gdm 身份运行 greeter:
 │      gnome-shell --mode=gdm          ← 一个"迷你 Mutter 会话"
 │      (此刻 greeter 的 Mutter 是 DRM master, 画出登录框)
 │
 ├─▶ 用户输入密码 ──▶ GDM 调 PAM (pam_gdm → pam_unix) 校验
 │
 └─▶ 校验通过:
        ├─▶ logind: 结束 greeter 会话
        ├─▶ logind: 创建用户会话 (Type=wayland, 新 VT)
        ├─▶ 切换 VT 到用户会话 ──▶ greeter 失去 DRM master
        └─▶ 以用户身份 exec 会话入口:
             /usr/bin/gnome-session
               └─▶ 启动 Mutter (compositor) ── drmSetMaster ✓
               └─▶ systemd --user 拉起门户/音频/指示器等用户服务
               └─▶ 自启动应用
```

几个容易忽略的事实：

- **greeter 本身就是一次 Wayland 会话**——GDM 在登录前就已经在「运行 Wayland」了；
- **登录动画/锁屏切换都是 VT 切换**——和 `Ctrl+Alt+F3` 同一机制，只是被 logind 编排得行云流水；
- **DM 与 compositor 的权力移交**完全走 §3.3 的规则，无 root 参与（greetd + sway/hyprland 用户的日常）。

## 5. 手动启动 compositor：三条路

本课程模块 11/12 需要独占显示，对照三条启动路径：

| 路径 | 命令 | 原理 | 适用 |
|------|------|------|------|
| A. sudo 直通 | `sudo ./compositor` | root 无视授权 | 课程实验最省事 |
| B. logind 会话内启动 | tty 登录后 `./compositor`（无 sudo） | 活跃会话的 ACL 让你能开 card0；无其他 master 时 `drmSetMaster` 成功 | Ubuntu 24.04 上可行，但要求该会话 active |
| C. logind TakeControl | compositor 启动时调 D-Bus `TakeControl()` | 生产 compositor 的正道（wlroots/libseat） | 进阶练习（模块 14 演示） |

> 课程采用 A 是为了把「授权」从教学路径里摘出去；但你应该知道 B/C 的存在——**module-11 的 `drmSetMaster()` 失败报错「another DRM master is running」，十有八九就是没停 gdm**。

## 6. 一个普通 app 如何使用 Wayland

换视角：现在你是 Firefox（或模块 1-7 你写的客户端），从被启动到画上屏幕。

### 6.1 第一步：找到 compositor 的 socket

Wayland 的服务发现极其朴素——一个 Unix domain socket 文件：

```
$ echo $XDG_RUNTIME_DIR $WAYLAND_DISPLAY
/run/user/1000  wayland-0

$ ls -l /run/user/1000/wayland-0
srwxr-xr-x 1 peter peter 0 ... /run/user/1000/wayland-0
```

连接规则（libwayland 内部逻辑，`wl_display_connect(NULL)`）：

1. 有 `WAYLAND_SOCKET` 环境变量？→ 直接用这个 fd（父进程传衣钵）；
2. 否则 `WAYLAND_DISPLAY`（默认 `wayland-0`）拼上 `XDG_RUNTIME_DIR` 得到路径，connect；
3. `XDG_RUNTIME_DIR` 不存在？→ 失败（模块 1 的经典报错场景：在 `sudo`/`ssh` 环境里丢了环境变量）。

> `XDG_RUNTIME_DIR` 正是 logind 在建会话时创建的用户运行时目录（`tmpfs`，UID 命名，退出登录回收）——**登录机制在这里直接决定了 Wayland 的可用性**。

### 6.2 第二步：协议握手与 toolkit 的角色

连上 socket 后的协议流程（模块 1/2 亲手做过的）：

```
wl_display_connect()
 └─▶ get_registry ──▶ 服务器广播 globals (wl_compositor, wl_shm, xdg_wm_base, wl_seat...)
 └─▶ bind 需要的接口
 └─▶ create_surface + xdg_toplevel + commit
 └─▶ 事件循环 (dispatch)
```

**没有任何 GUI 框架帮忙时，这就是全部**——模块 2 的 236 行代码就是「最小 Wayland 应用」。但真实 app 不会手搓这些，而是经工具箱：

| 工具箱 | Wayland 后端选择 | 强制环境变量 |
|--------|------------------|--------------|
| GTK 3/4 | 默认优先 Wayland | `GDK_BACKEND=wayland|x11` |
| Qt 5/6 | 默认 Wayland（平台插件） | `QT_QPA_PLATFORM=wayland` |
| SDL 2/3 | 运行时探测 | `SDL_VIDEODRIVER=wayland` |
| Electron/Chromium | Ozone Wayland | `--ozone-platform=wayland` |
| Firefox | 默认探测 | `MOZ_ENABLE_WAYLAND=1`（旧版） |

```bash
# 一个有趣的实验：强制 GTK 走 X11
GDK_BACKEND=x11 gtk4-demo          # 窗口实际由 XWayland 显示 (xeyes -info 可验证)
```

### 6.3 第三步：渲染与 buffer 的两条腿

- **CPU 腿**：toolkit 把控件画进 shm buffer（GTK 大量场景、以及模块 7 的 Cairo）；
- **GPU 腿**：EGL/OpenGL/Vulkan 经 Mesa 直接在 dmabuf 上渲染（WebGL、游戏、合成器特效）。EGL 的平台入口在 Wayland 下是 `EGL_PLATFORM_WAYLAND`——toolkit 替你做好，底层与 [01 号文档 §6](01-gpu-and-display.md) 相同。

### 6.4 桌面集成的最后一公里：portal

Wayland 安全模型的代价是应用失去了一批「随便拿」的能力（截屏/全局热键/选文件/知道自己窗口位置）。归还通道是 **xdg-desktop-portal**：

```
应用 ──D-Bus──▶ xdg-desktop-portal ──▶ 后端(按桌面环境: gnome/kde/wlr)
                                        │
                          截屏/录屏: 弹窗授权 ──▶ compositor 编流 ──▶ PipeWire ──▶ 应用
                          文件选择:   门户自己画文件对话框
                          全局热键:   注册进 compositor
```

portal 与 PipeWire 的合作（[03 号文档 §6](03-audio-stack.md)）是「Wayland 桌面 = 各服务分工」的最佳注脚：**compositor 管画面，PipeWire 管管道，portal 管授权**。

## 7. 动手观察

```bash
# ① 会话与 seat (详见模块 14 的 C 版本)
loginctl list-sessions
loginctl show-session self -p Type -p Class -p Active -p Display -p TTY
loginctl list-seats
loginctl show-user $USER -p Sessions -p State

# ② 运行时目录: 登录机制的直接产物
ls -la $XDG_RUNTIME_DIR | head        # wayland-0, pipewire-0, bus, dconf...
env | grep -E 'WAYLAND|XDG_|DISPLAY|DBUS_SESSION'

# ③ 谁是 DRM master
fuser -v /dev/dri/card0               # 几乎必然是你的 compositor
loginctl show-session self -p Active

# ④ toolkit 后端自检
GDK_BACKEND=wayland gtk4-demo &       # 观察它连的是 wayland-0 (lsbusir -p 可验证)
qdbus org.kde.KWin /KWin supportInformation 2>/dev/null | head -3

# ⑤ 移交复盘: 从 TTY 登录到桌面的会话痕迹
journalctl -b 0 | grep -iE 'logind|gdm|session' | tail -20
```

## 8. 与课程模块的关联

| 本文章节 | 对应动手 |
|----------|----------|
| §2 PAM 会话注册 | [模块 14](../module-14-session/) `sessions` 子命令 |
| §3 seat/设备 ACL | 模块 14 `seat`/`devices` 观察实验 |
| §5 TakeControl 正道 | 模块 14 `take-control`（D-Bus 调用演示） |
| §6.1 socket 发现 | [模块 1](../module-01-connection/) `wl_display_connect` 的失败分支 |
| §6.4 portal | [03 号文档](03-audio-stack.md) 屏幕共享链路 |

## 9. 延伸阅读

- [logind 官方文档](https://www.freedesktop.org/wiki/Software/systemd/logind/)
- [sd-login(3) 手册](https://man.archlinux.org/man/sd-login.3.en) — 模块 14 用的 API
- [Wayland 书: Compositor 与 display 管理](https://wayland-book.com/)
- 延伸实验：`loginctl terminate-session` 杀掉自己的图形会话再登录；`sudo systemctl isolate multi-user.target` 体验无 DM 世界。

---

至此七篇原理地图完整。回到 [README](../README.md) 查看课程全貌，或直接进入 [模块 1](../module-01-connection/) 开始动手。
