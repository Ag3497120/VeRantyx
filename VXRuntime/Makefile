# vxrt: the VeRantyx Windows runtime. An x86-64 macOS binary; Rosetta 2 runs it and the Windows program together.
CC      = clang
CFLAGS  = -arch x86_64 -O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=13.0
SRC     = src/loader.c src/kernel32.c src/ucrt.c src/sync.c src/seh.c src/context.S src/text.c src/files.c src/registry.c \
          src/user32.c src/gdi.c src/net.c src/mm.c src/vcruntime.c src/com.c src/cocoa.m src/d3d11.m src/dxgi.m
MINGW   = x86_64-w64-mingw32-gcc
MINGWXX = x86_64-w64-mingw32-g++

all: build/vxrt tests

build/vxrt: $(SRC) src/vx.h src/ui.h src/d3d_common.h src/d3d_tables.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $(SRC) -framework Cocoa -framework CoreText -framework Metal -framework QuartzCore

tests: build/hello_k32.exe build/hello_crt.exe build/threads.exe build/cxx_exceptions.exe build/hw_exceptions.exe \
       build/files.exe build/dll_test.exe build/vxplugin.dll build/dllprobe.exe \
       build/window.exe build/net.exe build/msvc_eh.exe build/com.exe build/vxcom.dll build/iocp.exe build/d3d11.exe

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

clean:
	rm -rf build

.PHONY: all tests check clean
