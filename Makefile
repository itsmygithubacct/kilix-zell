ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
MODULES ?= $(abspath $(ROOT)/../../kilix-modules)
FB_DIR := $(MODULES)/kitty-framebuffer
KB_DIR := $(MODULES)/kitty-input/third_party/kitty_keyboard
SR_DIR := $(MODULES)/soft-raster
BUILD := $(ROOT)/build

CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
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
	$(BUILD)/scene.o \
	$(BUILD)/draw.o \
	$(BUILD)/router.o \
	$(BUILD)/voice.o \
	$(BUILD)/sprites.o \
	$(BUILD)/room.o

TEST_OBJS := \
	$(BUILD)/test_scene.o \
	$(BUILD)/scene.o \
	$(BUILD)/draw.o \
	$(BUILD)/router.o \
	$(BUILD)/voice.o \
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
	$(CC) $(CFLAGS) $^ -lm -o $@

$(BUILD)/main.o: $(ROOT)/src/main.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/scene.o: $(ROOT)/src/scene.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/draw.o: $(ROOT)/src/draw.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/sprites.o: $(ROOT)/src/sprites.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/room.o: $(ROOT)/src/room.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/router.o: $(ROOT)/src/router.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/voice.o: $(ROOT)/src/voice.c | $(BUILD)
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

test: $(BUILD)/test-scene
	$(BUILD)/test-scene

# The Neuronte renderer, beside the default one: `make RENDER=neuronte`
# builds zell-neuronte, in which Neuronte nets draw the text, the outlines
# and the pictures. NEURONTE is a
# checkout of that library with `make` run in it. The default build above
# does not need it.
NEURONTE ?= $(HOME)/projects/neuronte
NEURONTE_WEIGHTS ?= $(NEURONTE)/weights/neuronte-split-v3.nrt
NEURONTE_LIB := $(NEURONTE)/build/libneuronte.a
N_CPPFLAGS := $(CPPFLAGS) -I$(NEURONTE)/include -DZELL_RENDER_NEURONTE \
	-DZELL_NEURONTE_WEIGHTS='"$(NEURONTE_WEIGHTS)"'

N_COMMON := \
	$(BUILD)/scene.o \
	$(BUILD)/draw_neuronte.o \
	$(BUILD)/router.o \
	$(BUILD)/voice.o \
	$(BUILD)/sprites.o \
	$(BUILD)/room.o

SNAP_COMMON := \
	$(BUILD)/scene.o \
	$(BUILD)/router.o \
	$(BUILD)/voice.o \
	$(BUILD)/sprites.o \
	$(BUILD)/room.o \
	$(BUILD)/soft_raster.o

.PHONY: neuronte test-neuronte snap

ifeq ($(RENDER),neuronte)
all: $(ROOT)/zell-neuronte
test: test-neuronte
endif

neuronte: $(ROOT)/zell-neuronte

$(BUILD)/draw_neuronte.o: $(ROOT)/src/draw_neuronte.c $(ROOT)/src/draw.c | $(BUILD)
	$(CC) $(N_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/main_neuronte.o: $(ROOT)/src/main.c | $(BUILD)
	$(CC) $(N_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(ROOT)/zell-neuronte: $(BUILD)/main_neuronte.o $(N_COMMON) $(LIB_OBJS) $(NEURONTE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_neuronte.o: $(ROOT)/tests/test_neuronte.c | $(BUILD)
	$(CC) $(N_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test-neuronte: $(BUILD)/test_neuronte.o $(N_COMMON) $(BUILD)/soft_raster.o $(NEURONTE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $^ -lm -o $@

# The scene tests again, with the Neuronte renderer in place of draw.o.
$(BUILD)/test_scene_neuronte.o: $(ROOT)/tests/test_scene.c | $(BUILD)
	$(CC) $(N_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test-scene-neuronte: $(BUILD)/test_scene_neuronte.o $(N_COMMON) $(BUILD)/soft_raster.o $(NEURONTE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $^ -lm -o $@

test-neuronte: $(BUILD)/test-neuronte $(BUILD)/test-scene-neuronte
	$(BUILD)/test-neuronte
	$(BUILD)/test-scene-neuronte

$(BUILD)/snap.o: $(ROOT)/tools/snap.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/snap_neuronte.o: $(ROOT)/tools/snap.c | $(BUILD)
	$(CC) $(N_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/snap: $(BUILD)/snap.o $(BUILD)/draw.o $(SNAP_COMMON) | $(BUILD)
	$(CC) $(CFLAGS) $^ -lm -o $@

$(BUILD)/snap-neuronte: $(BUILD)/snap_neuronte.o $(BUILD)/draw_neuronte.o $(SNAP_COMMON) $(NEURONTE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $^ -lm -o $@

snap: $(BUILD)/snap $(BUILD)/snap-neuronte

clean:
	rm -rf $(BUILD) $(ROOT)/zell $(ROOT)/zell-neuronte
