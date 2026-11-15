# SPDX-License-Identifier: GPL-3.0-or-later
# Built inside the arm64 Debian bullseye container (scripts/docker-build.sh).

CC ?= gcc
BUILD ?= build
CFLAGS ?= -O2 -g
HARDEN = -fstack-protector-strong -D_FORTIFY_SOURCE=2
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
	-Werror=implicit-function-declaration -Werror=incompatible-pointer-types \
	$(HARDEN) -D_GNU_SOURCE -Iinclude -Isrc
LDFLAGS += -Wl,-z,relro,-z,now

TOOLS = $(BUILD)/tsp-probe $(BUILD)/tsp-videotest $(BUILD)/tsp-mdns
ENGINE = $(BUILD)/moonlight-tsp-stream
UI = $(BUILD)/moonlight-tsp-ui

.PHONY: all tools engine ui deps clean check
all: tools engine ui
tools: $(TOOLS)
engine: $(ENGINE)
ui: $(UI)

# Static OpenSSL 3.5 and libcurl for the stream engine (built once).
DEPS = $(BUILD)/deps
DEPS_STAMP = $(DEPS)/.built
deps: $(DEPS_STAMP)
# The script skips the build when its own versioned stamp is present.
$(DEPS_STAMP): scripts/build-deps.sh
	SRC=$(CURDIR) DEPS=$(DEPS) DEPS_WORK=$(BUILD)/deps-build \
		sh scripts/build-deps.sh

$(BUILD):
	mkdir -p $@

$(BUILD)/tsp-probe: tools/tsp-probe.c include/sunxi_disp_abi.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/tsp-probe.c $(LDFLAGS)

# The vendor SBM parser must bind to our SemTimedWait (src/cedar_sync.c).
CEDAR_LDFLAGS = -Wl,--export-dynamic-symbol=SemTimedWait -ldl -lpthread
# Every library's close() must bind to the engine's guard (src/fdguard.c).
FDGUARD_LDFLAGS = -Wl,--export-dynamic-symbol=close -ldl

VIDEO_SRCS = src/cedar.c src/cedar_sync.c src/disp.c src/g2d_rot.c src/ion.c
VIDEO_HDRS = src/cedar.h src/cedar_sync.h src/disp.h src/g2d_rot.h src/ion.h \
	include/cedarc_abi.h include/sunxi_disp_abi.h include/sunxi_g2d_abi.h

$(BUILD)/tsp-videotest: tools/tsp-videotest.c $(VIDEO_SRCS) $(VIDEO_HDRS) \
		| $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/tsp-videotest.c $(VIDEO_SRCS) $(LDFLAGS) \
		$(CEDAR_LDFLAGS)

# ---- third-party code, compiled with upstream's own warning level -------
TP = third_party/src
COMMON = $(TP)/moonlight-common-c
ENET = $(TP)/enet
NANORS = $(TP)/nanors
GS = $(TP)/moonlight-embedded/libgamestream

