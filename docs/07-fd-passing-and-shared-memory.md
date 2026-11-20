# 07 — fd 传递与共享内存：`SCM_RIGHTS`、memfd 与 mmap

> 为什么 logind 能把设备 fd 交给合成器（[docs/06](06-seat-devices-and-revoke.md)）、
> 为什么 Wayland 能把像素 buffer 零拷贝交给 compositor（模块 3/11）、
> 以及为什么这些**只能**走 AF_UNIX 而不是 TCP。
>
> 🗺️ **全景定位**: [docs/00 全景地图](00-big-picture.md) ·
> [docs/06 会话设备管理](06-seat-devices-and-revoke.md) ·
> [docs/01 显卡与显示](01-gpu-and-display.md)

---

## 0. 一句话模型

**fd 是本机内核对象的“索引”，只有 AF_UNIX 的对端才必然在同一内核里；共享内存本身从不经过 socket，socket 只负责递一张“门票”（fd）。**

| 能力 | AF_UNIX | TCP | UDP |
|---|---|---|---|
| 传 fd（`SCM_RIGHTS`） | ✅ | ❌ **静默丢弃** | ❌ **静默丢弃** |
| 传凭据（`SCM_CREDENTIALS` / `SO_PEERCRED`） | ✅ | ❌ | ❌ |
| **共享内存本身** | ❌（谁都做不到） | ❌ | ❌ |
| 传“指向共享内存的 fd”（memfd / dmabuf） | ✅ | ❌ | ❌ |
| 抽象命名空间 / 文件系统权限 | ✅ | ❌ | ❌ |
| 跨机器 | ❌ | ✅ | ✅ |

---

## 1. 为什么 fd 只能走 AF_UNIX

### 1.1 fd 的语义

`fd` 只是本进程 fd table 里的一个整数索引，指向内核的
`struct file`（open file description）。`SCM_RIGHTS` 做的是：

```
发送方 sendmsg(fd = 7, cmsg = SCM_RIGHTS)
   内核：fd 7 → struct file  →  get_file()（引用 +1）
接收方 recvmsg()
   内核：在接收方的 fd table 里装一个新号 → 同一个 struct file
```

这要求双方**共用同一份内核对象表**。TCP 的对端可能在另一台机器上，
那边不存在“你的 fd 7 对应的内核对象”，协议层自然不会定义这个语义。

### 1.2 内核里的分界

| 协议 | 代码路径 | 是否处理 `SCM_RIGHTS` |
|------|----------|----------------------|
| AF_UNIX | `net/unix/af_unix.c` 的 `unix_stream_sendmsg()` / `unix_dgram_sendmsg()` | ✅ 走 `scm_send()` / `scm_detach_fds()` |
| TCP | `tcp_sendmsg()` 根本不看 `msg_control` | ❌ |
| UDP | `ip_cmsg_send()` 只认 `IP_*` 系列 | ❌ |

### 1.3 实测：最坑的是“不报错”

在本机（Linux 6.12）用同一段 `sendmsg` 分别走三种 socket：

```python
a.sendmsg([b'x'], [(SOL_SOCKET, SCM_RIGHTS, array('i', [fd]))])
```

| socket 类型 | `sendmsg` 返回值 | 接收端 `recvmsg` 的 ancdata |
|---|---|---|
| `AF_UNIX` STREAM | 1 字节 | `(SOL_SOCKET, SCM_RIGHTS, fd)` → `/proc/self/fd/N` 指向原文件 ✅ |
| `AF_UNIX` DGRAM | 1 字节 | 收到 fd ✅ |
| `AF_INET` TCP | **1 字节（成功！）** | **空** ❌ |
| `AF_INET` UDP | **1 字节（成功！）** | **空** ❌ |
| TCP + `SCM_CREDENTIALS` | **成功** | 空 ❌ |

所以「TCP/UDP 做不到」要加个注：**是静默做不到**——不报错、不警告，
接收端只是永远拿不到 fd。

---

## 2. `SCM_RIGHTS` 传的到底是什么

### 2.1 是 dup，不是复制内容

| 维度 | 是否共享 |
|------|----------|
| fd **编号** | 否（各自 fd table 独立） |
| `struct file`（open file description） | **是，同一个对象** |
| 文件偏移、`O_NONBLOCK` 等状态标志 | 是 |
| `file->private_data`（驱动私有状态） | 是 |

推论：

- 对普通文件，双方**共享同一个偏移**——会互相“抢”读写位置，协议里通常配
  `pread/pwrite` 或传完立刻 `lseek`；
- 对设备，`private_data` 里的状态是共享的——这正是 logind 能靠
  `EVIOCREVOKE` / `DROP_MASTER` 收回访问权的原因
  （见 [docs/06](06-seat-devices-and-revoke.md) §2）；
