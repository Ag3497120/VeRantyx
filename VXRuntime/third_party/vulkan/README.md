# Khronos Vulkan headers

`vulkan/vulkan_core.h` (header version 361, SPDX-License-Identifier: Apache-2.0 OR MIT) and `vk_video/*.h` (the
video codec headers it includes, SPDX-License-Identifier: Apache-2.0) are the Khronos Group's Vulkan API headers,
Copyright 2015-2026 The Khronos Group Inc., generated from the Vulkan XML registry and used unchanged for the exact
layout of every Vulkan structure and enumerant. VXRuntime distributes them under the Apache License 2.0, whose full
text is in `LICENSE` (THIRD_PARTY_NOTICES.md).

Source: copied unchanged from a local checkout of Valve's Fossilize (https://github.com/ValveSoftware/Fossilize,
commit 556da9ff7d76, its `khronos/` directory, which vendors the Khronos Vulkan-Headers); nothing was downloaded for
them.

`vulkan/vk_platform.h` is VXRuntime's own: the Windows calling convention for this macOS process.
