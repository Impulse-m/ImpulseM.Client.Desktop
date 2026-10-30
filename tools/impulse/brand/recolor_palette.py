"""Re-seeds the blue, teal and dark literals of a desktop theme to the ImpulseM palette.

Python port of the Android rule in Tools/branding/recolor.py. A blue (hue 190-222 degrees, saturation at least
0.35, value at least 0.40) is moved by the HSV offset between an upstream anchor blue and the brand target. The
anchors are the upstream Telegram Desktop values windowBgActive #40a7e3 and windowActiveTextFg #168acd; keys with
those names map exactly onto the brand values. Keys whose name starts with one of EXCLUDED_KEY_PREFIXES keep their
colour. Only the first literal of a value is rewritten, so references and fallbacks after "|" stay as they are. An
alpha pair (#RRGGBBAA) keeps its alpha. Line endings are kept.

The dark themes (DarkTheme) also follow the web surfaces. Their surface, background and divider keys are set to the
web colours exactly. In the Night theme the teal accent family moves to the brand blue by the same HSV offset. Every
other dark colour (value below 0.35, any saturation) moves by the per-channel delta between the theme's Telegram
surface and the web surface. Bubble backgrounds are never moved by the teal or dark rules.

The tool is meant for the upstream palette. Running it on a palette it already changed shifts the brand colours
again. Each output is computed from the upstream file, so re-running it is idempotent.

Usage: python -I tools/impulse/brand/recolor_palette.py <colors.palette> [--theme day|tinted|night] [--output <file>]
       python -I tools/impulse/brand/recolor_palette.py <name.tdesktop-theme> --theme day|tinted|night --output <file>
A palette is written to standard output when --output is omitted. A zip archive (.tdesktop-theme) needs --output.
"""

import argparse
import colorsys
import re
import struct
import sys
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, FrozenSet, List, Match, Optional, Pattern, Tuple


PRIMARY_DAY: int = 0x2085DF
LINK_DAY: int = 0x106ACC
PRIMARY_NIGHT: int = 0x58AEEE

# Upstream Telegram Desktop anchors. Each maps exactly onto its brand target.
UPSTREAM_DAY_BLUE: int = 0x40A7E3
UPSTREAM_DAY_TEXT: int = 0x168ACD
UPSTREAM_NIGHT_BLUE: int = 0x5288C1
UPSTREAM_TEAL: int = 0x3FC1B0

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

# Message bubble backgrounds and their shadows. The teal and dark rules skip them, as Android skips chat_in/chat_out.
BUBBLE_KEY_PREFIXES: List[str] = [
    "msgInBg",
    "msgOutBg",
    "msgServiceBg",
    "msgInShadow",
    "msgOutShadow",
]

SURFACE_KEYS: FrozenSet[str] = frozenset({
    "windowBg",
    "boxBg",
    "boxSearchBg",
    "menuBg",
    "topBarBg",
    "historyComposeAreaBg",
})
BACKGROUND_KEYS: FrozenSet[str] = frozenset({"sideBarBg"})
DIVIDER_KEYS: FrozenSet[str] = frozenset({"menuSeparatorFg"})

BLUE_HUE_MIN: float = 190 / 360
BLUE_HUE_MAX: float = 222 / 360
TEAL_HUE_MIN: float = 150 / 360
TEAL_HUE_MAX: float = 190 / 360
BLUE_SATURATION_MIN: float = 0.35
BLUE_VALUE_MIN: float = 0.40
DARK_VALUE_MAX: float = 0.35

# Group "head" is the key and the separator, group "rgb" the six RGB digits, group "alpha" the optional AA pair.
LITERAL_LINE: Pattern[str] = re.compile(
    r"^(?P<head>\s*(?P<key>[A-Za-z]\w*)\s*:\s*)#(?P<rgb>[0-9a-fA-F]{6})(?P<alpha>[0-9a-fA-F]{2})?(?![0-9a-fA-F])(?P<tail>.*)$"
)

PALETTE_ENTRY: str = "colors.tdesktop-theme"
BACKGROUND_ENTRY: str = "background.png"


@dataclass(frozen=True)
class DarkTheme:
    name: str
    telegram_surface: int
    surface: int
    background: int
    divider: int
    teal_anchor: Optional[int]

    def exact_color(self, key: str) -> Optional[int]:
        if key in NIGHT_EXACT:
            return NIGHT_EXACT[key]
        if key in SURFACE_KEYS:
            return self.surface
        if key in BACKGROUND_KEYS:
            return self.background
        if key in DIVIDER_KEYS:
            return self.divider
        return None


# The Tinted theme (night.tdesktop-theme, shown as "Tinted" in the picker).
TINTED: DarkTheme = DarkTheme(
    name="tinted",
    telegram_surface=0x17212B,
    surface=0x1D2733,
    background=0x151E27,
    divider=0x0F151B,
    teal_anchor=None,
)

# The Night theme (night-green.tdesktop-theme, shown as "Night" in the picker).
NIGHT: DarkTheme = DarkTheme(
    name="night",
    telegram_surface=0x282E33,
    surface=0x1D2128,
    background=0x14181F,
    divider=0x0B0F15,
    teal_anchor=UPSTREAM_TEAL,
)

THEMES: Dict[str, Optional[DarkTheme]] = {
    "day": None,
    "tinted": TINTED,
    "night": NIGHT,
}


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


def is_teal(rgb: int) -> bool:
    hsv: Tuple[float, float, float] = hsv_of(rgb)
    return TEAL_HUE_MIN <= hsv[0] < TEAL_HUE_MAX and hsv[1] >= BLUE_SATURATION_MIN and hsv[2] >= BLUE_VALUE_MIN


