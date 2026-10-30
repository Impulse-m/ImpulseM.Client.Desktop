"""Generates the monochrome ImpulseM marks that replace the paper plane in the intro screens.

The desktop client draws the intro cover plane and the QR screen centre as monochrome
icons. lib_ui reads the brightness of each pixel as the mask, not its alpha, so the files
are written as opaque RGBA: white is the shape, black is the background, and alpha is 255
everywhere. The cover plane is replaced by a disc with the pulse cut out of it, taken from
the master logo of the Android client. The cover keeps its four parts, so only the top part
carries the disc and the other three parts are written as empty black canvases of the same
size, which keeps the layout unchanged. The QR centre is the pulse alone, white on black,
with the geometry of the web client's padded logo: the QR screen paints the blue disc
itself, so the mask only has to supply the pulse on top of it.

Usage: python -I tools/impulse/brand/gen_intro_art.py --master <qr_logo.svg> --out-dir <icons dir>
"""
import argparse
import dataclasses
import struct
import sys
import zlib
from pathlib import Path
from typing import List, Optional, Tuple

# Isolated mode (python -I) leaves the script directory off sys.path, so add it explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_icons as gen  # noqa: E402


PNG_SIGNATURE: bytes = b"\x89PNG\r\n\x1a\n"
BLACK: gen.Rgb = (0, 0, 0)
SCALE_SUFFIXES: Tuple[Tuple[int, str], ...] = ((1, ""), (2, "@2x"), (3, "@3x"))
COVER_WIDTH: int = 202
COVER_HEIGHT: int = 143
COVER_DISC_FRACTION: float = 0.7
COVER_PARTS: Tuple[str, ...] = (
    "intro_plane_trace",
    "intro_plane_inner",
    "intro_plane_outer",
    "intro_plane_top",
)
COVER_DISC_PART: str = "intro_plane_top"
QR_NAME: str = "intro_qr_plane"
QR_SIZE: int = 44
# Follow the web client's logo_padded.svg: disc radius 84, pulse scale 0.655, stroke 16.
WEB_CENTER: float = 100.0
WEB_DISC_RADIUS: float = 84.0
WEB_PULSE_SCALE: float = 0.655
WEB_PULSE_STROKE: float = 16.0


def opaque_black(width: int, height: int) -> bytearray:
    canvas: bytearray = bytearray(width * height * 4)
    canvas[3::4] = b"\xff" * (width * height)
    return canvas


def brightness_mask(raw: bytes, size: int) -> bytes:
    out: bytearray = bytearray(size * size * 4)
    for slot in range(size * size):
        red: int = raw[slot * 4]
        alpha: int = raw[slot * 4 + 3]
        coverage: int = (alpha * red + 127) // 255
        out[slot * 4] = coverage
        out[slot * 4 + 1] = coverage
        out[slot * 4 + 2] = coverage
        out[slot * 4 + 3] = 255

    return bytes(out)


def render_mask(logo: gen.Logo, diameter: int) -> bytes:
    raw: bytes = gen.render_logo(logo, diameter, 0.0, gen.WHITE, BLACK)
    return brightness_mask(raw, diameter)


def pulse_logo(logo: gen.Logo) -> gen.Logo:
    points: Tuple[gen.Point, ...] = tuple(
        (
            WEB_CENTER + (x - WEB_CENTER) * WEB_PULSE_SCALE,
            WEB_CENTER + (y - WEB_CENTER) * WEB_PULSE_SCALE,
        )
        for x, y in logo.glyph_points
    )
    return dataclasses.replace(
        logo,
        disc_radius=WEB_DISC_RADIUS,
        glyph_points=points,
        glyph_width=WEB_PULSE_STROKE * WEB_PULSE_SCALE,
    )


def pulse_inset(view_size: float) -> float:
    # Negative inset: the web disc diameter (2 * WEB_DISC_RADIUS units) fills the whole canvas.
    return (1.0 - view_size / (2.0 * WEB_DISC_RADIUS)) / 2.0


def render_pulse_mask(logo: gen.Logo, size: int) -> bytes:
    raw: bytes = gen.render_logo(
        pulse_logo(logo),
        size,
        pulse_inset(logo.view_size),
        BLACK,
        gen.WHITE,
    )
    return brightness_mask(raw, size)


def place_centered(square: bytes, diameter: int, width: int, height: int) -> bytes:
    canvas: bytearray = opaque_black(width, height)
    left: int = (width - diameter) // 2
    top: int = (height - diameter) // 2
    stride: int = diameter * 4
    for row in range(diameter):
        source: int = row * stride
        target: int = ((top + row) * width + left) * 4
        canvas[target:target + stride] = square[source:source + stride]

    return bytes(canvas)


def encode_png(width: int, height: int, rgba: bytes) -> bytes:
    stride: int = width * 4
    raw: bytearray = bytearray()
    for row in range(height):
        raw.append(0)
        raw.extend(rgba[row * stride:(row + 1) * stride])

    header: bytes = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        PNG_SIGNATURE
        + gen.png_chunk(b"IHDR", header)
        + gen.png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + gen.png_chunk(b"IEND", b"")
    )


def generate(master_path: Path, out_dir: Path) -> List[Path]:
    logo: gen.Logo = gen.parse_logo(master_path.read_text(encoding="utf-8"))
    out_dir.mkdir(parents=True, exist_ok=True)
    written: List[Path] = []
    for scale, suffix in SCALE_SUFFIXES:
        cover_width: int = COVER_WIDTH * scale
        cover_height: int = COVER_HEIGHT * scale
        cover_diameter: int = round(cover_height * COVER_DISC_FRACTION)
        cover_disc: bytes = place_centered(
            render_mask(logo, cover_diameter),
            cover_diameter,
            cover_width,
            cover_height,
        )
        cover_empty: bytes = bytes(opaque_black(cover_width, cover_height))
        for part in COVER_PARTS:
            pixels: bytes = cover_disc if part == COVER_DISC_PART else cover_empty
            target: Path = out_dir / f"{part}{suffix}.png"
            target.write_bytes(encode_png(cover_width, cover_height, pixels))
            written.append(target)

        qr_size: int = QR_SIZE * scale
        qr_pixels: bytes = render_pulse_mask(logo, qr_size)
        qr_target: Path = out_dir / f"{QR_NAME}{suffix}.png"
        qr_target.write_bytes(encode_png(qr_size, qr_size, qr_pixels))
        written.append(qr_target)

    return written


def main(argv: Optional[List[str]] = None) -> int:
    parser: argparse.ArgumentParser = argparse.ArgumentParser(
        description="Generate the monochrome ImpulseM marks for the intro screens."
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
        help="icons directory that receives the generated files",
    )
    args: argparse.Namespace = parser.parse_args(argv)
    for path in generate(args.master, args.out_dir):
        print(path)

    return 0


if __name__ == "__main__":
    sys.exit(main())
