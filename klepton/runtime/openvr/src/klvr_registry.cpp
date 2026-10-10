// The vtables. Every OpenVR interface version, built by tools/gen_vtables.py
// from the SDK's own history; a method this shim implements lands in its slot
// in every version that has it, and anything else lands on a weak stub that
// names itself in the log.
#include "klvr_internal.h"

#include "generated/klvr_vtables.inc"
