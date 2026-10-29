"""Generates the desktop icon set from the ImpulseM master logo.

The master is the 200 x 200 SVG of the Android client: a blue disc with a white pulse
stroke. The script reads the disc and the pulse from the master, renders them with an
anti-aliased rasteriser that uses only the standard library, and writes the PNG set, the
Windows ICO and the tray SVG sources of the desktop client.

Usage: python -I tools/impulse/brand/gen_icons.py --master <qr_logo.svg> --out-dir <dir>
"""
import argparse
import math
import re
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


Rgb = Tuple[int, int, int]
Point = Tuple[float, float]

WHITE_ON_BLUE: str = "white_on_blue"
BLUE_ON_WHITE: str = "blue_on_white"
WHITE: Rgb = (255, 255, 255)
PNG_SIGNATURE: bytes = b"\x89PNG\r\n\x1a\n"
PNG_SIZES: Tuple[int, ...] = (16, 32, 48, 64, 128, 256, 512)
ICO_SIZES: Tuple[int, ...] = (16, 20, 24, 32, 40, 48, 64, 256)
LOGO_INSET: float = 0.04
ICO_NAME: str = "icon256.ico"
TRAY_NAMES: Dict[str, str] = {
    WHITE_ON_BLUE: "tray_white_on_blue.svg",
    BLUE_ON_WHITE: "tray_blue_on_white.svg",
}

ATTRIBUTE_RE: re.Pattern[str] = re.compile(r'([A-Za-z][\w:-]*)\s*=\s*"([^"]*)"')
PATH_TOKEN_RE: re.Pattern[str] = re.compile(
    r"[A-Za-z]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"
)
PATH_COMMANDS: Tuple[str, ...] = ("M", "L", "H", "V")


@dataclass(frozen=True)
class Logo:
    view_size: float
    disc_x: float
    disc_y: float
    disc_radius: float
    disc_color: Rgb
    glyph_points: Tuple[Point, ...]
    glyph_width: float
    glyph_color: Rgb


def parse_color(text: str) -> Rgb:
    value: str = text.strip()
    if len(value) != 7 or not value.startswith("#"):
        raise ValueError(f"unsupported color {text!r}, expected #RRGGBB")

    number: int = int(value[1:], 16)
    red: int = (number >> 16) & 0xFF
    green: int = (number >> 8) & 0xFF
    blue: int = number & 0xFF
    return (red, green, blue)


def to_hex(color: Rgb) -> str:
    return f"#{color[0]:02X}{color[1]:02X}{color[2]:02X}"


def find_tag(text: str, name: str) -> str:
    match: Optional[re.Match[str]] = re.search(rf"<{name}\b[^>]*>", text)
    if match is None:
        raise ValueError(f"master SVG has no <{name}> element")

    return match.group(0)


def read_attributes(tag: str) -> Dict[str, str]:
    return {name: value for name, value in ATTRIBUTE_RE.findall(tag)}


def parse_path_points(data: str) -> List[Point]:
    tokens: List[str] = PATH_TOKEN_RE.findall(data)
    points: List[Point] = []
    command: str = ""
    current_x: float = 0.0
    current_y: float = 0.0
    index: int = 0
    while index < len(tokens):
        token: str = tokens[index]
        if token.isalpha():
            if token not in PATH_COMMANDS:
                raise ValueError(f"unsupported path command {token!r}")

            command = token
            index += 1
            continue

        if command == "H":
            current_x = float(token)
            index += 1
        elif command == "V":
            current_y = float(token)
            index += 1
        elif command in ("M", "L"):
            if index + 1 >= len(tokens):
                raise ValueError("path coordinate pair is incomplete")

            current_x = float(token)
            current_y = float(tokens[index + 1])
            index += 2
            if command == "M":
                command = "L"
        else:
            raise ValueError("path data must start with a command")

        points.append((current_x, current_y))

    return points


