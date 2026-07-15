CFLAGS  ?= -Wall -Wextra -g -O2
LDFLAGS += $(shell pkg-config --libs wayland-client)

all: list-globals

list-globals: main.o
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

main.o: main.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags wayland-client) -c -o $@ $<

clean:
	rm -f list-globals *.o

.PHONY: all clean
