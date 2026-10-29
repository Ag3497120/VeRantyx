# vxrt: the VeRantyx Windows runtime. An x86-64 macOS binary; Rosetta 2 runs it and the Windows program together.
CC      = clang
CFLAGS  = -arch x86_64 -O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=14.0
SRC     = src/loader.c src/pe.c src/dotnet_metadata.c src/dotnet_resources.c src/dotnet_baml.c src/dotnet_il.c src/dotnet_exec.c src/dotnet_runtime.c src/dotnet_host.c src/kernel32.c src/kernelx.c src/jobs.c src/ntstatus.c src/system.c src/desktop.c src/winsta.c src/scm.c src/services.c src/security.c src/client.c src/steambridge.c src/calltrace.c src/pipes.c src/ucrt.c src/sync.c src/seh.c src/profile.c src/opengl.c src/wininet.m src/winhttp.m src/winhttp_proxy.m src/iphlp.c src/winsock_nsp.c src/midi.c src/directinput.m src/msxml.c src/extras.c src/context.S src/text.c src/files.c src/registry.c \
          src/user32.c src/gdi.c src/control.c src/net.c src/mm.c src/vcruntime.c src/com.c src/cocoa.m src/d3d11.m src/d3d11_draw.m src/d3d11_shared.m src/d3d11on12.m src/dxgi.m src/d3d9.m src/dxbc.c src/d3d_format.c src/d3d12.m src/gpu_ir.c src/gpu_ir_msl.c src/gpu_ir_metal.m src/dxil.c src/dxil_msl.c src/dxil_ir.c src/spirv.c src/spirv_ir.c src/vk_msl.c src/vk_format.m src/vk_device.m src/vk_descriptor.m src/vk_pipeline.m src/vk_command.m src/vk_internal.m src/vk_queue.m src/vk_wsi.m src/vk_dispatch.m src/directinput_buffer.c src/directinput_format.c src/input.m src/audio.m src/xaudio2.m src/sysdlls.c src/dbghelp.c src/aes_gcm.c src/dpapi.c src/crypt32.c src/dwrite.m src/osinfo.c src/pointer.c src/regnotify.c src/section.c src/shlpath.c src/wevtapi.c src/ktime.c src/power.c src/sysparams.c src/dwmapi.c src/winmisc.c src/region.c
SRC    += src/launchargs.c
SRC    += src/handlexfer.c
SRC    += src/version.c
SRC    += src/filestate.c
SRC    += src/inherit.c
SRC    += src/icons.c src/menus.m src/tray.m
SRC    += src/present.c
SRC    += src/stereo.m
SRC    += src/shlock.c
SRC    += src/tcptable.c
SRC    += src/winprops.c
SRC    += src/a11y.c
SRC    += src/mfplat.c
SRC    += src/imagehlp.c src/spellcheck.c src/ime.c src/clipboard.m
SRC    += src/npu_interp.m
SRC    += src/upscale_core.m
MINGW   = x86_64-w64-mingw32-gcc
MINGWXX = x86_64-w64-mingw32-g++

all: build/vxrt build/vxrt-npu tests

build/vxrt: $(SRC) src/vx.h src/npu_interp.h src/npu_proto.h src/pe.h src/dotnet_metadata.h src/dotnet_resources.h src/dotnet_baml.h src/dotnet_il.h src/dotnet_exec.h src/dotnet_host.h src/ui.h src/d3d_common.h src/d3d_format.h src/d3d12_view.h src/stereo.h src/upscale_core.h src/d3d_tables.h src/d3d12_tables.h src/dxil.h src/dxil_msl.h src/dxil_ir.h src/gpu_ir.h src/gpu_ir_msl.h src/gpu_ir_metal.h src/spirv.h src/spirv_ir.h src/vx_vulkan.h src/vk_impl.h src/vk_encode.h src/vk_msl.h src/vk_win32.h src/vk_dispatch_table.h src/directinput_buffer.h src/directinput_format.h src/audio_tables.h src/xaudio2_tables.h src/dwrite_tables.h src/dxbc.h src/aes_gcm.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Ithird_party/vulkan -fobjc-arc -o $@.tmp $(SRC) -framework Cocoa -framework CoreFoundation -framework CoreServices -framework CoreText -framework Metal -framework MetalFX -framework QuartzCore -framework GameController -framework CoreGraphics -framework IOSurface -framework AudioToolbox -framework CoreAudio -framework ImageIO -framework OpenGL -framework IOKit -framework SystemConfiguration -framework CFNetwork -framework Security -Wl,-pagezero_size,0x10000 -licucore -lz && mv -f $@.tmp $@

# NPU mode's frame interpolator (src/npu_interp.m, VXRT_NPU_INTERP=1): a native arm64 process, since VideoToolbox's
# frame interpolation is unsupported under Rosetta.
build/vxrt-npu: tools/vxrt_npu.m src/npu_proto.h
	@mkdir -p build
	$(CC) -arch arm64 -O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=14.0 -fobjc-arc -o $@ tools/vxrt_npu.m -framework Foundation -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework IOSurface

# The ARM64 build (phase 4, TRANSLATOR.md): the same runtime as ARM64 code, the program run by the translator's
# engine in the process. Thunks for every ms_abi function come from the sources' C signatures (tools/vx_thunks.py).
ARM64_SRC = $(filter-out src/context.S,$(SRC)) src/arm64.c src/context_arm64.S translator/xlat.c translator/x87.c translator/x86dec.c translator/engine.c translator/vcpu.c
build/thunks/stamp: $(filter %.c %.m,$(SRC)) src/vx.h tools/vx_thunks.py tools/vx_abi_scan.py
	@mkdir -p build/thunks
	python3 tools/vx_thunks.py build/thunks $(filter %.c %.m,$(SRC))
	@touch $@
# Code that runs inside the vCPU guest (translator/guest.c): no calls, no globals, no stack protector.
GUEST_CFLAGS = -arch arm64 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-stack-check -mmacosx-version-min=14.0 -Itranslator
build/guest.o: translator/guest.c translator/vcpu.h translator/xlat.h translator/xvheap.h
	@mkdir -p build
	$(CC) $(GUEST_CFLAGS) -c $< -o $@
build/libvxrt_vm.dylib: src/vm_hooks.c translator/xvheap.h
	@mkdir -p build
	$(CC) -arch arm64 -O1 -mmacosx-version-min=14.0 -dynamiclib -install_name @executable_path/libvxrt_vm.dylib -Itranslator -o $@ $<
build/vxrt-arm64: $(ARM64_SRC) src/npu_interp.h src/npu_proto.h build/thunks/stamp build/libvxrt_vm.dylib build/guest.o src/arm64.h src/pe.h src/dotnet_metadata.h src/dotnet_resources.h src/dotnet_baml.h src/ui.h src/d3d_common.h src/d3d_format.h src/d3d12_view.h src/stereo.h src/d3d_tables.h src/d3d12_tables.h src/dxil.h src/dxil_msl.h src/dxil_ir.h src/gpu_ir.h src/gpu_ir_msl.h src/gpu_ir_metal.h src/spirv.h src/spirv_ir.h src/vx_vulkan.h src/directinput_buffer.h src/directinput_format.h src/audio_tables.h src/xaudio2_tables.h src/dwrite_tables.h src/dxbc.h translator/xlat.h translator/xlat_sse.inc translator/a64.h translator/engine.h translator/x87.h translator/cpuid_table.h
	$(CC) -arch arm64 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -mmacosx-version-min=14.0 -Isrc -Ibuild -Ithird_party/vulkan -fobjc-arc -o $@ $(ARM64_SRC) -framework Cocoa -framework CoreFoundation -framework CoreServices -framework CoreText -framework Metal -framework MetalFX -framework QuartzCore -framework GameController -framework CoreGraphics -framework IOSurface -framework AudioToolbox -framework CoreAudio -framework ImageIO -framework OpenGL -framework IOKit -framework SystemConfiguration -framework CFNetwork -framework Security -framework Hypervisor build/libvxrt_vm.dylib build/guest.o -licucore -lz
	codesign -s - -f --entitlements translator/hypervisor.entitlements $@

tests: build/hello_k32.exe build/hello_crt.exe build/threads.exe build/cxx_exceptions.exe build/hw_exceptions.exe build/seh_try.exe build/unwind.exe build/breakpoint.exe build/child_stdout.exe \
       build/files.exe build/dll_test.exe build/vxplugin.dll build/dllprobe.exe \
       build/window.exe build/winsta.exe build/scm.exe build/net.exe build/winhttp.exe build/winhttp_proxy.exe build/iphlp.exe build/winsock_nsp.exe build/midi.exe build/directinput.exe build/msvc_eh.exe build/com.exe build/msxml.exe build/vxcom.dll build/iocp.exe build/d3d11.exe build/egl_host.exe build/gl.exe build/hlslc.exe build/dxbc2msl build/draw.exe build/compute.exe build/d3d12.exe build/d3d12_engine.exe build/d3d12_fxc.exe build/d3d11on12.exe build/input.exe build/audio.exe build/xaudio2.exe build/dwrite.exe build/dxbcscan build/pipes.exe build/ipc.exe build/shared.exe build/steam.exe build/smc.exe build/order.exe build/lowmem.exe build/clip.exe build/shoplist.exe build/vulkan_win.exe build/vulkan_features.exe build/vulkan_shaders.exe build/game_imports.exe build/ucrt_extra.exe build/kernelx.exe build/jobs.exe build/security.exe \
       build/sysdlls.exe build/dbghelp.exe build/crypt32.exe build/webhelper_apis.exe build/steam_client_ui.exe build/handle_xfer.exe build/open_thread.exe build/version_info.exe build/section_access.exe build/dbfiles.exe build/updater_files.exe build/shlock.exe build/net_events.exe build/regwait.exe build/tcptable.exe build/winprops.exe build/a11y.exe build/gdi_xform.exe build/view_align.exe \
       build/client_imports.exe build/hello32.exe build/clipboard.exe build/input_route.exe build/name_reuse.exe

build/hello_k32.exe: tests/hello_k32.c
	@mkdir -p build
	$(MINGW) -O1 -nostdlib -e start -o $@ $< -lkernel32

build/hello_crt.exe: tests/hello_crt.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/game_imports.exe: tests/game_imports.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/ucrt_extra.exe: tests/ucrt_extra.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/kernelx.exe: tests/kernelx.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/jobs.exe: tests/jobs.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/security.exe: tests/security.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

# -static: winpthread and libgcc are linked into the program, so it imports only the system DLLs.
build/threads.exe: tests/threads.c
	@mkdir -p build
	$(MINGW) -O1 -static -o $@ $<

build/lowmem.exe: tests/lowmem.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/hw_exceptions.exe: tests/hw_exceptions.c
	@mkdir -p build
	$(MINGW) -O1 -static -o $@ $<
# __try/__except is MSVC's extension: clang compiles it for the MinGW target (gcc has no __try), MinGW links it
build/seh_try.exe: tests/seh_try.c
	@mkdir -p build
	clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -isystem $(shell $(MINGW) -print-sysroot)/x86_64-w64-mingw32/include -c -o build/seh_try.o $<
	$(MINGW) -static -o $@ build/seh_try.o

build/unwind.exe: tests/unwind.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $<

build/breakpoint.exe: tests/breakpoint.c
	@mkdir -p build
	$(MINGW) -O1 -static -o $@ $<

build/child_stdout.exe: tests/child_stdout.c
	@mkdir -p build
	$(MINGW) -O1 -static -o $@ $<

# Pipe ends inherited by a child as asked (CreatePipe's attributes, SetHandleInformation): src/inherit.c.
tests: build/inherit.exe
build/inherit.exe: tests/inherit.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -static -o $@ $<

