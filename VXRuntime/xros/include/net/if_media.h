// visionOS (xros/README.md): the SDK has SIOCGIFMEDIA but not <net/if_media.h>. An app may not ask for interface media;
// these definitions let the ioctl be made, and it fails there (the interface is reported without media details).
#pragma once
#include <sys/types.h>
#define IFM_AVALID 0x00000001
#define IFM_ACTIVE 0x00000002
#define IFM_NMASK 0x000000e0
#define IFM_TMASK 0x0000001f
#define IFM_ETHER 0x00000020
#define IFM_IEEE80211 0x00000080
#define IFM_TYPE(x) ((x) & IFM_NMASK)
