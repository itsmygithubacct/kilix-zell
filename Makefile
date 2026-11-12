ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
MODULES ?= $(abspath $(ROOT)/../../kilix-modules)
FB_DIR := $(MODULES)/kitty-framebuffer
KB_DIR := $(MODULES)/kitty-input/third_party/kitty_keyboard
SR_DIR := $(MODULES)/soft-raster
BUILD := $(ROOT)/build

CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -MMD -MP -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	-I$(ROOT)/src -I$(FB_DIR)/include -I$(KB_DIR)/include -I$(SR_DIR)/include
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
override CFLAGS += -std=c11 -pthread $(WARNINGS)
LDLIBS := -lz -lm

LIB_OBJS := \
	$(BUILD)/kitty_framebuffer.o \
	$(BUILD)/kitty_keyboard.o \
	$(BUILD)/kitty_keyboard_posix.o \
	$(BUILD)/soft_raster.o

APP_OBJS := \
	$(BUILD)/main.o \
	$(BUILD)/desktop.o \
	$(BUILD)/desktop_assets.o \
	$(BUILD)/scene.o \
	$(BUILD)/draw.o \
	$(BUILD)/router.o \
	$(BUILD)/model.o \
	$(BUILD)/voice.o \
	$(BUILD)/history.o \
	$(BUILD)/sprites.o \
	$(BUILD)/room.o

TEST_OBJS := \
	$(BUILD)/test_scene.o \
	$(BUILD)/desktop.o \
	$(BUILD)/desktop_assets.o \
	$(BUILD)/scene.o \
	$(BUILD)/draw.o \
	$(BUILD)/router.o \
	$(BUILD)/model.o \
	$(BUILD)/voice.o \
	$(BUILD)/history.o \
	$(BUILD)/sprites.o \
	$(BUILD)/room.o \
	$(BUILD)/soft_raster.o

.PHONY: all test clean

all: $(ROOT)/zell

$(BUILD):
	mkdir -p $@

