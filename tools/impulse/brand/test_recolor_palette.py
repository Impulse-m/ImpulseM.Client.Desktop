import struct
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path
from typing import List

# Isolated mode (python -I) leaves the script directory off sys.path, so add it explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import recolor_palette as recolor  # noqa: E402


HUE_TOLERANCE: float = 0.01


def hue_of(rgb: int) -> float:
    return recolor.hsv_of(rgb)[0]


class RecolorPaletteTest(unittest.TestCase):

    def test_day_primary_anchor_maps_exactly(self) -> None:
        self.assertEqual(
            "windowBgActive: #2085DF; // bright blue",
            recolor.transform_line("windowBgActive: #40a7e3; // bright blue", False),
        )

    def test_day_link_anchor_maps_exactly(self) -> None:
        self.assertEqual(
            "windowActiveTextFg: #106ACC; // online blue",
            recolor.transform_line("windowActiveTextFg: #168acd; // online blue", False),
        )

    def test_night_targets(self) -> None:
        self.assertEqual(
            "windowBgActive: #58AEEE; // bright blue",
            recolor.transform_line("windowBgActive: #40a7e3; // bright blue", True),
        )
        self.assertEqual(
            "windowActiveTextFg: #58AEEE; // online blue",
            recolor.transform_line("windowActiveTextFg: #168acd; // online blue", True),
        )

    def test_reference_line_is_kept(self) -> None:
        line: str = "activeButtonBg: windowBgActive; // default active button background"
        self.assertEqual(line, recolor.transform_line(line, False))

    def test_non_blue_is_kept(self) -> None:
        self.assertEqual("windowBg: #ffffff; // white", recolor.transform_line("windowBg: #ffffff; // white", False))
        self.assertEqual("windowSubTextFg: #999999;", recolor.transform_line("windowSubTextFg: #999999;", False))
        self.assertEqual("callArrowFg: #2dad2d | boxTextFgGood;", recolor.transform_line("callArrowFg: #2dad2d | boxTextFgGood;", False))

    def test_excluded_prefixes_are_kept(self) -> None:
        for line in [
            "premiumGradient: #40a7e3; // premium",
            "peerNameFg: #40a7e3; // peer",
            "giftBg: #40a7e3; // gift",
            "chat_inBg: #40a7e3; // chat in",
            "stories_Bg: #40a7e3;",
        ]:
            self.assertEqual(line, recolor.transform_line(line, False))

    def test_blue_is_shifted_into_brand_family(self) -> None:
        source: int = 0x39A5DB
        result: str = recolor.transform_line("activeButtonBgOver: #39a5db; // over", False)
        changed: int = int(result.split("#")[1][:6], 16)
        self.assertTrue(recolor.is_blue(changed))
        expected_hue: float = (hue_of(source) - hue_of(recolor.UPSTREAM_DAY_BLUE) + hue_of(recolor.PRIMARY_DAY)) % 1.0
        self.assertLess(abs(hue_of(changed) - expected_hue), HUE_TOLERANCE)

    def test_link_key_uses_link_target(self) -> None:
        result: str = recolor.transform_line("historyLinkInFg: #40a7e3;", False)
        changed: int = int(result.split("#")[1][:6], 16)
        expected_hue: float = (hue_of(recolor.UPSTREAM_DAY_BLUE) - hue_of(recolor.UPSTREAM_DAY_TEXT) + hue_of(recolor.LINK_DAY)) % 1.0
        self.assertLess(abs(hue_of(changed) - expected_hue), HUE_TOLERANCE)

    def test_alpha_pair_is_kept(self) -> None:
        self.assertEqual("activeLineFg: #2085DF18;", recolor.transform_line("activeLineFg: #40a7e318;", False))

    def test_fallback_after_bar_is_kept(self) -> None:
        self.assertEqual("x: #2085DF | windowFg;", recolor.transform_line("x: #40a7e3 | windowFg;", False))

    def test_palette_keeps_line_endings(self) -> None:
        text: str = "windowBgActive: #40a7e3;\r\nwindowBg: #ffffff;\r\n"
        result: str = recolor.transform_palette(text, False)
        self.assertEqual("windowBgActive: #2085DF;\r\nwindowBg: #ffffff;\r\n", result)

    def test_command_line_writes_output(self) -> None:
        lines: List[str] = [
            "windowBgActive: #40a7e3; // bright blue",
            "windowActiveTextFg: #168acd; // online blue",
            "windowBg: #ffffff; // white",
            "",
        ]
        with tempfile.TemporaryDirectory() as directory:
            source: Path = Path(directory) / "colors.palette"
            target: Path = Path(directory) / "out.palette"
            source.write_bytes("\n".join(lines).encode("utf-8"))
            self.assertEqual(0, recolor.main([str(source), "--output", str(target)]))
            written: str = target.read_bytes().decode("utf-8")
        self.assertEqual(
            "windowBgActive: #2085DF; // bright blue\nwindowActiveTextFg: #106ACC; // online blue\nwindowBg: #ffffff; // white\n",
            written,
        )


