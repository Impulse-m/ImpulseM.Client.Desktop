import re
import unittest
from pathlib import Path
from typing import List, Set

import hidden_list

TOOLS_DIR: Path = Path(__file__).resolve().parent
ROOT: Path = TOOLS_DIR.parent.parent.parent
SCHEME_PATH: Path = ROOT / "Telegram" / "SourceFiles" / "mtproto" / "scheme" / "api.tl"
FUNCTION_RE: re.Pattern = re.compile(r"^([A-Za-z_][\w.]*)#[0-9a-f]+\s")
FUNCTIONS_MARKER: str = "---functions---"
LAYER_232_FUNCTION_COUNT: int = 859


def scheme_method_names() -> List[str]:
    names: List[str] = []
    inFunctions: bool = False
    with SCHEME_PATH.open(encoding="utf-8") as scheme:
        for line in scheme:
            text: str = line.strip()
            if text == FUNCTIONS_MARKER:
                inFunctions = True
                continue
            if not inFunctions:
                continue
            found: re.Match = FUNCTION_RE.match(text)
            if found is not None:
                names.append(found.group(1))
    return names


class HiddenListTest(unittest.TestCase):
    def setUp(self) -> None:
        self.methods: List[str] = scheme_method_names()

    def test_scheme_function_count(self) -> None:
        self.assertEqual(len(self.methods), LAYER_232_FUNCTION_COUNT)

    def test_group_sizes(self) -> None:
        self.assertEqual(len(hidden_list.WALLET_METHODS), 17)
        self.assertEqual(len(hidden_list.TON_CONNECT_METHODS), 9)
        self.assertEqual(len(hidden_list.TON_CENTER_METHODS), 2)
        self.assertEqual(len(hidden_list.CURRENCY_METHODS), 1)
        self.assertEqual(len(hidden_list.ONRAMP_METHODS), 6)
        self.assertEqual(len(hidden_list.SHARE_THEME_METHODS), 2)
        self.assertEqual(len(hidden_list.NO_PAID_MESSAGES_TOKEN_METHODS), 2)
        self.assertEqual(len(hidden_list.STAR_GIFT_METHODS), 5)
        self.assertEqual(len(hidden_list.HIDDEN_METHODS), 44)

    def test_every_hidden_name_exists_in_layer_232(self) -> None:
        known: Set[str] = set(self.methods)
        missing: List[str] = sorted(name for name in hidden_list.HIDDEN_METHODS if name not in known)
        self.assertEqual(missing, [])

    def test_every_wallet_namespace_method_is_hidden(self) -> None:
        wallet: List[str] = [name for name in self.methods if name.startswith("wallet.")]
        self.assertEqual(len(wallet), 26)
        notHidden: List[str] = [name for name in wallet if not hidden_list.is_hidden_method(name)]
        self.assertEqual(notHidden, [])

    def test_every_toncenter_method_is_hidden(self) -> None:
        toncenter: List[str] = [name for name in self.methods if name.startswith("toncenter.")]
        self.assertEqual(len(toncenter), 2)
        notHidden: List[str] = [name for name in toncenter if not hidden_list.is_hidden_method(name)]
        self.assertEqual(notHidden, [])

    def test_every_onramp_method_is_hidden(self) -> None:
        onramp: List[str] = [name for name in self.methods if "Onramp" in name and name.startswith("payments.")]
        self.assertEqual(len(onramp), 6)
        self.assertEqual(sorted(onramp), sorted(hidden_list.ONRAMP_METHODS))

    def test_star_gift_group_matches_scheme(self) -> None:
        starGift: List[str] = sorted(
            name for name in self.methods
            if name in {
                "payments.depositStarGift",
                "payments.withdrawStarGift",
                "payments.displayStarGift",
                "payments.deleteStarGift",
                "payments.getStarGiftWithdrawalUrl",
            }
        )
        self.assertEqual(starGift, sorted(hidden_list.STAR_GIFT_METHODS))

    def test_canonical_methods_are_not_hidden(self) -> None:
        self.assertFalse(hidden_list.is_hidden_method("messages.sendMessage"))
        self.assertFalse(hidden_list.is_hidden_method("payments.getStarGifts"))
        self.assertFalse(hidden_list.is_hidden_method("payments.getSavedStarGifts"))
        self.assertFalse(hidden_list.is_hidden_method("payments.getStarsRevenueWithdrawalUrl"))
        self.assertFalse(hidden_list.is_hidden_method("contacts.deleteContacts"))
        self.assertFalse(hidden_list.is_hidden_method("bots.addUsername"))

    def test_hidden_methods_in_filters_and_sorts(self) -> None:
        sample: List[str] = [
            "wallet.getState",
            "messages.sendMessage",
            "contacts.exportNoPaidMessagesToken",
            "payments.deleteStarGift",
        ]
        self.assertEqual(
            hidden_list.hidden_methods_in(sample),
            [
                "contacts.exportNoPaidMessagesToken",
                "payments.deleteStarGift",
                "wallet.getState",
            ],
        )


if __name__ == "__main__":
    unittest.main()
