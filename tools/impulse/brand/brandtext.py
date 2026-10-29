"""Brand substitution for user-visible localization values.

Python port of the Android build-time rule (buildSrc BrandText). "Telegram" at a word start (also as a compound
prefix, e.g. Telegramberichten) becomes the brand, TELEGRAM becomes the upper-case brand, and the native-script
spellings become the brand. @handles, #hashtags, URL paths, bare t.me links and the placeholder or link markers
are protected and returned unchanged.

Java regex semantics are kept on purpose: \\w, \\s and \\b are ASCII-only, the exclusion group is matched
ASCII-case-insensitively, and \\p{L} / \\p{N} are written as Unicode letter-or-digit classes.
"""

import re
from typing import Dict, List, Match, Pattern


# Spellings of the product name in the bundled non-Latin locales.
NATIVE_SPELLINGS: List[str] = [
    "تيليجرام",
    "تليجرام",
    "텔레그램",
]

# Protected spans. They are matched ASCII-case-insensitively and use ASCII \w, \s and \b (group 1 is scoped with
# the (?ai: ...) flags). URLs are scheme-anchored, so "[Telegram](https://...)" link text is still rewritten. Every
# span stops at whitespace and the markup delimiters " ' < > ) so anchor text after an href stays rewritable.
EXCLUSIONS: List[str] = [
    r"\{telegram\}",
    r"\]\(telegram\)",
    r"telegramdesktop",
    r"TelegramTips\w*",
    r"[A-Za-z][A-Za-z0-9+.\-]*://[^\s\"'<>)]*",
    r"(?<![\w.])(?:t\.me|telegram\.me|telegram\.dog)/[^\s\"'<>)]*",
    r"(?<!\w)www\.[^\s\"'<>)]*",
    r"@[\w.]+",
    r"#\w+",
]

# A word start that is not part of an @handle, a URL path or query value, or a snake_case identifier.
# An underscore only blocks when it joins two word parts (my_Telegram), so markdown emphasis (__Telegram) still works.
# [^\W_] is a Unicode letter or digit (the Java \p{L}\p{N} classes).
WORD_START: str = r"(?<![@/=])(?<![^\W_])(?<![^\W_]_)"

# Java \b after an ASCII word character: the next character is not an ASCII word character.
ASCII_WORD_END: str = r"(?![A-Za-z0-9_])"

# Group 1: protected exclusion, group 2: "Telegram" (also as a word prefix), group 3: upper case,
# group 4: native spelling.
PATTERN: Pattern[str] = re.compile(
    "((?ai:"
    + "|".join(EXCLUSIONS)
    + "))"
    + "|("
    + WORD_START
    + r"Telegram(?!\.(?:org|me|dog)"
    + ASCII_WORD_END
    + "))"
    + "|("
    + WORD_START
    + "TELEGRAM"
    + ASCII_WORD_END
    + r"(?!\.[^\W\d_]))"
    + "|("
    + "|".join(re.escape(spelling) for spelling in NATIVE_SPELLINGS)
    + ")"
)


def replace_match(match: Match[str], brand: str) -> str:
    # A protected match (group 1) is returned unchanged, so an exclusion can never be rewritten.
    if match.group(1) is not None:
        return match.group(0)

    if match.group(3) is not None:
        return brand.upper()

    return brand


def replace(value: str, brand: str) -> str:
    # The replacement is a function, so the brand is inserted literally (no backreference expansion).
    return PATTERN.sub(lambda match: replace_match(match, brand), value)


def app_names(brand: str) -> Dict[str, str]:
    # The app names, which the build defines through resValue instead of strings.xml. Every localization asset must
    # carry these entries as well.
    return {
        "AppName": brand,
        "AppNameBeta": brand + " Beta",
    }
