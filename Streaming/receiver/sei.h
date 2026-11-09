/* SPDX-License-Identifier: MIT */
#ifndef MVR_SEI_H
#define MVR_SEI_H
#include "geometry_types.h"
/* Finds exact UUID+232Bdescriptor in H264/HEVC AnnexB, AVCC or raw NAL. */
int mvrFindGeometrySei(const unsigned char*,size_t,unsigned codec,struct MvrAgreement*);
#endif
