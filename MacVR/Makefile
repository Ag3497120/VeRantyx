CC := clang
ARCH := -arch arm64
BUILD_DIR := build
DYLIB := $(BUILD_DIR)/libverantyx_macvr.dylib
TEST_BIN := $(BUILD_DIR)/macvr_test

CFLAGS := $(ARCH) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -fobjc-arc -fPIC -Isrc -I../VXRuntime/src
LDFLAGS := $(ARCH) \
	-framework Foundation \
	-framework AppKit \
	-framework QuartzCore \
	-framework Metal \
	-framework MetalFX \
	-framework CoreGraphics \
	-framework GameController \
	-framework IOSurface

SRCS := \
	src/macvr_metal.m \
	src/macvr_input.m \
	src/macvr_presenter.m \
	../VXRuntime/src/warp_core.m

.PHONY: all clean test

all: $(DYLIB) $(TEST_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DYLIB): $(SRCS) src/macvr.h ../VXRuntime/src/warp_core.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -dynamiclib $(SRCS) $(LDFLAGS) -install_name @rpath/libverantyx_macvr.dylib -o $@
	codesign -s - -f $@

$(TEST_BIN): tests/macvr_test.m $(DYLIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/macvr_test.m -L$(BUILD_DIR) -lverantyx_macvr -Wl,-rpath,@executable_path $(LDFLAGS) -o $@
	codesign -s - -f $@

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf $(BUILD_DIR)
