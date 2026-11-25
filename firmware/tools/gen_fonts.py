"""Generate the LVGL fonts used by the ClawHunt agent card.

Two families:
  - Fusion Pixel 12px proportional zh_hans (SIL OFL 1.1, assets/fonts/OFL.txt): everything Chinese. Sizes are
    integer multiples of the 12 px design grid, so every glyph stays pixel-exact at 1 bpp.
  - Cascadia Mono (SIL OFL 1.1, assets/fonts/OFL-CascadiaCode.txt; the three static weights in assets/fonts are
    the unchanged files of release v2407.24): Latin and digits, smoothed (4 bpp). The small sizes carry ASCII and
    fall back to the pixel font for anything else, so a Chinese name in a Latin line is still drawn. The display
    sizes carry only what a number, a level or a pairing code is made of.

Usage:
    python tools/gen_fonts.py <path-to-lv_font_conv.js>

Requires Node.js and lv_font_conv 1.5.3 (npm install lv_font_conv@1.5.3).
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "fonts"
PIXEL = OUT / "fusion-pixel-12px-proportional-zh_hans.ttf.woff"
MONO = {400: OUT / "CascadiaMono-Regular.ttf", 600: OUT / "CascadiaMono-SemiBold.ttf", 700: OUT / "CascadiaMono-Bold.ttf"}
PUNCT = "，。！？：；、“”‘’（）《》「」…—·￥＋"
# What the one large number of a screen is made of: an amount, its K or M, a level, a count.
NUMBER = "0123456789.,$+-/%# KMLV"
# A pairing code as well: the 72 px and 48 px sizes can set one.
CODE = NUMBER + "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def gb2312_hanzi() -> str:
    """All 6763 GB2312 level-1 and level-2 hanzi: enough for arbitrary job titles."""
    chars = []
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return "".join(chars)


def convert(conv_js: str, name: str, font: pathlib.Path, size: int, bpp: int, glyphs: list, extra=()) -> None:
    out = OUT / f"{name}.c"
    # the converter takes the font's path as it is given and writes it into the file's head: a path inside the
    # repository, so that the generated file does not name the folder it was generated in
    cmd = ["node", conv_js, "--font", font.relative_to(ROOT).as_posix(), *glyphs,
           "--size", str(size), "--bpp", str(bpp), "--format", "lvgl", "--no-compress", "--no-kerning", *extra,
           "--lv-font-name", name, "--lv-include", "lvgl.h", "--output", out.relative_to(ROOT).as_posix()]
    subprocess.run(cmd, check=True, cwd=ROOT)
    print(f"{out.name}: {out.stat().st_size // 1024} KB of C source")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    conv_js = str(pathlib.Path(sys.argv[1]).resolve())
    cjk = gb2312_hanzi() + PUNCT
    pixel = ["--autohint-off"]
    convert(conv_js, "font_px12", PIXEL, 12, 1, ["--range", "0x20-0x7E", "--symbols", cjk], pixel)
    convert(conv_js, "font_px24", PIXEL, 24, 1, ["--range", "0x20-0x7E", "--symbols", cjk], pixel)

    # ASCII and the middle dot; what is not there is drawn with the 12 px pixel font
    latin = ["--range", "0x20-0x7E", "--range", "0xB7"]
    fallback = ["--lv-fallback", "font_px12"]
    convert(conv_js, "font_mono11", MONO[600], 11, 4, latin, fallback)
    convert(conv_js, "font_mono12", MONO[400], 12, 4, latin, fallback)
    convert(conv_js, "font_mono12b", MONO[600], 12, 4, latin, fallback)
    convert(conv_js, "font_mono14", MONO[600], 14, 4, latin, fallback)

    # the display sizes, and the small size that sets a number's suffix beside each
    convert(conv_js, "font_big72", MONO[700], 72, 4, ["--symbols", CODE])
    convert(conv_js, "font_big60", MONO[700], 60, 4, ["--symbols", NUMBER])
    convert(conv_js, "font_big48", MONO[700], 48, 4, ["--symbols", CODE])
    convert(conv_js, "font_big32", MONO[700], 32, 4, ["--symbols", NUMBER])
    convert(conv_js, "font_big26", MONO[700], 26, 4, ["--symbols", NUMBER])
    convert(conv_js, "font_big21", MONO[700], 21, 4, ["--symbols", NUMBER])
    return 0


if __name__ == "__main__":
    sys.exit(main())
