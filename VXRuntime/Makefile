# vxrt: the VeRantyx Windows runtime. An x86-64 macOS binary; Rosetta 2 runs it and the Windows program together.
CC      = clang
CFLAGS  = -arch x86_64 -O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=14.0
SRC     = src/loader.c src/kernel32.c src/system.c src/desktop.c src/services.c src/client.c src/steambridge.c src/calltrace.c src/pipes.c src/ucrt.c src/sync.c src/seh.c src/profile.c src/context.S src/text.c src/files.c src/registry.c \
          src/user32.c src/gdi.c src/net.c src/mm.c src/vcruntime.c src/com.c src/cocoa.m src/d3d11.m src/d3d11_draw.m src/d3d11_shared.m src/dxgi.m src/d3d9.m src/dxbc.c src/input.m src/audio.m src/xaudio2.m
MINGW   = x86_64-w64-mingw32-gcc
MINGWXX = x86_64-w64-mingw32-g++

all: build/vxrt tests

build/vxrt: $(SRC) src/vx.h src/ui.h src/d3d_common.h src/d3d_tables.h src/audio_tables.h src/xaudio2_tables.h src/dxbc.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $(SRC) -framework Cocoa -framework CoreText -framework Metal -framework QuartzCore -framework GameController -framework CoreGraphics -framework IOSurface -framework AudioToolbox -framework CoreAudio -framework ImageIO -Wl,-pagezero_size,0x10000

tests: build/hello_k32.exe build/hello_crt.exe build/threads.exe build/cxx_exceptions.exe build/hw_exceptions.exe \
       build/files.exe build/dll_test.exe build/vxplugin.dll build/dllprobe.exe \
       build/window.exe build/net.exe build/msvc_eh.exe build/com.exe build/vxcom.dll build/iocp.exe build/d3d11.exe build/hlslc.exe build/dxbc2msl build/draw.exe build/compute.exe build/input.exe build/audio.exe build/xaudio2.exe build/dxbcscan build/pipes.exe build/ipc.exe build/shared.exe build/steam.exe

build/hello_k32.exe: tests/hello_k32.c
	@mkdir -p build
	$(MINGW) -O1 -nostdlib -e start -o $@ $< -lkernel32

build/hello_crt.exe: tests/hello_crt.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

# -static: winpthread and libgcc are linked into the program, so it imports only the system DLLs.
build/threads.exe: tests/threads.c
	@mkdir -p build
	$(MINGW) -O1 -static -o $@ $<

build/hw_exceptions.exe: tests/hw_exceptions.c
	@mkdir -p build
	$(MINGW) -O1 -static -o $@ $<

build/cxx_exceptions.exe: tests/cxx_exceptions.cpp
	@mkdir -p build
	$(MINGWXX) -O1 -static -o $@ $<

build/window.exe: tests/window.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lgdi32 -lwinmm

build/net.exe: tests/net.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lws2_32

# MSVC-ABI test programs: compiled by clang for x86_64-pc-windows-msvc, linked by MinGW's linker against import
# libraries of this runtime's DLLs, without any C runtime.
MSVCCC = clang -target x86_64-pc-windows-msvc -O1 -fexceptions -fcxx-exceptions
build/libvcruntime140.a: tests/msvc/vcruntime140.def
	@mkdir -p build
	x86_64-w64-mingw32-dlltool -d $< -l $@ -m i386:x86-64

build/msvc_eh.exe: tests/msvc_eh.cpp tests/msvc/start.c build/libvcruntime140.a
	$(MSVCCC) -c tests/msvc_eh.cpp -o build/msvc_eh.obj
	$(MSVCCC) -c tests/msvc/start.c -o build/msvc_start.obj
	$(MINGW) -nostdlib -e mainCRTStartup -o $@ build/msvc_eh.obj build/msvc_start.obj -Lbuild -lvcruntime140 -lkernel32 -lucrtbase

build/com.exe: tests/com.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lole32 -loleaut32 -luuid

build/vxcom.dll: tests/vxcom.c
	@mkdir -p build
	$(MINGW) -O1 -shared -o $@ $< -luuid

build/iocp.exe: tests/iocp.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/d3d11.exe: tests/d3d11.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ld3d11 -ldxgi -ldxguid -luuid