def is_dark(rgb: int) -> bool:
    hsv: Tuple[float, float, float] = hsv_of(rgb)
    return hsv[2] < DARK_VALUE_MAX


def is_excluded(key: str) -> bool:
    return any(key.startswith(prefix) for prefix in EXCLUDED_KEY_PREFIXES)


def is_bubble(key: str) -> bool:
    return any(key.startswith(prefix) for prefix in BUBBLE_KEY_PREFIXES)


def is_link_key(key: str) -> bool:
    return "link" in key.lower()


def tint(rgb: int, theme: DarkTheme) -> int:
    """Moves a dark colour by the per-channel delta between the web surface and the Telegram surface."""
    channels: List[int] = []
    for bits in (16, 8, 0):
        delta: int = ((theme.surface >> bits) & 0xFF) - ((theme.telegram_surface >> bits) & 0xFF)
        channels.append(max(0, min(255, ((rgb >> bits) & 0xFF) + delta)))
    return (channels[0] << 16) | (channels[1] << 8) | channels[2]


def recolor_color(key: str, rgb: int, theme: Optional[DarkTheme]) -> int:
    if theme is None:
        if key in DAY_EXACT:
            return DAY_EXACT[key]
        if is_excluded(key) or not is_blue(rgb):
            return rgb
        if is_link_key(key):
            return shift(rgb, UPSTREAM_DAY_TEXT, LINK_DAY)
        return shift(rgb, UPSTREAM_DAY_BLUE, PRIMARY_DAY)
    exact: Optional[int] = theme.exact_color(key)
    if exact is not None:
        return exact
    if is_excluded(key):
        return rgb
    if is_blue(rgb):
        return shift(rgb, UPSTREAM_NIGHT_BLUE, PRIMARY_NIGHT)
    if is_bubble(key):
        return rgb
    if theme.teal_anchor is not None and is_teal(rgb):
        return shift(rgb, theme.teal_anchor, PRIMARY_NIGHT)
    if is_dark(rgb):
        return tint(rgb, theme)
    return rgb


def recolor_line(line: str, theme: Optional[DarkTheme]) -> str:
    match: Optional[Match[str]] = LITERAL_LINE.match(line)
    if match is None:
        return line
    key: str = match.group("key")
    rgb: int = int(match.group("rgb"), 16)
    changed: int = recolor_color(key, rgb, theme)
    if changed == rgb:
        return line
    alpha: str = match.group("alpha") or ""
    return match.group("head") + "#{:06X}".format(changed) + alpha + match.group("tail")


def transform_line(line: str, night: bool, theme: DarkTheme = TINTED) -> str:
    return recolor_line(line, theme if night else None)


def transform_palette(text: str, night: bool, theme: DarkTheme = TINTED) -> str:
    lines: List[str] = []
    for line in text.split("\n"):
        lines.append(transform_line(line, night, theme))
    return "\n".join(lines)


def png_chunk(kind: bytes, body: bytes) -> bytes:
    checksum: int = zlib.crc32(kind + body) & 0xFFFFFFFF
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", checksum)


def png_of(rgb: int) -> bytes:
    """A 1x1 opaque RGBA PNG of the colour, the layout of the upstream background.png files."""
    header: bytes = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    pixel: bytes = bytes([(rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF])
    scanline: bytes = b"\x00" + pixel
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(scanline, 9))
        + png_chunk(b"IEND", b"")
    )


def transform_archive(source: Path, target: Path, theme: Optional[DarkTheme]) -> None:
    """Writes a recoloured copy of an upstream .tdesktop-theme archive.

    The palette is recoloured. For a dark theme background.png, when the archive has one, becomes the chat
    background colour. Entry names and their order are kept, entries are deflated, and the other entries keep
    their content.
    """
    with zipfile.ZipFile(source) as reader:
        infos: List[zipfile.ZipInfo] = reader.infolist()
        payload: Dict[str, bytes] = {info.filename: reader.read(info) for info in infos}
    text: str = payload[PALETTE_ENTRY].decode("utf-8")
    payload[PALETTE_ENTRY] = transform_palette(text, theme is not None, theme or TINTED).encode("utf-8")
    if theme is not None and BACKGROUND_ENTRY in payload:
        payload[BACKGROUND_ENTRY] = png_of(theme.background)
    with zipfile.ZipFile(target, "w") as writer:
        for info in infos:
            entry: zipfile.ZipInfo = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = info.external_attr
            writer.writestr(entry, payload[info.filename])


def main(argv: List[str]) -> int:
    parser: argparse.ArgumentParser = argparse.ArgumentParser(
        description="Recolours the blues, teal and dark colours of a desktop theme.",
    )
    parser.add_argument("source", type=Path, help="upstream colors.palette or .tdesktop-theme archive to read")
    parser.add_argument("--theme", choices=sorted(THEMES), default="day", help="target theme (default: day)")
    parser.add_argument("--output", type=Path, default=None, help="file to write; standard output when omitted")
    args: argparse.Namespace = parser.parse_args(argv)
    theme: Optional[DarkTheme] = THEMES[args.theme]
    if zipfile.is_zipfile(args.source):
        if args.output is None:
            parser.error("an archive needs --output")
        transform_archive(args.source, args.output, theme)
        return 0
    text: str = args.source.read_bytes().decode("utf-8")
    result: str = transform_palette(text, theme is not None, theme or TINTED)
    if args.output is None:
        sys.stdout.buffer.write(result.encode("utf-8"))
    else:
        args.output.write_bytes(result.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
