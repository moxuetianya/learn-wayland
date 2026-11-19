CFLAGS  ?= -Wall -Wextra -g -O2
CFLAGS  += $(shell pkg-config --cflags libdrm)
LDFLAGS += $(shell pkg-config --libs libdrm)

all: drm-demo drm-info

drm-demo: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# 只读的旁路工具：枚举 KMS 对象 + 解码 EDID，详见 README §3.1
drm-info: drm-info.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f drm-demo drm-info

.PHONY: all clean
