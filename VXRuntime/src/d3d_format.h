#ifndef VX_D3D_FORMAT_H
#define VX_D3D_FORMAT_H

#include <stdint.h>

// Whether two DXGI formats belong to a format family that D3D texture copies may reinterpret.
int d3d_copy_formats_compatible(uint32_t source, uint32_t destination);

#endif
