CFLAGS  ?= -Wall -Wextra -g -O2
CFLAGS  += $(shell pkg-config --cflags wayland-client wayland-server)
LDFLAGS += $(shell pkg-config --libs wayland-client wayland-server)

all: hello-client

hello-client-protocol.h: hello.xml
	wayland-scanner client-header $< $@

hello-protocol.c: hello.xml
	wayland-scanner private-code $< $@

main.o: main.c hello-client-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

hello-protocol.o: hello-protocol.c hello-client-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

hello-client: main.o hello-protocol.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f hello-client *.o hello-client-protocol.h hello-protocol.c

.PHONY: all clean
