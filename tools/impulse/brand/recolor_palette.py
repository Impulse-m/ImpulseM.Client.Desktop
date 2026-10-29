"""Re-seeds the blue literals of a desktop palette to the ImpulseM palette.

Python port of the Android rule in Tools/branding/recolor.py. A blue (hue 190-222 degrees, saturation at least
0.35, value at least 0.40) is moved by the HSV offset between an upstream anchor blue and the brand target. The
anchors are the upstream Telegram Desktop values windowBgActive #40a7e3 and windowActiveTextFg #168acd; keys with
those names map exactly onto the brand values. Keys whose name starts with one of EXCLUDED_KEY_PREFIXES keep their
colour. Only the first literal of a value is rewritten, so references and fallbacks after "|" stay as they are. An
alpha pair (#RRGGBBAA) keeps its alpha. Line endings are kept.

The tool is meant for the upstream palette. Running it on a palette it already changed shifts the brand colours
again.

Usage: python -I Tools/impulse/brand/recolor_palette.py <colors.palette> [--night] [--output <file>]
Without --output the result is written to standard output.
"""

import argparse
import colorsys
import re
import sys
from pathlib import Path
from typing import Dict, List, Match, Optional, Pattern, Tuple


PRIMARY_DAY: int = 0x2085DF
LINK_DAY: int = 0x106ACC
PRIMARY_NIGHT: int = 0x58AEEE

# Upstream Telegram Desktop anchors. Each maps exactly onto its brand target.
UPSTREAM_DAY_BLUE: int = 0x40A7E3
UPSTREAM_DAY_TEXT: int = 0x168ACD
UPSTREAM_NIGHT_BLUE: int = 0x5288C1

DAY_EXACT: Dict[str, int] = {
    "windowBgActive": PRIMARY_DAY,
    "windowActiveTextFg": LINK_DAY,
}
NIGHT_EXACT: Dict[str, int] = {
    "windowBgActive": PRIMARY_NIGHT,
    "windowActiveTextFg": PRIMARY_NIGHT,
}

EXCLUDED_KEY_PREFIXES: List[str] = [
    "peer",
    "userpic",
    "bubble",
    "premium",
    "gift",
    "chat_in",
    "chat_out",
    "chat_wallpaper",
    "avatar_",
    "stories_",
    "statisticChart",
    "color_",
    "code_",
]

BLUE_HUE_MIN: float = 190 / 360
BLUE_HUE_MAX: float = 222 / 360
BLUE_SATURATION_MIN: float = 0.35
BLUE_VALUE_MIN: float = 0.40

# Group "head" is the key and the separator, group "rgb" the six RGB digits, group "alpha" the optional AA pair.
LITERAL_LINE: Pattern[str] = re.compile(
    r"^(?P<head>\s*(?P<key>[A-Za-z]\w*)\s*:\s*)#(?P<rgb>[0-9a-fA-F]{6})(?P<alpha>[0-9a-fA-F]{2})?(?![0-9a-fA-F])(?P<tail>.*)$"
)


def hsv_of(rgb: int) -> Tuple[float, float, float]:
    red: float = ((rgb >> 16) & 0xFF) / 255
    green: float = ((rgb >> 8) & 0xFF) / 255
    blue: float = (rgb & 0xFF) / 255
    return colorsys.rgb_to_hsv(red, green, blue)


def shift(rgb: int, base: int, target: int) -> int:
    """Moves a colour by the HSV offset between base and target. base maps exactly onto target."""
    if rgb == base:
        return target
    source: Tuple[float, float, float] = hsv_of(rgb)
    anchor: Tuple[float, float, float] = hsv_of(base)
    goal: Tuple[float, float, float] = hsv_of(target)
    hue: float = (source[0] + goal[0] - anchor[0]) % 1.0
    saturation: float = min(1.0, source[1] * goal[1] / anchor[1])
    value: float = min(1.0, source[2] * goal[2] / anchor[2])
    red: float
    green: float
    blue: float
    red, green, blue = colorsys.hsv_to_rgb(hue, saturation, value)
    return (round(red * 255) << 16) | (round(green * 255) << 8) | round(blue * 255)


def is_blue(rgb: int) -> bool:
    hsv: Tuple[float, float, float] = hsv_of(rgb)
    return BLUE_HUE_MIN <= hsv[0] <= BLUE_HUE_MAX and hsv[1] >= BLUE_SATURATION_MIN and hsv[2] >= BLUE_VALUE_MIN


def is_excluded(key: str) -> bool:
    return any(key.startswith(prefix) for prefix in EXCLUDED_KEY_PREFIXES)


def is_link_key(key: str) -> bool:
    return "link" in key.lower()


def recolor_color(key: str, rgb: int, night: bool) -> int:
    exact: Dict[str, int] = NIGHT_EXACT if night else DAY_EXACT
    if key in exact:
        return exact[key]
    if is_excluded(key) or not is_blue(rgb):
        return rgb
    if night:
        return shift(rgb, UPSTREAM_NIGHT_BLUE, PRIMARY_NIGHT)
    if is_link_key(key):
        return shift(rgb, UPSTREAM_DAY_TEXT, LINK_DAY)
    return shift(rgb, UPSTREAM_DAY_BLUE, PRIMARY_DAY)


def transform_line(line: str, night: bool) -> str:
    match: Optional[Match[str]] = LITERAL_LINE.match(line)
    if match is None:
        return line
    key: str = match.group("key")
    rgb: int = int(match.group("rgb"), 16)
    changed: int = recolor_color(key, rgb, night)
    if changed == rgb:
        return line
    alpha: str = match.group("alpha") or ""
    return match.group("head") + "#{:06X}".format(changed) + alpha + match.group("tail")


def transform_palette(text: str, night: bool) -> str:
    lines: List[str] = []
    for line in text.split("\n"):
        lines.append(transform_line(line, night))
    return "\n".join(lines)


def main(argv: List[str]) -> int:
    parser: argparse.ArgumentParser = argparse.ArgumentParser(description="Recolours the blues of a palette file.")
    parser.add_argument("palette", type=Path, help="palette file to read (upstream colors.palette)")
    parser.add_argument("--night", action="store_true", help="use the night targets instead of the day targets")
    parser.add_argument("--output", type=Path, default=None, help="file to write; standard output when omitted")
    args: argparse.Namespace = parser.parse_args(argv)
    text: str = args.palette.read_bytes().decode("utf-8")
    result: str = transform_palette(text, args.night)
    if args.output is None:
        sys.stdout.buffer.write(result.encode("utf-8"))
    else:
        args.output.write_bytes(result.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