TP_SRCS = $(wildcard $(COMMON)/src/*.c) \
	$(filter-out $(ENET)/win32.c,$(wildcard $(ENET)/*.c)) \
	$(NANORS)/rs.c $(NANORS)/deps/obl/oblas_common.c \
	$(NANORS)/deps/obl/oblas_lite.c \
	$(GS)/client.c $(GS)/http.c $(GS)/mkcert.c $(GS)/xml.c
TP_OBJS = $(patsubst $(TP)/%.c,$(BUILD)/obj/tp/%.o,$(TP_SRCS))

# enet's configure checks, all true for glibc; HAVE_CLOCK_GETTIME makes
# common-c use CLOCK_MONOTONIC_RAW instead of the adjustable wall clock.
TP_DEFS = -DNDEBUG -DHAS_SOCKLEN_T=1 -DHAS_FCNTL=1 -DHAS_IOCTL=1 \
	-DHAS_POLL=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 \
	-DHAS_GETHOSTBYNAME_R=1 -DHAS_GETHOSTBYADDR_R=1 -DHAS_INET_PTON=1 \
	-DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 -DHAVE_CLOCK_GETTIME=1
TP_INCS = -I$(COMMON)/src -I$(ENET)/include -I$(NANORS) -I$(NANORS)/deps \
	-I$(NANORS)/deps/obl -I$(GS) -I$(DEPS)/include
TP_CFLAGS = -O2 -g -std=gnu11 -Wall -Wno-unused-parameter \
	-Wno-maybe-uninitialized -Wno-deprecated-declarations -Wno-pointer-sign $(HARDEN) \
	$(TP_DEFS) $(TP_INCS)

$(BUILD)/obj/tp/%.o: $(TP)/%.c $(DEPS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(TP_CFLAGS) -c -o $@ $<

# ---- engine ------------------------------------------------------------
ENGINE_SRCS = src/stream_main.c src/video_cedar.c src/audio_alsa.c \
	src/input_evdev.c src/wifi_ps.c src/fdguard.c $(VIDEO_SRCS)
ENGINE_OBJS = $(patsubst src/%.c,$(BUILD)/obj/engine/%.o,$(ENGINE_SRCS))
ENGINE_CFLAGS = $(CFLAGS) -I$(COMMON)/src -I$(GS) -I$(DEPS)/include

$(BUILD)/obj/engine/%.o: src/%.c $(VIDEO_HDRS) src/video_cedar.h \
		src/audio_alsa.h src/input_evdev.h src/wifi_ps.h src/fdguard.h \
		$(DEPS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(ENGINE_CFLAGS) -c -o $@ $<

# The device's libasound.so.2 (alsa-lib 1.1.4.1) has no symbol versions,
# so linking against Debian's versioned one makes ld.so warn at every start.
# Link against an unversioned stub with the same soname instead; the plain
# names on the device are the current (0.9.0rc4) hw_params API.
ASOUND_STUB = $(BUILD)/stub/libasound.so
$(ASOUND_STUB): $(BUILD)/obj/engine/audio_alsa.o
	@mkdir -p $(dir $@)
	nm -u $< | awk '$$2 ~ /^snd_/ { print "void " $$2 "(void) {}" }' \
		> $(BUILD)/stub/asound.c
	$(CC) -shared -fPIC -Wl,-soname,libasound.so.2 -o $@ \
		$(BUILD)/stub/asound.c

# Non-vendor libraries are static (the device's OpenWrt-built OpenSSL and
# curl do not match Debian's symbol versions); ALSA, glibc and the CedarC
# vendor libraries (dlopened) come from the device.
STATIC_LIBS = $(DEPS)/lib/libcurl.a $(DEPS)/lib/libssl.a \
	$(DEPS)/lib/libcrypto.a
$(ENGINE): $(ENGINE_OBJS) $(TP_OBJS) $(ASOUND_STUB) | $(BUILD)
	$(CC) -o $@ -Wl,--gc-sections $(ENGINE_OBJS) $(TP_OBJS) \
		$(STATIC_LIBS) -Wl,-Bstatic -lexpat -luuid -lopus \
		-Wl,-Bdynamic -L$(BUILD)/stub -lasound -lm $(LDFLAGS) \
		$(CEDAR_LDFLAGS) $(FDGUARD_LDFLAGS)

# ---- UI ----------------------------------------------------------------
# SDL2 and SDL2_ttf come from the device (/usr/trimui/lib: SDL 2.26.5 with
# the Mali fbdev driver). Debian's headers (2.0.14) are older, so only API
# the device has is used; like ALSA, the program links against unversioned
# stubs with the device sonames.
UI_SRCS = src/ui_main.c src/ui_draw.c src/ui_pad.c src/ui_engine.c \
	src/ui_config.c src/ui_mdns.c
UI_HDRS = src/ui_draw.h src/ui_pad.h src/ui_engine.h src/ui_config.h \
	src/ui_mdns.h src/input_evdev.h
UI_OBJS = $(patsubst src/%.c,$(BUILD)/obj/ui/%.o,$(UI_SRCS))
SDL_CFLAGS = $(shell pkg-config --cflags sdl2 SDL2_ttf)

$(BUILD)/obj/ui/%.o: src/%.c $(UI_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-format-truncation $(SDL_CFLAGS) -c -o $@ $<

SDL_STUB = $(BUILD)/stub/libSDL2.so
TTF_STUB = $(BUILD)/stub/libSDL2_ttf.so
$(SDL_STUB) $(TTF_STUB): $(UI_OBJS)
	@mkdir -p $(BUILD)/stub
	nm -u $(UI_OBJS) | awk '$$2 ~ /^SDL_/ { print "void " $$2 "(void) {}" }' \
		| sort -u > $(BUILD)/stub/sdl2.c
	nm -u $(UI_OBJS) | awk '$$2 ~ /^TTF_/ { print "void " $$2 "(void) {}" }' \
		| sort -u > $(BUILD)/stub/sdl2_ttf.c
	$(CC) -shared -fPIC -Wl,-soname,libSDL2-2.0.so.0 -o $(SDL_STUB) \
		$(BUILD)/stub/sdl2.c
	$(CC) -shared -fPIC -Wl,-soname,libSDL2_ttf-2.0.so.0 -o $(TTF_STUB) \
		$(BUILD)/stub/sdl2_ttf.c

$(UI): $(UI_OBJS) $(SDL_STUB) $(TTF_STUB) | $(BUILD)
	$(CC) -o $@ $(UI_OBJS) -L$(BUILD)/stub -lSDL2_ttf -lSDL2 -lm $(LDFLAGS)

# A host build for screenshots in the container (Debian's SDL, dummy video).
$(BUILD)/moonlight-tsp-ui-host: $(UI_SRCS) $(UI_HDRS)
	$(CC) $(CFLAGS) -Wno-format-truncation $(SDL_CFLAGS) -o $@ $(UI_SRCS) \
		$(shell pkg-config --libs sdl2 SDL2_ttf) -lm

$(BUILD)/tsp-mdns: tools/tsp-mdns.c src/ui_mdns.c src/ui_mdns.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/tsp-mdns.c src/ui_mdns.c $(LDFLAGS)

# Unit tests that run in the build container.
$(BUILD)/mdns-test: tests/mdns-test.c src/ui_mdns.c src/ui_mdns.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/mdns-test.c src/ui_mdns.c
$(BUILD)/dump-test: tests/dump-test.c src/ui_engine.c src/ui_engine.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/dump-test.c src/ui_engine.c -lpthread
# fdguard-test calls close() from a shared library, as vendor code does.
$(BUILD)/libfdguard-caller.so: tests/fdguard-caller.c | $(BUILD)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ tests/fdguard-caller.c
$(BUILD)/fdguard-test: tests/fdguard-test.c src/fdguard.c src/fdguard.h \
		$(BUILD)/libfdguard-caller.so
	$(CC) $(CFLAGS) -o $@ tests/fdguard-test.c src/fdguard.c \
		-L$(BUILD) -lfdguard-caller -Wl,-rpath,'$$ORIGIN' $(FDGUARD_LDFLAGS)
# input-test includes src/input_evdev.c and stubs the protocol calls.
$(BUILD)/input-test: tests/input-test.c src/input_evdev.c src/input_evdev.h \
		| $(BUILD)
	$(CC) $(CFLAGS) -I$(COMMON)/src -o $@ tests/input-test.c -lpthread -lm
check: $(BUILD)/mdns-test $(BUILD)/dump-test $(BUILD)/fdguard-test \
		$(BUILD)/input-test
	$(BUILD)/mdns-test
	$(BUILD)/dump-test
	$(BUILD)/fdguard-test
	$(BUILD)/input-test

clean:
	rm -rf $(BUILD)
