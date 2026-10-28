CFLAGS  ?= -Wall -Wextra -g -O2
CFLAGS  += $(shell pkg-config --cflags libinput libudev)
LDFLAGS += $(shell pkg-config --libs libinput libudev)

all: libinput-demo

libinput-demo: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f libinput-demo

.PHONY: all clean
