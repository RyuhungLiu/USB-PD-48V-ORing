"""Charger capability table page (page_caps.c) preview: redraws sample data using the firmware coordinates, outputs docs/ui_caps.png (3x scale)."""
import os
import sys
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from home_preview import ROOT, W, H, BG, INK, DIM, GRID, BLUE, YEL, AMB, load, width, draw  # noqa: E402

NAVY = (46, 22, 78)
F8 = load("font_8x16")

# (type, voltage text, current / power text, is EPR level, is current contract)
CH = [
    ("CH1 EPR", "140W", [
        ("FIX", "5V", "3A", 0, 0), ("FIX", "9V", "3A", 0, 0), ("FIX", "12V", "3A", 0, 0), ("FIX", "15V", "3A", 0, 0),
        ("FIX", "20V", "5A", 0, 0), ("PPS", "3.3-11V", "3A", 0, 0), ("PPS", "3.3-21V", "3A", 0, 0),
        ("FIX", "28V", "5A", 1, 0), ("EAV", "15-28V", "140W", 1, 1)]),
    ("CH2 EPR", "240W", [
        ("FIX", "5V", "3A", 0, 0), ("FIX", "9V", "3A", 0, 0), ("FIX", "15V", "3A", 0, 0), ("FIX", "20V", "5A", 0, 0),
        None, None, None,
        ("FIX", "28V", "5A", 1, 0), ("FIX", "36V", "5A", 1, 0), ("FIX", "48V", "5A", 1, 0),
        ("EAV", "15-48V", "240W", 1, 1)]),
]


def main():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    def text(x, y, s, c, bg=BG): draw(img, F8, x, y, s, c, bg)
    # Same as firmware gfx_draw_string_right: first fill the left side of the box with the background color, then draw the text (if the box covers other text, the preview shows it too)
    def rtext(xr, y, s, c, bg=BG, box=None):
        w = width(F8, s)
        if box and box > w:
            d.rectangle([xr - box, y, xr - w - 1, y + 15], fill=bg)
        draw(img, F8, xr - w, y, s, c, bg)

    # Title bar (ui_draw_title): EVA green square + title on the left, two-channel MOS status on the right
    d.rectangle([0, 3, 4, 17], fill=YEL)
    text(12, 3, "CHARGER CAPS", INK)
    rtext(W - 4, 3, "CH1:ON CH2:ON", BLUE)
    d.line([0, 21, W - 1, 21], fill=GRID)
    d.line([160, 24, 160, H - 1], fill=GRID)

    for ch, (title, pw, rows) in enumerate(CH):
        x0 = ch * 160
        x, xr = x0 + 4, x0 + 156
        text(x, 24, title, BLUE)
        rtext(xr, 24, pw, INK, BG, 80)
        d.line([x, 41, xr - 1, 41], fill=GRID)
        for r, row in enumerate(rows):
            y = 44 + 16 * r
            rtext(x + 16, y, str(r + 1), GRID, BG, 16)
            if row is None:
                continue                    # SPR filler gap: leave blank
            k, v, a, epr, cur = row
            bg = NAVY if cur else BG
            fg = YEL if cur else (BLUE if epr else INK)
            if cur:
                d.rectangle([x + 18, y, xr + 1, y + 15], fill=NAVY)
            text(x + 20, y, k, fg, bg)
            rtext(x + 104, y, v, fg, bg, 60)
            rtext(xr, y, a, fg, bg, 48)

    img.resize((W * 3, H * 3), Image.NEAREST).save(os.path.join(ROOT, "docs", "ui_caps.png"))


if __name__ == "__main__":
    main()
