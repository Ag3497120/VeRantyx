# vxrt: the VeRantyx Windows runtime. An x86-64 macOS binary; Rosetta 2 runs it and the Windows program together.
CC      = clang
CFLAGS  = -arch x86_64 -O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=13.0
SRC     = src/loader.c src/kernel32.c src/ucrt.c src/sync.c src/seh.c src/context.S src/text.c src/files.c src/registry.c
MINGW   = x86_64-w64-mingw32-gcc
MINGWXX = x86_64-w64-mingw32-g++

all: build/vxrt tests

build/vxrt: $(SRC) src/vx.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC)

tests: build/hello_k32.exe build/hello_crt.exe build/threads.exe build/cxx_exceptions.exe build/hw_exceptions.exe \
       build/files.exe build/dll_test.exe build/vxplugin.dll build/dllprobe.exe

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
check: all
	build/vxrt build/hello_k32.exe
	build/vxrt build/hello_crt.exe one "two words"
	build/vxrt build/threads.exe
	build/vxrt build/cxx_exceptions.exe
	build/vxrt build/hw_exceptions.exe
	build/vxrt build/files.exe
	build/vxrt build/dll_test.exe

clean:
	rm -rf build

.PHONY: all tests check clean
