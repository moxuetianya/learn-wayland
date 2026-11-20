# 06 — 会话设备管理：TakeDevice、revoke 与多座席

> 对应 [模块 10](../module-10-libinput/)（libinput）与
> [模块 14](../module-14-session/)（logind）；同时是 [模块 9](../module-09-drm/)
> 和 [模块 11](../module-11-tty-compositor/) 里那句 `drmSetMaster()` 的「生产版」解释：
> 设备 fd 到底归谁、会话切换时谁把它收回去。
>
> 🗺️ **全景定位**: [docs/00 全景地图](00-big-picture.md) ·
> [docs/04 登录与会话](04-login-and-session.md) ·
> [docs/05 KMS/EDID/链路协商](05-kms-edid-and-link.md)

---

## 0. 一句话模型

**设备归 logind 所有，合成器拿到的只是 fd 的一份 dup；会话切换时收回去的是「访问权」
（revoke / drop master），不是把 fd 搬家。**

```
   logind (PID1 的子进程，持有 CAP_SYS_ADMIN)
     │  open("/dev/dri/card0") / open("/dev/input/event3")   → fd A
     │  ioctl(SET_MASTER) / 设置 ACL
     │
     │  D-Bus: Session.TakeDevice(major, minor)
     │  ← reply (fd A', inactive)         ← SCM_RIGHTS，dup 同一个 struct file
     ▼
   compositor ── open_restricted() ──▶ libinput（只负责 read，不知道 logind 存在）
     │
     │  会话切走：ioctl(EVIOCREVOKE, A) / ioctl(DROP_MASTER, A)
     │           → A' 立刻失效（A' 与 A 是同一个 evdev_client / drm_file）
     ▼
   PauseDevice("force") → 合成器 libinput_suspend()
```

---

## 1. fd 是怎么交出去的

### 1.1 D-Bus 方法，不是父子继承

```
Session.TakeDevice(u major, u minor) → (h fd, b inactive)
```

- `h` 是 D-Bus 的 fd 类型，底层走 **`SCM_RIGHTS`**（Unix socket ancillary data）；
- 调用者必须是该 session 的 **controller**（`session_is_controller(sender)`），
  且 session class 允许拿设备；
- 同一 session 对同一设备**只能 take 一次**：

```c
/* systemd src/login/logind-session-dbus.c */
if (sd)
        return sd_bus_error_set(error, BUS_ERROR_DEVICE_IS_TAKEN, "Device already taken");
/* The caller should use dup() if it requires more than one fd */
```

logind 在 `session_device_new()` 里自己打开节点：

```c
fd = open(sd->node, O_RDWR|O_CLOEXEC|O_NOCTTY|O_NONBLOCK);
```

然后把 fd 塞进回复：

```c
r = sd_bus_reply_method_return(message, "hb", sd->fd, !sd->active);
```

客户端（libseat 的 logind 后端）收到后必须**再把 fd 克隆一次**，因为 sd-bus
消息释放时会关掉消息里的那个 fd：

```c
sd_bus_message_read(msg, "hb", &tmpfd, &paused);
tmpfd = fcntl(tmpfd, F_DUPFD_CLOEXEC, 0);   /* 否则消息释放就没了 */
```

### 1.2 内核语义：dup 的是「打开实例」，不是数据

| 维度 | 是否共享 |
|------|----------|
| fd **编号** | 否（各进程 fd table 独立） |
| `struct file`（open file description） | **是，同一个对象** |
| 文件偏移、`O_NONBLOCK` 等状态 | 是 |
| `file->private_data` | 是 ← **关键** |
| `close()` | 各自关各自的，引用计数归零才释放 |

`private_data` 里放的就是驱动「每次打开」的状态：

- evdev → `struct evdev_client`（读队列、grab、`revoked`）
- DRM → `struct drm_file`（**是否 master**、handle 表）

于是：

