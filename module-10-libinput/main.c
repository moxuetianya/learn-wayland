#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int open_restricted(const char *path, int flags, void *user_data) {
    (void)user_data;
    int fd = open(path, flags);
    if (fd < 0)
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
    return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *user_data) {
    (void)user_data;
    close(fd);
}

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
    libinput_dispatch(li);

    struct libinput_event *ev;
    while ((ev = libinput_get_event(li))) {
        enum libinput_event_type type = libinput_event_get_type(ev);
        struct libinput_device *dev = libinput_event_get_device(ev);

        switch (type) {

        case LIBINPUT_EVENT_DEVICE_ADDED:
            printf("[dev] added:   %s\n", libinput_device_get_name(dev));
            break;

        case LIBINPUT_EVENT_DEVICE_REMOVED:
            printf("[dev] removed: %s\n", libinput_device_get_name(dev));
            break;

        case LIBINPUT_EVENT_KEYBOARD_KEY: {
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

    if (libinput_udev_assign_seat(li, "seat0") != 0) {
        fprintf(stderr, "Failed to assign seat0 -- "
                        "run as root or member of 'input' group\n");
        libinput_unref(li);
        udev_unref(udev);
        return 1;
    }

    struct pollfd fds[1];
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
