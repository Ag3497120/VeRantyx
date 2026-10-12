# vxrt: the VeRantyx Windows runtime. An x86-64 macOS binary; Rosetta 2 runs it and the Windows program together.
CC      = clang
CFLAGS  = -arch x86_64 -O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=13.0
SRC     = src/loader.c src/kernel32.c src/ucrt.c
MINGW   = x86_64-w64-mingw32-gcc

all: build/vxrt tests

build/vxrt: $(SRC) src/vx.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC)

tests: build/hello_k32.exe build/hello_crt.exe

build/hello_k32.exe: tests/hello_k32.c
	@mkdir -p build
	$(MINGW) -O1 -nostdlib -e start -o $@ $< -lkernel32

build/hello_crt.exe: tests/hello_crt.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

check: all
	build/vxrt build/hello_k32.exe
	build/vxrt build/hello_crt.exe one "two words"

clean:
	rm -rf build

.PHONY: all tests check clean
