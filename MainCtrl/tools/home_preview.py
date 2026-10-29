"""Home page layout preview: redraws on the PC using the coordinates in src/ui/pages/page_home.c (sample data), outputs a PNG at 3x scale.

All text (including the 8x16 / 16x24 small fonts) is read directly from the bitmaps in src/ui/fonts/*.c, matching the real device.
Usage: python tools/home_preview.py            -> docs/ui_home_off.png, docs/ui_home_balance.png
"""
import os
import re
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 320, 240

# EVA color scheme (matches src/ui/gfx.h); YEL is the EVA green of the total-output color block (BH_ACCENT), AMB is the amber hint color
BG = (0, 0, 0); INK = (236, 230, 255); DIM = (150, 128, 200); GRID = (72, 42, 112)
RED = (255, 60, 92); BLUE = (160, 96, 255); YEL = (118, 226, 48); AMB = (255, 150, 0)


def load(name):
    src = open(os.path.join(ROOT, "src", "ui", "fonts", f"{name}.c"), encoding="utf-8").read()
    m = re.search(r"const font_t \w+ = \{(\d+), (\d+), (0x[0-9A-Fa-f]+|\d+), (\d+), (NULL|\"[^\"]*\"), \w+, (\d), (\w+)(?:, \w+)?\}", src)
    w, h, first, count = int(m.group(1)), int(m.group(2)), int(m.group(3), 0), int(m.group(4))
    chars = m.group(5)[1:-1] if m.group(5) != "NULL" else "".join(chr(first + i) for i in range(count))
    bpp = int(m.group(6))
    body = src.split("_bitmap[] = {")[1].split("};")[0]
    data = [int(x, 16) for x in re.findall(r"0x([0-9A-F]{2})", body)]
    if "_offset[] = {" in src:
        # Zero run-length compression (0x00 followed by the count of consecutive 0 bytes): expand into an uncompressed bitmap
        raw, k = [], 0
        while k < len(data):
            if data[k] == 0:
                raw += [0] * data[k + 1]
                k += 2
            else:
                raw.append(data[k])
                k += 1
        data = raw
    am = re.search(r"_advance\[\] = \{([^}]*)\}", src)
    adv = [int(x) for x in am.group(1).split(",")] if am else [w] * len(chars)
    return dict(w=w, h=h, chars=chars, data=data, adv=adv, bpp=bpp, rb=(w * bpp + 7) // 8)


def width(font, text):
    return sum(font["adv"][font["chars"].index(c)] if c in font["chars"] else font["w"] for c in text)


def draw(img, font, x, y, text, fg, bg):
    px = img.load()
    for c in text:
        if c not in font["chars"]:
            x += font["w"]; continue
        i = font["chars"].index(c)
        base = i * font["rb"] * font["h"]
        for row in range(font["h"]):
            for col in range(font["adv"][i]):
                byte = font["data"][base + row * font["rb"] + (col * font["bpp"]) // 8]
                if font["bpp"] == 2:
                    k = (byte >> (6 - 2 * (col % 4))) & 3
                else:
                    k = 3 if byte & (0x80 >> (col % 8)) else 0
                px[x + col, y + row] = tuple((bg[j] * (3 - k) + fg[j] * k) // 3 for j in range(3))
        x += font["adv"][i]


def render(balancing, path):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f8, fch, fout = load("font_8x16"), load("font_num_ch"), load("font_num_out")

    def rect(x, y, w, h, c): d.rectangle([x, y, x + w - 1, y + h - 1], fill=c)
    def text(x, y, s, c, bg=BG, font=f8): draw(img, font, x, y, s, c, bg)
    def rtext(xr, y, s, c, bg=BG, font=f8): draw(img, font, xr + 1 - width(font, s), y, s, c, bg)

    HALF, LX, LR = 160, 12, 150
    RX, RR = 169, 311
    BX, BR = 171, 309
    rect(0, 119, HALF, 1, GRID); rect(HALF, 0, 1, H, GRID)
    bands = [("58.6", "19.96V", "2.93A", "+0mV", "52%", RED, "on"),
             ("56.0" if balancing else "240.0", "20.04V", "2.80A" if balancing else "12.00A", "+40mV", "48%", BLUE,
              "on" if balancing else "arm")]
    for k, (pw, v, a, off, pct, bc, st) in enumerate(bands):
        y0 = k * 120
        rect(0, y0 + 10, 4, 99, INK if st == "on" else AMB)
        cx, cy = LX + 6, y0 + 20 + fch["h"] // 2
        if st == "on": d.ellipse([cx - 6, cy - 6, cx + 6, cy + 6], fill=INK)
        else: d.ellipse([cx - 6, cy - 6, cx + 6, cy + 6], outline=AMB, width=2)
        rtext(LR - 9, y0 + 20, pw, INK, font=fch)
        text(LR - 7, y0 + 20 + fch["h"] - 16, "W", DIM)
        text(LX, y0 + 60, v, DIM); rtext(LR, y0 + 60, a, DIM)
        rect(LX, y0 + 82, LR - LX + 1, 1, GRID)
        for i in range(11):
            x = LX + i * (LR - LX) // 10; rect(x, y0 + 83, 1, 5 if i % 5 == 0 else 2, GRID)
        rect(LX + 40, y0 + 82, 12, 1, INK)
        if balancing:
            # Same as page_home.c: current difference / (total current x 20%) in 16 steps, blending from white to red / blue (sample difference 2.3%)
            t = min(16, int(abs(293 - 280) * 16 * 100 / (573 * 20)))
            col = tuple((INK[j] * (16 - t) + bc[j] * t) // 16 for j in range(3))
            text(LX, y0 + 92, off, col); rtext(LR, y0 + 92, pct, col)

    # Right half: MAX x2, BALANCE / yellow color block / current difference + bar, two rows of values
    for k in range(2):
        text(RX, 6 + 18 * k, f"CH{k + 1} MAX " + ("60.0W" if balancing else "100.0W"), DIM)
        rtext(RR, 6 + 18 * k, "SP" if balancing else "SF", INK)
    rect(161, 64, W - 161, 114, YEL)
    x, y, w, h, L = 165, 68, W - 161 - 8, 106, 10
    for a_, b_ in [(x, y), (x + w - L, y), (x, y + h - 1), (x + w - L, y + h - 1)]: rect(a_, b_, L, 1, BG)
    for a_, b_ in [(x, y), (x + w - 1, y), (x, y + h - L), (x + w - 1, y + h - L)]: rect(a_, b_, 1, L, BG)
    text(BX, 72, "OUT", BG, YEL); text(BR - 7, 72, "W", BG, YEL)
    rtext(BR, 64 + (114 - fout["h"]) // 2, "114.6", BG, YEL, fout)
    text(BX, 154, "5.73A", BG, YEL); rtext(BR, 154, "20.04V", BG, YEL)
    bw, bh, by = 56, 12, 184
    bx = RR - bw + 1
    d.rectangle([bx - 1, by - 1, bx + bw, by + bh], outline=GRID)
    split = bw * 293 // 573
    if balancing:
        text(RX, 42, "BALANCE", DIM); rtext(RR, 42, "RUN", INK)
        text(RX, 182, "DIFF 2.3%", INK)
        t = min(16, int(abs(293 - 280) * 16 * 100 / (573 * 20)))
        mix = lambda c: tuple((INK[j] * (16 - t) + c[j] * t) // 16 for j in range(3))
        rect(bx, by, split, bh, mix(RED)); rect(bx + split, by, bw - split, bh, mix(BLUE))
        rect(bx + split, by, 1, bh, BG)
        text(RX, 202, "CENTER", DIM); rtext(RR, 202, "20.00V", DIM)
        text(RX, 220, "ADJUST", DIM); rtext(RR, 220, "132", DIM)
    else:
        text(RX, 42, "BALANCE", DIM); rtext(RR, 42, "OFF", GRID)
        text(RX, 182, "DIFF --", GRID)
        rect(bx, by, split, bh, DIM); rect(bx + split, by, bw - split, bh, GRID)
        text(RX, 202, "PEAK", DIM); rtext(RR, 202, "118.4W", DIM)
        text(RX, 220, "ENERGY", DIM); rtext(RR, 220, "84.95Wh", DIM)
    img.resize((W * 3, H * 3), Image.NEAREST).save(path)


if __name__ == "__main__":
    render(False, os.path.join(ROOT, "docs", "ui_home_off.png"))
    render(True, os.path.join(ROOT, "docs", "ui_home_balance.png"))
