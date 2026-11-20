/* main.c — 模块 14: 登录与会话观察 (systemd-logind)
 *
 * 三个子命令, 对应 docs/04-login-and-session.md 的三块原理:
 *
 *   ./session-info sessions      列出所有 seat 与其上的会话 (§3.1/§3.2)
 *   ./session-info me            当前进程所属会话的完整画像 + 设备访问实验 (§3.3/§6.1)
 *   ./session-info take-control  尝试 D-Bus 调用 logind 的 TakeControl ——
 *                                生产 compositor 免 sudo 接管显示的正道 (§5)
 *
 * 另有 inspect.sh: 纯 shell 的系统性观察清单。
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>

static void free_strv(char **v) {
    if (!v) return;
    for (char **s = v; *s; s++) free(*s);
    free(v);
}

/* 优雅取会话属性: 失败返回 "?" 而不是 NULL */
static char *sess_get(const char *id, int (*fn)(const char *, char **)) {
    char *out = NULL;
    if (fn(id, &out) < 0 || !out)
        return strdup("?");
    return out;
}

static const char *class_color(const char *class) {
    /* 图形会话 vs 文本会话 vs 服务类 */
    if (strcmp(class, "user") == 0)  return "user  (常规用户会话)";
    if (strcmp(class, "greeter") == 0) return "greeter(显示管理器登录界面!)";
    if (strcmp(class, "lock-screen") == 0) return "lock   (锁屏会话)";
    if (strcmp(class, "background") == 0) return "bg     (无 seat 后台)";
    return class;
}

/* ================================================================== */
/* sessions: seat → session 全景                                       */
/* ================================================================== */

static int cmd_sessions(void) {
    char **seats = NULL;
    int n_seats = sd_get_seats(&seats);

    printf("=== systemd-logind: seat 与 session 全景 ===\n\n");

    if (n_seats < 0) {
        fprintf(stderr, "sd_get_seats 失败: %s\n", strerror(-n_seats));
        return 1;
    }
    if (n_seats == 0) {
        printf("(无 seat —— 可能在容器里, logind 不可用)\n");
        return 0;
    }

    for (int s = 0; s < n_seats; s++) {
        char **sessions = NULL;
        uid_t *uids = NULL;
        unsigned n_uids = 0;
        int n_sess = sd_seat_get_sessions(seats[s], &sessions, &uids, &n_uids);

        printf("seat \"%s\" 上有 %d 个会话:\n", seats[s], n_sess > 0 ? n_sess : 0);

        for (int i = 0; i < n_sess; i++) {
            const char *id = sessions[i];
            char *type = sess_get(id, sd_session_get_type);
            char *cls  = sess_get(id, sd_session_get_class);
            char *st   = sess_get(id, sd_session_get_state);
            char *tty  = sess_get(id, sd_session_get_tty);
            char *dspl = sess_get(id, sd_session_get_display);
            char *desk = sess_get(id, sd_session_get_desktop);
            int active = 0;
            uid_t uid = 0;
            struct passwd *pw;
            char *user;

            active = sd_session_is_active(id) > 0;
            sd_session_get_uid(id, &uid);
            /* sd_session_get_username 是 systemd 252+ API (dut2 是 249 编不过);
             * 通用写法: 先取 uid, 再 getpwuid 查用户名 */
            pw = getpwuid(uid);
            user = strdup(pw ? pw->pw_name : "?");

            printf("  ── session %s %s ─────────────────────────\n",
                   id, active ? "★ active" : "(后台, 被冻结)");
            printf("     %-8s %s\n", "active:",  active ? "yes ★ (拥有 seat 前台)" : "no  (切走时被冻结)");
            printf("     %-8s %s (uid=%u)\n", "user:", user, uid);
            printf("     %-8s %s\n", "type:", type);
            printf("     %-8s %s\n", "class:", class_color(cls));
            printf("     %-8s %s\n", "state:", st);
            printf("     %-8s %s%s%s\n", "tty/dspl:",
                   tty && strcmp(tty, "?") ? tty : "",
                   (tty && strcmp(tty, "?") && dspl && strcmp(dspl, "?")) ? " / " : "",
                   dspl && strcmp(dspl, "?") ? dspl : "(无)");
            printf("     %-8s %s\n", "desktop:", desk);

            free(type); free(cls); free(st); free(tty); free(dspl); free(desk); free(user);
        }
        printf("\n");
        free_strv(sessions);
        free(uids);
    }

    free_strv(seats);
    printf("解读: Type=wayland 的会话就是你现在的图形桌面 (由显示管理器创建);\n");
    printf("     切到 tty3 登录会产生一个 type=tty 的新会话 —— Ctrl+Alt+F3/F2 切换的\n");
    printf("     本质就是这两个会话轮流 active (docs/04 §3.2)。\n");
    return 0;
}

