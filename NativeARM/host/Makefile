# alyx_host: loads Valve's Linux ARM64 Half-Life: Alyx libraries on macOS
# through Klepton's loader, with VeRantyx's glibc shim layer on top.
#
#   make                      # build ../build/host/alyx_host
#   make probe LIB=libtier0.so
#
# Klepton (patched by ../klepton-patches/apply.py) lives in ../build/Klepton;
# its runtime source list is read from its own Makefile so the two never drift.
KL  ?= ../build/Klepton
OUT ?= ../build/host
CC  := clang

RUNTIME := $(shell $(MAKE) -s -C $(KL) -f Makefile -f $(CURDIR)/print-runtime.mk print-runtime)
INC := -I$(KL)/runtime -I$(KL)/runtime/jni -I$(KL)/runtime/libc -I$(KL)/runtime/gfx \
       -I$(KL)/runtime/xr -I$(KL)/runtime/media -I$(KL)/runtime/guest -I$(KL)/runtime/diag \
       -I$(KL)/vendor-moltenvk/out/include
CFLAGS := -g -O1 -Wall -Wextra -Wno-unused-parameter -arch arm64 $(INC)
LDLIBS := -lz -lresolv -liconv -framework AudioToolbox -framework CoreAudio \
          -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework IOSurface \
          -framework CoreFoundation -framework AVFoundation -framework Foundation

RT_OBJS   := $(patsubst %,$(OUT)/kl/%.o,$(RUNTIME))
HOST_SRCS := alyx_host.c kl_glibc.c kl_glibc_va.S kl_enginelog.c kl_sdlshim.c kl_steambridge.c
HOST_OBJS := $(patsubst %,$(OUT)/%.o,$(HOST_SRCS))
# The OpenVR runtime (C++): ../vrruntime.
VRRT_DIR  := ../vrruntime
VRRT_SRCS := $(notdir $(wildcard $(VRRT_DIR)/*.cpp))
VRRT_OBJS := $(patsubst %,$(OUT)/vrrt/%.o,$(VRRT_SRCS))
VRRT_HDRS := $(wildcard $(VRRT_DIR)/*.h) $(wildcard $(VRRT_DIR)/openvr/*.h)
CXXFLAGS  := -std=c++17 -g -O1 -Wall -Wno-unused-parameter -arch arm64 -I$(VRRT_DIR) -I$(KL)/vendor-moltenvk/out/include
MVK_DIR   := $(abspath $(KL)/vendor-moltenvk/out/macos)
HDRS      := $(wildcard $(KL)/runtime/*.h) $(wildcard $(KL)/runtime/*/*.h) $(wildcard *.h)

all: $(OUT)/alyx_host

$(OUT)/alyx_host: $(RT_OBJS) $(HOST_OBJS) $(VRRT_OBJS)
	clang++ -arch arm64 -g -o $@ $^ $(LDLIBS)

$(OUT)/vrrt/%.o: $(VRRT_DIR)/% $(VRRT_HDRS)
	@mkdir -p $(dir $@)
	clang++ $(CXXFLAGS) -c -o $@ $<

# Compiled from inside the Klepton tree: two of its .S files .incbin data by a
# tree-relative path.
$(OUT)/kl/%.o: $(KL)/% $(HDRS)
	@mkdir -p $(dir $@)
	cd $(KL) && $(CC) $(subst -I$(KL)/,-I,$(CFLAGS)) -w -c -o $(abspath $@) $*

$(OUT)/%.o: % $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DKLEPTON_MVK_DIR='"$(MVK_DIR)"' -c -o $@ $<

LIB ?= libtier0.so
probe: $(OUT)/alyx_host
	$(OUT)/alyx_host --probe $(LIB)

clean:
	rm -rf $(OUT)

.PHONY: all probe clean
