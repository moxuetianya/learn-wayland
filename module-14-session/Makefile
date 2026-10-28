CFLAGS  ?= -Wall -Wextra -g -O2
CFLAGS  += $(shell pkg-config --cflags libsystemd)
LDFLAGS += $(shell pkg-config --libs libsystemd)

all: session-info

session-info: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f session-info

run: session-info
	./session-info me

.PHONY: all clean run
