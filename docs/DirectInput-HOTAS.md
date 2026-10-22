# DirectInput and Elite Dangerous readiness

VXRuntime's `dinput8.dll` exposes HID joystick, gamepad and multi-axis devices through IOKit. It supports DirectInput 8
device discovery, caller-selected data formats, axis range, immediate state, buffered object changes, polling and event
handles. The buffered queue reserves one item as DirectInput specifies: a requested size of `N` stores at most `N - 1`
events. `GetDeviceData` supports `DIGDD_PEEK`, consuming reads, NULL-buffer flush/count queries and overflow reporting.

Data-format resolution honors the 16-bit `DIDFT_ANYINSTANCE` field, matches standard axis/button/POV GUIDs to HID
usages, assigns repeated sliders and buttons to separate objects, accepts missing objects only when marked optional,
and rejects overlapping output fields and malformed object ranges. Absolute axes are supported; relative-axis formats,
force feedback and action maps are not. The resolver is C-only and its synthetic inventory tests run without attached
hardware. This follows Microsoft's `DIDATAFORMAT` and `DIOBJECTDATAFORMAT` descriptions, which require apps to describe
device objects and distinguish absolute from relative axes: [DIDATAFORMAT](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416606%28v%3Dvs.85%29).

`SetEventNotification` stores the caller's event handle and signals it when `Poll`, `GetDeviceState` or
`GetDeviceData` observes an axis, button or POV change. HID state is sampled synchronously; the implementation does not
run a background HID event loop. Applications must poll or read the device to observe changes. Force feedback, action
maps, device output reports and asynchronous hot-plug notifications are not implemented.

The API, queue and synthetic-format tests pass on the current host. The host reports zero attached DirectInput
controllers, so no physical HOTAS has been exercised. The test program performs `SetDataFormat`, `DIPROP_BUFFERSIZE`, event setup,
`Acquire`, `Poll`, `GetDeviceState` and `GetDeviceData` automatically when a controller is attached.

## Elite Dangerous

Steam app 359320 is installed. Static PE inspection found that the official `EDLaunch.exe` is an I386 PE32 image with
an IL-only CLR header, metadata version `v4.0.30319`, and no `32BITREQUIRED` flag. Its config targets .NET Framework
4.8. Its managed references include `PresentationFramework`, `PresentationCore` and `WindowsBase`, and the install
contains `HtmlRenderer.WPF.dll`; this is a Windows desktop/WPF launcher, not a native 32-bit launcher. A 32-bit x86
instruction backend by itself would not run it.

The existing official-launcher harness run `20261018T105449Z-elite_dangerous-start` exited after 4.7 seconds at the
old PE32+ architecture check. VXRuntime now has a bounds-checked PE32/PE32+ and CLR-header reader, and the loader will
report managed images separately instead of treating their IL entry point as machine code. This is diagnostic/parser
support only: CLR hosting, .NET Framework compatibility and WPF APIs are still absent, so the launcher has not been
run again. The main `EliteDangerous64.exe` was not started, no credentials were entered, and game rendering and online
login remain untested. The game's own executable remains x64 Direct3D 11; D3D12 and Vulkan are not imported.

WinHTTP currently covers synchronous HTTP(S) request handles, upload, common response headers, buffered response reads
and timeouts. A named HTTP/HTTPS proxy is applied to requests, including the supplied bypass list; loopback tests
verify absolute-form proxy requests and direct requests for bypassed hosts. The default proxy query remains direct,
and the current-user query maps macOS auto-detect, PAC URL, HTTP/HTTPS/SOCKS proxy and bypass preferences. PAC/WPAD
resolution, proxy authentication, HTTPS through a proxy, asynchronous callbacks and large streaming responses have
not been verified. HOTAS input also remains unverified without a physical controller.
