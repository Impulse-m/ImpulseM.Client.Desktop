import sys
import unittest
from pathlib import Path
from typing import Any, Dict, List

# Isolated mode (python -I) leaves the script directory off sys.path, so add it explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import parse_protos as protos  # noqa: E402


FIXTURE_PATH: Path = Path(__file__).resolve().parent / "fixtures" / "types_mini.proto"
VENDORED_TYPES_PATH: Path = Path(__file__).resolve().parents[1] / "protos" / "v1" / "types.proto"
SERVER_LAYER_MESSAGE_COUNT: int = 1930


def field(name: str, number: int, fieldType: str, repeated: bool, optional: bool, oneof: Any) -> Dict[str, Any]:
    return {
        "name": name,
        "number": number,
        "type": fieldType,
        "repeated": repeated,
        "optional": optional,
        "oneof": oneof,
    }


class FixtureTest(unittest.TestCase):
    messages: Dict[str, List[Dict[str, Any]]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.messages = protos.parse_proto_file(FIXTURE_PATH)

    def test_message_names_only(self) -> None:
        self.assertEqual(
            sorted(self.messages.keys()),
            sorted([
                "AccountAuthorizations",
                "AccountChatThemes",
                "AccountChatThemesNotModified",
                "AccountChatThemesValue",
                "LongResponse",
                "EmptyInline",
            ]),
        )

    def test_plain_and_repeated_fields(self) -> None:
        self.assertEqual(
            self.messages["AccountAuthorizations"],
            [
                field("authorization_ttl_days", 1, "int32", False, False, None),
                field("authorizations", 2, "v1.Authorization", True, False, None),
            ],
        )

    def test_oneof_fields_keep_group_name(self) -> None:
        self.assertEqual(
            self.messages["AccountChatThemes"],
            [
                field("account_chat_themes_not_modified", 1, "AccountChatThemesNotModified", False, False, "value"),
                field("account_chat_themes", 2, "AccountChatThemesValue", False, False, "value"),
            ],
        )

    def test_optional_and_repeated_labels(self) -> None:
        self.assertEqual(
            self.messages["AccountChatThemesValue"],
            [
                field("hash", 1, "int64", False, False, None),
                field("next_offset", 5, "string", False, True, None),
                field("srp_B", 6, "bytes", False, True, None),
                field("flags", 7, "bool", True, False, None),
            ],
        )

    def test_empty_messages(self) -> None:
        self.assertEqual(self.messages["AccountChatThemesNotModified"], [])
        self.assertEqual(self.messages["EmptyInline"], [])

    def test_one_line_message(self) -> None:
        self.assertEqual(
            self.messages["LongResponse"],
            [field("value", 1, "int64", False, False, None)],
        )

    def test_reserved_names_are_not_fields(self) -> None:
        names: List[str] = [f["name"] for f in self.messages["AccountChatThemesValue"]]
        self.assertNotIn("text", names)
        self.assertNotIn("option", names)


class ErrorTest(unittest.TestCase):
    def test_unterminated_block_raises(self) -> None:
        with self.assertRaises(ValueError):
            protos.parse_proto_text("message A {\n  int32 x = 1;\n")

    def test_unbalanced_brace_raises(self) -> None:
        with self.assertRaises(ValueError):
            protos.parse_proto_text("message A {\n}\n}\n")

    def test_duplicate_field_number_raises(self) -> None:
        with self.assertRaises(ValueError):
            protos.parse_proto_text("message A {\n  int32 x = 1;\n  int32 y = 1;\n}\n")

    def test_duplicate_field_name_raises(self) -> None:
        with self.assertRaises(ValueError):
            protos.parse_proto_text("message A {\n  int32 x = 1;\n  int32 x = 2;\n}\n")

    def test_duplicate_message_raises(self) -> None:
        with self.assertRaises(ValueError):
            protos.parse_proto_text("message A {\n}\nmessage A {\n}\n")

    def test_unknown_statement_raises(self) -> None:
        with self.assertRaises(ValueError):
            protos.parse_proto_text("message A {\n  map<string, int32> m = 1;\n}\n")

    def test_statement_outside_message_raises(self) -> None:
        with self.assertRaises(ValueError):
            protos.parse_proto_text("int32 x = 1;\n")

    def test_nested_message_raises(self) -> None:
        with self.assertRaises(ValueError):
            protos.parse_proto_text("message A {\n  message B {\n  }\n}\n")

    def test_label_inside_oneof_raises(self) -> None:
        with self.assertRaises(ValueError):
            protos.parse_proto_text("message A {\n  oneof v {\n    repeated int32 x = 1;\n  }\n}\n")


class VendoredTypesTest(unittest.TestCase):
    def test_vendored_layer_229_types_parse(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = protos.parse_proto_file(VENDORED_TYPES_PATH)
        self.assertEqual(len(messages), SERVER_LAYER_MESSAGE_COUNT)
        self.assertIn("AccountChatThemes", messages)
        oneofNames: List[Any] = [f["oneof"] for f in messages["AccountChatThemes"]]
        self.assertEqual(oneofNames, ["value", "value"])
        authorizations: List[Dict[str, Any]] = messages["AccountAuthorizations"]
        self.assertEqual(authorizations[0]["name"], "authorization_ttl_days")
        self.assertEqual(authorizations[0]["type"], "int32")
        self.assertTrue(authorizations[1]["repeated"])


if __name__ == "__main__":
    unittest.main()