- 与 `fork()` 继承的区别只在**怎么拿到**：fork 是复制 fd table，
  `SCM_RIGHTS` 是跨任意进程传递；内核里都是同一份 `struct file`。

### 2.2 一个最小的 C 例子

```c
/* 发送方 */
char byte = 'x';
struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
char cbuf[CMSG_SPACE(sizeof(int))];
struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = cbuf, .msg_controllen = sizeof(cbuf),
};
struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
c->cmsg_level = SOL_SOCKET;
c->cmsg_type  = SCM_RIGHTS;
c->cmsg_len   = CMSG_LEN(sizeof(int));
memcpy(CMSG_DATA(c), &fd, sizeof(fd));
sendmsg(sock, &msg, 0);

/* 接收方 */
char cbuf[CMSG_SPACE(sizeof(int))];
struct msghdr msg = { .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
recvmsg(sock, &msg, 0);
struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
int newfd;
memcpy(&newfd, CMSG_DATA(c), sizeof(newfd));   /* 这就是对方的 fd */
```

---

## 3. 共享内存：socket 只递门票

### 3.1 三种共享方式

| 方式 | 怎么建立 | 谁能共享 | 需要 socket 吗 |
|------|----------|----------|----------------|
| `fork()` + `MAP_SHARED|MAP_ANONYMOUS` | 父子继承同一映射 | 仅有亲缘关系的进程 | 不需要 |
| **memfd / `shm_open` / 临时文件 + `mmap(MAP_SHARED)`** | 一方创建，把 **fd 传过去**，双方各自 `mmap` | 任意进程（同机） | 传 fd 时需要（AF_UNIX） |
| SysV `shmget/shmat` | 双方用同一个 key / ID | 任意进程（同机） | 不需要 |

现代做法基本是第二种，`memfd_create(2)` 是标配。

### 3.2 Wayland 的两个例子

```
wl_shm.create_pool(fd, size)        ← fd = memfd，经 AF_UNIX 的 SCM_RIGHTS
        compositor: mmap(fd, size)  → 同一份物理页
```

- 客户端负责画，compositor 直接读同一块内存，**零拷贝**；
- `wl_buffer.release` 只负责生命周期，不搬运数据。

`zwp_linux_dmabuf_v1` 更极端：同样传 fd，但那是 GPU buffer，
**通常不能 mmap**，只能通过 DMA-BUF ioctl 在 GPU / 显示引擎之间共享。

### 3.3 跨机器为什么不行

两台机器的内存页不可能是“同一份”，只能：

- 传数据（序列化 / 压缩 / RDMA）；
- 或者共享**文件**（NFS 等）——那是两份页缓存靠协议同步，不是同一份内存。

---

## 4. 除了 `SCM_RIGHTS`，还能怎么拿到别人的 fd

| 方法 | 说明 |
|------|------|
| `pidfd_getfd(2)` | 从别的进程直接“偷”一个 fd（需要 ptrace 权限），5.6+ |
| `/proc/<pid>/fd/N` | 打开这个路径（需要权限）；对 socket / pipe 不行（`ENXIO`）|
| `SCM_PIDFD` | 5.6+，在 AF_UNIX 上传 pidfd（不是普通 fd）|
| `SCM_RIGHTS` | 唯一的通用进程间 fd 传递方式 |

跨机器时只能传“重新打开所需的信息”（路径、名字、URI），让对端自己 open。

---

## 5. 容易踩的坑

1. **`SCM_MAX_FD` = 253**：一条消息最多带 253 个 fd。
2. **`SOCK_STREAM` 必须带至少 1 字节数据**。实测（Linux 6.12）：

   | 类型 | 0 字节 payload + `SCM_RIGHTS` |
   |---|---|
   | STREAM | `sendmsg` 返回 0，接收端 `recvmsg` 仍然 would-block ❌ |
   | DGRAM / SEQPACKET | 收到空消息 + fd ✅（Linux 行为；可移植代码仍建议带 1 字节）|

3. **接收端必须用 `recvmsg()`**：用 `read()` 读会丢弃控制消息，那些 fd 直接没了
   （不会泄漏到接收方）。
4. **cmsg 缓冲区要按 `CMSG_SPACE()` 计算**：长度不够时 fd 会被截断丢弃。
5. **传的是 dup，不是拷贝**：共享文件偏移、`O_NONBLOCK` 等状态（见 §2.1）。
6. **fd 号只在各自进程内有效**：发送方是 7，接收方可能是 12。
7. **发送成功 ≠ 传到了**：TCP/UDP 会静默丢弃（见 §1.3）。

---

## 6. 本课程里的实例

