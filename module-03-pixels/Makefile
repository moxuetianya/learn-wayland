WAYLAND_PROTOCOLS_DIR ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

CFLAGS  ?= -Wall -Wextra -g -O2
CFLAGS  += $(shell pkg-config --cflags wayland-client)
LDFLAGS += $(shell pkg-config --libs wayland-client) -lm

all: pixel-anim

xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner client-header $< $@

%.o: %.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

pixel-anim: main.o
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f pixel-anim *.o xdg-shell-client-protocol.h

.PHONY: all clean
