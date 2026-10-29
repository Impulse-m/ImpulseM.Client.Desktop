import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from typing import Dict, List, Tuple

# Isolated mode (python -I) leaves the script directory off sys.path, so add it explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_icons as gen  # noqa: E402


MASTER_SVG: str = (
    '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">'
    '<circle cx="100" cy="100" r="100" fill="#2085DF"/>'
    '<path d="M22 100 H64 L74 100 L84 70 L98 146 L112 44 L126 100 L136 100 H178" '
    'fill="none" stroke="#FFFFFF" stroke-width="14" stroke-linecap="round" '
    'stroke-linejoin="round"/></svg>'
)

EXPECTED_PNG_SIZES: Dict[str, int] = {
    "icon16.png": 16,
    "icon16@2x.png": 32,
    "icon32.png": 32,
    "icon32@2x.png": 64,
    "icon48.png": 48,
    "icon48@2x.png": 96,
    "icon64.png": 64,
    "icon64@2x.png": 128,
    "icon128.png": 128,
    "icon128@2x.png": 256,
    "icon256.png": 256,
    "icon256@2x.png": 512,
    "icon512.png": 512,
    "icon512@2x.png": 1024,
    "icon_round512@2x.png": 1024,
    "logo_256.png": 256,
    "logo_256_no_margin.png": 256,
    "icon_green.png": 1024,
    "iconbig_green.png": 256,
}
EXPECTED_ICO_SIZES: List[int] = [16, 20, 24, 32, 40, 48, 64, 256]
EXPECTED_TRAY_NAMES: List[str] = [
    "tray_white_on_blue.svg",
    "tray_blue_on_white.svg",
    "tray_monochrome.svg",
]


def read_png(data: bytes) -> Tuple[int, int, bytes]:
    if data[:8] != gen.PNG_SIGNATURE:
        raise ValueError("not a PNG file")

    position: int = 8
    width: int = 0
    height: int = 0
    compressed: bytearray = bytearray()
    while position < len(data):
        length: int = struct.unpack(">I", data[position:position + 4])[0]
        tag: bytes = data[position + 4:position + 8]
        body: bytes = data[position + 8:position + 8 + length]
        position += 12 + length
        if tag == b"IHDR":
            fields: Tuple[int, ...] = struct.unpack(">IIBBBBB", body)
            width = fields[0]
            height = fields[1]
            if fields[2] != 8 or fields[3] != 6:
                raise ValueError("expected 8-bit RGBA")

        elif tag == b"IDAT":
            compressed.extend(body)

    stride: int = width * 4
    raw: bytes = zlib.decompress(bytes(compressed))
    pixels: bytearray = bytearray()
    for row in range(height):
        start: int = row * (stride + 1)
        if raw[start] != 0:
            raise ValueError("test reader supports filter type 0 only")

        pixels.extend(raw[start + 1:start + 1 + stride])

    return (width, height, bytes(pixels))


def pixel_at(width: int, pixels: bytes, x: int, y: int) -> Tuple[int, int, int, int]:
    offset: int = (y * width + x) * 4
    return (pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3])


