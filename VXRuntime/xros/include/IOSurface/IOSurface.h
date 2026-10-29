// visionOS (xros/README.md): IOSurface is public there without the macOS umbrella header.
#pragma once
#include <IOSurface/IOSurfaceRef.h>
#ifdef __OBJC__
#include <IOSurface/IOSurfaceObjC.h>
#endif
