import struct
import sys
import tempfile
import unittest
from pathlib import Path
from typing import List, Tuple

# Isolated mode (python -I) leaves the script directory off sys.path, so add it explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_icons as gen  # noqa: E402
import gen_intro_art as art  # noqa: E402


MASTER_SVG: str = (
    '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">'
    '<circle cx="100" cy="100" r="100" fill="#2085DF"/>'
    '<path d="M22 100 H64 L74 100 L84 70 L98 146 L112 44 L126 100 L136 100 H178" '
    'fill="none" stroke="#FFFFFF" stroke-width="14" stroke-linecap="round" '
    'stroke-linejoin="round"/></svg>'
)
QR_SIZES: Tuple[int, ...] = (44, 88, 132)


def white_extent(size: int, pixels: bytes) -> Tuple[int, int]:
    left: int = size
    right: int = -1
    for y in range(size):
        for x in range(size):
            if pixels[(y * size + x) * 4] >= 128:
                left = min(left, x)
                right = max(right, x)

    return (left, right)


class GenIntroArtTest(unittest.TestCase):
    temp_dir: tempfile.TemporaryDirectory[str]
    out_dir: Path
    logo: gen.Logo

    @classmethod
    def setUpClass(cls) -> None:
        cls.temp_dir = tempfile.TemporaryDirectory()
        cls.out_dir = Path(cls.temp_dir.name)
        cls.logo = gen.parse_logo(MASTER_SVG)
        master: Path = cls.out_dir / "master.svg"
        master.write_text(MASTER_SVG, encoding="utf-8")
        art.generate(master, cls.out_dir / "icons")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp_dir.cleanup()

    def test_pulse_geometry_follows_web_logo(self) -> None:
        pulse: gen.Logo = art.pulse_logo(self.logo)
        self.assertEqual(pulse.disc_radius, 84.0)
        self.assertAlmostEqual(pulse.glyph_width, 16.0 * 0.655)
        self.assertAlmostEqual(pulse.glyph_points[0][0], 100.0 + (22.0 - 100.0) * 0.655)
        self.assertAlmostEqual(pulse.glyph_points[-1][0], 100.0 + (178.0 - 100.0) * 0.655)
        self.assertAlmostEqual(pulse.glyph_points[0][1], 100.0)

    def test_qr_mask_is_opaque_grey(self) -> None:
        for size in QR_SIZES:
            pixels: bytes = art.render_pulse_mask(self.logo, size)
            self.assertEqual(len(pixels), size * size * 4, size)
            for slot in range(size * size):
                red: int = pixels[slot * 4]
                self.assertEqual(pixels[slot * 4 + 1], red, size)
                self.assertEqual(pixels[slot * 4 + 2], red, size)
                self.assertEqual(pixels[slot * 4 + 3], 255, size)

    def test_qr_corners_are_black_and_pulse_is_white(self) -> None:
        for size in QR_SIZES:
            pixels: bytes = art.render_pulse_mask(self.logo, size)
            self.assertEqual(pixels[0:4], bytes((0, 0, 0, 255)), size)
            left, right = white_extent(size, pixels)
            self.assertGreater(right, left, size)

    def test_qr_pulse_is_horizontally_centred(self) -> None:
        for size in QR_SIZES:
            pixels: bytes = art.render_pulse_mask(self.logo, size)
            left, right = white_extent(size, pixels)
            right_margin: int = size - 1 - right
            self.assertLessEqual(abs(left - right_margin), 1, size)

    def test_qr_files_have_circle_sizes(self) -> None:
        for suffix, size in (("", 44), ("@2x", 88), ("@3x", 132)):
            data: bytes = (self.out_dir / "icons" / f"intro_qr_plane{suffix}.png").read_bytes()
            width: int
            height: int
            width, height = struct.unpack(">II", data[16:24])
            self.assertEqual((width, height), (size, size), suffix)


if __name__ == "__main__":
    unittest.main()