- 对 A 做 `DRM_IOCTL_DROP_MASTER` → 共享同一个 `drm_file` 的 A' 立刻失去 master；
- 对 A 做 `EVIOCREVOKE` → 共享同一个 `evdev_client` 的 A' 立刻读到 `ENODEV`。

### 1.3 还有其他持有者

logind 会把 fd 通过 `sd_notify(FDSTORE=1)` **存进 PID1**（`session_device_save()`），
这样 logind 自己重启时 fd 不会丢。所以同一个 file description 可能同时被
**logind、PID1、合成器**三方持有——但都指向同一个内核对象。

限制：一条 D-Bus 消息最多带 **253** 个 fd（`SCM_MAX_FD`）。

> `SCM_RIGHTS` 的底层机制（为什么只有 AF_UNIX 可以、TCP/UDP 会静默丢弃、
> 0 字节 payload 的坑、以及 memfd/共享内存），见
> [07 fd 传递与共享内存](07-fd-passing-and-shared-memory.md)。

---

## 2. 为什么不能只靠 ACL

udev/logind 会给**当前活跃会话**的用户在设备节点上设 ACL：

```c
/* systemd src/login/logind-seat.c */
r = devnode_acl_all(s->id, false,
                    !!old_active, old_active ? old_active->user->user_record->uid : 0,
                    !!s->active, s->active ? s->active->user->user_record->uid : 0);
```

ACL 解决的是「能不能 `open()`」；**它收不回已经 open 的 fd**。所以对需要强制
收回的设备，logind 用「自己 open + 把 dup 发出去」的模型，换来两个「收回按钮」：

| 设备类型 | 收回手段 | logind 里的函数 |
|----------|----------|-----------------|
| evdev | `ioctl(fd, EVIOCREVOKE)` | `sd_eviocrevoke()` |
| hidraw | `ioctl(fd, HIDIOCREVOKE)` | `sd_hidiocrevoke()` |
| DRM | `DRM_IOCTL_DROP_MASTER` / `SET_MASTER` | `sd_drmdropmaster()` / `sd_drmsetmaster()` |

### 2.1 没有通用的 `revoke(2)`

Linux 上不存在 BSD 那种 `revoke(2)`；「收回」是各子系统自己实现的 **ioctl**：

| 子系统 | ioctl | 语义 | 起始 |
|--------|-------|------|------|
| evdev | `EVIOCREVOKE` | 让**这个 client** 立即失效 | 3.12 |
| hidraw | `HIDIOCREVOKE` | 同上 | 5.8 左右 |
| tty | `TIOCVHANGUP` | 挂断 tty，对端读到 EOF/HUP | 很早 |
| DRM | `DROP_MASTER` | 放弃 master，fd 仍可用 | — |
| socket | `shutdown(SHUT_RDWR)` | 半关闭，不是撤销 | — |
| 通用 | `close_range(2)` | 只关自己进程的 fd | 5.9 |
| 通用 | `pidfd_getfd(2)` | 反向：从别的进程“偷” fd | 5.6 |

### 2.2 `EVIOCREVOKE` 的作用域：一个 open file description

内核里它只是把调用者的 `evdev_client` 打上标记并唤醒：

```c
static int evdev_revoke(struct evdev *evdev, struct evdev_client *client,
                        struct file *file)
{
        client->revoked = true;
        evdev_ungrab(evdev, client);
        input_flush_device(&evdev->handle, file);
        wake_up_interruptible_poll(&client->wait, EPOLLHUP | EPOLLERR);
        return 0;
}
```

`evdev_client` 是**每次 `open()` 一个**（dup 共享同一个）。因此：

- fd 是 logind 传来的 dup → revoke 一打，合成器的 fd 立刻失效 ✅
- 合成器用 ACL 自己 `open()` → 另一个 `evdev_client` → revoke **打不到它** ❌

而且 revoke **不可逆**：revoked 的 client 永久报废，恢复时只能**重新 open**
（也就是重新 `TakeDevice`）。

---

## 3. 会话切换时发生了什么

### 3.1 seat0 + VT：内核主导

```
内核 VT 层处于 VT_PROCESS 模式
   │  切换前：SIGRTMIN+0（VT-release 信号）→ logind
   │  logind 立刻 ack（不阻塞切换）
   │  读 /sys/class/tty/tty0/active 得知新 VT
   ▼
seat_set_active(new)
   ├── session_device_pause_all(old) → session_device_stop()
   │      ├── DRM  ：ioctl(DROP_MASTER)，fd 不关，资源保留
   │      └── evdev：ioctl(EVIOCREVOKE)，fd 实际报废
   └── 给旧会话 controller 发 PauseDevice(major, minor, "force")
```

`session_device_stop()` 的注释就是设计意图：

```c
case DEVICE_TYPE_DRM:
        /* On DRM devices we simply drop DRM-Master but keep it open.
         * This allows the user to keep resources allocated. */
        sd_drmdropmaster(sd->fd);
case DEVICE_TYPE_EVDEV:
        /* Revoke access on evdev file-descriptors during deactivation.
         * This will basically prevent any operations on the fd and
         * cannot be undone. */
        sd_eviocrevoke(sd->fd);
```

### 3.2 没有 VT 的 seat：协商式切换

seat1+ 没有 VT，logind 自己做切换，且会**等客户端**：

```c
/* session_activate() */
s->seat->pending_switch = s;
num_pending = session_device_try_pause_all(s);   /* 发 "pause" */
if (!num_pending)
        seat_complete_switch(s->seat);
```

客户端处理完设备后调 `PauseDeviceComplete(major, minor)`，全部完成后才真正切换。

### 3.3 三种暂停语义

| type | 含义 | 客户端该做什么 |
|------|------|----------------|
| `pause` | 请优雅放手（只出现在无 VT 的 seat） | 停用设备 → `PauseDeviceComplete` |
| `force` | 访问权已被收回（drop master / revoke） | 别再读写了，等 resume |
| `gone` | 设备被拔掉 | 释放设备对象 |

### 3.4 切回

`session_device_resume_all()`：

- **DRM**：fd 一直开着，只需 `DRM_IOCTL_SET_MASTER`（注释原话：
  “Device is kept open. Simply call drmSetMaster() and hope there is no-one else.”）；
- **evdev / hidraw**：revoke 不可逆 → **重新 `open()`**；
- 然后给客户端发 `ResumeDevice(major, minor, fd)`，信号里**再带一个 fd**。

---

## 4. seat 与 VT 的真相

### 4.1 VT 只属于 seat0

```c
/* logind-seat.c */
bool seat_has_vts(Seat *s) {
        return seat_is_seat0(s) && s->manager->console_active_fd >= 0;
}
```

- **seat0**：连着物理控制台的那一套硬件，唯一拥有 `/dev/tty1…tty63`；
- **seat1、seat2…**：`vtnr == 0`，**没有 VT**，会话切换走 §3.2 的 D-Bus 协商；
- 多座席 = **多套独立硬件**（各自的显卡与键鼠，`ID_SEAT` 标签不同），
  不是共享同一套 tty1–7。

### 4.2 同一 seat0 上的多个 session：各占一个 VT

| 方向 | 行为 |
|------|------|
| 输出 | 只有 active VT 被扫描上屏；后台 VT 的内容留在内存里，切过去才看到 |
| 输入（tty 层） | 键盘事件只喂 active VT 的行规程，只有它的前台进程组能 `read()` |
| DRM | 切走时 master 被收回（logind 对自己发出去的 fd 直接 `DROP_MASTER`；自己 open 的靠客户端配合），fd 还在但不能 modeset |
| **evdev 层** | ⚠️ **不隔离**：任何持有有效 `/dev/input/event*` fd 的进程，在所有 VT 上都能收到物理按键 |

