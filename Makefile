# Makefile — builds bin/demo
#
#   make          build
#   make run      build and run
#   make track    export the built-in music to track.wav
#   make clean    remove build products
#
# Layout:  src/    sources (C, headers, the GLSL shader)
#          build/  object files   (generated, not committed)
#          bin/    the executable (generated, not committed)
#
# Dependencies (Ubuntu):
#   sudo apt install build-essential pkg-config libglfw3-dev libgl1-mesa-dev libasound2-dev

SRC_DIR   := src
BUILD_DIR := build
BIN_DIR   := bin
TARGET    := $(BIN_DIR)/demo

SRCS    := $(addprefix $(SRC_DIR)/,main.c console_entry.c hud.c synth.c audio.c audio_common.c platform.c)
OBJS    := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o) $(BUILD_DIR)/shader_frag.o $(BUILD_DIR)/post_frag.o
HEADERS := $(wildcard $(SRC_DIR)/*.h)

# pkg-config tells us the right compiler/linker flags for GLFW on this system,
# so we don't have to hard-code include or library paths.
GLFW_CFLAGS := $(shell pkg-config --cflags glfw3)
GLFW_LIBS   := $(shell pkg-config --libs glfw3)

# gnu11: C11 plus the POSIX/GNU extras (clock_gettime, M_PI) that ALSA's headers need.
CFLAGS  ?= -O2
CFLAGS  += -std=gnu11 -Wall -Wextra $(GLFW_CFLAGS)

# -lGL      OpenGL (libglvnd dispatches to Mesa or the NVIDIA driver)
# -lasound  ALSA sound
# -lm       math (sin, exp, tanh...)
# -pthread  the audio thread
LDLIBS  += $(GLFW_LIBS) -lGL -lasound -lm -pthread
# Embedded binary data carries no "stack is non-executable" note, so we tell
# the linker explicitly. Otherwise newer binutils warn about an executable stack.
LDFLAGS += -pthread -Wl,-z,noexecstack

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Embed the shader source into the executable as raw bytes.
# `ld -r -b binary FILE` makes a relocatable object containing FILE's bytes and
# symbols named after the PATH it was given: _binary_shader_frag_start / _end.
# So we cd into src/ first. Running it on "src/shader.frag" would instead name
# them _binary_src_shader_frag_start, and main.c wouldn't find them.
# The same pattern rule handles shader.frag and post.frag ($* = the stem).
$(BUILD_DIR)/%_frag.o: $(SRC_DIR)/%.frag | $(BUILD_DIR)
	cd $(SRC_DIR) && $(LD) -r -b binary -o $(abspath $@) $*.frag

# "Order-only" prerequisites (after the |): the directory must exist, but its
# timestamp changing doesn't force a rebuild.
$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

run: $(TARGET)
	./$(TARGET)

track: $(TARGET)
	./$(TARGET) --export track.wav

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) track.wav

.PHONY: run track clean
