"""Generates the monochrome ImpulseM marks that replace the paper plane in the intro screens.

The desktop client draws the intro cover plane and the QR screen centre as monochrome
icons. lib_ui reads the brightness of each pixel as the mask, not its alpha, so the files
are written as opaque RGBA: white is the shape, black is the background, and alpha is 255
everywhere. The plane is replaced by a disc with the pulse cut out of it, taken from the
same master logo as the icon set. The cover keeps its four parts, so only the top part
carries the disc and the other three parts are written as empty black canvases of the same
size, which keeps the layout unchanged.

Usage: python -I tools/impulse/brand/gen_intro_art.py --master <qr_logo.svg> --out-dir <icons dir>
"""
import argparse
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
QR_SIZE: int = 35
QR_DISC_FRACTION: float = 0.9


def opaque_black(width: int, height: int) -> bytearray:
    canvas: bytearray = bytearray(width * height * 4)
    canvas[3::4] = b"\xff" * (width * height)
    return canvas


def render_mask(logo: gen.Logo, diameter: int) -> bytes:
    raw: bytes = gen.render_logo(logo, diameter, 0.0, gen.WHITE, BLACK)
    out: bytearray = bytearray(diameter * diameter * 4)
    for slot in range(diameter * diameter):
        red: int = raw[slot * 4]
        alpha: int = raw[slot * 4 + 3]
        coverage: int = (alpha * red + 127) // 255
        out[slot * 4] = coverage
        out[slot * 4 + 1] = coverage
        out[slot * 4 + 2] = coverage
        out[slot * 4 + 3] = 255

    return bytes(out)


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
        qr_diameter: int = round(qr_size * QR_DISC_FRACTION)
        qr_pixels: bytes = place_centered(
            render_mask(logo, qr_diameter),
            qr_diameter,
            qr_size,
            qr_size,
        )
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
