# Fonts

`gen_font.py` uses **Chakra Petch Bold** (SIL Open Font License 1.1) when generating the home page digit glyphs; the font file is not included in the repository.

Download Chakra Petch from Google Fonts and place `ChakraPetch-Bold.ttf` in this folder.

The bitmap glyphs actually used by the firmware are already generated in `src/ui/fonts/*.c`; this font is only needed when regenerating the glyphs.
