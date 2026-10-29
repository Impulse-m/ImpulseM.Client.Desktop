import sys
import unittest
from pathlib import Path

# Isolated mode (python -I) leaves the script directory off sys.path, so add it explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import brandtext as brand  # noqa: E402


class BrandTextTest(unittest.TestCase):

    def test_replaces_plain_brand(self) -> None:
        self.assertEqual("ImpulseM Premium", brand.replace("Telegram Premium", "ImpulseM"))

    def test_keeps_telegram_tips_url(self) -> None:
        self.assertEqual("https://t.me/TelegramTips", brand.replace("https://t.me/TelegramTips", "ImpulseM"))
        self.assertEqual("TelegramTipsRU", brand.replace("TelegramTipsRU", "ImpulseM"))

    def test_keeps_placeholder_and_link_markers(self) -> None:
        self.assertEqual("{telegram}", brand.replace("{telegram}", "ImpulseM"))
        self.assertEqual("{Telegram}", brand.replace("{Telegram}", "ImpulseM"))
        self.assertEqual("[x](telegram)", brand.replace("[x](telegram)", "ImpulseM"))

    def test_keeps_telegram_desktop(self) -> None:
        self.assertEqual("telegramdesktop", brand.replace("telegramdesktop", "ImpulseM"))
        self.assertEqual("TelegramDesktop", brand.replace("TelegramDesktop", "ImpulseM"))

    def test_keeps_lowercase_hosts(self) -> None:
        self.assertEqual("https://telegram.org/faq", brand.replace("https://telegram.org/faq", "ImpulseM"))
        self.assertEqual("telegram.org", brand.replace("telegram.org", "ImpulseM"))

    def test_replaces_around_exclusions(self) -> None:
        self.assertEqual(
            "ImpulseM {telegram} ImpulseM https://t.me/TelegramTips",
            brand.replace("Telegram {telegram} Telegram https://t.me/TelegramTips", "ImpulseM")
        )

    def test_replaces_arabic_transliteration(self) -> None:
        self.assertEqual("مرحبا في ImpulseM", brand.replace("مرحبا في تيليجرام", "ImpulseM"))
        self.assertEqual("مرحبا في ImpulseM", brand.replace("مرحبا في تليجرام", "ImpulseM"))

    def test_replaces_korean_transliteration(self) -> None:
        self.assertEqual("ImpulseM에 오신 것을 환영합니다", brand.replace("텔레그램에 오신 것을 환영합니다", "ImpulseM"))

    def test_brand_value_drives_the_result(self) -> None:
        self.assertEqual("Xyz Premium", brand.replace("Telegram Premium", "Xyz"))
        self.assertEqual("Xyz", brand.replace("텔레그램", "Xyz"))

    def test_brand_is_inserted_literally(self) -> None:
        self.assertEqual("A$1 B", brand.replace("Telegram B", "A$1"))

    def test_replaces_uppercase_with_uppercase_brand(self) -> None:
        self.assertEqual("INVITE TO IMPULSEM", brand.replace("INVITE TO TELEGRAM", "ImpulseM"))
        self.assertEqual("CONVIDAR PARA O XYZ", brand.replace("CONVIDAR PARA O TELEGRAM", "Xyz"))

    def test_replaces_compound_words(self) -> None:
        self.assertEqual("Controleer je ImpulseMberichten", brand.replace("Controleer je Telegramberichten", "ImpulseM"))

    def test_keeps_handles_and_url_paths(self) -> None:
        self.assertEqual("write to @Telegram now", brand.replace("write to @Telegram now", "ImpulseM"))
        self.assertEqual("@TelegramBot", brand.replace("@TelegramBot", "ImpulseM"))
        self.assertEqual("https://t.me/Telegram", brand.replace("https://t.me/Telegram", "ImpulseM"))
        self.assertEqual("https://t.me/TelegramNews", brand.replace("https://t.me/TelegramNews", "ImpulseM"))
        self.assertEqual("Telegram.org", brand.replace("Telegram.org", "ImpulseM"))

    def test_keeps_telegram_tips_while_replacing_neighbours(self) -> None:
        self.assertEqual("https://t.me/TelegramTipsDE", brand.replace("https://t.me/TelegramTipsDE", "ImpulseM"))
        self.assertEqual(
            "ImpulseM https://t.me/TelegramTips ImpulseMberichten",
            brand.replace("Telegram https://t.me/TelegramTips Telegramberichten", "ImpulseM")
        )

    def test_replaces_brand_inside_markdown_emphasis(self) -> None:
        self.assertEqual(
            "by subscribing to __ImpulseM Premium.__",
            brand.replace("by subscribing to __Telegram Premium.__", "ImpulseM")
        )

    def test_keeps_custom_scheme_query(self) -> None:
        self.assertEqual("tg://resolve?domain=Telegram", brand.replace("tg://resolve?domain=Telegram", "ImpulseM"))

    def test_keeps_http_query_value(self) -> None:
        self.assertEqual(
            "https://example.com/?start=Telegram",
            brand.replace("https://example.com/?start=Telegram", "ImpulseM")
        )

    def test_keeps_hashtag(self) -> None:
        self.assertEqual("#Telegram", brand.replace("#Telegram", "ImpulseM"))

    def test_keeps_handle_with_underscore(self) -> None:
        self.assertEqual("@my_Telegram", brand.replace("@my_Telegram", "ImpulseM"))

    def test_keeps_uppercase_domain(self) -> None:
        self.assertEqual("TELEGRAM.ORG", brand.replace("TELEGRAM.ORG", "ImpulseM"))

    def test_replaces_brand_in_markdown_link_text(self) -> None:
        self.assertEqual(
            "[ImpulseM FAQ](https://telegram.org/faq)",
            brand.replace("[Telegram FAQ](https://telegram.org/faq)", "ImpulseM")
        )

    def test_replaces_anchor_text_after_http_href(self) -> None:
        self.assertEqual(
            "<a href=\"https://telegram.org/faq\">ImpulseM FAQ</a>",
            brand.replace("<a href=\"https://telegram.org/faq\">Telegram FAQ</a>", "ImpulseM")
        )

    def test_replaces_anchor_text_after_custom_scheme_href(self) -> None:
        self.assertEqual(
            "<a href=\"tg://x\">ImpulseM</a>",
            brand.replace("<a href=\"tg://x\">Telegram</a>", "ImpulseM")
        )

    def test_replaces_markdown_link_text_with_tme_url(self) -> None:
        self.assertEqual("[ImpulseM](https://t.me/x)", brand.replace("[Telegram](https://t.me/x)", "ImpulseM"))

    def test_keeps_url_in_parentheses(self) -> None:
        self.assertEqual(
            "(see https://telegram.org/Telegram)",
            brand.replace("(see https://telegram.org/Telegram)", "ImpulseM")
        )

    def test_keeps_format_string_placeholders(self) -> None:
        self.assertEqual("%1$s joined ImpulseM", brand.replace("%1$s joined Telegram", "ImpulseM"))

    def test_app_names_follow_the_brand(self) -> None:
        self.assertEqual(
            {"AppName": "Acme", "AppNameBeta": "Acme Beta"},
            brand.app_names("Acme")
        )


if __name__ == "__main__":
    unittest.main()
