WAYLAND_PROTOCOLS_DIR ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

CFLAGS  ?= -Wall -Wextra -g -O2
CFLAGS  += $(shell pkg-config --cflags wayland-server libdrm libinput libudev xkbcommon)
LDFLAGS += $(shell pkg-config --libs wayland-server libdrm libinput libudev xkbcommon) -lm

all: compositor

xdg-shell-server-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner server-header $< $@

xdg-shell-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@

compositor.o: compositor.c xdg-shell-server-protocol.h xdg-shell-protocol.c
	$(CC) $(CFLAGS) -c -o $@ $<

xdg-shell-protocol.o: xdg-shell-protocol.c xdg-shell-server-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

compositor: compositor.o xdg-shell-protocol.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f compositor *.o xdg-shell-server-protocol.h xdg-shell-protocol.c

.PHONY: all clean