build/cxx_exceptions.exe: tests/cxx_exceptions.cpp
	@mkdir -p build
	$(MINGWXX) -O1 -static -o $@ $<

build/callbench.exe: tests/callbench.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/smc.exe: tests/smc.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/order.exe: tests/order.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/window.exe: tests/window.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lgdi32 -lwinmm

build/winsta.exe: tests/winsta.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/scm.exe: tests/scm.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

# (<VXRT_ROOT>/program_env.txt: src/launchargs.c)
tests: build/program_env.exe
build/program_env.exe: tests/program_env.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

# (VXRT_EXTRA_ARGS: src/launchargs.c)
tests: build/extra_args.exe
build/extra_args.exe: tests/extra_args.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lshell32

build/clip.exe: tests/clip.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lgdi32

# GDI to the screen (tests/present.c, driven by tests/present_check.py): made-up gradients through every path.
tests: build/present.exe
build/present.exe: tests/present.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lgdi32 -lmsimg32 -ldwmapi

# The notification area (tests/tray.c, driven by tests/tray_check.py): a made-up icon file, also the program's resources,
# and made-up DLLs for ExtractIconEx (tests/tray_pe.py writes build/tray_pe_*.dll).
tests: build/tray.exe build/tray_pe.stamp
build/tray.ico: tests/tray_ico.py
	@mkdir -p build
	python3 tests/tray_ico.py $@
build/tray_pe.stamp: tests/tray_pe.py tests/tray_ico.py
	@mkdir -p build
	python3 tests/tray_pe.py build && touch $@
build/tray_res.o: tests/tray.rc build/tray.ico
	x86_64-w64-mingw32-windres -I build -i $< -o $@
build/tray.exe: tests/tray.c build/tray_res.o
	$(MINGW) -O1 -o $@ $< build/tray_res.o -lgdi32

build/shoplist.exe: tests/shoplist.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lgdi32

build/net.exe: tests/net.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lws2_32

# ReadFileEx/WriteFileEx: completion routines in alertable waits.
tests: build/file_io_ex.exe
build/file_io_ex.exe: tests/file_io_ex.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $<

# ShellExecute(Ex) with classes registered per user, and one process seeing what another saved in the registry.
tests: build/shellexec.exe
build/shellexec.exe: tests/shellexec.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $< -lshell32 -ladvapi32

# Registered waits and timer-queue timers ended with a completion event, as Chromium's ObjectWatcher does (src/system.c).
build/regwait.exe: tests/regwait.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $<

# Thread-pool callback instances: the *WhenCallbackReturns actions, CallbackMayRunLong, DisassociateCurrentThreadFromCallback.
tests: build/tp_callbacks.exe build/fiber_pool.exe
# Fibers as job systems use them: gs:0x20 read inline, fibers moving between threads.
build/fiber_pool.exe: tests/fiber_pool.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $<
build/tp_callbacks.exe: tests/tp_callbacks.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $<

# Views of file mappings on the 64 KB allocation granularity (src/files.c).
build/view_align.exe: tests/view_align.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $<

# A DC's world transform, ROP2 and fill mode (src/gdi.c).
build/gdi_xform.exe: tests/gdi_xform.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $< -lgdi32

# oleacc, UIAutomationCore, uxtheme without assistive technology or visual styles (src/a11y.c).
build/a11y.exe: tests/a11y.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $< -loleacc -luxtheme -luser32 -lole32

# SHGetPropertyStoreForWindow: a window's property store keeps what is set in it (src/winprops.c).
build/winprops.exe: tests/winprops.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $< -lshell32 -lole32 -luser32 -luuid

# The Windows Steam client's sign-in imports (imagehlp certificates, RegDeleteTree, the event log, WSASendMsg, shutdown
# reasons, touch injection, SHOpenWithDialog, RtlCaptureStackBackTrace, ...) and CreateProcess of a 32-bit program
# (build/hello32.exe, built for i386, is refused with ERROR_BAD_EXE_FORMAT).
build/client_imports.exe: tests/client_imports.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -D_WIN32_WINNT=0x0A00 -o $@ $< -limagehlp -lws2_32 -ladvapi32 -lshell32 -lole32 -luser32
build/hello32.exe: tests/hello32.c
	@mkdir -p build
	i686-w64-mingw32-gcc -O1 -o $@ $<

# The clipboard on a pasteboard of the test's own (src/clipboard.m).
build/clipboard.exe: tests/clipboard.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $< -luser32

# Input through child windows (tests/input_route_check.py drives it through Command Clips).
build/input_route.exe: tests/input_route.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $< -limm32 -lgdi32

# GetExtendedTcpTable: a loopback connection's rows with this process as their owner (src/tcptable.c).
build/tcptable.exe: tests/tcptable.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $< -liphlpapi -lws2_32

# WSAEventSelect's network events and their error codes, as Chromium's sockets use them (src/net.c).
build/net_events.exe: tests/net_events.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $< -lws2_32

build/winhttp.exe: tests/winhttp.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lwinhttp -lws2_32

build/winhttp_proxy.exe: tests/winhttp_proxy.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lwinhttp -lws2_32

build/iphlp.exe: tests/iphlp.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -liphlpapi -lws2_32

build/winsock_nsp.exe: tests/winsock_nsp.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lws2_32

build/midi.exe: tests/midi.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lwinmm

# userenv, winspool.drv, dhcpcsvc, secur32, oleaut32 by ordinal (src/sysdlls.c); dbghelp's stack walk and symbols (src/dbghelp.c).
build/sysdlls.exe: tests/sysdlls.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -luserenv -lwinspool -ldhcpcsvc -lsecur32 -liphlpapi -loleaut32

# IsOS, downlevel API sets, touch and pointer input, RegNotifyChangeKeyValue, NtQuerySection (src/osinfo.c, pointer.c,
# regnotify.c, section.c): what the Windows Steam client's steamwebhelper stopped at.
build/webhelper_apis.exe: tests/webhelper_apis.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lgdi32

# TLS expansion slots, GetClassWord(GCW_ATOM), WindowFromPoint, WM_WINDOWPOSCHANGING/CHANGED (src/kernel32.c, client.c,
# user32.c): what the Windows Steam client's steam.exe needed past its Steam Service Error box.
build/steam_client_ui.exe: tests/steam_client_ui.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

# DuplicateHandle between processes both ways: pipe ends and events taken from and given to another process
# (src/handlexfer.c): Steam's process pipes.
build/handle_xfer.exe: tests/handle_xfer.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

# OpenThread by ID for any running thread (the main thread; one whose CreateThread handle was closed) (src/sync.c):
# Valve's tier0 freed the data of Steam's main thread when it could not open it.
build/open_thread.exe: tests/open_thread.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

# version.dll (src/version.c): a file's version resource (this test's own, made-up: tests/version_info.rc), a runtime
# DLL's made-up one, VerQueryValueA/W. Chromium (Steam's web helper) stopped when kernel32.dll had none.
build/version_info_res.o: tests/version_info.rc
	@mkdir -p build
	x86_64-w64-mingw32-windres -i $< -o $@
build/version_info.exe: tests/version_info.c build/version_info_res.o
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< build/version_info_res.o -lversion

# What a file mapping handle may do (src/sync.c, src/files.c): no more access through DuplicateHandle than its source,
# views by access. Chromium's read-only shared memory check (Steam's web helper).
build/section_access.exe: tests/section_access.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

build/dbghelp.exe: tests/dbghelp.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ldbghelp

# crypt32/wintrust with made-up certificates (tests/crypt32_certs.sh made them) and DPAPI
build/crypt32.exe: tests/crypt32.c tests/crypt32_certs.h
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lcrypt32 -lwintrust

build/directinput.exe: tests/directinput.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ldinput8 -ldxguid

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

build/msxml.exe: tests/msxml.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lole32 -loleaut32 -luuid

build/vxcom.dll: tests/vxcom.c
	@mkdir -p build
	$(MINGW) -O1 -shared -o $@ $< -luuid

build/iocp.exe: tests/iocp.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

# What ANGLE asks of the system before it makes an EGL display on Direct3D 11 (tests/egl_host.c).
build/egl_host.exe: tests/egl_host.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ld3d11 -ldxgi -ldxguid -luuid -lgdi32

build/gl.exe: tests/gl.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lopengl32 -lgdi32

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

# The translator (TRANSLATOR.md) is native ARM64 code: its tools build for this Mac, not for Rosetta.
HOSTCC = clang -O2 -Wall -Wextra
build/x86dec_test: translator/x86dec.c translator/x86dec.h translator/x86dec_test.c
	@mkdir -p build
	$(HOSTCC) translator/x86dec.c translator/x86dec_test.c -o $@
# llvm-objdump -d --no-show-raw-insn <pe> | build/x86dec_check <pe>: the decoder against LLVM on a real binary.
build/x86dec_check: translator/x86dec.c translator/x86dec.h translator/x86dec_check.c
	@mkdir -p build
	$(HOSTCC) translator/x86dec.c translator/x86dec_check.c -o $@

# The differential test (TRANSLATOR.md): ref_x86 runs instructions for real under Rosetta 2, difftest translates them
# and compares. `make translator-check` runs 3000 cases per form; make check a quick 200.
build/ref_x86: translator/diff/ref_x86.c translator/diff/difftest.h
	@mkdir -p build
	$(CC) -arch x86_64 -O1 -Wall -o $@ translator/diff/ref_x86.c
build/difftest: build/guest.o translator/vcpu.c translator/vcpu.h translator/diff/difftest.c translator/diff/difftest.h translator/xlat.c translator/xlat_sse.inc translator/xlat.h translator/a64.h translator/x86dec.c translator/x86dec.h translator/x87.c translator/x87.h translator/engine.c translator/engine.h translator/cpuid_table.h
	@mkdir -p build
	$(HOSTCC) -Wno-missing-field-initializers translator/x86dec.c translator/xlat.c translator/x87.c translator/engine.c translator/vcpu.c translator/diff/difftest.c build/guest.o -framework Hypervisor -o $@
	codesign -s - -f --entitlements translator/hypervisor.entitlements $@
# translator coverage over the installed games' PEs (every function in .pdata): `make coverage`
build/coverage: translator/cpuid_table.h translator/coverage.c translator/xlat.c translator/xlat_sse.inc translator/xlat.h translator/a64.h translator/x86dec.c translator/x86dec.h translator/x87.c translator/x87.h
	@mkdir -p build
	$(HOSTCC) -Wno-missing-field-initializers -Itranslator translator/coverage.c translator/x86dec.c translator/xlat.c translator/x87.c -o $@
coverage: build/coverage
	python3 translator/coverage.py build/coverage $(HOME)/Verantyx_VR_Drive/vxrt_steam/steamapps/common
# the engine against Rosetta 2 and native ARM64 on the same kernels: `make xlat-bench`
BENCH_X86 = -arch x86_64 -O2 -fno-stack-protector -ffreestanding -fno-builtin -fno-vectorize
build/bench_kernels_x86.o: translator/bench/kernels.c
	@mkdir -p build
	$(CC) $(BENCH_X86) -c $< -o $@
build/bench_image: build/bench_kernels_x86.o
	ld -arch x86_64 -preload -e _start -segaddr __TEXT 0x500000000 $< -o $@
build/bench_rosetta: build/bench_kernels_x86.o translator/bench/rosetta_main.c
	$(CC) -arch x86_64 -O2 translator/bench/rosetta_main.c build/bench_kernels_x86.o -o $@
build/bench_native: translator/bench/kernels.c translator/bench/rosetta_main.c
	$(CC) -arch arm64 -O2 -fno-stack-protector -fno-builtin -fno-vectorize translator/bench/rosetta_main.c translator/bench/kernels.c -o $@
