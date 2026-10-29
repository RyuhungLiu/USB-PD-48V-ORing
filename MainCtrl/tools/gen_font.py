"""
Bitmap font generator -> src/ui/fonts/*.c

Monospace 1bpp fonts: glyphs stored row by row, ceil(W/8) bytes per row, MSB on the left. Default font Cascadia Mono (OFL 1.1).
Home page large-digit 2bpp fonts: 2 bits per pixel (4 gray levels, anti-aliased), ceil(W/4) bytes per row, high bits on the left;
each glyph also stores its advance width. Font Chakra Petch Bold (OFL 1.1, tools/fonts/).

Usage:
    python tools/gen_font.py            # generate all fonts + preview image tools/font_preview.png
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from used_chars import used_chars  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "src", "ui", "fonts")
DEFAULT_TTF = r"C:\Windows\Fonts\CascadiaMono.ttf"

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))

# name, cell W, cell H, charset (None = contiguous ASCII range)
CHAKRA_BOLD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts", "ChakraPetch-Bold.ttf")

# name, cell W, cell H, charset (None = contiguous ASCII range), ttf, bpp
# All pages use Chakra Petch Bold; centered within a monospace cell, 2bpp anti-aliased
FONTS = [
    # 8x16 keeps only the characters actually used by UI strings (tools/used_chars.py scans src/; regenerate after adding strings)
    ("font_8x16", 8, 16, used_chars(), CHAKRA_BOLD, 2),
    # 16x24 is only used for menu items (MOS / VOLTAGE / SETUP) and diagnostic page values: keep only the characters actually used
    # (if new menu text uses J Q X Z * / ( ) + etc., add them back)
    ("font_16x24", 16, 24, " %-.0123456789>ABCDEFGHIKLMNOPRSTUVWY[]", CHAKRA_BOLD, 2),
]

# 2bpp proportional fonts: name, ttf, digit height (pixels), charset
USER_FONTS = os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\Windows\Fonts")
TOOLS_FONTS = os.path.join(ROOT, "tools", "fonts")
AA_FONTS = [
    # Chakra Petch Bold (SIL OFL 1.1), tools/fonts/ChakraPetch-Bold.ttf (Google Fonts)
    ("font_num_ch", os.path.join(TOOLS_FONTS, "ChakraPetch-Bold.ttf"), 28, "0123456789."),    # home page channel power
    ("font_num_out", os.path.join(TOOLS_FONTS, "ChakraPetch-Bold.ttf"), 34, "0123456789."),   # home page total output
]

THRESHOLD = 110


def fit_font(ttf, w, h, chars):
    """Find the largest font size at which every glyph fits in a w*h cell, return (font, top, bottom)."""
    best = None
    for size in range(4, 200):
        font = ImageFont.truetype(ttf, size)
        tops, bottoms, adv = [], [], 0
        for ch in chars:
            if ch == " ":
                continue
            l, t, r, b = font.getbbox(ch)
            tops.append(t)
            bottoms.append(b)
            adv = max(adv, font.getlength(ch), r)
        top, bottom = min(tops), max(bottoms)
        if adv > w or (bottom - top) > h:
            break
        best = (font, top, bottom)
    if best is None:
        raise SystemExit(f"cell {w}x{h} too small")
    return best


def render_glyph(font, top, bottom, ch, w, h, threshold=True):
    img = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(img)
    adv = font.getlength(ch)
    x = (w - adv) / 2
    y = -top + (h - (bottom - top)) // 2
    draw.text((x, y), ch, fill=255, font=font)
    return img.point(lambda v: 255 if v >= THRESHOLD else 0) if threshold else img


def pack(img, w, h):
    row_bytes = (w + 7) // 8
    out = []
    px = img.load()
    for y in range(h):
        for bx in range(row_bytes):
            byte = 0
            for bit in range(8):
                x = bx * 8 + bit
                if x < w and px[x, y]:
                    byte |= 0x80 >> bit
            out.append(byte)
    return out


def rle0(data):
    """Zero-byte run-length compression: 0x00 followed by the count of consecutive 0 bytes (1..255), other bytes unchanged (decoding: see gfx_draw_char)."""
    out, i = [], 0
    while i < len(data):
        if data[i] == 0:
            j = i
            while j < len(data) and data[j] == 0 and j - i < 255:
                j += 1
            out += [0, j - i]
            i = j
        else:
            out.append(data[i])
            i += 1
    return out


def bitmap_lines(name, chars, packed):
    """Per-glyph compressed bitmaps and the per-glyph start-offset table (font_t.offset), returns (source lines, byte count)."""
    lines = [f"static const uint8_t {name}_bitmap[] = {{"]
    offs, pos = [], 0
    for ch, data in zip(chars, packed):
        z = rle0(data)
        offs.append(pos)
        pos += len(z)
        lines.append(f"    /* {char_comment(ch)} */ " + ", ".join(f"0x{b:02X}" for b in z) + ",")
    lines += ["};", "", f"static const uint16_t {name}_offset[] = {{{', '.join(str(o) for o in offs)}}};", ""]
    return lines, pos + 2 * len(offs)


def char_comment(ch):
    return "space" if ch == " " else ("backslash" if ch == "\\" else ch)


def pack2(img, w, h):
    """2bpp: 4 gray levels per pixel, ceil(w/4) bytes per row, high bits on the left."""
    row_bytes = (w + 3) // 4
    out = []
    px = img.load()
    for y in range(h):
        for bx in range(row_bytes):
            byte = 0
            for k in range(4):
                x = bx * 4 + k
                level = (px[x, y] * 3 + 127) // 255 if x < w else 0
                byte |= level << (6 - 2 * k)
            out.append(byte)
    return out


def fit_cap_height(ttf, w, h, chars):
    """Fit a proportional font into a monospace cell: font size chosen so the uppercase height is about 0.68 x h, and the vertical extent of the whole charset does not exceed h."""
    cap_target = round(h * 0.68)
    best = None
    for size in range(4, 200):
        font = ImageFont.truetype(ttf, size)
        l, t, r, b = font.getbbox("H")
        tops = [font.getbbox(ch)[1] for ch in chars if ch != " "]
        bottoms = [font.getbbox(ch)[3] for ch in chars if ch != " "]
        if max(bottoms) - min(tops) > h or b - t > cap_target:
            break
        best = (font, min(tops), max(bottoms))
    return best


def render_squeezed(font, top, bottom, ch, w, h):
    """Render with each glyph's own advance width; glyphs wider than the cell (M, W, etc.) are compressed horizontally to the cell width, the rest are centered."""
    adv = max(1, int(round(font.getlength(ch))))
    l, t, r, b = font.getbbox(ch) if ch != " " else (0, 0, 0, 0)
    src_w = max(adv, r)
    img = Image.new("L", (src_w, h), 0)
    y = -top + (h - (bottom - top)) // 2
    ImageDraw.Draw(img).text((0, y), ch, fill=255, font=font)
    if src_w > w:
        return img.resize((w, h), Image.LANCZOS)
    cell = Image.new("L", (w, h), 0)
    cell.paste(img, ((w - src_w) // 2, 0))
    return cell


def gen(name, w, h, charset, ttf, bpp=1):
    contiguous = charset is None
    chars = ASCII if contiguous else charset
    if bpp == 2 or ttf == CHAKRA_BOLD:
        # Proportional font: same font size and layout as 2bpp; 1bpp is then binarized with THRESHOLD
        font, top, bottom = fit_cap_height(ttf, w, h, chars)
        glyphs = [render_squeezed(font, top, bottom, ch, w, h) for ch in chars]
        if bpp == 1:
            glyphs = [g.point(lambda v: 255 if v >= THRESHOLD else 0) for g in glyphs]
    else:
        font, top, bottom = fit_font(ttf, w, h, chars)
        glyphs = [render_glyph(font, top, bottom, ch, w, h) for ch in chars]

    lines = [
        "// Auto-generated by tools/gen_font.py, do not edit by hand",
        f"// Source font: {os.path.basename(ttf)}  cell: {w}x{h}  size: {font.size}  {bpp}bpp",
        "",
        '#include "ui/fonts/fonts.h"',
        "",
    ]
    packed = [pack2(g, w, h) if bpp == 2 else pack(g, w, h) for g in glyphs]
    body, size = bitmap_lines(name, chars, packed)
    lines += body

    if contiguous:
        lines.append(f"const font_t {name} = {{{w}, {h}, 0x20, {len(chars)}, NULL, {name}_bitmap, {bpp}, NULL, {name}_offset}};")
    else:
        esc = chars.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'const font_t {name} = {{{w}, {h}, 0, {len(chars)}, "{esc}", {name}_bitmap, {bpp}, NULL, {name}_offset}};')
    lines.append("")

    with open(os.path.join(OUT_DIR, f"{name}.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print(f"{name}: {len(chars)} glyphs {bpp}bpp, {len(chars) * ((w * bpp + 7) // 8) * h} -> {size} bytes")
    return chars, glyphs


def fit_digit_height(ttf, digit_h):
    """Smallest font size at which the height of digit '0' reaches digit_h."""
    for size in range(8, 300):
        font = ImageFont.truetype(ttf, size)
        l, t, r, b = font.getbbox("0")
        if b - t >= digit_h:
            return font
    raise SystemExit("digit height too large")


def gen_aa(name, ttf, digit_h, chars):
    """2bpp anti-aliased font: cell height = charset height, cell width = maximum advance width.
    Digits are made monospace (maximum digit width, glyph centered) so the decimal point does not jump when values change; other characters keep their own width."""
    font = fit_digit_height(ttf, digit_h)
    top = min(font.getbbox(ch)[1] for ch in chars)
    bottom = max(font.getbbox(ch)[3] for ch in chars)
    h = bottom - top
    own = [int(round(font.getlength(ch))) for ch in chars]
    tab = max(a for ch, a in zip(chars, own) if ch.isdigit())
    adv = [tab if ch.isdigit() else a for ch, a in zip(chars, own)]
    w = max(adv)

    glyphs, packed = [], []
    for ch, a, o in zip(chars, adv, own):
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text(((a - o) / 2, -top), ch, fill=255, font=font)
        glyphs.append(img)
        px = img.load()
        row_bytes = (w + 3) // 4
        data = []
        for y in range(h):
            for bx in range(row_bytes):
                byte = 0
                for k in range(4):
                    x = bx * 4 + k
                    level = (px[x, y] * 3 + 127) // 255 if x < w else 0
                    byte |= level << (6 - 2 * k)
                data.append(byte)
        packed.append(data)

    lines = [
        "// Auto-generated by tools/gen_font.py, do not edit by hand",
        f"// Source font: {os.path.basename(ttf)} (SIL OFL 1.1)  cell: {w}x{h}  size: {font.size}  2bpp proportional width",
        "",
        '#include "ui/fonts/fonts.h"',
        "",
    ]
    body, size = bitmap_lines(name, chars, packed)
    lines += body
    lines += [f"static const uint8_t {name}_advance[] = {{{', '.join(str(a) for a in adv)}}};", ""]
    esc = chars.replace("\\", "\\\\").replace('"', '\\"')
    lines.append(f'const font_t {name} = {{{w}, {h}, 0, {len(chars)}, "{esc}", {name}_bitmap, 2, {name}_advance, {name}_offset}};')
    lines.append("")
    with open(os.path.join(OUT_DIR, f"{name}.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"{name}: {len(chars)} glyphs {w}x{h} size {font.size}, {len(chars) * ((w + 3) // 4) * h} -> {size} bytes")
    return w, h, chars, glyphs


def preview(results, path):
    scale = 2
    rows = []
    for name, w, h, chars, glyphs in results:
        per_line = max(1, 640 // w)
        n_lines = (len(glyphs) + per_line - 1) // per_line
        sheet = Image.new("L", (per_line * w, n_lines * h), 40)
        for i, g in enumerate(glyphs):
            sheet.paste(g, ((i % per_line) * w, (i // per_line) * h))
        rows.append(sheet)
    total_w = max(r.width for r in rows)
    total_h = sum(r.height for r in rows) + 8 * len(rows)
    img = Image.new("L", (total_w, total_h), 0)
    y = 0
    for r in rows:
        img.paste(r, (0, y))
        y += r.height + 8
    img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    img.save(path)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    results = []
    for name, w, h, charset, ttf, bpp in FONTS:
        if not os.path.isfile(ttf):
            sys.exit(f"font not found: {ttf}")
        chars, glyphs = gen(name, w, h, charset, ttf, bpp)
        results.append((name, w, h, chars, glyphs))

    for name, ttf, digit_h, chars in AA_FONTS:
        if not os.path.isfile(ttf):
            sys.exit(f"font not found: {ttf}")
        w, h, chars, glyphs = gen_aa(name, ttf, digit_h, chars)
        results.append((name, w, h, chars, glyphs))

    preview(results, os.path.join(ROOT, "tools", "font_preview.png"))


if __name__ == "__main__":
    main()
