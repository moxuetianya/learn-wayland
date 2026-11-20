CFLAGS  ?= -Wall -Wextra -g -O2
CFLAGS  += $(shell pkg-config --cflags wayland-server libdrm libinput libudev)
LDFLAGS += $(shell pkg-config --libs wayland-server libdrm libinput libudev) -lm

CLIENT_CFLAGS  += $(shell pkg-config --cflags wayland-client)
CLIENT_LDFLAGS += $(shell pkg-config --libs wayland-client)

all: compositor client

compositor: compositor.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# 纯 wl_surface 测试客户端: weston >= 6 的 weston-simple-shm 依赖
# xdg_wm_base / zwp_fullscreen_shell_v1, 连不上本模块 (见 README 常见坑)
client: client.c
	$(CC) $(CFLAGS) $(CLIENT_CFLAGS) -o $@ $< $(CLIENT_LDFLAGS)

clean:
	rm -f compositor client

run: compositor
	sudo ./compositor

.PHONY: all clean run
