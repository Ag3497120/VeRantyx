// 256 vtable slots for inert Steam interface placeholders (kl_steamnet.cpp).
// Slot i: w9 = i, then the shared tail logs (object, slot) once and returns 0
// in x0 and d0 — false / null / zero for every method shape we care about.
.text
.p2align 2
.globl _kl_steam_null_slots
_kl_steam_null_slots:
.set i, 0
.rept 256
    mov  w9, #i
    b    _kl_steam_null_common
.set i, i + 1
.endr

.p2align 2
_kl_steam_null_common:
    stp  x29, x30, [sp, #-16]!
    mov  x29, sp
    mov  w1, w9
    bl   _kl_steam_null_called            // (void *this, unsigned slot)
    mov  x0, #0
    movi d0, #0
    ldp  x29, x30, [sp], #16
    ret
