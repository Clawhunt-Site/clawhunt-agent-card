"""Generate the LVGL pixel fonts used by the ClawHunt agent card.

Source: Fusion Pixel 12px proportional zh_hans (SIL OFL 1.1, see assets/fonts/OFL.txt).
Sizes are integer multiples of the 12 px design grid so every glyph stays pixel-exact at 1 bpp.

Usage:
    python tools/gen_fonts.py <path-to-lv_font_conv.js>

Requires Node.js and lv_font_conv 1.5.3 (npm install lv_font_conv@1.5.3).
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FONT = ROOT / "assets" / "fonts" / "fusion-pixel-12px-proportional-zh_hans.ttf.woff"
OUT = ROOT / "assets" / "fonts"
PUNCT = "，。！？：；、“”‘’（）《》「」…—·￥＋"
# The hanzi the screens set at 36 px (their titles). The 36 px font carries only these, not all of GB2312:
# a title that needs another character must add it here.
TITLES = "手慢了没付成收到确认连上正在接机配网设置升级账新单已款请求绑定这张卡中有人点名要你按住抢继续试运行只记录不扣口令对冻结"


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


def convert(conv_js: str, name: str, size: int, symbols: str) -> None:
    out = OUT / f"{name}.c"
    cmd = [
        "node", conv_js,
        "--font", str(FONT),
        "--range", "0x20-0x7E",
        "--size", str(size), "--bpp", "1", "--format", "lvgl",
        "--no-compress", "--no-kerning", "--autohint-off",
        "--lv-font-name", name, "--lv-include", "lvgl.h",
        "--output", str(out),
    ]
    if symbols:
        cmd[6:6] = ["--symbols", symbols]
    subprocess.run(cmd, check=True)
    print(f"{out.name}: {out.stat().st_size // 1024} KB of C source")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    conv_js = sys.argv[1]
    cjk = gb2312_hanzi() + PUNCT
    convert(conv_js, "font_px12", 12, cjk)
    convert(conv_js, "font_px24", 24, cjk)
    convert(conv_js, "font_px36", 36, TITLES)
    # Display sizes for the one big number of a screen (an amount, a level, a code): ASCII only.
    convert(conv_js, "font_px48", 48, "")
    convert(conv_js, "font_px72", 72, "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
