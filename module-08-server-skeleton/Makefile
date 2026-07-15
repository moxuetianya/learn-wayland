CFLAGS  ?= -Wall -Wextra -g -O2
CFLAGS  += $(shell pkg-config --cflags wayland-server)
LDFLAGS += $(shell pkg-config --libs wayland-server)

all: hello-server

hello-server-protocol.h: hello.xml
	wayland-scanner server-header $< $@

hello-client-protocol.h: hello.xml
	wayland-scanner client-header $< $@

hello-protocol.c: hello.xml
	wayland-scanner private-code $< $@

hello-server.o: hello-server.c hello-server-protocol.h hello-protocol.c
	$(CC) $(CFLAGS) -c -o $@ $<

hello-protocol.o: hello-protocol.c hello-server-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

hello-server: hello-server.o hello-protocol.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f hello-server *.o *-protocol.h hello-protocol.c

.PHONY: all clean
