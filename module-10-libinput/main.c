#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* libinput 不自己打开设备节点——它要求使用者提供这对回调。
 * 为什么：设备打开需要特权（root 或 logind 授权），libinput 把这部分
 * 留给合成器，自己保持无特权；谁有权、怎么拿 fd 由我们决定。 */
static int open_restricted(const char *path, int flags, void *user_data) {
    (void)user_data;
    int fd = open(path, flags);
    if (fd < 0)
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
    /* 约定：失败返回负的 errno（返回 0 会被当成合法 fd） */
    return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *user_data) {
    (void)user_data;
    close(fd);
}

/* 把 open/close 回调打包交给 libinput 的"接口"：
 * 本模块以 root 运行，直接 open/close 即可；生产合成器在回调里走
 * logind 授权（sd_session_take_device），普通用户身份也能拿设备。 */
static const struct libinput_interface interface = {
    .open_restricted  = open_restricted,
    .close_restricted = close_restricted,
};

static const char *key_state_str(enum libinput_key_state s) {
    switch (s) {
    case LIBINPUT_KEY_STATE_PRESSED:  return "DOWN";
    case LIBINPUT_KEY_STATE_RELEASED: return "UP";
    default:                          return "?";
    }
}

static const char *button_state_str(enum libinput_button_state s) {
    switch (s) {
    case LIBINPUT_BUTTON_STATE_PRESSED:  return "DOWN";
    case LIBINPUT_BUTTON_STATE_RELEASED: return "UP";
    default:                             return "?";
    }
}