$(ROOT)/zell: $(APP_OBJS) $(LIB_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test-scene: $(TEST_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -lz -lm -o $@

$(BUILD)/main.o: $(ROOT)/src/main.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/scene.o: $(ROOT)/src/scene.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/draw.o: $(ROOT)/src/draw.c $(ROOT)/src/desktop_draw.inc | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/sprites.o: $(ROOT)/src/sprites.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/room.o: $(ROOT)/src/room.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/router.o: $(ROOT)/src/router.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/model.o: $(ROOT)/src/model.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/voice.o: $(ROOT)/src/voice.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/history.o: $(ROOT)/src/history.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test_scene.o: $(ROOT)/tests/test_scene.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/kitty_framebuffer.o: $(FB_DIR)/src/kitty_framebuffer.c | $(BUILD)
	$(CC) $(CPPFLAGS) -I$(FB_DIR)/src $(CFLAGS) -c $< -o $@

$(BUILD)/kitty_keyboard.o: $(KB_DIR)/src/kitty_keyboard.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/kitty_keyboard_posix.o: $(KB_DIR)/src/kitty_keyboard_posix.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/soft_raster.o: $(SR_DIR)/src/soft_raster.c | $(BUILD)
	$(CC) $(CPPFLAGS) -I$(SR_DIR)/src $(CFLAGS) -c $< -o $@

# Sectioned copy for the Neuronte link. --gc-sections then drops the font
# and the scaler, which that renderer does not call. The default build
# keeps soft_raster.o and does not pass --gc-sections, so zell keeps them.
$(BUILD)/soft_raster_neuronte.o: $(SR_DIR)/src/soft_raster.c | $(BUILD)
	$(CC) $(CPPFLAGS) -I$(SR_DIR)/src $(CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

test: $(BUILD)/test-scene
	$(BUILD)/test-scene

# The Neuronte renderer, beside the default one: `make RENDER=neuronte`
# builds zell-neuronte, in which Neuronte nets draw the text, the outlines
# and the pictures. NEURONTE is a
# checkout of that library with `make` run in it. The default build above
# does not need it.
NEURONTE ?= $(HOME)/projects/neuronte
NEURONTE_WEIGHTS ?= $(NEURONTE)/weights/neuronte-split-v4.nrt
NEURONTE_LIB := $(NEURONTE)/build/libneuronte.a
N_CPPFLAGS := $(CPPFLAGS) -I$(NEURONTE)/include -DZELL_RENDER_NEURONTE \
	-DZELL_NEURONTE_WEIGHTS='"$(NEURONTE_WEIGHTS)"'

N_COMMON := \
	$(BUILD)/desktop.o \
	$(BUILD)/desktop_assets.o \
	$(BUILD)/scene.o \
	$(BUILD)/draw_neuronte.o \
	$(BUILD)/router.o \
	$(BUILD)/model.o \
	$(BUILD)/voice.o \
	$(BUILD)/history.o \
	$(BUILD)/sprites.o \
	$(BUILD)/room.o

SNAP_COMMON := \
	$(BUILD)/desktop.o \
	$(BUILD)/desktop_assets.o \
	$(BUILD)/scene.o \
	$(BUILD)/router.o \
	$(BUILD)/model.o \
	$(BUILD)/voice.o \
	$(BUILD)/history.o \
	$(BUILD)/sprites.o \
	$(BUILD)/room.o \
	$(BUILD)/soft_raster.o

# Same link as LIB_OBJS, with the sectioned soft-raster. --gc-sections
# applies to the Neuronte program, its tests, and snap-neuronte only.
N_LDFLAGS := -Wl,--gc-sections
N_LIB_OBJS := \
	$(BUILD)/kitty_framebuffer.o \
	$(BUILD)/kitty_keyboard.o \
	$(BUILD)/kitty_keyboard_posix.o \
	$(BUILD)/soft_raster_neuronte.o

SNAP_NEURONTE := \
	$(BUILD)/desktop.o \
	$(BUILD)/desktop_assets.o \
	$(BUILD)/scene.o \
	$(BUILD)/router.o \
	$(BUILD)/model.o \
	$(BUILD)/voice.o \
	$(BUILD)/history.o \
	$(BUILD)/sprites.o \
	$(BUILD)/room.o \
	$(BUILD)/soft_raster_neuronte.o

.PHONY: neuronte test-neuronte snap

ifeq ($(RENDER),neuronte)
all: $(ROOT)/zell-neuronte
test: test-neuronte
endif

neuronte: $(ROOT)/zell-neuronte

$(BUILD)/draw_neuronte.o: $(ROOT)/src/draw_neuronte.c $(ROOT)/src/draw.c $(ROOT)/src/desktop_draw.inc | $(BUILD)
	$(CC) $(N_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/main_neuronte.o: $(ROOT)/src/main.c | $(BUILD)
	$(CC) $(N_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(ROOT)/zell-neuronte: $(BUILD)/main_neuronte.o $(N_COMMON) $(N_LIB_OBJS) $(NEURONTE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(N_LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_neuronte.o: $(ROOT)/tests/test_neuronte.c | $(BUILD)
	$(CC) $(N_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test-neuronte: $(BUILD)/test_neuronte.o $(N_COMMON) $(BUILD)/soft_raster_neuronte.o $(NEURONTE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(N_LDFLAGS) $^ -lz -lm -o $@

# The scene tests again, with the Neuronte renderer in place of draw.o.
$(BUILD)/test_scene_neuronte.o: $(ROOT)/tests/test_scene.c | $(BUILD)
	$(CC) $(N_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test-scene-neuronte: $(BUILD)/test_scene_neuronte.o $(N_COMMON) $(BUILD)/soft_raster_neuronte.o $(NEURONTE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(N_LDFLAGS) $^ -lz -lm -o $@

test-neuronte: $(BUILD)/test-neuronte $(BUILD)/test-scene-neuronte
	$(BUILD)/test-neuronte
	$(BUILD)/test-scene-neuronte

$(BUILD)/snap.o: $(ROOT)/tools/snap.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/snap_neuronte.o: $(ROOT)/tools/snap.c | $(BUILD)
	$(CC) $(N_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/snap: $(BUILD)/snap.o $(BUILD)/draw.o $(SNAP_COMMON) | $(BUILD)
	$(CC) $(CFLAGS) $^ -lz -lm -o $@

$(BUILD)/snap-neuronte: $(BUILD)/snap_neuronte.o $(BUILD)/draw_neuronte.o $(SNAP_NEURONTE) $(NEURONTE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(N_LDFLAGS) $^ -lz -lm -o $@

snap: $(BUILD)/snap $(BUILD)/snap-neuronte

clean:
	rm -rf $(BUILD) $(ROOT)/zell $(ROOT)/zell-neuronte

$(BUILD)/test_process.o: $(ROOT)/tests/test_process.c $(ROOT)/src/main.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test-process: $(BUILD)/test_process.o $(filter-out $(BUILD)/main.o,$(APP_OBJS)) $(LIB_OBJS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test: test-process
.PHONY: test-process
test-process: $(BUILD)/test-process
	python3 $(ROOT)/tests/test_process.py $(BUILD)/test-process

$(BUILD)/desktop.o: $(ROOT)/src/desktop.c $(ROOT)/src/desktop.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/desktop_assets.o: $(ROOT)/src/desktop_assets.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(APP_OBJS) $(TEST_OBJS) $(BUILD)/main_neuronte.o $(BUILD)/test_scene_neuronte.o $(BUILD)/test_neuronte.o $(BUILD)/snap.o $(BUILD)/snap_neuronte.o: $(ROOT)/src/scene.h $(ROOT)/src/desktop.h

$(BUILD)/test_desktop.o: $(ROOT)/tests/test_desktop.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test-desktop: $(BUILD)/test_desktop.o $(filter-out $(BUILD)/main.o,$(APP_OBJS)) $(LIB_OBJS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

.PHONY: test-desktop
test: test-desktop
test-desktop: $(BUILD)/test-desktop
	$(BUILD)/test-desktop
	python3 $(ROOT)/tests/test_desktop.py
	python3 $(ROOT)/tests/test_media.py
	python3 $(ROOT)/tests/test_dialogue.py
	python3 $(ROOT)/tests/test_library.py
	python3 $(ROOT)/tests/test_vitals.py
	python3 $(ROOT)/tests/test_rooms.py
	python3 $(ROOT)/tests/test_projects.py
	python3 $(ROOT)/tests/test_terminal.py
	python3 $(ROOT)/tests/test_secrets.py
	python3 $(ROOT)/tests/test_journal.py
	python3 $(ROOT)/tests/test_shell.py
	python3 $(ROOT)/tests/test_weather.py

$(BUILD)/test_process.o $(BUILD)/test_desktop.o: $(ROOT)/src/scene.h $(ROOT)/src/desktop.h

-include $(wildcard $(BUILD)/*.d)

$(BUILD)/test_desktop_neuronte.o: $(ROOT)/tests/test_desktop.c | $(BUILD)
	$(CC) $(N_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test-desktop-neuronte: $(BUILD)/test_desktop_neuronte.o $(N_COMMON) $(BUILD)/soft_raster_neuronte.o $(NEURONTE_LIB)
	$(CC) $(CFLAGS) $(N_LDFLAGS) $^ $(LDLIBS) -o $@

.PHONY: test-desktop-neuronte
test-neuronte: test-desktop-neuronte
test-desktop-neuronte: $(BUILD)/test-desktop-neuronte
	$(BUILD)/test-desktop-neuronte

# The short installed command always selects the Neuronte desktop. The legacy
# raster binary remains available only inside the source tree.
ZELL_INSTALL_DIR ?= $(if $(GPU_TERMINAL_HOME),$(GPU_TERMINAL_HOME),$(HOME)/.local/gpu_terminal)/kilix/data/desktop-apps/kilix-zell
ZELL_BIN_DIR ?= $(HOME)/.local/bin
.PHONY: test-snap
test: test-snap
test-snap: $(BUILD)/snap
	python3 $(ROOT)/tests/test_snap.py $(BUILD)/snap

test-neuronte: test-snap-neuronte
.PHONY: test-snap-neuronte
test-snap-neuronte: $(BUILD)/snap-neuronte
	python3 $(ROOT)/tests/test_snap.py $(BUILD)/snap-neuronte

.PHONY: install
install: neuronte
	python3 tools/package_runtime.py --modules "$(MODULES)" --neuronte "$(NEURONTE)" --install "$(DESTDIR)$(ZELL_INSTALL_DIR)" --bin-dir "$(DESTDIR)$(ZELL_BIN_DIR)" $(if $(DESTDIR),,--user-unit-dir "$(HOME)/.config/systemd/user")

.PHONY: runtime
runtime: neuronte
	python3 tools/package_runtime.py --modules "$(MODULES)" --neuronte "$(NEURONTE)" --output "$(BUILD)/zell-$(shell cat VERSION)-$(shell uname -m).tar.xz"