build/bench: build/guest.o translator/bench/bench.c translator/vcpu.c translator/vcpu.h translator/engine.c translator/engine.h translator/xlat.c translator/xlat_sse.inc translator/xlat.h translator/a64.h translator/x86dec.c translator/x87.c translator/cpuid_table.h
	$(HOSTCC) -Wno-missing-field-initializers -Itranslator translator/bench/bench.c translator/engine.c translator/vcpu.c translator/xlat.c translator/x87.c translator/x86dec.c build/guest.o -framework Hypervisor -o $@
	codesign -s - -f --entitlements translator/hypervisor.entitlements $@
xlat-bench: build/bench build/bench_image build/bench_rosetta build/bench_native
	@echo "== Rosetta 2"; build/bench_rosetta 5
	@echo "== VXRuntime engine"; build/bench build/bench_image 5 0
	@echo "== VXRuntime engine, ordered accesses (in-process, no TSO)"; build/bench build/bench_image 5 1
	@echo "== native ARM64"; build/bench_native 5
# x87 transcendentals against exact values (slow: decimal arithmetic): `make x87-exact`
build/x87_ref: translator/diff/x87_ref.c
	@mkdir -p build
	$(CC) -arch x86_64 -O1 -Wall -o $@ translator/diff/x87_ref.c
build/x87_ours: translator/diff/x87_ours.c translator/x87.c translator/x87.h translator/xlat.h
	@mkdir -p build
	$(HOSTCC) -O1 -Wall -Itranslator -o $@ translator/diff/x87_ours.c translator/x87.c
x87-exact: build/x87_ref build/x87_ours
	python3 translator/diff/x87_exact.py build/x87_ref build/x87_ours 500
translator-check: build/ref_x86 build/difftest build/x86dec_test
	build/x86dec_test
	build/difftest build/ref_x86 3000
	DT_ORDERED=1 build/difftest build/ref_x86 1000    # (the ARM64 runtime's default: x86's order with LDAPR / STLR)

build/hvprobe: translator/hvprobe.c translator/hypervisor.entitlements
	@mkdir -p build
	$(HOSTCC) translator/hvprobe.c -framework Hypervisor -o $@
	codesign -s - -f --entitlements translator/hypervisor.entitlements $@

build/dxbcscan: tools/dxbcscan.m src/dxbc.c src/dxbc.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ tools/dxbcscan.m src/dxbc.c -framework Metal -framework Foundation

build/dxbc2msl: tools/dxbc2msl.m src/dxbc.c src/dxbc.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ tools/dxbc2msl.m src/dxbc.c -framework Metal

# DXIL (Shader Model 6, Direct3D 12): src/dxil.c reads the container and its LLVM bitcode.
build/dxildump: tools/dxildump.c src/dxil.c src/dxil.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/dxildump.c src/dxil.c

# DXIL -> Metal Shading Language (src/dxil_msl.c): translated and compiled (dxil2msl), compute shaders run and checked (dxilrun).
build/dxil2msl: tools/dxil2msl.m src/dxil_msl.c src/dxil_msl.h src/dxil.c src/dxil.h
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=14.0 -fobjc-arc -o $@ tools/dxil2msl.m src/dxil_msl.c src/dxil.c -framework Metal -framework Foundation

build/dxilrun: tools/dxilrun.m src/dxil_msl.c src/dxil_msl.h src/dxil.c src/dxil.h
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=14.0 -fobjc-arc -o $@ tools/dxilrun.m src/dxil_msl.c src/dxil.c -framework Metal -framework Foundation

build/dxil_msl_test: src/dxil_msl.c src/dxil_msl.h src/dxil.c src/dxil.h tests/dxil_msl.c tests/shaders12/bindless_ps.dxil tests/shaders12/samplegrad_ps.dxil tests/shaders12/cmp_ps.dxil tests/shaders12/sum_cs.dxil
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ src/dxil_msl.c src/dxil.c tests/dxil_msl.c

build/dxil_ir_test.o: tests/dxil_ir.c src/dxil_ir.h src/dxil_msl.h src/gpu_ir.h src/gpu_ir_msl.h src/gpu_ir_metal.h
	@mkdir -p build
	$(HOSTCC) -Isrc -c -o $@ tests/dxil_ir.c

build/dxil_ir_test: src/dxil_ir.c src/dxil_ir.h src/dxil_msl.c src/dxil_msl.h src/dxil.c src/dxil.h src/gpu_ir.c src/gpu_ir.h src/gpu_ir_msl.c src/gpu_ir_msl.h src/gpu_ir_metal.m src/gpu_ir_metal.h build/dxil_ir_test.o tests/shaders12/typed_buffer_cs.dxil tests/shaders12/typed_sint_buffer_cs.dxil tests/shaders12/typed_float_buffer_cs.dxil tests/shaders12/texture_dimensions_cs.dxil tests/shaders12/texture_dimensions_msaa_cs.dxil tests/shaders12/texture_load_msaa_cs.dxil tests/shaders12/texload_bounds_cs.dxil tests/shaders12/texload_array_bounds_cs.dxil tests/shaders12/texload_sint_bounds_cs.dxil tests/shaders12/texload_array_sint_bounds_cs.dxil tests/shaders12/texload_float_bounds_cs.dxil tests/shaders12/signed_conversion_cs.dxil tests/shaders12/integer_minmax_buffer_cs.dxil tests/shaders12/integer_mad_buffer_cs.dxil tests/shaders12/float_mad_buffer_cs.dxil tests/shaders12/float_minmax_buffer_cs.dxil tests/shaders12/wave_ops_partial_cs.dxil tests/shaders12/samplegrad_ps.dxil
	@mkdir -p build
	$(HOSTCC) -fobjc-arc -Isrc -o $@ build/dxil_ir_test.o src/dxil_ir.c src/dxil_msl.c src/dxil.c src/gpu_ir.c src/gpu_ir_msl.c src/gpu_ir_metal.m -framework Metal -framework Foundation

build/dxil_core_compute.metal: build/dxil_ir_test
	build/dxil_ir_test --write-core-compute $@

build/dxil_core_compute.air: build/dxil_core_compute.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_wave_queries.metal: build/dxil_ir_test
	build/dxil_ir_test --write-common-wave-queries $@

build/dxil_wave_queries.air: build/dxil_wave_queries.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_bit_intrinsics.metal: build/dxil_ir_test
	build/dxil_ir_test --write-bit-intrinsics $@

build/dxil_bit_intrinsics.air: build/dxil_bit_intrinsics.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_fragment_discard.metal: build/dxil_ir_test
	build/dxil_ir_test --write-fragment-discard $@

build/dxil_fragment_discard.air: build/dxil_fragment_discard.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_target0_fragment.metal: build/dxil_ir_test
	build/dxil_ir_test --write-dxil-target0 $@

build/dxil_target0_fragment.air: build/dxil_target0_fragment.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_vertex_position.metal: build/dxil_ir_test
	build/dxil_ir_test --write-dxil-vertex-position $@

build/dxil_vertex_position.air: build/dxil_vertex_position.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_vertex_input.metal: build/dxil_ir_test
	build/dxil_ir_test --write-dxil-vertex-input $@

build/dxil_vertex_input.air: build/dxil_vertex_input.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_vertex_system_values.metal: build/dxil_ir_test
	build/dxil_ir_test --write-dxil-vertex-system-values $@

build/dxil_vertex_system_values.air: build/dxil_vertex_system_values.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_typed_buffer.metal: build/dxil_ir_test tests/shaders12/typed_buffer_cs.dxil
	build/dxil_ir_test --write-typed-buffer $@

build/dxil_typed_buffer.air: build/dxil_typed_buffer.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_ir_raw_buffer_load.metal: build/dxil_ir_test tests/shaders12/typed_buffer_cs.dxil
	build/dxil_ir_test --write-raw-buffer-common $@

build/dxil_ir_raw_buffer_load.air: build/dxil_ir_raw_buffer_load.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_ir_byteaddress_load_lanes.metal: build/dxil_ir_test tests/shaders12/typed_buffer_cs.dxil tests/shaders12/typed_float_buffer_cs.dxil
	build/dxil_ir_test --write-byteaddress-load-lanes $@

build/dxil_ir_byteaddress_load_lanes.air: build/dxil_ir_byteaddress_load_lanes.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_typed_sint_buffer.metal: build/dxil_ir_test tests/shaders12/typed_sint_buffer_cs.dxil
	build/dxil_ir_test --write-typed-sint-buffer $@

build/dxil_typed_sint_buffer.air: build/dxil_typed_sint_buffer.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_typed_float_buffer.metal: build/dxil_ir_test tests/shaders12/typed_buffer_cs.dxil
	build/dxil_ir_test --write-typed-float-buffer $@

build/dxil_typed_float_buffer.air: build/dxil_typed_float_buffer.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_signed_conversions.metal: build/dxil_ir_test tests/shaders12/signed_conversion_cs.dxil
	build/dxil_ir_test --write-signed-conversions $@

build/dxil_signed_conversions.air: build/dxil_signed_conversions.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_typed_float_real.metal: build/dxil_ir_test tests/shaders12/typed_float_buffer_cs.dxil
	build/dxil_ir_test --write-typed-float-dxil $@

build/dxil_typed_float_real.air: build/dxil_typed_float_real.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_dimensions.metal: build/dxil_ir_test tests/shaders12/texture_dimensions_cs.dxil
	build/dxil_ir_test --write-texture-dimensions $@

build/dxil_texture_dimensions.air: build/dxil_texture_dimensions.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_dimensions_lod1.metal: build/dxil_ir_test tests/shaders12/texture_dimensions_cs.dxil
	build/dxil_ir_test --write-texture-dimensions-lod1 $@

build/dxil_texture_dimensions_lod1.air: build/dxil_texture_dimensions_lod1.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_dimensions_array.metal: build/dxil_ir_test tests/shaders12/texture_dimensions_cs.dxil
	build/dxil_ir_test --write-texture-dimensions-array $@

build/dxil_texture_dimensions_array.air: build/dxil_texture_dimensions_array.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_dimensions_msaa.metal: build/dxil_ir_test tests/shaders12/texture_dimensions_msaa_cs.dxil
	build/dxil_ir_test --write-texture-dimensions-msaa $@

build/dxil_texture_dimensions_msaa.air: build/dxil_texture_dimensions_msaa.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_msaa.metal: build/dxil_ir_test tests/shaders12/texture_load_msaa_cs.dxil
	build/dxil_ir_test --write-texture-load-msaa $@

build/dxil_texture_load_msaa.air: build/dxil_texture_load_msaa.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_msaa_array.metal: build/dxil_ir_test tests/shaders12/texture_load_msaa_cs.dxil
	build/dxil_ir_test --write-texture-load-msaa-array $@

build/dxil_texture_load_msaa_array.air: build/dxil_texture_load_msaa_array.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load.metal: build/dxil_ir_test tests/shaders12/texload_bounds_cs.dxil
	build/dxil_ir_test --write-texture-load $@

build/dxil_texture_load.air: build/dxil_texture_load.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_array.metal: build/dxil_ir_test tests/shaders12/texload_bounds_cs.dxil
	build/dxil_ir_test --write-texture-load-array $@

build/dxil_texture_load_array.air: build/dxil_texture_load_array.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_array_dxil.metal: build/dxil_ir_test tests/shaders12/texload_array_bounds_cs.dxil
	build/dxil_ir_test --write-texture-load-array-dxil $@

build/dxil_texture_load_array_dxil.air: build/dxil_texture_load_array_dxil.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_sint_dxil.metal: build/dxil_ir_test tests/shaders12/texload_sint_bounds_cs.dxil
	build/dxil_ir_test --write-texture-load-sint-dxil $@

build/dxil_texture_load_sint_dxil.air: build/dxil_texture_load_sint_dxil.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_array_sint_dxil.metal: build/dxil_ir_test tests/shaders12/texload_array_sint_bounds_cs.dxil
	build/dxil_ir_test --write-texture-load-array-sint-dxil $@