static int handle_events(struct libinput *li) {
    int quit = 0;
    /* 两段式的第一段：fd 可读 ≠ 事件就绪。dispatch 让 libinput 从各设备
     * fd 读原始 evdev 数据并跑完内部状态机（去抖/加速/手势判定/滚动
     * 合成），处理结果才会进事件队列。跳过这步直接 get_event 只会
     * 取到旧的甚至取空——"明明动了鼠标却没事件"多半是它。 */
    libinput_dispatch(li);

    /* 第二段：把事件队列取空。每个事件用完必须 libinput_event_destroy，
     * libinput 不会替调用者释放。 */
    struct libinput_event *ev;
    while ((ev = libinput_get_event(li))) {
        enum libinput_event_type type = libinput_event_get_type(ev);
        struct libinput_device *dev = libinput_event_get_device(ev);

        switch (type) {

        /* DEVICE_ADDED/REMOVED 在 assign_seat 后立即成批涌入（libinput
         * 把"枚举"实现成回放一遍添加事件），之后由 udev 监视器负责
         * 热插拔——我们不用自己盯 /dev/input 的增删。 */
        case LIBINPUT_EVENT_DEVICE_ADDED:
            printf("[dev] added:   %s\n", libinput_device_get_name(dev));
            break;

        case LIBINPUT_EVENT_DEVICE_REMOVED:
            printf("[dev] removed: %s\n", libinput_device_get_name(dev));
            break;

        case LIBINPUT_EVENT_KEYBOARD_KEY: {
            /* 注意拿到的是 linux keycode（如 KEY_ESC==1），不是 keysym：
             * keycode→keysym→字符 的翻译是 xkb 的职责（模块 4 客户端侧
             * 见过，模块 12 会在合成器侧接上） */
            struct libinput_event_keyboard *k =
                libinput_event_get_keyboard_event(ev);
            uint32_t key   = libinput_event_keyboard_get_key(k);
            uint32_t state = libinput_event_keyboard_get_key_state(k);
            printf("[key] keycode=%3u  %-4s\n", key, key_state_str(state));
            if (key == KEY_ESC && state == LIBINPUT_KEY_STATE_PRESSED)
                quit = 1;
            break;
        }

        case LIBINPUT_EVENT_POINTER_MOTION: {
            /* dx/dy 已经是 libinput 套完加速曲线的相对运动量（按设备
             * 分辨率换算过），不再是鼠标/触摸板的原始计数——把"设备
             * 差异+加速"从每个合成器收编进库，正是 libinput 存在的意义 */
            struct libinput_event_pointer *p =
                libinput_event_get_pointer_event(ev);
            double dx = libinput_event_pointer_get_dx(p);
            double dy = libinput_event_pointer_get_dy(p);
            printf("[ptr]  motion  dx=%+.2f  dy=%+.2f\n", dx, dy);
            break;
        }

        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
            struct libinput_event_pointer *p =
                libinput_event_get_pointer_event(ev);
            double x = libinput_event_pointer_get_absolute_x(p);
            double y = libinput_event_pointer_get_absolute_y(p);
            printf("[ptr]  abs     x=%.2f  y=%.2f\n", x, y);
            break;
        }

        case LIBINPUT_EVENT_POINTER_BUTTON: {
            struct libinput_event_pointer *p =
                libinput_event_get_pointer_event(ev);
            uint32_t btn    = libinput_event_pointer_get_button(p);
            uint32_t state  = libinput_event_pointer_get_button_state(p);
            uint32_t seat_c = libinput_event_pointer_get_seat_button_count(p);
            printf("[ptr]  button  btn=%u  %-4s  seat_count=%u\n",
                   btn, button_state_str(state), seat_c);
            break;
        }

        case LIBINPUT_EVENT_POINTER_AXIS: {
            /* 滚轮每"一格"被归一化为 1.0 的离散刻度，触摸板双指滚动则
             * 是以像素为单位的连续值——两种截然不同的设备在这里统一
             * 成同一种 axis 事件，合成器无需区分来源 */
            struct libinput_event_pointer *p =
                libinput_event_get_pointer_event(ev);
            if (libinput_event_pointer_has_axis(
                    p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
                double v = libinput_event_pointer_get_axis_value(
                    p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
                printf("[ptr]  axis    VERT  %.2f\n", v);
            }
            if (libinput_event_pointer_has_axis(
                    p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)) {
                double v = libinput_event_pointer_get_axis_value(
                    p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
                printf("[ptr]  axis    HORIZ %.2f\n", v);
            }
            break;
        }

        case LIBINPUT_EVENT_TOUCH_DOWN: {
            struct libinput_event_touch *t =
                libinput_event_get_touch_event(ev);
            uint32_t slot = libinput_event_touch_get_slot(t);
            uint32_t seat_slot = libinput_event_touch_get_seat_slot(t);
            double x = libinput_event_touch_get_x(t);
            double y = libinput_event_touch_get_y(t);
            printf("[tch]  down    slot=%u  seat_slot=%u  x=%.2f  y=%.2f\n",
                   slot, seat_slot, x, y);
            break;
        }

        case LIBINPUT_EVENT_TOUCH_UP: {
            struct libinput_event_touch *t =
                libinput_event_get_touch_event(ev);
            uint32_t slot      = libinput_event_touch_get_slot(t);
            uint32_t seat_slot = libinput_event_touch_get_seat_slot(t);
            printf("[tch]  up      slot=%u  seat_slot=%u\n", slot, seat_slot);
            break;
        }

        case LIBINPUT_EVENT_TOUCH_MOTION: {
            struct libinput_event_touch *t =
                libinput_event_get_touch_event(ev);
            uint32_t slot = libinput_event_touch_get_slot(t);
            double x      = libinput_event_touch_get_x(t);
            double y      = libinput_event_touch_get_y(t);
            printf("[tch]  motion  slot=%u  x=%.2f  y=%.2f\n", slot, x, y);
            break;
        }

        case LIBINPUT_EVENT_TOUCH_FRAME:
            printf("[tch]  frame\n");
            break;

        case LIBINPUT_EVENT_TOUCH_CANCEL:
            printf("[tch]  cancel\n");
            break;

        default:
            break;
        }

        libinput_event_destroy(ev);
    }
    return quit;
}

int main(void) {
    struct libinput *li;
    /* udev 上下文只用于设备发现：libinput 借它枚举属于目标 seat 的输入
     * 设备并监听热插拔；事件数据本身仍从各设备的 evdev fd 读取 */
    struct udev *udev = udev_new();
    if (!udev) {
        fprintf(stderr, "Failed to create udev context\n");
        return 1;
    }

    printf("=== 模块 10: libinput 输入处理 ===\n");

    li = libinput_udev_create_context(&interface, NULL, udev);
    if (!li) {
        fprintf(stderr, "Failed to create libinput context\n");
        udev_unref(udev);
        return 1;
    }

    /* "seat0" 是主 seat 的约定名：一组可同时使用的键盘+指针+屏幕。
     * 分配后 libinput 立即枚举该 seat 的设备并回放 DEVICE_ADDED。
     * 需要 root/input 组权限，否则 open_restricted 会连番失败。 */
    if (libinput_udev_assign_seat(li, "seat0") != 0) {
        fprintf(stderr, "Failed to assign seat0 -- "
                        "run as root or member of 'input' group\n");
        libinput_unref(li);
        udev_unref(udev);
        return 1;
    }

    struct pollfd fds[1];
    /* libinput_get_fd 只承诺"该读了"，不承诺"已有完整事件"——唤醒后
     * 必须先 dispatch 再 get_event（见 handle_events）。
     * 模块 11 会把这个 fd 挂进 wl_event_loop_add_fd，让协议、输入、
     * 输出共用一个主循环 */
    fds[0].fd     = libinput_get_fd(li);
    fds[0].events = POLLIN;

    printf("Listening for input events ... (press Esc to exit)\n");

    while (1) {
        if (poll(fds, 1, -1) < 0) {
            perror("poll");
            break;
        }
        if (fds[0].revents & POLLIN) {
            if (handle_events(li))
                break;
        }
    }

    libinput_unref(li);
    udev_unref(udev);
    return 0;
}
