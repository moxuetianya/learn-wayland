CFLAGS  ?= -Wall -Wextra -g -O2
CFLAGS  += $(shell pkg-config --cflags wayland-server libdrm libinput libudev)
LDFLAGS += $(shell pkg-config --libs wayland-server libdrm libinput libudev) -lm

all: compositor

compositor: compositor.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f compositor

run: compositor
	sudo ./compositor

.PHONY: all clean run