build/dxil_texture_load_array_sint_dxil.air: build/dxil_texture_load_array_sint_dxil.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_float_dxil.metal: build/dxil_ir_test tests/shaders12/texload_float_bounds_cs.dxil
	build/dxil_ir_test --write-texture-load-float-dxil $@

build/dxil_texture_load_float_dxil.air: build/dxil_texture_load_float_dxil.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_float_channel_dxil.metal: build/dxil_ir_test tests/shaders12/texload_float_bounds_cs.dxil
	build/dxil_ir_test --write-texture-load-float-channel-dxil $@

build/dxil_texture_load_float_channel_dxil.air: build/dxil_texture_load_float_channel_dxil.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_float_channel_z_dxil.metal: build/dxil_ir_test tests/shaders12/texload_float_bounds_cs.dxil
	build/dxil_ir_test --write-texture-load-float-channel-z-dxil $@

build/dxil_texture_load_float_channel_z_dxil.air: build/dxil_texture_load_float_channel_z_dxil.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_float_channel_w_dxil.metal: build/dxil_ir_test tests/shaders12/texload_float_bounds_cs.dxil
	build/dxil_ir_test --write-texture-load-float-channel-w-dxil $@

build/dxil_texture_load_float_channel_w_dxil.air: build/dxil_texture_load_float_channel_w_dxil.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_mapping.metal: build/dxil_ir_test tests/shaders12/texload_float_bounds_cs.dxil
	build/dxil_ir_test --write-direct-texture-load-mapping $@

build/dxil_texture_load_mapping.air: build/dxil_texture_load_mapping.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_mapping_uint.metal: build/dxil_ir_test tests/shaders12/texload_bounds_cs.dxil
	build/dxil_ir_test --write-direct-texture-load-mapping-uint $@

build/dxil_texture_load_mapping_uint.air: build/dxil_texture_load_mapping_uint.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_texture_load_mapping_sint.metal: build/dxil_ir_test tests/shaders12/texload_sint_bounds_cs.dxil
	build/dxil_ir_test --write-direct-texture-load-mapping-sint $@

build/dxil_texture_load_mapping_sint.air: build/dxil_texture_load_mapping_sint.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_typed_buffer_load.metal: build/dxil_ir_test tests/shaders12/typed_buffer_cs.dxil
	build/dxil_ir_test --write-direct-typed-buffer-mapping $@

build/dxil_typed_buffer_load.air: build/dxil_typed_buffer_load.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_typed_buffer_texture_load.metal: build/dxil_ir_test tests/shaders12/typed_buffer_cs.dxil
	build/dxil_ir_test --write-direct-typed-buffer-texture-load-mapping $@

build/dxil_typed_buffer_texture_load.air: build/dxil_typed_buffer_texture_load.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_raw_buffer_load.metal: build/dxil_ir_test tests/shaders12/typed_buffer_cs.dxil
	build/dxil_ir_test --write-direct-raw-buffer-mapping $@

build/dxil_raw_buffer_load.air: build/dxil_raw_buffer_load.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_structured_buffer_load.metal: build/dxil_ir_test tests/shaders12/typed_buffer_cs.dxil
	build/dxil_ir_test --write-direct-structured-buffer-mapping $@

build/dxil_structured_buffer_load.air: build/dxil_structured_buffer_load.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_derivative_modes.metal: build/dxil_ir_test tests/shaders12/samplegrad_ps.dxil
	build/dxil_ir_test --write-derivative-modes $@

build/dxil_derivative_modes.air: build/dxil_derivative_modes.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_unary_intrinsics.metal: build/dxil_ir_test
	build/dxil_ir_test --write-unary-intrinsics $@

build/dxil_unary_intrinsics.air: build/dxil_unary_intrinsics.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_integer_minmax.metal: build/dxil_ir_test tests/shaders12/integer_minmax_buffer_cs.dxil
	build/dxil_ir_test --write-integer-minmax $@

build/dxil_integer_minmax.air: build/dxil_integer_minmax.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_integer_mad.metal: build/dxil_ir_test tests/shaders12/integer_mad_buffer_cs.dxil
	build/dxil_ir_test --write-integer-mad $@

build/dxil_integer_mad.air: build/dxil_integer_mad.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_vector_integer_mad.metal: build/dxil_ir_test
	build/dxil_ir_test --check-vector-integer-mad $@

build/dxil_vector_integer_mad.air: build/dxil_vector_integer_mad.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_float_mad.metal: build/dxil_ir_test tests/shaders12/float_mad_buffer_cs.dxil
	build/dxil_ir_test --write-float-mad $@

build/dxil_float_mad.air: build/dxil_float_mad.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_float_minmax.metal: build/dxil_ir_test tests/shaders12/float_minmax_buffer_cs.dxil
	build/dxil_ir_test --write-float-minmax $@

build/dxil_float_minmax.air: build/dxil_float_minmax.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_atomic_add.metal: build/dxil_ir_test
	build/dxil_ir_test --write-dxil-atomic-add $@

build/dxil_atomic_add.air: build/dxil_atomic_add.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_atomic_ops.metal: build/dxil_ir_test
	build/dxil_ir_test --write-dxil-atomic-ops $@

build/dxil_atomic_ops.air: build/dxil_atomic_ops.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxil_atomic_compare_exchange.metal: build/dxil_ir_test
	build/dxil_ir_test --write-dxil-atomic-compare-exchange $@

build/dxil_atomic_compare_exchange.air: build/dxil_atomic_compare_exchange.metal
	xcrun -sdk macosx metal -c $< -o $@

build/dxc.exe: tools/dxc.cpp
	@mkdir -p build
	$(MINGWXX) -O1 -static -o $@ $<

