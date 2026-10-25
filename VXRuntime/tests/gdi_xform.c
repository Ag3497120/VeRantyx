// A DC's world transform and drawing settings as documented: SetWorldTransform only in GM_ADVANCED, identity at first,
// ModifyWorldTransform's three modes, a singular transform refused; SetROP2 / SetPolyFillMode return the previous value
// (src/gdi.c). Chromium's skia::InitializeDC sets these on the bitmaps it draws into.
#include <windows.h>
#include <stdio.h>
#include <math.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static int same(const XFORM *a, const XFORM *b) {
    return fabsf(a->eM11 - b->eM11) < 1e-5f && fabsf(a->eM12 - b->eM12) < 1e-5f && fabsf(a->eM21 - b->eM21) < 1e-5f &&
           fabsf(a->eM22 - b->eM22) < 1e-5f && fabsf(a->eDx - b->eDx) < 1e-4f && fabsf(a->eDy - b->eDy) < 1e-4f;
}

int main(void) {
    HDC dc = CreateCompatibleDC(NULL);
    XFORM x, identity = { 1, 0, 0, 1, 0, 0 }, scale = { 2, 0, 0, 3, 0, 0 }, move = { 1, 0, 0, 1, 10, 20 };
    CHECK(GetWorldTransform(dc, &x) && same(&x, &identity), "identity at first");
    CHECK(!SetWorldTransform(dc, &scale), "refused in GM_COMPATIBLE");
    CHECK(SetGraphicsMode(dc, GM_ADVANCED) == GM_COMPATIBLE, "GM_ADVANCED");
    CHECK(SetWorldTransform(dc, &scale) && GetWorldTransform(dc, &x) && same(&x, &scale), "set");
    CHECK(ModifyWorldTransform(dc, &move, MWT_RIGHTMULTIPLY) && GetWorldTransform(dc, &x), "right multiply");
    XFORM want = { 2, 0, 0, 3, 10, 20 };                           // scale, then move
    CHECK(same(&x, &want), "scale then move: %g %g %g %g %g %g", x.eM11, x.eM12, x.eM21, x.eM22, x.eDx, x.eDy);
    CHECK(SetWorldTransform(dc, &scale) && ModifyWorldTransform(dc, &move, MWT_LEFTMULTIPLY) && GetWorldTransform(dc, &x), "left multiply");
    XFORM want2 = { 2, 0, 0, 3, 20, 60 };                          // move, then scale
    CHECK(same(&x, &want2), "move then scale: %g %g", x.eDx, x.eDy);
    CHECK(ModifyWorldTransform(dc, NULL, MWT_IDENTITY) && GetWorldTransform(dc, &x) && same(&x, &identity), "identity again");
    XFORM singular = { 1, 2, 2, 4, 0, 0 };
    CHECK(!SetWorldTransform(dc, &singular), "singular refused");
    CHECK(SetROP2(dc, R2_XORPEN) == R2_COPYPEN && GetROP2(dc) == R2_XORPEN, "ROP2");
    CHECK(SetPolyFillMode(dc, WINDING) == ALTERNATE && GetPolyFillMode(dc) == WINDING, "poly fill mode");
    DeleteDC(dc);
    printf(fails ? "gdi_xform: %d failures\n" : "gdi_xform: ok\n", fails);
    return fails != 0;
}
