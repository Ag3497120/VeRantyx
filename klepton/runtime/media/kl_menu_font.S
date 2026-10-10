// A real font, baked in, for VR ports whose menu text is drawn from a system
// font that does not exist on visionOS.
//
// The Source-VR ports (portal, hl2 / HL2Q3VR) draw their menu labels with the
// Android system font: the engine "Found font: /system/fonts/Roboto-Regular.ttf"
// and fopen()s it directly. visionOS has no /system/fonts, and these builds do
// not stage the game's own resource/*.ttf either, so EVERY font load fails and
// the menu renders no glyphs — a panel with a cursor and nothing to click. Serve
// this font for any missing /system/fonts/*.ttf so the labels appear (kl_libc.c
// klb_fopen, via fmemopen).
//
// The bytes are DejaVu Sans (dejavusans.ttf) — the standard freely
// redistributable fallback face (Bitstream Vera license + public-domain DejaVu
// additions), already committed in the repo's guest asset trees. COMMITTED as a
// build INPUT (like runtime/data/phonon_hrtf_cipic_124.sofa) so a bare checkout
// still compiles; .incbin rather than a C table so `make check` does not parse a
// multi-MB source. The path is relative to the repo root, where make runs.
.section __DATA,__const
.globl _kl_menu_font
.globl _kl_menu_font_end
.align 4
_kl_menu_font:
.incbin "runtime/data/kl_menu_font.ttf"
_kl_menu_font_end:
