CFLAGS  ?= -Wall -Wextra -g -O2
CFLAGS  += $(shell pkg-config --cflags alsa libpipewire-0.3)
LDFLAGS += $(shell pkg-config --libs alsa libpipewire-0.3) -lm

all: audio-lab

audio-lab: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f audio-lab

run: audio-lab
	./audio-lab alsa-list

.PHONY: all clean run
