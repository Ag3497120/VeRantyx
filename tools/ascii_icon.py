#!/usr/bin/env python3
"""Mitten's icons, drawn from ASCII art (tools/mitten.txt) in a monospaced font: the headset's three layers
(packages/alyx/icon: back, middle, front, 1024 px), the Mac app's AppIcon set and the installer's small marks.

    python3 tools/ascii_icon.py            # writes them all in place"""
import os, random
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = "/System/Library/Fonts/Menlo.ttc"
BG_TOP, BG_BOTTOM = (16, 22, 38), (6, 9, 18)          # night blue
INK = (255, 176, 64)                                   # amber, as an old terminal's
INK_DIM = (30, 38, 60)
SIZE = 1024

def art():
    with open(os.path.join(ROOT, "tools", "mitten.txt")) as f:
        lines = [l.rstrip("\n") for l in f if not l.startswith("#")]
    while lines and not lines[-1].strip(): lines.pop()
    return lines

def text_block(lines, cell_h, color, glow):
    """The lines drawn in Menlo Bold, centered on a transparent square, with a soft glow."""
    font = ImageFont.truetype(FONT, cell_h, index=1)
    cw = font.getbbox("M")[2]
    w, h = cw * max(len(l) for l in lines), int(cell_h * 1.05) * len(lines)
    layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0, y0 = (SIZE - w) // 2, (SIZE - h) // 2
    for i, l in enumerate(lines): d.text((x0, y0 + i * int(cell_h * 1.05)), l, font=font, fill=color + (255,))
    if not glow: return layer
    halo = layer.filter(ImageFilter.GaussianBlur(cell_h * 0.45))
    return Image.alpha_composite(halo, layer)

def back():
    img = Image.new("RGB", (SIZE, SIZE))
    d = ImageDraw.Draw(img)
    for y in range(SIZE):                               # (a vertical gradient)
        t = y / (SIZE - 1)
        d.line([(0, y), (SIZE, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)))
    font = ImageFont.truetype(FONT, 22)                 # faint rows of characters, as a terminal's screen
    rnd = random.Random(7)
    for y in range(8, SIZE, 30):
        row = "".join(rnd.choice("01<>/\\|_-=+*.:;()[]{}") for _ in range(90))
        d.text((6, y), row, font=font, fill=INK_DIM)
    return img

def front():
    """Two Joy-Con, as ASCII, in front of the cuff."""
    layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    font = ImageFont.truetype(FONT, 54)
    d = ImageDraw.Draw(layer)
    for x, txt, col in ((130, "[(o) -]", (90, 200, 255)), (650, "[+ (o)]", (255, 95, 95))):
        d.text((x, 845), txt, font=font, fill=col + (255,))
    halo = layer.filter(ImageFilter.GaussianBlur(14))
    return Image.alpha_composite(halo, layer)

def composite(rounded):
    img = back().convert("RGBA")
    img = Image.alpha_composite(img, text_block(art(), 46, INK, True))
    img = Image.alpha_composite(img, front())
    if not rounded: return img
    # macOS's shape: a rounded square of 824 px on the 1024 canvas, with a soft shadow
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).rounded_rectangle((100, 100, 924, 924), radius=185, fill=255)
    shadow = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((100, 112, 924, 936), radius=185, fill=(0, 0, 0, 110))
    canvas = Image.alpha_composite(canvas, shadow.filter(ImageFilter.GaussianBlur(12)))
    inner = img.resize((824, 824), Image.LANCZOS)
    full = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0)); full.paste(inner, (100, 100))
    canvas.paste(full, (0, 0), mask)
    return canvas

def main():
    icon = os.path.join(ROOT, "packages", "alyx", "icon")
    back().save(os.path.join(icon, "back.png"))
    text_block(art(), 46, INK, True).save(os.path.join(icon, "middle.png"))
    front().save(os.path.join(icon, "front.png"))
    mac = composite(True)
    sets = os.path.join(ROOT, "installer", "App", "Glovebox", "Assets.xcassets")
    for px in (16, 32, 128, 256, 512):
        mac.resize((px, px), Image.LANCZOS).save(os.path.join(sets, "AppIcon.appiconset", f"icon_{px}x{px}.png"))
        mac.resize((2 * px, 2 * px), Image.LANCZOS).save(os.path.join(sets, "AppIcon.appiconset", f"icon_{px}x{px}@2x.png"))
    flat = composite(False)
    disc = Image.new("L", (SIZE, SIZE), 0); ImageDraw.Draw(disc).ellipse((0, 0, SIZE, SIZE), fill=255)
    circle = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0)); circle.paste(flat, (0, 0), disc)
    for name, px in (("gloveboxcircle", 120), ("gloveboxmark", 32)):
        src = circle if name == "gloveboxcircle" else mac
        src.resize((px, px), Image.LANCZOS).save(os.path.join(sets, name.replace("glovebox", "Glovebox").replace("circle", "Circle").replace("mark", "Mark") + ".imageset", f"{name}.png"))
        src.resize((2 * px, 2 * px), Image.LANCZOS).save(os.path.join(sets, name.replace("glovebox", "Glovebox").replace("circle", "Circle").replace("mark", "Mark") + ".imageset", f"{name}@2x.png"))
    print("icons written")

if __name__ == "__main__":
    main()
