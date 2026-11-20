# 模块 14 — 登录与会话观察：systemd-logind

> **全景图定位**: [docs/04 登录与会话](../docs/04-login-and-session.md) — 把「按下电源到看到桌面」里最模糊的一环（登录后发生了什么）变成可观测的数据。
> **前置**: 读一遍 [docs/04](../docs/04-login-and-session.md) · **产出**: `session-info` 三个子命令 + `inspect.sh` 观察清单

## 目标

- 用 **sd-login API** 遍历 seat / session / VT（logind 的数据模型）
- 亲手验证**设备访问控制**：为什么你在桌面上能打开 `/dev/input/event0`，ssh 里不能
- 演示 compositor 免 sudo 的正道：D-Bus **TakeControl**

## 原理讲解（速览，详见 docs/04）

```
你输入密码
  → PAM 校验 (pam_systemd.so)          身份 ✓
  → logind 创建 session (绑 seat+VT)    权力 ✓
      → ACL 授权 /dev/dri /dev/input   给这个会话的用户
      → 创建 XDG_RUNTIME_DIR (tmpfs)   wayland-0 将诞生于此
  → 显示管理器 (GDM) 把 VT 切给你的会话
  → compositor 启动: drmSetMaster 或 TakeControl
```

本模块就是把这串链条的每一环变成可打印的事实。三个子命令对应三块：

| 子命令 | 观察 | docs/04 章节 |
|--------|------|--------------|
| `sessions` | seat 上的所有会话（含 greeter！） | §3.1/§3.2 |
| `me` | 当前进程的会话 + socket 三要素 + 设备 ACL 实验 | §3.3/§6.1 |
| `take-control` | D-Bus 调 logind Session.TakeControl | §5 |

**`me` 的设计意图**：在图形会话的终端里跑 vs 在 ssh 里跑，输出截然不同——「登录」与「起个 shell」的区别不再是抽象概念。

> `TakeControl` 之后 `TakeDevice` 交出的 fd 是什么（dup）、切会话时如何被
> `EVIOCREVOKE` / `DROP_MASTER` 收回，见
> [docs/06 会话设备管理](../docs/06-seat-devices-and-revoke.md)。

## 代码走读

[`main.c`](main.c)：

| 函数 | 要点 |
|------|------|
| `cmd_sessions` [L67](main.c#L67) | `sd_get_seats` → `sd_seat_get_sessions` → 逐会话取属性；注意 `sd_session_is_active` 的轮换语义 |
| `cmd_me` [L139](main.c#L139) | `sd_pid_get_session`（进程→会话的映射）；`try_open` 实测设备 ACL；扫 `XDG_RUNTIME_DIR` 找 wayland socket |
| `cmd_take_control` [L221](main.c#L221) | sd-bus 调 Manager.GetSession 拿对象路径 → Session.TakeControl(false)；失败时打印「为什么被拒」的教学解读 |

对照课程其他模块的 API 模式：`sd_pid_get_session` ≈ 「向系统查询自己的坐标」；GetSession+TakeControl ≈ [模块 9](../module-09-drm/) 的 `drmSetMaster` 的**文明版**（可审计、可授权、可撤销）。

## 动手实验

```bash
cd module-14-session
make
./session-info sessions
./session-info me
./session-info take-control
bash inspect.sh          # 系统性观察清单 (6 章)
```

### 观察点

1. **找出 greeter**：`sessions` 输出里 `class=greeter` 的会话——**GDM 登录界面本身就是一次 Wayland 会话**（[docs/04 §4](../docs/04-login-and-session.md#4-显示管理器图形登录的完整编排)）；未登录的机器上它是唯一 active 的会话；
2. **对照实验（本模块的灵魂）**：在图形会话终端里 `./session-info me`（session 存在、设备 ✓）→ 再 `ssh localhost` 进来跑同一命令（无 session、设备 ✗）→ 同一个用户，两种世界；
3. **VT 轮换**：tty3 登录一个文本会话后跑 `sessions`，与图形会话对比 active 标记；`Ctrl+Alt+F1/F3` 来回切，重跑 `sessions` 看 ★ 移动；
4. **socket 三要素**：`me` 打印的 `XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY`/socket 列表，正是 [模块 1](../module-01-connection/) `wl_display_connect` 依赖的全部环境——在 ssh 里把它们 export 齐了，模块 1-7 的客户端就能远程显示（或用 waypipe）；
5. **DRM master 现状**：`bash inspect.sh 3` 的 `fuser -v /dev/dri/card0`——持有者就是当前 compositor；跑模块 11 前再执行一次，确认没有别人占着；
6. **take-control 的两种结局**：桌面会话里跑（大概率被 polkit 拒绝——观察拒绝理由）vs 停掉 DM 后在 tty 会话里跑（本用户会话 active，可能成功）——体会「sudo 直通 vs 授权正道」；
7. **移交复盘**：`bash inspect.sh 5` 的 journalctl 输出里找出「New session c2」与 gdm 的启动记录，按 [docs/02 §6](../docs/02-boot-to-pixels.md#6-t5--桌面会话compositor-接管) 的时间线对号入座。

## 进阶练习

1. **会话监视器**：用 `sd_login_monitor_new` 监听会话变化，实时打印 active 轮换（一个 20 行的 `loginctl monitor`）；
2. **TakeDevice 全套**：take-control 成功后继续 D-Bus 调 `TakeDevice("drm", major:minor)`，配合模块 9 的 dumb buffer 显示——一个完全免 sudo 的迷你 compositor（终极目标）；
3. **多 seat 实验**：插第二个 USB 键盘，研究 udev 规则把它标到 seat1（`udevadm info` + `TAG+="seat"`）；
4. **扫描 fuser 替代方案**：用 `sd_session_get_active` + `/proc/*/fd` 自己实现「谁开着 card0」。

## 常见坑

| 现象 | 原因 |
|------|------|
| `sessions` 报错/空 | 容器或无 logind 环境（WSL1 等），`systemctl status systemd-logind` 检查 |
| `me` 说不在会话里 | ssh/cron/服务里运行——**这不是 bug，是最重要的教学输出**，做对照实验 |
| take-control 总被拒 | polkit 策略 / 会话非 active / 已有控制者（你的桌面 compositor） |
| 设备实验全 ✗ 但在桌面里 | 本进程无会话（跑在 ssh/服务里）；或 ACL 只给了 active 会话 |
| inspect.sh 某些命令不存在 | `apt install acl psmisc`（getfacl/fuser） |

## 延伸阅读

- [sd-login(3) 手册](https://man.archlinux.org/man/sd-login.3.en)
- [docs/06 会话设备管理](../docs/06-seat-devices-and-revoke.md) — `TakeDevice`
  的 fd 语义、revoke 与 seat/VT 隔离
- [logind D-Bus 接口](https://www.freedesktop.org/wiki/Software/systemd/logind/) — TakeControl/TakeDevice 的权威定义
- [sddm 源码](https://github.com/sddm/sddm) — 读一个真实显示管理器的会话编排

回到 [课程地图](../README.md)。至此 14 个模块 + 6 篇原理文档全部完成 🎉
