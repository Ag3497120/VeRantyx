# Offline checks: no Klepton/depot downloads, Steam, games, headset or network.
CXX := clang++
CC := clang
OUT := build/tests
VR := vrruntime
VR_SRCS := $(VR)/vr_runtime.cpp $(VR)/vr_input.cpp $(VR)/vr_skeleton.cpp $(VR)/vr_system.cpp
TEST_FLAGS := -std=c++17 -g -O1 -Wall -Wextra -Wno-unused-parameter -I../VXRuntime/third_party/vulkan
ASYNC_FLAGS := -std=c11 -g -O1 -Wall -Wextra -Wno-unused-function -I../VXRuntime/third_party/vulkan
SWIFT_CACHE := build/swift-cache-$(shell pwd | shasum | cut -c1-12)

check: $(OUT)/runtime_test $(OUT)/async_pipeline $(OUT)/vulkan_compat $(OUT)/support_test $(OUT)/session_test $(OUT)/fake_peer build/native-session
	bash tests/run_runtime.sh
	$(OUT)/async_pipeline
	$(OUT)/vulkan_compat
	$(OUT)/support_test $(abspath $(OUT))
	$(OUT)/session_test $(abspath $(OUT))

$(OUT)/support_test: shared/NativeSupport.swift tests/SupportTests.swift
	@mkdir -p $(OUT)
	swiftc -swift-version 5 shared/NativeSupport.swift tests/SupportTests.swift -o $@

$(OUT)/session_test: shared/NativeSupport.swift tests/SessionTests.swift
	@mkdir -p $(OUT)
	swiftc -swift-version 5 shared/NativeSupport.swift tests/SessionTests.swift -o $@

$(OUT)/fake_peer: shared/NativeSupport.swift tests/FakePeer.swift
	@mkdir -p $(OUT)
	swiftc -swift-version 5 shared/NativeSupport.swift tests/FakePeer.swift -o $@

build/native-session: shared/NativeSupport.swift session/main.swift
	@mkdir -p build
	swiftc -O -swift-version 5 -target arm64-apple-macos14.2 shared/NativeSupport.swift session/main.swift -o $@

$(OUT)/runtime_test: tests/runtime_test.cpp $(VR_SRCS) $(wildcard $(VR)/*.h) $(VR)/vr_compositor.cpp $(VR)/vr_misc.cpp $(VR)/hand_skeleton_alvr.inc
	@mkdir -p $(OUT)
	$(CXX) $(TEST_FLAGS) $(VR_SRCS) tests/runtime_test.cpp -o $@

$(OUT)/async_pipeline: tests/async_pipeline.c klepton-patches/runtime/kl_vk_async.inc
	@mkdir -p $(OUT)
	$(CC) $(ASYNC_FLAGS) $< -o $@

$(OUT)/vulkan_compat: tests/vulkan_compat.c klepton-patches/runtime/kl_vk_compat.inc
	@mkdir -p $(OUT)
	$(CC) $(ASYNC_FLAGS) $< -o $@

# Optional: uses the already downloaded MoltenVK, without fetching dependencies.
$(OUT)/pipeline_cache_gpu: tests/pipeline_cache_gpu.c tests/shadow_depth_gpu.inc klepton-patches/runtime/kl_vk_async.inc klepton-patches/runtime/kl_vk_compat.inc ../VXRuntime/tests/vulkan_shaders/fullscreen.vert.h ../VXRuntime/tests/vulkan_shaders/solid.frag.h
	@mkdir -p $(OUT)
	$(CC) -std=c11 -g -O1 -Wall -Wextra -Wno-unused-function -Ibuild/Klepton/vendor-moltenvk/out/include $< -o $@

check-pipeline-cache: $(OUT)/pipeline_cache_gpu
	$(OUT)/pipeline_cache_gpu $(abspath build/Klepton/vendor-moltenvk/out/macos/libMoltenVK.dylib) $(abspath $(OUT)/moltenvk-cache.bin)

check-syntax:
	$(CXX) $(TEST_FLAGS) -fsyntax-only $(VR)/*.cpp
	bash -n scripts/launch_native_vr.sh

check-encoder:
	@mkdir -p $(SWIFT_CACHE)
	swiftc -typecheck -module-cache-path $(SWIFT_CACHE) ../MacEncoder/HardwareEncoder.swift

$(OUT)/encoder-map-test: ../MacEncoder/HardwareEncoder.swift
	@mkdir -p $(OUT)
	swiftc -O -target arm64-apple-macos14.2 $< -o $@

check-loading-map: $(OUT)/encoder-map-test
	VERANTYX_NATIVE_MAP_SELFTEST=$(abspath $(OUT)/loading-map.png) VERANTYX_NATIVE_GAME_DIR=$(abspath tests/fixtures/loading-map) $(OUT)/encoder-map-test

check-app:
	bash tests/check_app.sh

$(OUT)/runtime_asan: tests/runtime_test.cpp $(VR_SRCS) $(wildcard $(VR)/*.h) $(VR)/vr_compositor.cpp $(VR)/vr_misc.cpp
	@mkdir -p $(OUT)
	$(CXX) $(TEST_FLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(VR_SRCS) tests/runtime_test.cpp -o $@

$(OUT)/runtime_tsan: tests/runtime_test.cpp $(VR_SRCS) $(wildcard $(VR)/*.h) $(VR)/vr_compositor.cpp $(VR)/vr_misc.cpp
	@mkdir -p $(OUT)
	$(CXX) $(TEST_FLAGS) -fsanitize=thread $(VR_SRCS) tests/runtime_test.cpp -o $@

$(OUT)/async_asan: tests/async_pipeline.c klepton-patches/runtime/kl_vk_async.inc
	@mkdir -p $(OUT)
	$(CC) $(ASYNC_FLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $< -o $@

$(OUT)/async_tsan: tests/async_pipeline.c klepton-patches/runtime/kl_vk_async.inc
	@mkdir -p $(OUT)
	$(CC) $(ASYNC_FLAGS) -fsanitize=thread $< -o $@

check-sanitize: $(OUT)/runtime_asan $(OUT)/runtime_tsan $(OUT)/async_asan $(OUT)/async_tsan
	bash tests/run_runtime.sh $(abspath $(OUT)/runtime_asan)
	bash tests/run_runtime.sh $(abspath $(OUT)/runtime_tsan)
	$(OUT)/async_asan
	$(OUT)/async_tsan

.PHONY: check check-syntax check-encoder check-sanitize check-app check-pipeline-cache check-loading-map