/* ================================================================== */
/* me: 当前会话画像 + 设备访问实验                                      */
/* ================================================================== */

static void try_open(const char *path) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        printf("     %-24s ✓ 可读写 (logind ACL 已授权给你的活跃会话)\n", path);
        close(fd);
    } else {
        int fr = open(path, O_RDONLY | O_CLOEXEC);
        if (fr >= 0) {
            printf("     %-24s ~ 只读 (write 被拒: %s)\n", path, strerror(errno));
            close(fr);
        } else {
            printf("     %-24s ✗ 不可打开 (%s)\n", path, strerror(errno));
        }
    }
}

static int cmd_me(void) {
    char *session = NULL;
    int r = sd_pid_get_session(getpid(), &session);

    printf("=== 当前进程的会话画像 ===\n\n");

    if (r < 0 || !session) {
        printf("本进程不属于任何 logind 会话 (r=%d)。\n", r);
        printf("典型原因: 在 ssh/cron/容器/systemd 服务里运行 —— 没有「坐在 seat 前」。\n");
        printf("对照 docs/04 §2: 登录(login)与起进程(shell)的本质区别就在这里。\n");
        session = NULL;
    } else {
        int active = 0;
        uid_t uid = 0;
        char *type = sess_get(session, sd_session_get_type);
        char *cls  = sess_get(session, sd_session_get_class);
        char *tty  = sess_get(session, sd_session_get_tty);
        char *desk = sess_get(session, sd_session_get_desktop);
        unsigned vt = 0;
        char vtbuf[16];
        char *vtstr;

        if (sd_session_get_vt(session, &vt) < 0 || vt == 0) {
            snprintf(vtbuf, sizeof(vtbuf), "(无)");
        } else {
            snprintf(vtbuf, sizeof(vtbuf), "vt%u", vt);
        }
        vtstr = vtbuf;

        active = sd_session_is_active(session) > 0;
        sd_session_get_uid(session, &uid);

        printf("  session id : %s\n", session);
        printf("  active     : %s\n", active ? "yes" : "no");
        printf("  uid        : %u\n", uid);
        printf("  type/class : %s / %s\n", type, cls);
        printf("  tty / vt   : %s / %s\n", tty, vtstr);
        printf("  desktop    : %s\n", desk);

        free(type); free(cls); free(tty); free(desk);
    }

    /* --- Wayland 服务发现三要素 (docs/04 §6.1) --- */
    printf("\n--- Wayland socket 服务发现 ---\n");
    const char *rdir = getenv("XDG_RUNTIME_DIR");
    const char *wdsp = getenv("WAYLAND_DISPLAY");
    printf("  XDG_RUNTIME_DIR = %s   (logind 建会话时创建的 tmpfs)\n",
           rdir ? rdir : "(未设置! 连不上 Wayland)");
    printf("  WAYLAND_DISPLAY = %s\n", wdsp ? wdsp : "(未设置, libwayland 将试 wayland-0..wayland-32)");

    if (rdir) {
        DIR *d = opendir(rdir);
        if (d) {
            struct dirent *e;
            printf("  运行时目录里的 wayland socket:\n");
            int found = 0;
            while ((e = readdir(d))) {
                if (strncmp(e->d_name, "wayland-", 8) == 0) {
                    printf("     • %s/%s\n", rdir, e->d_name);
                    found++;
                }
            }
            if (!found) printf("     (无 —— 此会话没有 compositor)\n");
            closedir(d);
        }
        printf("  其他值得认识的文件: bus(D-Bus) pipewire-0(音频) doc(门户)\n");
    }

    /* --- 设备访问实验 (docs/04 §3.3) --- */
    printf("\n--- 设备访问实验 ---\n");
    if (access("/dev/dri", F_OK) == 0) {
        /* 显卡不一定叫 card0 (virtio-gpu 常为 card1), 扫描前 16 个 */
        char path[32];
        int found = 0;
        for (int i = 0; i < 16; i++) {
            snprintf(path, sizeof(path), "/dev/dri/card%d", i);
            if (access(path, F_OK) == 0) { try_open(path); found++; }
        }
        for (int i = 0; i < 16 && i < 2; i++) {
            snprintf(path, sizeof(path), "/dev/dri/renderD%d", 128 + i);
            if (access(path, F_OK) == 0) try_open(path);
        }
        if (!found) printf("     (存在 /dev/dri 但没有 card* 节点)\n");
    } else {
        printf("     /dev/dri 不存在 (无显卡或容器)\n");
    }
    try_open("/dev/input/event0");
    printf("\n  解读: 能打开 = 会话活跃 + logind 的 uaccess ACL 加持;\n");
    printf("        ssh 里跑这些通常全部 ✗。 drm master 是另一层互斥 (模块 9)。\n");

    free(session);
    return 0;
}

