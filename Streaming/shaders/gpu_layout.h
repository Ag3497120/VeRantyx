#pragma once
// SPDX-License-Identifier: MIT
#include <cstddef>
namespace mvr_foveated_gpu {
struct alignas(16) Axis {
    float sourcePixels, packedPixels, sourceBegin, sourceEnd;
    float packedBegin, packedEnd, reserved0{}, reserved1{};
};
struct alignas(16) Eye { Axis x, y; };
struct alignas(16) Params { Eye eyes[2]; };
static_assert(sizeof(Axis)==32 && alignof(Axis)==16, "Axis GPU ABI");
static_assert(sizeof(Eye)==64 && sizeof(Params)==128, "Stereo GPU ABI");
static_assert(offsetof(Axis,packedBegin)==16, "Axis vector layout");
}