def parse_logo(text: str) -> Logo:
    root: Dict[str, str] = read_attributes(find_tag(text, "svg"))
    view_box: List[float] = [float(item) for item in root["viewBox"].split()]
    if len(view_box) != 4 or view_box[2] != view_box[3]:
        raise ValueError("master SVG viewBox must be square")

    circle: Dict[str, str] = read_attributes(find_tag(text, "circle"))
    path: Dict[str, str] = read_attributes(find_tag(text, "path"))
    if path.get("stroke-linecap") != "round" or path.get("stroke-linejoin") != "round":
        raise ValueError("master pulse must use round caps and joins")

    return Logo(
        view_size=view_box[2],
        disc_x=float(circle["cx"]),
        disc_y=float(circle["cy"]),
        disc_radius=float(circle["r"]),
        disc_color=parse_color(circle["fill"]),
        glyph_points=tuple(parse_path_points(path["d"])),
        glyph_width=float(path["stroke-width"]),
        glyph_color=parse_color(path["stroke"]),
    )


def palette(logo: Logo, variant: str) -> Tuple[Rgb, Rgb]:
    if variant == WHITE_ON_BLUE:
        return (logo.disc_color, logo.glyph_color)

    if variant == BLUE_ON_WHITE:
        return (WHITE, logo.disc_color)

    raise ValueError(f"unknown variant {variant!r}")


def clamp_unit(value: float) -> float:
    if value < 0.0:
        return 0.0

    if value > 1.0:
        return 1.0

    return value


def mix_channel(base: int, top: int, amount: float) -> int:
    return int(round(base + (top - base) * amount))


def render_logo(
        logo: Logo,
        size: int,
        inset: float,
        disc_color: Rgb,
        glyph_color: Rgb,
) -> bytes:
    scale: float = size * (1.0 - 2.0 * inset) / logo.view_size
    offset: float = size * inset
    center_x: float = offset + logo.disc_x * scale
    center_y: float = offset + logo.disc_y * scale
    disc_radius: float = logo.disc_radius * scale
    half_width: float = logo.glyph_width * scale / 2.0
    points: List[Point] = [
        (offset + x * scale, offset + y * scale) for x, y in logo.glyph_points
    ]

    distance: List[float] = [math.inf] * (size * size)
    reach: float = half_width + 1.0
    for index in range(len(points) - 1):
        ax: float = points[index][0]
        ay: float = points[index][1]
        bx: float = points[index + 1][0]
        by: float = points[index + 1][1]
        dx: float = bx - ax
        dy: float = by - ay
        length_sq: float = dx * dx + dy * dy
        x0: int = max(0, math.floor(min(ax, bx) - reach))
        x1: int = min(size - 1, math.ceil(max(ax, bx) + reach))
        y0: int = max(0, math.floor(min(ay, by) - reach))
        y1: int = min(size - 1, math.ceil(max(ay, by) + reach))
        for py in range(y0, y1 + 1):
            cy: float = py + 0.5
            for px in range(x0, x1 + 1):
                cx: float = px + 0.5
                t: float = 0.0
                if length_sq > 0.0:
                    t = clamp_unit(((cx - ax) * dx + (cy - ay) * dy) / length_sq)

                qx: float = ax + t * dx - cx
                qy: float = ay + t * dy - cy
                d: float = math.sqrt(qx * qx + qy * qy)
                slot: int = py * size + px
                if d < distance[slot]:
                    distance[slot] = d

    out: bytearray = bytearray(size * size * 4)
    for py in range(size):
        for px in range(size):
            slot: int = py * size + px
            disc_alpha: float = clamp_unit(
                disc_radius - math.hypot(px + 0.5 - center_x, py + 0.5 - center_y) + 0.5
            )
            if disc_alpha <= 0.0:
                continue

            glyph_alpha: float = clamp_unit(half_width - distance[slot] + 0.5)
            out[slot * 4] = mix_channel(disc_color[0], glyph_color[0], glyph_alpha)
            out[slot * 4 + 1] = mix_channel(disc_color[1], glyph_color[1], glyph_alpha)
            out[slot * 4 + 2] = mix_channel(disc_color[2], glyph_color[2], glyph_alpha)
            out[slot * 4 + 3] = int(round(disc_alpha * 255))

    return bytes(out)


def png_chunk(tag: bytes, body: bytes) -> bytes:
    crc: int = zlib.crc32(tag + body) & 0xFFFFFFFF
    return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", crc)


def encode_png(size: int, rgba: bytes) -> bytes:
    stride: int = size * 4
    raw: bytearray = bytearray()
    for row in range(size):
        raw.append(0)
        raw.extend(rgba[row * stride:(row + 1) * stride])

    header: bytes = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (
        PNG_SIGNATURE
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + png_chunk(b"IEND", b"")
    )