class GenIconsTest(unittest.TestCase):
    temp_dir: tempfile.TemporaryDirectory[str]
    out_dir: Path
    written: List[Path]

    @classmethod
    def setUpClass(cls) -> None:
        cls.temp_dir = tempfile.TemporaryDirectory()
        cls.out_dir = Path(cls.temp_dir.name)
        master: Path = cls.out_dir / "master.svg"
        master.write_text(MASTER_SVG, encoding="utf-8")
        cls.written = gen.generate(master, cls.out_dir / "icons")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp_dir.cleanup()

    def icon_path(self, name: str) -> Path:
        return self.out_dir / "icons" / name

    def test_parse_master_geometry(self) -> None:
        logo: gen.Logo = gen.parse_logo(MASTER_SVG)
        self.assertEqual(logo.view_size, 200.0)
        self.assertEqual(logo.disc_radius, 100.0)
        self.assertEqual(logo.disc_color, (0x20, 0x85, 0xDF))
        self.assertEqual(logo.glyph_color, (255, 255, 255))
        self.assertEqual(logo.glyph_width, 14.0)
        self.assertEqual(len(logo.glyph_points), 9)
        self.assertEqual(logo.glyph_points[0], (22.0, 100.0))
        self.assertEqual(logo.glyph_points[-1], (178.0, 100.0))

    def test_every_output_is_written(self) -> None:
        names: List[str] = sorted(path.name for path in self.written)
        expected: List[str] = sorted(
            list(EXPECTED_PNG_SIZES.keys()) + ["icon256.ico"] + EXPECTED_TRAY_NAMES
        )
        self.assertEqual(names, expected)
        for path in self.written:
            self.assertTrue(path.is_file(), str(path))

    def test_png_header_sizes(self) -> None:
        for name, size in EXPECTED_PNG_SIZES.items():
            data: bytes = self.icon_path(name).read_bytes()
            width, height, _ = read_png(data)
            self.assertEqual((width, height), (size, size), name)

    def test_ico_entry_count_and_sizes(self) -> None:
        data: bytes = self.icon_path("icon256.ico").read_bytes()
        reserved, kind, count = struct.unpack("<HHH", data[:6])
        self.assertEqual(reserved, 0)
        self.assertEqual(kind, 1)
        self.assertEqual(count, len(EXPECTED_ICO_SIZES))

        sizes: List[int] = []
        for index in range(count):
            entry: bytes = data[6 + 16 * index:6 + 16 * (index + 1)]
            width, height = entry[0], entry[1]
            size_bytes, offset = struct.unpack("<II", entry[8:16])
            payload: bytes = data[offset:offset + size_bytes]
            self.assertEqual(payload[:8], gen.PNG_SIGNATURE)
            png_width, png_height, _ = read_png(payload)
            actual: int = 256 if width == 0 else width
            self.assertEqual(height, width)
            self.assertEqual((png_width, png_height), (actual, actual))
            sizes.append(actual)

        self.assertEqual(sizes, EXPECTED_ICO_SIZES)

    def test_launcher_logo_has_four_percent_inset(self) -> None:
        width, height, inset_pixels = read_png(self.icon_path("logo_256.png").read_bytes())
        full_width, full_height, full_pixels = read_png(
            self.icon_path("logo_256_no_margin.png").read_bytes()
        )
        self.assertEqual((height, full_width, full_height), (256, 256, 256))
        self.assertEqual(pixel_at(width, inset_pixels, 0, 0)[3], 0)
        self.assertEqual(pixel_at(width, inset_pixels, 2, 128)[3], 0)
        self.assertEqual(pixel_at(full_width, full_pixels, 2, 128)[3], 255)
        self.assertEqual(pixel_at(full_width, full_pixels, 128, 8)[:3], (0x20, 0x85, 0xDF))
        self.assertEqual(pixel_at(full_width, full_pixels, 128, 8)[3], 255)

    def test_tray_svg_colours(self) -> None:
        white_on_blue: str = self.icon_path("tray_white_on_blue.svg").read_text(encoding="utf-8")
        blue_on_white: str = self.icon_path("tray_blue_on_white.svg").read_text(encoding="utf-8")
        self.assertIn('fill="#2085DF"', white_on_blue)
        self.assertIn('stroke="#FFFFFF"', white_on_blue)
        self.assertIn('fill="#FFFFFF"', blue_on_white)
        self.assertIn('stroke="#2085DF"', blue_on_white)

    def test_tray_mask_is_glyph_only(self) -> None:
        mask: str = self.icon_path("tray_monochrome.svg").read_text(encoding="utf-8")
        self.assertNotIn("<circle", mask)
        self.assertIn('stroke="#000000"', mask)
        self.assertIn('stroke-linecap="round"', mask)


if __name__ == "__main__":
    unittest.main()