最后一行就是「跨 VT 偷看输入」的真正来源：

```
VT1: gdm 登录框，你在输密码
VT3: sudo ./libinput-demo        ← root 直开的 fd，没有 revoke
     → 照样打印 [key] code=30 DOWN ...，包括密码的每一个键
```

VT 这一层不会串，但**绕过 VT 直接读 evdev 会串**。

边角情况：logind 允许**多个 session 挂在同一个 VT 上**（代码注释里吐槽过老用户态
这么干）。这种情况下两者确实共享同一套输入输出。

### 4.3 验证

```bash
loginctl list-seats                 # seat0 常驻；是否还有 seat1
loginctl seat-status seat0          # 这个 seat 绑了哪些 tty/graphics/input
loginctl show-session $XDG_SESSION_ID -p VTNr -p Seat -p Active
cat /sys/class/tty/tty0/active      # 当前前台 VT
sudo fgconsole
loginctl show-seat seat0 -p ActiveSession
```

---

## 5. libinput 在链条里的位置

libinput **不认识 logind**。它只调用合成器提供的回调：

```c
/* libinput src/evdev.c */
fd = open_restricted(libinput, devnode, O_RDWR | O_NONBLOCK | O_CLOEXEC);
```

`open_restricted` 由使用者实现，于是出现三条路线：

| 实现 | 输入 fd 来源 | 会话切换时 | 需要的权限 |
|------|--------------|-----------|------------|
| 本课程模块 10/11 | 自己 `open()` | **无动作**（继续读） | root / `input` 组 |
| wlroots / sway | logind `TakeDevice`（经 libseat） | `force` → 重新 take | 普通用户 |
| mutter / GNOME | 自己按 ACL `open()`（`TakeControl` 只拿会话控制权） | 监听 `Active` → pause/resume | 普通用户 |

wlroots 把 libinput 的打开回调直接接到 session 后端：

```c
/* wlroots/backend/libinput/backend.c */
static int libinput_open_restricted(const char *path, int flags, void *_backend) {
        struct wlr_device *dev = wlr_session_open_file(backend->session, path);
        return dev->fd;      /* → libseat → logind TakeDevice */
}
```

暂停也由合成器负责（libinput 不感知会话）：

```c
/* wlroots：session 失活时 */
libinput_suspend(backend->libinput_context);
```

因为 ACL 直开的 fd 内核收不回去，只能靠使用者自觉。

---

## 6. 本课程的三处简化

| 模块 | 代码现状 | 生产做法 | 代价 |
|------|----------|----------|------|
| 模块 9 | `sudo` + `drmSetMaster()` / `drmDropMaster()` | 普通用户经 logind `TakeDevice` 成为 master | 必须停 gdm；不处理 VT 切换 |
| 模块 10 | root 直开 evdev，无 logind | `TakeDevice` + `libinput_suspend()` | 切 VT 后仍收键；绕过 ACL |
| 模块 11 | root 直开 DRM + evdev，自己 `drmSetMaster()` | logind 代持特权 + 会话事件 | 多 seat / 多会话不安全 |

模块 11 的注释其实已经点破了方向：

```c
/* 需要 logind 授权 (gnome-shell 就是这么做的)。 */
if (drmSetMaster(c->drm_fd) == 0) { ... }
```

**`drmSetMaster` 之所以在课程里能成功**，是因为你以 root 运行、且已经停掉图形会话；
生产合成器不能这么干，于是把「特权」外包给 logind，自己只拿 fd。

### 6.1 要把课程代码推向生产，缺这些

1. 用 `TakeDevice` 替换 `open_restricted` 里的 `open(2)`（换 libseat 最省事）；
2. 订阅 `PauseDevice` / `ResumeDevice`，失活时 `libinput_suspend()` + `DropMaster`；
3. 处理 `gone`（设备拔出）与 `DEVICE_IS_TAKEN`（自己 dup 而不是重复 take）；
4. 多输出/多 seat 时按 `ID_SEAT` 过滤，而不是硬编码 `"seat0"`。

