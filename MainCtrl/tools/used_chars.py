"""List the characters actually used by UI strings in src/ (excluding LOG and comments), for trimming the 8x16 font.

Format specifiers (%s/%d/%lu...) are removed; digits, uppercase hex and common numeric symbols are always added, to cover formatted output.
Usage: python tools/used_chars.py      -> prints the character set
"""
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
FMT_RE = re.compile(r"%[-+ 0#]*\d*(?:\.\d+)?(?:ll|l|h)?[sdiuxXc%]")
BASE = " 0123456789ABCDEF+-.:%/"


def used_chars():
    chars = set(BASE)
    for path in glob.glob(os.path.join(ROOT, "src", "**", "*.c"), recursive=True):
        if os.sep + "fonts" + os.sep in path:
            continue
        for line in open(path, encoding="utf-8"):
            if "LOG(" in line or line.strip().startswith("//"):
                continue
            for m in STR_RE.finditer(line):
                s = FMT_RE.sub("", m.group(1)).replace('\\"', '"').replace("\\\\", "\\")
                chars |= {c for c in s if 0x20 <= ord(c) < 0x7F}
    return "".join(sorted(chars))


if __name__ == "__main__":
    cs = used_chars()
    print(len(cs))
    print(repr(cs))