/* ================================================================== */
/* take-control: logind D-Bus 正道演示                                  */
/* ================================================================== */

static int cmd_take_control(void) {
    sd_bus *bus = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    char *session = NULL;
    const char *path = NULL;
    int r;

    printf("=== logind TakeControl 演示 ===\n");
    printf("生产 compositor (Mutter/wlroots) 免 sudo 接管显示的官方途径:\n");
    printf("Session.TakeControl(false) 之后再用 TakeDevice 逐个领设备。\n\n");

    r = sd_pid_get_session(getpid(), &session);
    if (r < 0 || !session) {
        printf("✗ 本进程不在会话里 (%s)。\n", r < 0 ? strerror(-r) : "?");
        printf("  TakeControl 只能由「坐在 seat 前」的进程调用 —— 这正是它的安全意义:\n");
        printf("  键盘前的人才配接管屏幕。\n");
        return 1;
    }
    printf("本进程会话: %s\n", session);

    r = sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "无法连接系统总线: %s\n", strerror(-r));
        free(session);
        return 1;
    }

    /* 1. 向 Manager 查询本会话的对象路径 */
    r = sd_bus_call_method(bus,
                           "org.freedesktop.login1",
                           "/org/freedesktop/login1",
                           "org.freedesktop.login1.Manager",
                           "GetSession",
                           &err, &reply, "s", session);
    if (r < 0) {
        fprintf(stderr, "GetSession 失败: %s\n", err.message);
        goto out;
    }
    r = sd_bus_message_read(reply, "o", &path);
    if (r < 0) {
        fprintf(stderr, "读取对象路径失败: %s\n", strerror(-r));
        goto out;
    }
    printf("会话对象路径: %s\n\n", path);

    /* 2. 请求 TakeControl (force=false: 只接管自己会话的设备) */
    r = sd_bus_call_method(bus,
                           "org.freedesktop.login1",
                           path,
                           "org.freedesktop.login1.Session",
                           "TakeControl",
                           &err, NULL, "b", false);
    if (r < 0) {
        printf("✗ TakeControl 被拒绝: %s\n", err.message);
        printf("\n  这是预期内的教学结果。拒绝原因通常是:\n");
        printf("  · polkit 策略未授权当前用户 (默认仅特定组/活动会话可调用)\n");
        printf("  · 会话不是 active (切到别的 VT 了)\n");
        printf("  · 已有程序持有控制权 (比如你正在跑的桌面 compositor!)\n");
        printf("\n  对照: GDM 登录的 Mutter / wlroots 系 compositor 成功走的正是这条路。\n");
        printf("  课程模块 11/12 用 sudo 绕开了这一切 —— 权力 vs 便利的取舍。\n");
    } else {
        printf("✓ TakeControl 成功! 本进程现在拥有会话的设备控制权。\n");
        printf("  接下来生产 compositor 会: TakeDevice('drm', card0) → drmSetMaster\n");
        printf("  → TakeDevice('input', event*) ... 然后主动 ReleaseControl 归还。\n");
        sd_bus_call_method(bus, "org.freedesktop.login1", path,
                           "org.freedesktop.login1.Session",
                           "ReleaseControl", NULL, NULL, "");
    }

out:
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    free(session);
    return 0;
}

/* ================================================================== */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "用法: %s <sessions|me|take-control>\n"
            "  sessions      列出所有 seat 与会话\n"
            "  me            当前会话画像 + 设备访问实验\n"
            "  take-control  演示 logind 设备接管正道\n"
            "配合 ./inspect.sh 使用 (系统性观察清单)\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "sessions") == 0)     return cmd_sessions();
    if (strcmp(argv[1], "me") == 0)           return cmd_me();
    if (strcmp(argv[1], "take-control") == 0) return cmd_take_control();

    fprintf(stderr, "未知子命令: %s\n", argv[1]);
    return 1;
}
