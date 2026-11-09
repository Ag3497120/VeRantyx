// Copyright (c) 2026 MoltenVR experiment contributors
// SPDX-License-Identifier: MIT
#pragma once
#include <stdlib.h>
#include <string.h>
namespace PyroWave {
// Experimental matched-pair mode. Fixed per process; never change while devices live.
// CDF5/3 uses the Quest3-Pyrowave extension sequence code1. It requires that decoder.
inline bool cdf53_enabled()
{
    static const bool enabled = [] {
        const char *value = getenv("PYROWAVE_WAVELET");
        if (!value || !*value) return false;
        if (!strcmp(value,"97") || !strcmp(value,"cdf97") || !strcmp(value,"0")) return false;
        return !strcmp(value,"53") || !strcmp(value,"cdf53") || !strcmp(value,"1");
    }();
    return enabled;
}
}