build/draw.exe: tests/draw.c tests/shaders/*.h
	@mkdir -p build
	$(MINGW) -O1 -Itests -o $@ $< -ld3d11 -ldxgi -ldxguid -luuid -lm

build/compute.exe: tests/compute.c tests/shaders/*.h
	@mkdir -p build
	$(MINGW) -O1 -Itests -o $@ $< -ld3d11 -ldxguid -luuid -lm

# Vulkan from a Windows program: an import library of this runtime's vulkan-1.dll (every command it exports, as the
# Vulkan SDK's vulkan-1.lib names them), and a program linked against it that draws, reads back and presents.
build/vulkan-1.def: build/vxrt
	@mkdir -p build
	(echo "LIBRARY vulkan-1.dll"; echo "EXPORTS"; build/vxrt --list-exports | awk '$$1=="vulkan-1.dll" { print "    " $$2 }') > $@

build/libvulkan-1.a: build/vulkan-1.def
	x86_64-w64-mingw32-dlltool -d $< -l $@ -m i386:x86-64

build/vulkan_shaders.exe: tests/vulkan_shaders.c $(wildcard tests/vulkan_shaders/*.h) build/libvulkan-1.a third_party/vulkan/vulkan/vulkan_core.h
	$(MINGW) -O1 -Wall -Ithird_party/vulkan -o $@ $< -Lbuild -lvulkan-1

# The SPIR-V of tests/vulkan_shaders/* is committed (as C arrays): after changing a shader there, run this (needs
# glslang); the tests themselves need no shader compiler.
vulkan-test-shaders:
	cd tests/vulkan_shaders && for f in *.comp *.vert *.frag *.geom *.tesc *.tese; do \
		glslang -V --target-env vulkan1.2 --vn spv_$$(echo $$f | tr . _) -o $$f.h $$f > /dev/null || exit 1; done

build/vulkan_features.exe: tests/vulkan_features.c build/libvulkan-1.a third_party/vulkan/vulkan/vulkan_core.h
	$(MINGW) -O1 -Wall -Ithird_party/vulkan -o $@ $< -Lbuild -lvulkan-1

build/vulkan_win.exe: tests/vulkan_win.c src/vx_vulkan.h build/libvulkan-1.a build/vulkan_spirv_vertex_fixture.spv build/vulkan_spirv_fragment_fixture.spv build/vulkan_spirv_uniform_vertex_fixture.spv build/vulkan_spirv_uniform_fragment_fixture.spv
	$(MINGW) -O1 -Wall -o $@ $< -Lbuild -lvulkan-1

build/d3d12.exe: tests/d3d12.c tests/shaders12/sum_cs.dxil tests/shaders12/tri_vs.dxil tests/shaders12/tri_ps.dxil tests/shaders12/cmp_ps.dxil tests/shaders12/gather_ps.dxil tests/shaders12/gather_cmp_ps.dxil tests/shaders12/dim_ps.dxil tests/shaders12/dim1da_ps.dxil tests/shaders12/typed_buffer_cs.dxil tests/shaders12/typed_sint_buffer_cs.dxil tests/shaders12/integer_minmax_buffer_cs.dxil tests/shaders12/integer_mad_buffer_cs.dxil tests/shaders12/float_mad_buffer_cs.dxil tests/shaders12/float_minmax_buffer_cs.dxil tests/shaders12/signed_conversion_cs.dxil tests/shaders12/typed_float_buffer_cs.dxil tests/shaders12/texture_dimensions_cs.dxil tests/shaders12/texture_dimensions_msaa_cs.dxil tests/shaders12/texture_load_msaa_cs.dxil tests/shaders12/atomic_resources_cs.dxil tests/shaders12/texload_bounds_cs.dxil tests/shaders12/texload_array_bounds_cs.dxil tests/shaders12/texload_sint_bounds_cs.dxil tests/shaders12/texload_array_sint_bounds_cs.dxil tests/shaders12/texload_float_bounds_cs.dxil tests/shaders12/wave_ops_cs.dxil tests/shaders12/wave_ops_partial_cs.dxil tests/shaders12/half_ops_cs.dxil
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ld3d12 -ldxgi -lm

build/d3d12_engine.exe: tests/d3d12_engine.c tests/shaders12/tri_vs.dxil tests/shaders12/tri_ps.dxil tests/shaders12/sum_cs.dxil
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ld3d12 -ldxgi

build/d3d12_fxc.exe: tests/d3d12_fxc.c tests/shaders12/tess_vs.dxil tests/shaders12/tess_hs.dxil tests/shaders12/tess_ds.dxil tests/shaders12/passcolor_vs.dxil tests/shaders12/quad_gs.dxil tests/shaders12/passcolor_ps.dxil tests/shaders/tex_vs.dxbc tests/shaders/tex_ps.dxbc tests/shaders/sum_cs.dxbc tests/shaders/append_cs.dxbc tests/shaders/color_vs.dxbc tests/shaders/quad_gs.dxbc tests/shaders/color_ps.dxbc
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ld3d12

build/d3d11on12.exe: tests/d3d11on12.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ld3d12 -ld3d11

build/d3d12_view_test: src/d3d12_view.h src/gpu_ir.h tests/d3d12_view.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ tests/d3d12_view.c

build/d3d_format_test: src/d3d_format.c src/d3d_format.h tests/d3d_format.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ tests/d3d_format.c src/d3d_format.c

build/input.exe: tests/input.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lxinput

build/audio.exe: tests/audio.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lole32 -luuid -lm

build/xaudio2.exe: tests/xaudio2.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -lxaudio2_9 -lm

build/dwrite.exe: tests/dwrite.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ldwrite -lgdi32 -lm

# Vulkan images shared with Direct3D 11 through Win32 handles (VK_KHR_external_memory_win32).
tests: build/vulkan_external.exe
build/vulkan_external.exe: tests/vulkan_external.c build/libvulkan-1.a third_party/vulkan/vulkan/vulkan_core.h
	$(MINGW) -O1 -Wall -Ithird_party/vulkan -o $@ $< -Lbuild -lvulkan-1 -ld3d11 -ldxgi -ldxguid -luuid

build/shared.exe: tests/shared.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ld3d11 -ldxgi -ldxguid -luuid

build/name_reuse.exe: tests/name_reuse.c
	@mkdir -p build
	$(MINGW) -O1 -o $@ $<

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

# A database engine's file calls (SQLite's documented patterns: locks near 1 GiB, OVERLAPPED offsets, a mapped
# database, a -shm file, deleting on close): src/filestate.c, src/files.c.
build/dbfiles.exe: tests/dbfiles.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $<

# A self-updating program's file calls (Steam's bootstrapper): seeking before the start of a file, ...: src/files.c.
build/updater_files.exe: tests/updater_files.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $<

# A process ending while it holds the lock of a named object's shared state: the next one takes it over (src/shlock.c).
build/shlock.exe: tests/shlock.c
	@mkdir -p build
	$(MINGW) -O1 -Wall -o $@ $<

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
SHADERS = color_vs:vs_4_0 color_ps:ps_4_0 tex_vs:vs_4_0 tex_ps:ps_4_0 fullscreen_vs:vs_4_0 math_ps:ps_4_0 sum_cs:cs_5_0 atomic_cs:cs_5_0 append_cs:cs_5_0 views_cs:cs_5_0 formats_cs:cs_5_0 features_ps:ps_5_0 features_vs:vs_5_0 features_cs:cs_5_0 quad_gs:gs_4_0 bounds_cs:cs_5_0 cbounds_cs:cs_5_0
shaders: build/vxrt build/hlslc.exe
	@for s in $(SHADERS); do n=$${s%%:*}; p=$${s##*:}; \
	  VXRT_ROOT=build/root build/vxrt build/hlslc.exe '$(D3DCOMPILER)' tests/shaders/$$n.hlsl main $$p tests/shaders/$$n.h $$n || exit 1; done

# Shader Model 6 test shaders: HLSL compiled to DXIL by Microsoft's dxcompiler.dll running on vxrt (Chromium ships one
# beside SteamVR's vrwebhelper), with dxcompiler's own disassembly of each as the reference tools/dxil_check.py reads
# src/dxil.c's reading against. tests/shaders12/*.dxil and *.dis.txt are kept in the repository; regenerate with
#   make shaders12 DXCOMPILER='Z:\path\to\dxcompiler.dll'
SHADERS12 = color_ps:ps_6_0 tex_ps:ps_6_0 sum_cs:cs_6_0 color_vs:vs_6_0 bindless_ps:ps_6_6 tri_vs:vs_6_0 tri_ps:ps_6_0 texq_vs:vs_6_0 texq_ps:ps_6_0 cmp_ps:ps_6_0 gather_ps:ps_6_0 gather_cmp_ps:ps_6_0 dim_ps:ps_6_0 dim1da_ps:ps_6_0 vector_math_ps:ps_6_0 samplegrad_ps:ps_6_0 atomic_resources_cs:cs_6_0 texload_bounds_cs:cs_6_0 texload_array_bounds_cs:cs_6_0 texload_sint_bounds_cs:cs_6_0 texload_array_sint_bounds_cs:cs_6_0 texload_float_bounds_cs:cs_6_0 signed_conversion_cs:cs_6_0 typed_sint_buffer_cs:cs_6_0 typed_float_buffer_cs:cs_6_0 texture_dimensions_cs:cs_6_0 texture_dimensions_msaa_cs:cs_6_0 texture_load_msaa_cs:cs_6_0 integer_minmax_buffer_cs:cs_6_0 integer_mad_buffer_cs:cs_6_0 float_mad_buffer_cs:cs_6_0 float_minmax_buffer_cs:cs_6_0 wave_ops_cs:cs_6_5 wave_ops_partial_cs:cs_6_5 half_ops_cs:cs_6_2+16 sm66_misc_ps:ps_6_6+16 cmp_level_zero_cs:cs_6_0 passcolor_vs:vs_6_0 quad_gs:gs_6_0 passcolor_ps:ps_6_0 minprec_ps:ps_6_0 tess_vs:vs_6_0 tess_hs:hs_6_0 tess_ds:ds_6_0
shaders12: build/vxrt build/dxc.exe
	@for s in $(SHADERS12); do n=$${s%%:*}; p=$${s##*:}; \
	  VXRT_ROOT=build/root build/vxrt build/dxc.exe '$(DXCOMPILER)' tests/shaders12/$$n.hlsl main $$p tests/shaders12/$$n.h $$n || exit 1; \
	  VXRT_ROOT=build/root build/vxrt build/dxc.exe -dis '$(DXCOMPILER)' tests/shaders12/$$n.dxil tests/shaders12/$$n.dis.txt || exit 1; done

# Games in VR against their profiles' baselines, games/*.json (tools/vxrt_run.py: own roots, simulated headset, evidence in runs/):
# minutes per scenario; Mac Steam must be signed in and nothing else running. GAME=alyx|thelab|earth: only that one.
vr-check: build/vxrt
	python3 ../tools/vxrt_run.py --baselines $(GAME)

# The tests' C: drive and registry live in build/root, not in ~/.vxrt.
build/gpu_ir_test: src/gpu_ir.c src/gpu_ir.h tests/gpu_ir.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ src/gpu_ir.c tests/gpu_ir.c

build/gpu_ir_metal_test: src/gpu_ir.c src/gpu_ir.h src/gpu_ir_msl.c src/gpu_ir_msl.h src/gpu_ir_metal.m src/gpu_ir_metal.h tests/gpu_ir_metal.m
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -fobjc-arc -o $@ src/gpu_ir.c src/gpu_ir_msl.c src/gpu_ir_metal.m tests/gpu_ir_metal.m -framework Metal -framework Foundation

# SPIR-V -> common shader IR -> MSL is exercised by dispatching a storage-buffer compute kernel on Metal.
build/spirv_msl_test: src/gpu_ir.c src/gpu_ir.h src/gpu_ir_msl.c src/gpu_ir_msl.h src/gpu_ir_metal.m src/gpu_ir_metal.h src/spirv.c src/spirv.h src/spirv_ir.c src/spirv_ir.h tests/spirv_msl.m
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -fobjc-arc -o $@ src/gpu_ir.c src/gpu_ir_msl.c src/gpu_ir_metal.m src/spirv.c src/spirv_ir.c tests/spirv_msl.m -framework Metal -framework Foundation

build/spirv_loop.metal: build/spirv_msl_test
	build/spirv_msl_test --emit-loop $@

build/spirv_graphics_sampled_image.metal: build/spirv_msl_test
	build/spirv_msl_test --emit-graphics-sampled-image-layout $@

build/spirv_graphics_sampled_image.air: build/spirv_graphics_sampled_image.metal
	xcrun -sdk macosx metal -c $< -o $@

build/spirv_graphics_sampled_image_implicit.metal: build/spirv_msl_test
	build/spirv_msl_test --emit-graphics-sampled-image-implicit-layout $@

build/spirv_graphics_sampled_image_implicit.air: build/spirv_graphics_sampled_image_implicit.metal
	xcrun -sdk macosx metal -c $< -o $@

build/spirv_fragment_termination.metal: build/spirv_msl_test
	build/spirv_msl_test --write-fragment-termination $@

build/spirv_fragment_termination.air: build/spirv_fragment_termination.metal
	xcrun -sdk macosx metal -c $< -o $@

build/spirv_graphics_separate_image_sampler.metal: build/spirv_msl_test
	build/spirv_msl_test --emit-graphics-separate-image-sampler-layout $@

build/spirv_graphics_separate_image_sampler.air: build/spirv_graphics_separate_image_sampler.metal
	xcrun -sdk macosx metal -c $< -o $@

build/spirv_graphics_separate_image_sampler_implicit.metal: build/spirv_msl_test
	build/spirv_msl_test --emit-graphics-separate-image-sampler-implicit-layout $@

build/spirv_graphics_separate_image_sampler_implicit.air: build/spirv_graphics_separate_image_sampler_implicit.metal
	xcrun -sdk macosx metal -c $< -o $@

build/spirv_helper.metal: build/spirv_msl_test
	build/spirv_msl_test --emit-helper $@

build/spirv_helper_capture.metal: build/spirv_msl_test
	build/spirv_msl_test --emit-helper-capture $@

build/spirv_vector_classification.metal: build/spirv_msl_test
	build/spirv_msl_test --emit-vector-classification $@

build/spirv_vector_classification.air: build/spirv_vector_classification.metal
	xcrun -sdk macosx metal -c $< -o $@

build/spirv_vertex_input.metal: build/spirv_msl_test
	build/spirv_msl_test --emit-vertex-input $@

build/spirv_vertex_input.air: build/spirv_vertex_input.metal
	xcrun -sdk macosx metal -c $< -o $@

build/spirv_uint_vertex_input.metal: build/spirv_msl_test
	build/spirv_msl_test --emit-uint-vertex-input $@

build/spirv_sint_vertex_input.metal: build/spirv_msl_test
	build/spirv_msl_test --emit-sint-vertex-input $@

build/spirv_uint_vertex_input.air: build/spirv_uint_vertex_input.metal
	xcrun -sdk macosx metal -c $< -o $@

build/spirv_sint_vertex_input.air: build/spirv_sint_vertex_input.metal
	xcrun -sdk macosx metal -c $< -o $@

build/vulkan_spirv_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-fixture $@

build/vulkan_spirv_vertex_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-graphics-vertex-fixture $@

build/vulkan_spirv_fragment_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-graphics-fragment-fixture $@

build/vulkan_spirv_uniform_vertex_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-graphics-uniform-vertex-fixture $@

build/vulkan_spirv_uniform_fragment_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-graphics-uniform-fragment-fixture $@

build/vulkan_spirv_graphics_sampled_image_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-graphics-sampled-image-fixture $@

build/vulkan_spirv_graphics_separate_image_sampler_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-graphics-separate-image-sampler-fixture $@

build/vulkan_spirv_graphics_sampled_image_implicit_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-graphics-sampled-image-implicit-fixture $@

build/vulkan_spirv_graphics_separate_image_sampler_implicit_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-graphics-separate-image-sampler-implicit-fixture $@

build/vulkan_spirv_uint_vertex_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-graphics-uint-vertex-fixture $@

build/vulkan_spirv_sint_vertex_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-graphics-sint-vertex-fixture $@

build/vulkan_spirv_array_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-array-fixture $@

build/vulkan_spirv_dynamic_array_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-dynamic-array-fixture $@

build/vulkan_spirv_dynamic_storage_image_array_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-dynamic-storage-image-array-fixture $@

build/vulkan_spirv_dynamic_sampled_image_array_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-dynamic-sampled-image-array-fixture $@

build/vulkan_spirv_dynamic_separate_image_sampler_array_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-dynamic-separate-image-sampler-array-fixture $@

build/vulkan_spirv_dynamic_nonuniform_sampled_image_array_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-nonuniform-sampled-image-array-fixture $@

build/vulkan_spirv_uniform_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-uniform-fixture $@

build/vulkan_spirv_push_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-push-fixture $@

build/vulkan_spirv_storage_image_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-storage-image-fixture $@

build/vulkan_spirv_sampled_image_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-sampled-image-fixture $@

build/vulkan_spirv_image_size_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-image-size-fixture $@

build/vulkan_spirv_image_size_lod_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-image-size-lod-fixture $@

build/vulkan_spirv_image_size_lod1_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-image-size-lod1-fixture $@

build/vulkan_spirv_image_array_size_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-image-array-size-fixture $@

build/vulkan_spirv_image_levels_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-image-levels-fixture $@

build/vulkan_spirv_image_levels_storage_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-image-levels-storage-fixture $@

build/vulkan_spirv_atomic_add_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-atomic-add-fixture $@

build/vulkan_spirv_atomic_compare_exchange_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-atomic-compare-exchange-fixture $@

build/vulkan_spirv_atomic_rmw_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-atomic-rmw-fixture $@

build/vulkan_spirv_sampler_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-sampler-fixture $@

build/vulkan_spirv_separate_sampler_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-separate-sampler-fixture $@

build/vulkan_spirv_combined_image_array_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-combined-image-array-fixture $@

build/vulkan_spirv_separate_image_sampler_array_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-separate-image-sampler-array-fixture $@

build/vulkan_spirv_high_texture_fixture.spv: build/spirv_msl_test
	build/spirv_msl_test --write-high-texture-fixture $@

# Translate a compute SPIR-V module through the in-tree reader, common IR and MSL emitter.
# vkmsl: the Vulkan front end's SPIR-V -> MSL translator (src/vk_msl.c) on one shader, or as a scanner of many (--scan).
build/vkmsl: tools/vkmsl.m src/vk_msl.c src/vk_msl.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -fobjc-arc -o $@ src/vk_msl.c tools/vkmsl.m -framework Metal -framework Foundation

build/spirv2msl: tools/spirv2msl.m src/gpu_ir.c src/gpu_ir.h src/gpu_ir_msl.c src/gpu_ir_msl.h src/spirv.c src/spirv.h src/spirv_ir.c src/spirv_ir.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -fobjc-arc -o $@ src/gpu_ir.c src/gpu_ir_msl.c src/spirv.c src/spirv_ir.c tools/spirv2msl.m -framework Metal -framework Foundation

# SPIR-V's binary reader and CPU-only common-IR/MSL checks (no Metal device required).
build/spirv_test: src/spirv.c src/spirv.h src/spirv_ir.c src/spirv_ir.h src/gpu_ir.c src/gpu_ir.h src/gpu_ir_msl.c src/gpu_ir_msl.h tests/spirv.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ src/spirv.c src/spirv_ir.c src/gpu_ir.c src/gpu_ir_msl.c tests/spirv.c

# DirectInput's buffered event queue is independent from host HID enumeration and can be tested without a joystick.
build/directinput_buffer_test: src/directinput_buffer.c src/directinput_buffer.h tests/directinput_buffer.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ src/directinput_buffer.c tests/directinput_buffer.c

# DirectInput's standard and custom data formats can be resolved against synthetic HID inventories without hardware.
build/directinput_format_test: src/directinput_format.c src/directinput_format.h tests/directinput_format.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ src/directinput_format.c tests/directinput_format.c

# iphlpapi's change notifications when a change completes a request that is being cancelled or started again on the same
# OVERLAPPED: a Mac program that includes src/iphlp.c, since a real change cannot be made on demand without privileges.
build/iphlp_notify_test: tests/iphlp_notify.c src/iphlp.c src/vx.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/iphlp_notify.c -framework CoreFoundation -framework SystemConfiguration

build/pe_test: src/pe.c src/pe.h tests/pe.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ src/pe.c tests/pe.c

# stereo.m's DIBR pass on the GPU (tests/stereo.m): a made-up scene, or a dumped game frame
build/stereo_test: src/stereo.m src/stereo.h tests/stereo.m
	@mkdir -p build
	$(HOSTCC) -fobjc-arc -Isrc -o $@ tests/stereo.m src/stereo.m -framework Foundation -framework Metal -framework CoreGraphics -framework ImageIO

# The warp core (src/warp_core.m: prediction from motion vectors, reprojection, stereo eyes in one warp) on synthetic
# frames; and, when the visionOS SDK is installed, the core compiled for visionOS (it must stay Metal + Foundation).
build/warp_core_test: src/warp_core.m src/warp_core.h tests/warp_core_test.m
	@mkdir -p build
	$(HOSTCC) -arch arm64 -O1 -fobjc-arc -Isrc -o $@ tests/warp_core_test.m src/warp_core.m -framework Foundation -framework Metal
# The upscale core (src/upscale_core.m: MetalFX in place of FSR2) on synthetic frames shaped as Cyberpunk's.
build/upscale_core_test: src/upscale_core.m src/upscale_core.h tests/upscale_core_test.m
	@mkdir -p build
	$(HOSTCC) -arch arm64 -O1 -fobjc-arc -Isrc -o $@ tests/upscale_core_test.m src/upscale_core.m -framework Foundation -framework Metal -framework MetalFX
check: build/warp_core_test build/upscale_core_test xros-check
xros-check: src/warp_core.m src/warp_core.h src/upscale_core.m src/upscale_core.h
	@if xcrun --sdk xros --show-sdk-path >/dev/null 2>&1; then \
	  mkdir -p build && xcrun -sdk xros clang -target arm64-apple-xros2.0 -fobjc-arc -Wall -Wextra -Werror -c -o build/warp_core_xros.o src/warp_core.m && \
	  echo "xros-check: src/warp_core.m builds for visionOS ($$(xcrun --sdk xros --show-sdk-version))" && \
	  xcrun -sdk xros clang -target arm64-apple-xros2.0 -fobjc-arc -Wall -Wextra -Werror -c -o build/upscale_core_xros.o src/upscale_core.m && \
	  echo "xros-check: src/upscale_core.m builds for visionOS (spatial scaler only: the temporal one is API_UNAVAILABLE(visionos))"; \
	else echo "xros-check: no visionOS SDK, skipped"; fi
.PHONY: xros-check

build/aes_gcm_test: src/aes_gcm.c src/aes_gcm.h tests/aes_gcm.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ tests/aes_gcm.c src/aes_gcm.c

build/dotnet_eh_test: src/pe.c src/pe.h src/dotnet_metadata.c src/dotnet_metadata.h src/dotnet_resources.c src/dotnet_resources.h src/dotnet_baml.c src/dotnet_baml.h src/dotnet_il.c src/dotnet_il.h src/dotnet_exec.c src/dotnet_runtime.c src/dotnet_exec.h tests/dotnet_eh.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ src/pe.c src/dotnet_metadata.c src/dotnet_resources.c src/dotnet_baml.c src/dotnet_il.c src/dotnet_exec.c src/dotnet_runtime.c tests/dotnet_eh.c -framework CoreFoundation -framework CoreServices -licucore -lz

build/dotnet_metadata_test: src/pe.c src/pe.h src/dotnet_metadata.c src/dotnet_metadata.h src/dotnet_resources.c src/dotnet_resources.h src/dotnet_baml.c src/dotnet_baml.h src/dotnet_il.c src/dotnet_il.h src/dotnet_exec.c src/dotnet_runtime.c src/dotnet_exec.h tests/dotnet_metadata.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ src/pe.c src/dotnet_metadata.c src/dotnet_resources.c src/dotnet_baml.c src/dotnet_il.c src/dotnet_exec.c src/dotnet_runtime.c tests/dotnet_metadata.c -framework CoreFoundation -framework CoreServices -licucore -lz

build/dotnet_resources_test: src/dotnet_resources.c src/dotnet_resources.h tests/dotnet_resources.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ src/dotnet_resources.c tests/dotnet_resources.c

build/dotnet_baml_test: src/dotnet_baml.c src/dotnet_baml.h tests/dotnet_baml.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ src/dotnet_baml.c tests/dotnet_baml.c

build/dotnet_il_test: src/dotnet_il.c src/dotnet_il.h tests/dotnet_il.c
	@mkdir -p build
	$(HOSTCC) -Isrc -o $@ src/dotnet_il.c tests/dotnet_il.c

build/vulkan_test: src/vulkan.c src/vx_vulkan.h src/vx.h src/gpu_ir.c src/gpu_ir.h src/gpu_ir_msl.c src/gpu_ir_msl.h src/gpu_ir_metal.m src/gpu_ir_metal.h src/spirv.c src/spirv.h src/spirv_ir.c src/spirv_ir.h tests/vulkan.c build/vulkan_spirv_fixture.spv build/vulkan_spirv_vertex_fixture.spv build/vulkan_spirv_fragment_fixture.spv build/vulkan_spirv_graphics_sampled_image_fixture.spv build/vulkan_spirv_graphics_separate_image_sampler_fixture.spv build/vulkan_spirv_uint_vertex_fixture.spv build/vulkan_spirv_sint_vertex_fixture.spv build/vulkan_spirv_array_fixture.spv build/vulkan_spirv_dynamic_array_fixture.spv build/vulkan_spirv_dynamic_storage_image_array_fixture.spv build/vulkan_spirv_dynamic_sampled_image_array_fixture.spv build/vulkan_spirv_dynamic_separate_image_sampler_array_fixture.spv build/vulkan_spirv_dynamic_nonuniform_sampled_image_array_fixture.spv build/vulkan_spirv_uniform_fixture.spv build/vulkan_spirv_push_fixture.spv build/vulkan_spirv_storage_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_image_size_fixture.spv build/vulkan_spirv_image_size_lod_fixture.spv build/vulkan_spirv_image_levels_fixture.spv build/vulkan_spirv_image_levels_storage_fixture.spv build/vulkan_spirv_sampler_fixture.spv build/vulkan_spirv_separate_sampler_fixture.spv build/vulkan_spirv_combined_image_array_fixture.spv build/vulkan_spirv_separate_image_sampler_array_fixture.spv build/vulkan_spirv_high_texture_fixture.spv build/vulkan_spirv_atomic_add_fixture.spv
	@mkdir -p build
	$(HOSTCC) -DVX_VULKAN_TEST_HOOKS -Isrc -fobjc-arc -o $@ src/vulkan.c src/gpu_ir.c src/gpu_ir_msl.c src/gpu_ir_metal.m src/spirv.c src/spirv_ir.c tests/vulkan.c -framework Metal -framework Foundation -framework CoreFoundation

build/vulkan_test: build/vulkan_spirv_atomic_compare_exchange_fixture.spv build/vulkan_spirv_atomic_rmw_fixture.spv
build/vulkan_test: build/vulkan_spirv_graphics_sampled_image_implicit_fixture.spv
build/vulkan_test: build/vulkan_spirv_graphics_separate_image_sampler_implicit_fixture.spv

check: export VXRT_ROOT = build/root
check: export VXRT_NO_ACTIVATE = 1
check: build/iphlp_notify_test
check: build/vulkan_spirv_atomic_compare_exchange_fixture.spv
check: build/aes_gcm_test
check: build/stereo_test
check: build/vulkan_spirv_atomic_rmw_fixture.spv
check: build/spirv_graphics_separate_image_sampler.air
check: build/spirv_graphics_sampled_image_implicit.air
check: build/spirv_graphics_separate_image_sampler_implicit.air
check: build/dxil_texture_dimensions_array.air
check: build/dxil_texture_dimensions_msaa.air
check: build/dxil_vector_integer_mad.air
check: build/dxil_texture_load_msaa.air
check: build/dxil_texture_load_msaa_array.air
check: build/dxil_texture_load.air
check: build/dxil_texture_load_array.air
check: build/dxil_texture_load_array_dxil.air
check: build/dxil_texture_load_sint_dxil.air
check: build/dxil_texture_load_array_sint_dxil.air
check: build/dxil_texture_load_float_dxil.air
check: build/dxil_texture_load_float_channel_dxil.air
check: build/dxil_texture_load_float_channel_z_dxil.air
check: build/dxil_texture_load_float_channel_w_dxil.air
check: build/dxil_texture_load_mapping.air
check: build/dxil_texture_load_mapping_uint.air
check: build/dxil_texture_load_mapping_sint.air
check: build/dxil_typed_buffer_load.air
check: build/dxil_typed_buffer_texture_load.air
check: build/dxil_raw_buffer_load.air
check: build/dxil_ir_raw_buffer_load.air
check: build/dxil_ir_byteaddress_load_lanes.air
check: build/dxil_structured_buffer_load.air
check: build/dxil_derivative_modes.air
check: build/dxil_wave_queries.air
check: build/dxil_target0_fragment.air
check: build/dxil_vertex_position.air build/dxil_vertex_input.air build/dxil_vertex_system_values.air
check: all build/vkmsl build/x86dec_test build/x86dec_check build/ref_x86 build/difftest build/dxildump build/dxil2msl build/dxilrun build/dxil_msl_test build/dxil_ir_test build/dxil_core_compute.air build/dxil_bit_intrinsics.air build/dxil_fragment_discard.air build/dxil_typed_buffer.air build/dxil_typed_sint_buffer.air build/dxil_signed_conversions.air build/dxil_typed_float_real.air build/dxil_texture_dimensions.air build/dxil_texture_dimensions_lod1.air build/dxil_unary_intrinsics.air build/dxil_integer_minmax.air build/dxil_integer_mad.air build/dxil_float_mad.air build/dxil_float_minmax.air build/dxil_atomic_add.air build/dxil_atomic_ops.air build/dxil_atomic_compare_exchange.air build/gpu_ir_test build/gpu_ir_metal_test build/spirv_test build/spirv_msl_test build/spirv_graphics_sampled_image.air build/spirv_fragment_termination.air build/spirv_loop.metal build/spirv_helper.metal build/spirv_helper_capture.metal build/spirv_vector_classification.air build/spirv_vertex_input.air build/spirv_uint_vertex_input.air build/spirv_sint_vertex_input.air build/spirv2msl build/directinput_buffer_test build/directinput_format_test build/d3d_format_test build/pe_test build/dotnet_metadata_test build/dotnet_resources_test build/dotnet_baml_test build/dotnet_il_test build/vulkan_test build/vulkan_spirv_storage_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_image_size_fixture.spv build/vulkan_spirv_image_size_lod_fixture.spv build/vulkan_spirv_image_size_lod1_fixture.spv build/vulkan_spirv_image_array_size_fixture.spv build/vulkan_spirv_image_levels_fixture.spv build/vulkan_spirv_image_levels_storage_fixture.spv build/vulkan_spirv_sampler_fixture.spv build/vulkan_spirv_separate_sampler_fixture.spv build/vulkan_spirv_combined_image_array_fixture.spv build/vulkan_spirv_separate_image_sampler_array_fixture.spv build/vulkan_spirv_high_texture_fixture.spv build/vulkan_spirv_dynamic_array_fixture.spv build/vulkan_spirv_dynamic_storage_image_array_fixture.spv build/vulkan_spirv_dynamic_sampled_image_array_fixture.spv build/vulkan_spirv_dynamic_separate_image_sampler_array_fixture.spv build/vulkan_spirv_atomic_add_fixture.spv build/d3d12_view_test
	python3 ../tools/vxrt_games.py check
	python3 ../tests/vxrt_run_profile_test.py
	python3 ../tests/vxrt_doctor_pe_test.py
	bash ../tests/launch_vxrt_windows_steam_test.sh
	python3 ../tools/vxrt_agents.py check
	python3 ../tools/vxrt_tune.py --selftest
	python3 ../tools/vxrt_playtest.py --selftest
	python3 tools/vk_dispatch.py --check
	build/x86dec_test
	python3 translator/a64_check.py
	python3 tools/dxil_check.py build/dxildump tests/shaders12
	build/dxil_msl_test
	build/dxil_ir_test --check-fragment-discard
	build/dxil_ir_test --write-system-values build/dxil_system_values.metal
	xcrun -sdk macosx metal -c build/dxil_system_values.metal -o build/dxil_system_values.air
	build/dxil_msl_test --sample-mask build/dxil_sample_mask.metal
	xcrun -sdk macosx metal -c build/dxil_sample_mask.metal -o build/dxil_sample_mask.air
	build/dxil_msl_test --cube-array-min-lod build/dxil_cube_array_min_lod.metal
	xcrun -sdk macosx metal -c build/dxil_cube_array_min_lod.metal -o build/dxil_cube_array_min_lod.air
	xcrun -sdk macosx metal -D VXRT_HAS_MIN_LOD_CLAMP=1 -c build/dxil_cube_array_min_lod.metal -o build/dxil_cube_array_min_lod_supported.air
	build/dxil_msl_test --depth-cube-compare build/dxil_depth_cube_compare.metal
	xcrun -sdk macosx metal -c build/dxil_depth_cube_compare.metal -o build/dxil_depth_cube_compare.air
	xcrun -sdk macosx metal -D VXRT_HAS_MIN_LOD_CLAMP=1 -c build/dxil_depth_cube_compare.metal -o build/dxil_depth_cube_compare_supported.air
	build/dxil_msl_test --fcmp-ueq build/dxil_fcmp_ueq.metal
	xcrun -sdk macosx metal -c build/dxil_fcmp_ueq.metal -o build/dxil_fcmp_ueq.air
	build/dxil_msl_test --texture-gather build/dxil_texture_gather.metal
	xcrun -sdk macosx metal -c build/dxil_texture_gather.metal -o build/dxil_texture_gather.air
	build/dxil_msl_test build/wave_prefix_min.metal build/wave_prefix_max.metal build/wave_match_multiprefix.metal build/wave_match_pixel.metal build/wave_pixel_helpers.metal build/buffer_update_counter.metal build/dxil_atomic
	xcrun -sdk macosx metal -c build/wave_prefix_min.metal -o build/wave_prefix_min.air
	xcrun -sdk macosx metal -c build/wave_prefix_max.metal -o build/wave_prefix_max.air
	xcrun -sdk macosx metal -c build/wave_match_multiprefix.metal -o build/wave_match_multiprefix.air
	xcrun -sdk macosx metal -c build/wave_match_pixel.metal -o build/wave_match_pixel.air
	xcrun -sdk macosx metal -c build/wave_pixel_helpers.metal -o build/wave_pixel_helpers.air
	xcrun -sdk macosx metal -c build/buffer_update_counter.metal -o build/buffer_update_counter.air
	xcrun -sdk macosx metal -c build/dxil_atomic_typed_buffer.metal -o build/dxil_atomic_typed_buffer.air
	xcrun -sdk macosx metal -c build/dxil_atomic_texture_1d.metal -o build/dxil_atomic_texture_1d.air
	xcrun -sdk macosx metal -c build/dxil_atomic_texture_1d_array.metal -o build/dxil_atomic_texture_1d_array.air
	xcrun -sdk macosx metal -c build/dxil_atomic_texture_2d.metal -o build/dxil_atomic_texture_2d.air
	xcrun -sdk macosx metal -c build/dxil_atomic_texture_2d_array.metal -o build/dxil_atomic_texture_2d_array.air
	xcrun -sdk macosx metal -c build/dxil_atomic_texture_3d.metal -o build/dxil_atomic_texture_3d.air
	xcrun -sdk macosx metal -c build/dxil_atomic_typed_sint_umin.metal -o build/dxil_atomic_typed_sint_umin.air
	xcrun -sdk macosx metal -c build/dxil_atomic_typed_uint_imin.metal -o build/dxil_atomic_typed_uint_imin.air
	xcrun -sdk macosx metal -c build/dxil_atomic_texture_2d_uint_imin.metal -o build/dxil_atomic_texture_2d_uint_imin.air
	xcrun -sdk macosx metal -c build/dxil_atomic_texture_2d_sint_umin.metal -o build/dxil_atomic_texture_2d_sint_umin.air
	build/gpu_ir_test
	build/gpu_ir_metal_test
	build/spirv_test
	build/spirv_msl_test
	build/spirv_msl_test --check-graphics-sampled-image-layout
	xcrun -sdk macosx metal -c build/spirv_loop.metal -o build/spirv_loop.air
	xcrun -sdk macosx metal -c build/spirv_helper.metal -o build/spirv_helper.air
	xcrun -sdk macosx metal -c build/spirv_helper_capture.metal -o build/spirv_helper_capture.air
	build/spirv_msl_test --write-fixture build/spirv_arithmetic_cs.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/spirv_arithmetic_cs.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_atomic_add_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_atomic_compare_exchange_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_atomic_rmw_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_array_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_uniform_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_storage_image_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_image_size_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_image_size_lod_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_image_size_lod1_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_image_array_size_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_combined_image_array_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_separate_image_sampler_array_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_dynamic_array_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_dynamic_storage_image_array_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_dynamic_sampled_image_array_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_dynamic_separate_image_sampler_array_fixture.spv
	python3 ../tools/spirv_check.py --translator build/spirv2msl --compile build/vulkan_spirv_dynamic_nonuniform_sampled_image_array_fixture.spv
	build/directinput_buffer_test
	build/directinput_format_test
	build/iphlp_notify_test
	build/d3d_format_test
	build/d3d12_view_test
	build/pe_test
	build/aes_gcm_test
	build/stereo_test
	build/warp_core_test
	build/upscale_core_test
	build/dotnet_metadata_test
	build/dotnet_eh_test | tail -1
	build/dotnet_resources_test
	build/dotnet_baml_test
	build/dotnet_il_test
	build/vulkan_test --check-graphics-specialization-fixture build/vulkan_spirv_vertex_fixture.spv
	build/vulkan_test build/vulkan_spirv_fixture.spv build/vulkan_spirv_array_fixture.spv build/vulkan_spirv_uniform_fixture.spv build/vulkan_spirv_push_fixture.spv build/vulkan_spirv_storage_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampler_fixture.spv build/vulkan_spirv_separate_sampler_fixture.spv build/vulkan_spirv_high_texture_fixture.spv build/vulkan_spirv_combined_image_array_fixture.spv build/vulkan_spirv_separate_image_sampler_array_fixture.spv build/vulkan_spirv_dynamic_array_fixture.spv build/vulkan_spirv_dynamic_storage_image_array_fixture.spv build/vulkan_spirv_dynamic_sampled_image_array_fixture.spv build/vulkan_spirv_dynamic_separate_image_sampler_array_fixture.spv build/vulkan_spirv_dynamic_nonuniform_sampled_image_array_fixture.spv build/vulkan_spirv_image_size_fixture.spv build/vulkan_spirv_image_size_lod_fixture.spv build/vulkan_spirv_atomic_add_fixture.spv build/vulkan_spirv_atomic_compare_exchange_fixture.spv build/vulkan_spirv_atomic_rmw_fixture.spv build/vulkan_spirv_image_levels_fixture.spv build/vulkan_spirv_image_levels_storage_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_vertex_fixture.spv build/vulkan_spirv_fragment_fixture.spv
	build/vulkan_test build/vulkan_spirv_fixture.spv build/vulkan_spirv_array_fixture.spv build/vulkan_spirv_uniform_fixture.spv build/vulkan_spirv_push_fixture.spv build/vulkan_spirv_storage_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampler_fixture.spv build/vulkan_spirv_separate_sampler_fixture.spv build/vulkan_spirv_high_texture_fixture.spv build/vulkan_spirv_combined_image_array_fixture.spv build/vulkan_spirv_separate_image_sampler_array_fixture.spv build/vulkan_spirv_dynamic_array_fixture.spv build/vulkan_spirv_dynamic_storage_image_array_fixture.spv build/vulkan_spirv_dynamic_sampled_image_array_fixture.spv build/vulkan_spirv_dynamic_separate_image_sampler_array_fixture.spv build/vulkan_spirv_dynamic_nonuniform_sampled_image_array_fixture.spv build/vulkan_spirv_image_size_fixture.spv build/vulkan_spirv_image_size_lod_fixture.spv build/vulkan_spirv_atomic_add_fixture.spv build/vulkan_spirv_atomic_compare_exchange_fixture.spv build/vulkan_spirv_atomic_rmw_fixture.spv build/vulkan_spirv_image_levels_fixture.spv build/vulkan_spirv_image_levels_storage_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_vertex_fixture.spv build/vulkan_spirv_fragment_fixture.spv
	build/vulkan_test build/vulkan_spirv_fixture.spv build/vulkan_spirv_array_fixture.spv build/vulkan_spirv_uniform_fixture.spv build/vulkan_spirv_push_fixture.spv build/vulkan_spirv_storage_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampler_fixture.spv build/vulkan_spirv_separate_sampler_fixture.spv build/vulkan_spirv_high_texture_fixture.spv build/vulkan_spirv_combined_image_array_fixture.spv build/vulkan_spirv_separate_image_sampler_array_fixture.spv build/vulkan_spirv_dynamic_array_fixture.spv build/vulkan_spirv_dynamic_storage_image_array_fixture.spv build/vulkan_spirv_dynamic_sampled_image_array_fixture.spv build/vulkan_spirv_dynamic_separate_image_sampler_array_fixture.spv build/vulkan_spirv_dynamic_nonuniform_sampled_image_array_fixture.spv build/vulkan_spirv_image_size_fixture.spv build/vulkan_spirv_image_size_lod_fixture.spv build/vulkan_spirv_atomic_add_fixture.spv build/vulkan_spirv_atomic_compare_exchange_fixture.spv build/vulkan_spirv_atomic_rmw_fixture.spv build/vulkan_spirv_image_levels_fixture.spv build/vulkan_spirv_image_levels_storage_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_sampled_image_fixture.spv build/vulkan_spirv_vertex_fixture.spv build/vulkan_spirv_fragment_fixture.spv build/vulkan_spirv_uint_vertex_fixture.spv build/vulkan_spirv_sint_vertex_fixture.spv build/vulkan_spirv_graphics_sampled_image_fixture.spv build/vulkan_spirv_graphics_separate_image_sampler_fixture.spv build/vulkan_spirv_graphics_sampled_image_implicit_fixture.spv build/vulkan_spirv_graphics_separate_image_sampler_implicit_fixture.spv
	build/dxil2msl $(filter-out tests/shaders12/samplegrad_ps.dxil tests/shaders12/quad_gs.dxil tests/shaders12/tess_hs.dxil tests/shaders12/tess_ds.dxil,$(wildcard tests/shaders12/*.dxil))
	build/dxil2msl -vs tests/shaders12/tess_vs.dxil -hs tests/shaders12/tess_hs.dxil tests/shaders12/tess_ds.dxil
	build/dxil2msl -vs tests/shaders12/passcolor_vs.dxil tests/shaders12/quad_gs.dxil
	build/dxilrun tests/shaders12
	build/difftest build/ref_x86 200
	$(MAKE) runtime-tests VXRT=build/vxrt

# The runtime's own tests, run by $(VXRT): the x86 build under Rosetta (make check) or the ARM64 build with the
# translator (make check-arm64).
VXRT ?= build/vxrt
runtime-tests: export VXRT_ROOT = build/root
runtime-tests: export VXRT_NO_ACTIVATE = 1
runtime-tests: $(VXRT) tests
	$(VXRT) build/hello_k32.exe
	$(VXRT) build/smc.exe
	$(VXRT) build/order.exe
	$(VXRT) build/hello_crt.exe one "two words"
	$(VXRT) build/game_imports.exe
	$(VXRT) build/ucrt_extra.exe
	$(VXRT) build/security.exe
	$(VXRT) build/kernelx.exe
	$(VXRT) build/jobs.exe
	$(VXRT) build/threads.exe
	$(VXRT) build/cxx_exceptions.exe
	$(VXRT) build/hw_exceptions.exe
	$(VXRT) build/seh_try.exe
	$(VXRT) build/unwind.exe
	$(VXRT) build/breakpoint.exe
	$(VXRT) build/child_stdout.exe
	$(VXRT) build/inherit.exe
	VXRT_EXTRA_ARGS='other.exe=--not-me;EXTRA_ARGS.EXE=--first "two words"' $(VXRT) build/extra_args.exe self tail
	mkdir -p build/root && printf '# a comment\nother.exe VXRT_TEST_PROGRAM_ENV_OTHER=1\nPROGRAM_ENV.EXE VXRT_TEST_PROGRAM_ENV="two words" VXRT_TEST_PROGRAM_ENV_B=1\n' > build/root/program_env.txt
	$(VXRT) build/program_env.exe; s=$$?; rm -f build/root/program_env.txt; exit $$s
	$(VXRT) build/lowmem.exe
	$(VXRT) build/files.exe
	rm -rf build/ovtree build/ovshadow && mkdir -p build/ovtree/save && echo old > build/ovtree/save/old.sav && echo orig > build/ovtree/save/same.hl1
	VXRT_OVERLAY=$(CURDIR)/build/ovtree:$(CURDIR)/build/ovshadow $(VXRT) build/files.exe overlay 'Z:$(subst /,\,$(CURDIR))\build\ovtree'
	test "$$(cat build/ovtree/save/same.hl1)" = orig && test ! -e build/ovtree/save/a.hl1
	$(VXRT) build/dbfiles.exe
	for via in dbov dbovlink; do \
	  rm -rf build/dbov build/dbovlink build/dbovshadow && mkdir -p build/dbov/keep && ln -s dbov build/dbovlink && \
	  for f in a b c d e; do echo orig > build/dbov/$$f.db; done && \
	  VXRT_OVERLAY=$(CURDIR)/build/$$via:$(CURDIR)/build/dbovshadow $(VXRT) build/dbfiles.exe overlay "Z:$(subst /,\,$(CURDIR))\\build\\$$via" && \
	  for f in a b c d e; do test "$$(cat build/dbov/$$f.db)" = orig || exit 1; test ! -e build/dbovshadow/$$f.db || exit 1; done && \
	  test -d build/dbov/keep && test ! -e build/dbovshadow/new.db || exit 1; \
	done
	$(VXRT) build/updater_files.exe
	$(VXRT) build/shlock.exe
	$(VXRT) build/dll_test.exe
	$(VXRT) build/net.exe
	$(VXRT) build/net_events.exe
	$(VXRT) build/regwait.exe
	$(VXRT) build/tp_callbacks.exe
	$(VXRT) build/fiber_pool.exe
	build/vxrt-npu --selftest 2>/dev/null   # NPU mode's interpolator and optical flow (skipped where unsupported)
	$(VXRT) build/shellexec.exe
	$(VXRT) build/file_io_ex.exe
	$(VXRT) build/tcptable.exe
	$(VXRT) build/winprops.exe
	$(VXRT) build/a11y.exe
	$(VXRT) build/client_imports.exe build/hello32.exe
	VXRT_PASTEBOARD=vxrt-check $(VXRT) build/clipboard.exe
	$(VXRT) build/gdi_xform.exe
	$(VXRT) build/view_align.exe
	$(VXRT) build/winhttp.exe
	$(VXRT) build/winhttp_proxy.exe
	$(VXRT) build/iphlp.exe
	$(VXRT) build/winsock_nsp.exe
	$(VXRT) build/midi.exe
	$(VXRT) build/crypt32.exe
	$(VXRT) build/crypt32.exe nomem
	for k in 2 3 5 7 11 13 23 47 97; do VXRT_TEST_CRYPT32_NOMEM=$$k $(VXRT) build/crypt32.exe nomem || exit 1; done
	$(VXRT) build/crypt32.exe protect build/dpapi_blob.bin && $(VXRT) build/crypt32.exe unprotect build/dpapi_blob.bin
	test "$$(stat -f %Lp build/root/dpapi)" = 700 && test "$$(stat -f %Lp build/root/dpapi/user.key)" = 600
	tmutil isexcluded build/root/dpapi | grep -q '^\[Excluded\]'
	$(VXRT) build/directinput.exe
	$(VXRT) build/msvc_eh.exe
	$(VXRT) build/com.exe
	$(VXRT) build/msxml.exe
	$(VXRT) build/iocp.exe
	$(VXRT) build/sysdlls.exe
	$(VXRT) build/dbghelp.exe
	$(VXRT) build/webhelper_apis.exe
	$(VXRT) build/steam_client_ui.exe
	$(VXRT) build/handle_xfer.exe
	$(VXRT) build/open_thread.exe
	$(VXRT) build/version_info.exe build/hello_crt.exe
	$(VXRT) build/section_access.exe
	$(VXRT) build/window.exe
	$(VXRT) build/winsta.exe
	$(VXRT) build/scm.exe
	python3 tests/clip_check.py $(VXRT)
	python3 tests/present_check.py $(VXRT)
	python3 tests/shoplist_check.py $(VXRT)
	python3 tests/input_route_check.py $(VXRT)
	python3 tests/name_reuse_check.py $(VXRT)
	python3 tests/tray_check.py $(VXRT)
	$(VXRT) build/d3d11.exe
	$(VXRT) build/egl_host.exe
	$(VXRT) build/gl.exe
	build/dxbcscan tests/shaders/*.dxbc
	$(VXRT) build/draw.exe
	$(VXRT) build/compute.exe
	$(VXRT) build/d3d12.exe
	$(VXRT) build/d3d12_engine.exe
	$(VXRT) build/d3d12_fxc.exe
	$(VXRT) build/d3d11on12.exe
	$(VXRT) build/vulkan_features.exe
	$(VXRT) build/vulkan_shaders.exe
	$(VXRT) build/vulkan_external.exe
	$(VXRT) build/vulkan_win.exe build/vulkan_spirv_vertex_fixture.spv build/vulkan_spirv_fragment_fixture.spv \
		build/vulkan_spirv_uniform_vertex_fixture.spv build/vulkan_spirv_uniform_fragment_fixture.spv
	$(VXRT) build/input.exe
	$(VXRT) build/audio.exe
	$(VXRT) build/xaudio2.exe
	$(VXRT) build/dwrite.exe
	$(VXRT) build/pipes.exe
	$(VXRT) build/pipes.exe serve & server=$$!; $(VXRT) build/pipes.exe ask; client_status=$$?; wait $$server; server_status=$$?; test $$client_status -eq 0 -a $$server_status -eq 0
	$(VXRT) build/ipc.exe second & second=$$!; $(VXRT) build/ipc.exe first; first_status=$$?; wait $$second; second_status=$$?; test $$first_status -eq 0 -a $$second_status -eq 0
	$(VXRT) build/shared.exe make & creator=$$!; $(VXRT) build/shared.exe open; open_status=$$?; wait $$creator; creator_status=$$?; test $$open_status -eq 0 -a $$creator_status -eq 0
	$(VXRT) build/shared.exe pingpong 1 & first=$$!; $(VXRT) build/shared.exe pingpong 2 & second=$$!; wait $$first; first_status=$$?; wait $$second; second_status=$$?; test $$first_status -eq 0 -a $$second_status -eq 0

check-arm64: build/vxrt-arm64 tests
	$(MAKE) runtime-tests VXRT=build/vxrt-arm64

# CPU cost per Direct3D 11 draw (numbers only, not a test).
bench: export VXRT_ROOT = build/root
bench: export VXRT_NO_ACTIVATE = 1
bench: build/vxrt build/drawbench.exe
	build/vxrt build/drawbench.exe

# CPU and GPU cost of a Direct3D 12 frame with a game-sized descriptor heap (numbers only, not a test).
d3d12-bench: export VXRT_ROOT = build/root
d3d12-bench: export VXRT_NO_ACTIVATE = 1
d3d12-bench: build/vxrt build/d3d12_bench.exe
	build/vxrt build/d3d12_bench.exe 500000 20

build/d3d12_bench.exe: tests/d3d12_bench.c tests/shaders/tex_vs.dxbc tests/shaders/tex_ps.dxbc tests/shaders/sum_cs.dxbc
	@mkdir -p build
	$(MINGW) -O1 -o $@ $< -ld3d12

clean:
	rm -rf build

.PHONY: vr-check all tests check bench d3d12-bench clean shaders translator-check vulkan-test-shaders
