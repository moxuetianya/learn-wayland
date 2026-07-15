WAYLAND_PROTOCOLS_DIR ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

CFLAGS  ?= -Wall -Wextra -g -O2
CFLAGS  += $(shell pkg-config --cflags wayland-client cairo)
LDFLAGS += $(shell pkg-config --libs wayland-client cairo) -lm

all: cairo-demo

xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner client-header $< $@

xdg-shell-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@

%.o: %.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

cairo-demo: main.o xdg-shell-protocol.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f cairo-demo *.o xdg-shell-client-protocol.h xdg-shell-protocol.c

.PHONY: all clean