---

## 7. 排障对照表

| 现象 | 最可能的原因 |
|------|--------------|
| 切到别的 VT 后，本程序仍在打印按键 | root 直开，没有 revoke（见 §2.2） |
| `TakeDevice` 返回 `Device already taken` | 同 session 已 take 过；自己 `dup()` |
| `TakeDevice` 返回 `Not in control` | 先 `TakeControl` / 成为 session controller |
| revoke 后 `read()` 返回 `ENODEV` | 正常：等 `ResumeDevice` 重新 take |
| `drmSetMaster` 报 `Permission denied` | 已有 master（图形会话在跑） |
| 多座席机器上只有一套键鼠生效 | 设备的 `ID_SEAT` 没配对，或都在 seat0 |
| seat1 的会话无法 `chvt` | seat1 没有 VT（§4.1），切换由 logind 协商 |
| 切换 VT 后黑屏、输入全无 | 客户端没 ack `pause` / 没恢复 `SET_MASTER` |

---

## 8. 动手观察

```bash
# ① 会话对象上到底有哪些方法（TakeDevice 在 Session 上）
busctl introspect org.freedesktop.login1 /org/freedesktop/login1/session/auto

# ② 实时看 fd 传递与暂停信号：切 VT 时会看到 PauseDevice/ResumeDevice
sudo busctl monitor org.freedesktop.login1

# ③ 对比 logind 与合成器持有的同一份设备
sudo ls -l /proc/$(pidof systemd-logind)/fd | grep -E 'card|event'
sudo ls -l /proc/$(pidof sway)/fd          | grep -E 'card|event'

# ④ 看 ACL（会话活跃时，对应用户有 rw）
getfacl /dev/input/event0
loginctl show-session $XDG_SESSION_ID -p Active -p State

# ⑤ 亲手制造「跨 VT 偷看输入」
#   tty3 上：sudo ./module-10-libinput/libinput-demo
#   切到 tty1 随便敲键 —— 观察 tty3 的输出
```

---

## 9. 与课程模块的关联

| 模块 | 本篇解释的那一部分 |
|------|--------------------|
| [9 DRM/KMS](../module-09-drm/) | `drmSetMaster` 为什么是特权，master 归谁 |
| [10 libinput](../module-10-libinput/) | `open_restricted` 为什么存在，三条实现路线 |
| [11 最小合成器](../module-11-tty-compositor/) | root 直跑与 logind 接管之间的差距 |
| [14 登录与会话](../module-14-session/) | `TakeControl` / `TakeDevice` 的实验入口 |
| [04 登录与会话](04-login-and-session.md) | session/seat/ACL 的上层视角 |

---

## 10. 延伸阅读

- [logind D-Bus 接口](https://www.freedesktop.org/wiki/Software/systemd/logind/) —
  `TakeDevice` / `ReleaseDevice` / `PauseDeviceComplete` 的权威定义
- [systemd `logind-session-device.c`](https://github.com/systemd/systemd/blob/main/src/login/logind-session-device.c) —
  本篇所有 logind 行为（open flags、revoke、master 交接、pause 流程）的源码
- [内核 `drivers/input/evdev.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/input/evdev.c) —
  `EVIOCREVOKE` 的实现与作用域
- [libseat](https://git.sr.ht/~kennylevinsen/seatd) — wlroots 系的会话抽象，
  把 logind/seatd 的差异藏在一个 `open_device()` 后面
- [wlroots libinput 后端](https://github.com/swaywm/wlroots/blob/master/backend/libinput/backend.c) —
  `open_restricted` + `libinput_suspend()` 的真实接线
- [sd-login(3)](https://www.freedesktop.org/software/systemd/man/sd-login.html) —
  用代码查 seat/session 的 API