# The Steam bridge against the Steam client running on the Mac (VXRT_STEAM_BRIDGE=1 build/vxrt build/steam.exe;
# not part of make check, which must not depend on Steam).
build/steam.exe: tests/steam.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/drawbench.exe: tests/drawbench.c tests/shaders/*.h
	@mkdir -p build
	$(MINGW) -O2 -Itests -o $@ $<  -ld3d11 -ldxgi -ldxguid -luuid

build/dxbcscan: tools/dxbcscan.m src/dxbc.c src/dxbc.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ tools/dxbcscan.m src/dxbc.c -framework Metal -framework Foundation

build/dxbc2msl: tools/dxbc2msl.m src/dxbc.c src/dxbc.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ tools/dxbc2msl.m src/dxbc.c -framework Metal

build/draw.exe: tests/draw.c tests/shaders/*.h
	@mkdir -p build
	$(MINGW) -O1 -Itests -o $@ $< -ld3d11 -ldxgi -ldxguid -luuid -lm

build/compute.exe: tests/compute.c tests/shaders/*.h
	@mkdir -p build
	$(MINGW) -O1 -Itests -o $@ $< -ld3d11 -ldxguid -luuid -lm

build/input.exe: tests/input.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lxinput

build/audio.exe: tests/audio.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lole32 -luuid -lm

build/xaudio2.exe: tests/xaudio2.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lxaudio2_9 -lm

build/shared.exe: tests/shared.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ld3d11 -ldxgi -ldxguid -luuid

build/ipc.exe: tests/ipc.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/pipes.exe: tests/pipes.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/hlslc.exe: tools/hlslc.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/dllprobe.exe: tools/dllprobe.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/files.exe: tests/files.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/vxdll.dll: tests/vxdll.cpp
	@mkdir -p build
	$(MINGWXX) -O1 -shared -static -o $@ $< -Wl,--out-implib,build/libvxdll.a

build/dll_test.exe: tests/dll_test.cpp build/vxdll.dll
	@mkdir -p build
	$(MINGWXX) -O1 -static -o $@ $< build/libvxdll.a

build/vxplugin.dll: tests/vxplugin.c tests/vxplugin.def
	@mkdir -p build
	$(MINGW) -O1 -shared -o $@ tests/vxplugin.c tests/vxplugin.def

# Test shaders: HLSL compiled to DXBC by a d3dcompiler DLL (Microsoft's, as games ship it) running on vxrt.
# The generated tests/shaders/*.h are kept in the repository; regenerate with
#   make shaders D3DCOMPILER='Z:\path\to\d3dcompiler_43.dll'
SHADERS = color_vs:vs_4_0 color_ps:ps_4_0 tex_vs:vs_4_0 tex_ps:ps_4_0 fullscreen_vs:vs_4_0 math_ps:ps_4_0 sum_cs:cs_5_0 atomic_cs:cs_5_0 append_cs:cs_5_0 views_cs:cs_5_0 formats_cs:cs_5_0 features_ps:ps_5_0 features_vs:vs_5_0 features_cs:cs_5_0 quad_gs:gs_4_0
shaders: build/vxrt build/hlslc.exe
	@for s in $(SHADERS); do n=$${s%%:*}; p=$${s##*:}; \
	  VXRT_ROOT=build/root build/vxrt build/hlslc.exe '$(D3DCOMPILER)' tests/shaders/$$n.hlsl main $$p tests/shaders/$$n.h $$n || exit 1; done

# The tests' C: drive and registry live in build/root, not in ~/.vxrt.
check: export VXRT_ROOT = build/root
check: export VXRT_NO_ACTIVATE = 1
check: all
	build/vxrt build/hello_k32.exe
	build/vxrt build/hello_crt.exe one "two words"
	build/vxrt build/threads.exe
	build/vxrt build/cxx_exceptions.exe
	build/vxrt build/hw_exceptions.exe
	build/vxrt build/files.exe
	build/vxrt build/dll_test.exe
	build/vxrt build/net.exe
	build/vxrt build/msvc_eh.exe
	build/vxrt build/com.exe
	build/vxrt build/iocp.exe
	build/vxrt build/window.exe
	build/vxrt build/d3d11.exe
	build/dxbcscan tests/shaders/*.dxbc
	build/vxrt build/draw.exe
	build/vxrt build/compute.exe
	build/vxrt build/input.exe
	build/vxrt build/audio.exe
	build/vxrt build/xaudio2.exe
	build/vxrt build/pipes.exe
	build/vxrt build/pipes.exe serve & build/vxrt build/pipes.exe ask; wait
	build/vxrt build/ipc.exe second & build/vxrt build/ipc.exe first; wait
	build/vxrt build/shared.exe make & build/vxrt build/shared.exe open; wait
	build/vxrt build/shared.exe pingpong 1 & build/vxrt build/shared.exe pingpong 2; wait

# CPU cost per Direct3D 11 draw (numbers only, not a test).
bench: export VXRT_ROOT = build/root
bench: export VXRT_NO_ACTIVATE = 1
bench: build/vxrt build/drawbench.exe
	build/vxrt build/drawbench.exe

clean:
	rm -rf build

.PHONY: all tests check bench clean shaders