def encode_ico(entries: List[Tuple[int, bytes]]) -> bytes:
    header: bytes = struct.pack("<HHH", 0, 1, len(entries))
    directory: bytearray = bytearray()
    payload: bytearray = bytearray()
    offset: int = 6 + 16 * len(entries)
    for size, png in entries:
        dimension: int = 0 if size >= 256 else size
        directory.extend(
            struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(png), offset)
        )
        payload.extend(png)
        offset += len(png)

    return header + bytes(directory) + bytes(payload)


def tray_svg(logo: Logo, variant: str) -> str:
    disc_color, glyph_color = palette(logo, variant)
    path_data: str = "M" + " L".join(f"{x:g},{y:g}" for x, y in logo.glyph_points)
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{logo.view_size:g}" '
        f'height="{logo.view_size:g}" viewBox="0 0 {logo.view_size:g} {logo.view_size:g}">'
        f'<circle cx="{logo.disc_x:g}" cy="{logo.disc_y:g}" r="{logo.disc_radius:g}" '
        f'fill="{to_hex(disc_color)}"/>'
        f'<path d="{path_data}" fill="none" stroke="{to_hex(glyph_color)}" '
        f'stroke-width="{logo.glyph_width:g}" stroke-linecap="round" stroke-linejoin="round"/>'
        f"</svg>"
    )


def png_plan() -> List[Tuple[str, int, float, str]]:
    plan: List[Tuple[str, int, float, str]] = []
    for base in PNG_SIZES:
        plan.append((f"icon{base}.png", base, 0.0, WHITE_ON_BLUE))
        plan.append((f"icon{base}@2x.png", base * 2, 0.0, WHITE_ON_BLUE))

    plan.append(("icon_round512@2x.png", 1024, 0.0, WHITE_ON_BLUE))
    plan.append(("logo_256.png", 256, LOGO_INSET, WHITE_ON_BLUE))
    plan.append(("logo_256_no_margin.png", 256, 0.0, WHITE_ON_BLUE))
    plan.append(("icon_green.png", 1024, 0.0, BLUE_ON_WHITE))
    plan.append(("iconbig_green.png", 256, 0.0, BLUE_ON_WHITE))
    return plan


def render_cached(
        logo: Logo,
        size: int,
        inset: float,
        variant: str,
        cache: Dict[Tuple[int, float, str], bytes],
) -> bytes:
    key: Tuple[int, float, str] = (size, inset, variant)
    if key not in cache:
        disc_color, glyph_color = palette(logo, variant)
        cache[key] = render_logo(logo, size, inset, disc_color, glyph_color)

    return cache[key]


def generate(master_path: Path, out_dir: Path) -> List[Path]:
    logo: Logo = parse_logo(master_path.read_text(encoding="utf-8"))
    out_dir.mkdir(parents=True, exist_ok=True)
    cache: Dict[Tuple[int, float, str], bytes] = {}
    written: List[Path] = []

    for name, size, inset, variant in png_plan():
        pixels: bytes = render_cached(logo, size, inset, variant, cache)
        target: Path = out_dir / name
        target.write_bytes(encode_png(size, pixels))
        written.append(target)

    ico_entries: List[Tuple[int, bytes]] = []
    for size in ICO_SIZES:
        pixels = render_cached(logo, size, 0.0, WHITE_ON_BLUE, cache)
        ico_entries.append((size, encode_png(size, pixels)))

    ico_path: Path = out_dir / ICO_NAME
    ico_path.write_bytes(encode_ico(ico_entries))
    written.append(ico_path)

    for variant, name in TRAY_NAMES.items():
        tray_path: Path = out_dir / name
        tray_path.write_text(tray_svg(logo, variant), encoding="utf-8")
        written.append(tray_path)

    return written


def main(argv: Optional[List[str]] = None) -> int:
    parser: argparse.ArgumentParser = argparse.ArgumentParser(
        description="Generate the desktop icon set from the ImpulseM master logo."
    )
    parser.add_argument(
        "--master",
        required=True,
        type=Path,
        help="path to the master qr_logo.svg",
    )
    parser.add_argument(
        "--out-dir",
        required=True,
        type=Path,
        help="directory that receives the generated files",
    )
    args: argparse.Namespace = parser.parse_args(argv)
    for path in generate(args.master, args.out_dir):
        print(path)

    return 0


if __name__ == "__main__":
    sys.exit(main())
