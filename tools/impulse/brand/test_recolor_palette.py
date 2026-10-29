import sys
import tempfile
import unittest
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


if __name__ == "__main__":
    unittest.main()