def png_rgb_of(data: bytes) -> int:
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    position: int = 8
    body: bytes = b""
    while position < len(data):
        length: int = struct.unpack(">I", data[position:position + 4])[0]
        kind: bytes = data[position + 4:position + 8]
        if kind == b"IDAT":
            body += data[position + 8:position + 8 + length]
        position += 12 + length
    raw: bytes = zlib.decompress(body)
    return (raw[1] << 16) | (raw[2] << 8) | raw[3]


class DarkThemeTest(unittest.TestCase):

    def test_night_surface_background_divider_are_exact(self) -> None:
        self.assertEqual("windowBg: #1D2128; // x", recolor.transform_line("windowBg: #282e33; // x", True, recolor.NIGHT))
        self.assertEqual("menuBg: #1D2128;", recolor.recolor_line("menuBg: #282e33;", recolor.NIGHT))
        self.assertEqual("topBarBg: #1D2128;", recolor.recolor_line("topBarBg: #282e33;", recolor.NIGHT))
        self.assertEqual("historyComposeAreaBg: #1D2128;", recolor.recolor_line("historyComposeAreaBg: #282e33;", recolor.NIGHT))
        self.assertEqual("boxSearchBg: #1D2128;", recolor.recolor_line("boxSearchBg: #282e33;", recolor.NIGHT))
        self.assertEqual("sideBarBg: #14181F;", recolor.recolor_line("sideBarBg: #18191d;", recolor.NIGHT))
        self.assertEqual("menuSeparatorFg: #0B0F15;", recolor.recolor_line("menuSeparatorFg: #42484d;", recolor.NIGHT))

    def test_tinted_surface_background_divider_are_exact(self) -> None:
        self.assertEqual("windowBg: #1D2733;", recolor.recolor_line("windowBg: #17212b;", recolor.TINTED))
        self.assertEqual("boxBg: #1D2733;", recolor.recolor_line("boxBg: #17212b;", recolor.TINTED))
        self.assertEqual("sideBarBg: #151E27;", recolor.recolor_line("sideBarBg: #0e1621;", recolor.TINTED))
        self.assertEqual("menuSeparatorFg: #0F151B;", recolor.recolor_line("menuSeparatorFg: #232f39;", recolor.TINTED))

    def test_references_are_kept(self) -> None:
        for line in ["boxBg: windowBg;", "dialogsBg: windowBg;", "mainMenuBg: windowBg; // ref"]:
            self.assertEqual(line, recolor.recolor_line(line, recolor.NIGHT))
            self.assertEqual(line, recolor.recolor_line(line, recolor.TINTED))

    def test_tint_moves_dark_colour_by_surface_delta(self) -> None:
        # Night: web #1D2128 minus Telegram #282E33 is (-11, -13, -11); contactsBg #222528 becomes #17181D.
        self.assertEqual("contactsBg: #17181D;", recolor.recolor_line("contactsBg: #222528;", recolor.NIGHT))
        # Tinted: web #1D2733 minus Telegram #17212B is (+6, +6, +8); contactsBg #17212b becomes #1D2733.
        self.assertEqual("contactsBg: #1D2733;", recolor.recolor_line("contactsBg: #17212b;", recolor.TINTED))

    def test_tint_clamps_to_channel_range(self) -> None:
        self.assertEqual("callBg: #000000;", recolor.recolor_line("callBg: #000000;", recolor.NIGHT))

    def test_dark_value_gate_boundary(self) -> None:
        # value 89/255 = 0.349 is dark, value 90/255 = 0.353 is not.
        self.assertTrue(recolor.is_dark(0x595959))
        self.assertFalse(recolor.is_dark(0x5A5A5A))
        self.assertEqual("x: #4E4C4E;", recolor.recolor_line("x: #595959;", recolor.NIGHT))
        self.assertEqual("x: #5A5A5A;", recolor.recolor_line("x: #5A5A5A;", recolor.NIGHT))

    def test_dark_gate_ignores_saturation(self) -> None:
        self.assertTrue(recolor.is_dark(0x001A33))
        self.assertTrue(recolor.is_dark(0x0E1621))
        # Tinted blue-grey keys with saturation near 0.5 move too.
        self.assertEqual("lightButtonBg: #1D2733;", recolor.recolor_line("lightButtonBg: #17212b;", recolor.TINTED))

    def test_bright_colours_are_not_tinted(self) -> None:
        self.assertEqual("historyTextOutFg: #e4ecf2;", recolor.recolor_line("historyTextOutFg: #e4ecf2;", recolor.NIGHT))

    def test_alpha_is_kept_on_dark_colour(self) -> None:
        self.assertEqual("radialBg: #10121Acc;", recolor.recolor_line("radialBg: #1b1f25cc;", recolor.NIGHT))

    def test_bubbles_are_not_tinted(self) -> None:
        for line in ["msgInBg: #33393f;", "msgOutBg: #2a2f33;", "msgServiceBg: #363c43c8;", "msgInShadow: #748ea200;"]:
            self.assertEqual(line, recolor.recolor_line(line, recolor.NIGHT))

    def test_excluded_dark_keys_are_kept(self) -> None:
        self.assertEqual("peerNameFg: #222528;", recolor.recolor_line("peerNameFg: #222528;", recolor.NIGHT))

    def test_tinted_bubble_blues_keep_the_blue_rule(self) -> None:
        # 1b67cb5d84 shifted these bubble blues; the blue rule is unchanged for Tinted.
        self.assertEqual(
            recolor.transform_line("msgInBgSelected: #2e70a5;", True),
            recolor.recolor_line("msgInBgSelected: #2e70a5;", recolor.TINTED),
        )
        self.assertEqual("msgInBgSelected: #2B92CB;", recolor.recolor_line("msgInBgSelected: #2e70a5;", recolor.TINTED))

    def test_teal_moves_to_brand_blue_for_night(self) -> None:
        self.assertEqual("windowBgActive: #58AEEE;", recolor.recolor_line("windowBgActive: #3fc1b0;", recolor.NIGHT))
        self.assertEqual("windowActiveTextFg: #58AEEE;", recolor.recolor_line("windowActiveTextFg: #4be1c3;", recolor.NIGHT))
        result: str = recolor.recolor_line("activeButtonBg: #2da192;", recolor.NIGHT)
        changed: int = int(result.split("#")[1][:6], 16)
        expected_hue: float = (recolor.hsv_of(0x2DA192)[0] - recolor.hsv_of(recolor.UPSTREAM_TEAL)[0] + recolor.hsv_of(recolor.PRIMARY_NIGHT)[0]) % 1.0
        self.assertTrue(recolor.is_blue(changed))
        self.assertLess(abs(recolor.hsv_of(changed)[0] - expected_hue), HUE_TOLERANCE)

    def test_teal_gate_boundaries(self) -> None:
        self.assertTrue(recolor.is_teal(0x2DA192))
        # Saturated teal with value 0.37 is below the teal value gate (0.40) and above the dark gate (0.35): kept.
        self.assertFalse(recolor.is_teal(0x0A5E5A))
        self.assertFalse(recolor.is_dark(0x0A5E5A))
        self.assertEqual("x: #0A5E5A;", recolor.recolor_line("x: #0A5E5A;", recolor.NIGHT))
        # Hue 207 degrees is the blue range, not teal.
        self.assertFalse(recolor.is_teal(0x2F7FBF))

    def test_teal_is_not_applied_to_tinted(self) -> None:
        self.assertEqual("activeButtonBg: #2da192;", recolor.recolor_line("activeButtonBg: #2da192;", recolor.TINTED))

    def test_archive_writes_background_png_and_keeps_entries(self) -> None:
        names: List[str] = ["background.png", "colors.tdesktop-theme", "other.bin"]
        palette: bytes = b"windowBg: #282e33;\nsideBarBg: #18191d;\n"
        with tempfile.TemporaryDirectory() as directory:
            source: Path = Path(directory) / "src.tdesktop-theme"
            target: Path = Path(directory) / "out.tdesktop-theme"
            with zipfile.ZipFile(source, "w") as writer:
                writer.writestr("background.png", recolor.png_of(0x18191D), compress_type=zipfile.ZIP_STORED)
                writer.writestr("colors.tdesktop-theme", palette)
                writer.writestr("other.bin", b"\x00\x01\x02")
            recolor.transform_archive(source, target, recolor.NIGHT)
            with zipfile.ZipFile(target) as reader:
                self.assertEqual(names, [info.filename for info in reader.infolist()])
                self.assertEqual(zipfile.ZIP_DEFLATED, reader.infolist()[0].compress_type)
                self.assertEqual(0x14181F, png_rgb_of(reader.read("background.png")))
                self.assertEqual(
                    b"windowBg: #1D2128;\nsideBarBg: #14181F;\n",
                    reader.read("colors.tdesktop-theme"),
                )
                self.assertEqual(b"\x00\x01\x02", reader.read("other.bin"))

    def test_archive_without_png_keeps_background_entry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source: Path = Path(directory) / "src.tdesktop-theme"
            target: Path = Path(directory) / "out.tdesktop-theme"
            with zipfile.ZipFile(source, "w") as writer:
                writer.writestr("colors.tdesktop-theme", b"windowBg: #17212b;\n")
                writer.writestr("background.jpg", b"\xff\xd8\xff\xe0")
            recolor.transform_archive(source, target, recolor.TINTED)
            with zipfile.ZipFile(target) as reader:
                self.assertEqual(b"\xff\xd8\xff\xe0", reader.read("background.jpg"))
                self.assertEqual(b"windowBg: #1D2733;\n", reader.read("colors.tdesktop-theme"))

    def test_rerun_on_same_upstream_gives_same_bytes(self) -> None:
        # The output is computed from the upstream file, so a second run on that file writes the same bytes.
        # Feeding the output back in is not part of this property: the docstring says it shifts colours again.
        lines: List[str] = [
            "windowBg: #282e33; // surface",
            "boxBg: #282e33;",
            "boxSearchBg: #282e33;",
            "menuBg: #282e33;",
            "topBarBg: #282e33;",
            "historyComposeAreaBg: #282e33;",
            "sideBarBg: #18191d;",
            "menuSeparatorFg: #42484d;",
            "windowBgActive: #3fc1b0; // teal accent",
            "activeButtonBg: #2da192;",
            "contactsBg: #222528;",
            "historyTextOutFg: #e4ecf2; // light",
            "",
        ]
        with tempfile.TemporaryDirectory() as directory:
            source: Path = Path(directory) / "colors.palette"
            first: Path = Path(directory) / "first.palette"
            second: Path = Path(directory) / "second.palette"
            source.write_bytes("\n".join(lines).encode("utf-8"))
            upstream_before: bytes = source.read_bytes()
            self.assertEqual(0, recolor.main([str(source), "--theme", "night", "--output", str(first)]))
            self.assertEqual(0, recolor.main([str(source), "--theme", "night", "--output", str(second)]))
            self.assertEqual(upstream_before, source.read_bytes())
            first_bytes: bytes = first.read_bytes()
            second_bytes: bytes = second.read_bytes()
        self.assertEqual(first_bytes, second_bytes)
        # The first run really recoloured the input: surfaces, teal accent and contacts moved, the light colour did not.
        first_text: str = first_bytes.decode("utf-8")
        self.assertIn("windowBg: #1D2128; // surface\n", first_text)
        self.assertIn("sideBarBg: #14181F;\n", first_text)
        self.assertIn("menuSeparatorFg: #0B0F15;\n", first_text)
        self.assertIn("windowBgActive: #58AEEE; // teal accent\n", first_text)
        self.assertIn("activeButtonBg: #418DC7;\n", first_text)
        self.assertIn("contactsBg: #17181D;\n", first_text)
        self.assertIn("historyTextOutFg: #e4ecf2; // light\n", first_text)


if __name__ == "__main__":
    unittest.main()
