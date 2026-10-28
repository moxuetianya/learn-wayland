MODULES = \
	module-01-connection \
	module-02-hello-window \
	module-03-pixels \
	module-04-input \
	module-05-protocol \
	module-06-frame-cb \
	module-07-cairo \
	module-08-server-skeleton \
	module-09-drm \
	module-10-libinput \
	module-11-tty-compositor \
	module-12-full-compositor \
	module-13-audio \
	module-14-session

.PHONY: all clean $(MODULES) client server system

all: client server system

client: module-01-connection module-02-hello-window module-03-pixels \
        module-04-input module-05-protocol module-06-frame-cb module-07-cairo

server: module-08-server-skeleton module-09-drm module-10-libinput \
        module-11-tty-compositor module-12-full-compositor

system: module-13-audio module-14-session

$(MODULES):
	$(MAKE) -C $@

clean:
	for d in $(MODULES); do $(MAKE) -C $$d clean 2>/dev/null || true; done