| 位置 | 通道 | 传的东西 |
|------|------|----------|
| logind `TakeDevice`（[docs/06](06-seat-devices-and-revoke.md)） | D-Bus（底层 AF_UNIX） | `/dev/dri/card0`、`/dev/input/event*` 的 fd |
| Wayland `wl_shm.create_pool`（[模块 3](../module-03-pixels/)） | Wayland socket（AF_UNIX） | memfd |
| Wayland `zwp_linux_dmabuf_v1`（[模块 11](../module-11-tty-compositor/) 之后） | 同上 | DMA-BUF fd |
| 模块 2 的 `wl_surface.attach` | 同上 | 引用 pool 里的 buffer，不重复传 fd |

也正因为 AF_UNIX 天然带凭据，Wayland/DBus 才能用 `SO_PEERCRED` /
`SCM_CREDENTIALS` 做客户端身份校验——TCP 没有这个能力。

---

## 7. 常见误解

| 误解 | 事实 |
|------|------|
| “TCP 也在同一台机器上，应该能传 fd” | 不行；看的是 **socket 族**，不是物理距离 |
| “unix socket 能共享内存” | 不；共享的是 memfd/文件的页，socket 只传 fd |
| “传 fd 等于把文件内容复制过去” | 不；复制的是“打开实例”的引用，数据仍在原文件/设备里 |
| “mmap 之后 fd 可以立刻关掉” | 映射确实持有引用，但**要传 fd 就必须在关之前** |
| “sendmsg 成功就说明 fd 传到了” | 对 TCP/UDP 恰恰相反：成功但什么都没传 |
| “0 字节也能传 fd” | STREAM 上不行（实测收不到），DGRAM/SEQPACKET 才可以 |

---

## 8. 动手实验

### 8.1 三种 socket 对照（Python）

```python
import socket, array, os

fd = os.open('/etc/hostname', os.O_RDONLY)
fds = array.array('i', [fd])

def probe(sock, label):
    try:
        n = sock.sendmsg([b'x'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, fds)])
        print(f'{label}: sendmsg 成功 {n} 字节')
    except OSError as e:
        print(f'{label}: sendmsg 失败 {e.errno} {e.strerror}')

a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
probe(a, 'AF_UNIX STREAM')

srv = socket.socket(); srv.bind(('127.0.0.1', 0)); srv.listen(1)
c = socket.socket(); c.connect(srv.getsockname()); conn, _ = srv.accept()
probe(c, 'AF_INET TCP')

m, anc, _, _ = b.recvmsg(16, socket.CMSG_SPACE(16))
print('AF_UNIX 收到 ancdata:', bool(anc))          # True
m, anc, _, _ = conn.recvmsg(16, socket.CMSG_SPACE(16))
print('TCP 收到 ancdata:', bool(anc))              # False
```

### 8.2 memfd + fd 传递（C 伪代码）

```c
int fd = memfd_create("pool", MFD_CLOEXEC);
ftruncate(fd, 4096);
void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
strcpy(p, "hello");
/* 把 fd 通过 AF_UNIX 发出去；对端 mmap 同一 fd 后也能看到 "hello" */
```

### 8.3 观察真实进程

```bash
# Wayland 客户端与 compositor 的 shm 是同一个文件
ls -l /proc/$(pidof sway)/fd | grep memfd
ls -l /proc/$(pidof foot)/fd | grep memfd
# 或者看 Wayland 协议流量
WAYLAND_DEBUG=1 foot 2>&1 | grep -A2 create_pool
```

---

## 9. 选型指南

| 需求 | 方案 |
|------|------|
| 给对端一个已打开的文件/设备/socket | AF_UNIX + `SCM_RIGHTS` |
| 大量数据零拷贝共享（同机） | `memfd_create` + `mmap(MAP_SHARED)`，用 `SCM_RIGHTS` 传 fd |
| GPU buffer 跨进程共享 | DMA-BUF（传 fd + ioctl，通常不能 mmap） |
| 跨机器 | 序列化传输 / 网络文件系统 / RDMA（**没有** fd 传递） |

---

## 10. 延伸阅读

- [`unix(7)`](https://man7.org/linux/man-pages/man7/unix.7.html) — `SCM_RIGHTS` /
  `SCM_CREDENTIALS` / `SO_PEERCRED` 的权威定义
- [`cmsg(3)`](https://man7.org/linux/man-pages/man3/cmsg.3.html) —
  `CMSG_SPACE` / `CMSG_FIRSTHDR` 的正确用法
- [`memfd_create(2)`](https://man7.org/linux/man-pages/man2/memfd_create.2.html) —
  匿名共享内存的现代做法
- [`pidfd_getfd(2)`](https://man7.org/linux/man-pages/man2/pidfd_getfd.2.html) —
  从别的进程获取 fd
- [Wayland `wl_shm` 协议](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_shm) —
  pool/buffer 与 fd 传递
- [`drivers/dma-buf`](https://docs.kernel.org/driver-api/dma-buf.html) —
  DMA-BUF 的 fd 语义与跨设备共享
