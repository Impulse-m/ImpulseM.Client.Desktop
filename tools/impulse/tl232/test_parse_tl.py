import sys
import unittest
from pathlib import Path
from typing import Any, Dict, List

# Isolated mode (python -I) leaves the script directory off sys.path, so add it explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import parse_tl as tl  # noqa: E402


CONSTRUCTOR_COUNT: int = 1720
METHOD_COUNT: int = 859


class SchemeCountTest(unittest.TestCase):
    scheme: Dict[str, Any]

    @classmethod
    def setUpClass(cls) -> None:
        cls.scheme = tl.load_scheme(tl.SCHEMA_PATH)

    def test_layer_is_232(self) -> None:
        self.assertEqual(self.scheme["layer"], 232)

    def test_constructor_count(self) -> None:
        self.assertEqual(len(self.scheme["constructors"]), CONSTRUCTOR_COUNT)

    def test_method_count(self) -> None:
        self.assertEqual(len(self.scheme["methods"]), METHOD_COUNT)

    def test_schema_path_points_into_repository(self) -> None:
        self.assertTrue(tl.SCHEMA_PATH.is_file(), str(tl.SCHEMA_PATH))
        self.assertEqual(tl.SCHEMA_PATH.parts[-5:], ("Telegram", "SourceFiles", "mtproto", "scheme", "api.tl"))

    def test_known_constructor_round_trip(self) -> None:
        byName: Dict[str, Dict[str, Any]] = {c["predicate"]: c for c in self.scheme["constructors"]}
        self.assertEqual(byName["boolTrue"]["id"], 0x997275B5)
        self.assertEqual(byName["boolTrue"]["result"], "Bool")
        self.assertEqual(byName["boolTrue"]["params"], [])


class ParseLineTest(unittest.TestCase):
    def test_flags_bang_and_vector_params(self) -> None:
        entry: Dict[str, Any] = tl.parse_tl_line("wrap#00000001 {X:Type} flags:# a:flags.2?true b:Vector<int> q:!X = X;")
        self.assertEqual(entry["predicate"], "wrap")
        self.assertEqual(entry["id"], 1)
        self.assertEqual(entry["result"], "X")
        self.assertEqual([p["name"] for p in entry["params"]], ["flags", "a", "b", "q"])
        flagsParam: Dict[str, Any] = tl.parse_tl_type(entry["params"][0]["type"], "t")
        self.assertEqual(flagsParam, {"kind": "flags"})
        flagged: Dict[str, Any] = tl.parse_tl_type(entry["params"][1]["type"], "t")
        self.assertEqual(flagged, {"kind": "true", "flag": ["flags", 2]})
        vector: Dict[str, Any] = tl.parse_tl_type(entry["params"][2]["type"], "t")
        self.assertEqual(vector, {"kind": "vector", "elem": {"kind": "int"}})
        bang: Dict[str, Any] = tl.parse_tl_type(entry["params"][3]["type"], "t")
        self.assertEqual(bang, {"kind": "bang", "type": "X"})

    def test_flags2_marks_second_flags_word(self) -> None:
        flagged: Dict[str, Any] = tl.parse_tl_type("flags2.5?string", "t")
        self.assertEqual(flagged, {"kind": "string", "flag": ["flags2", 5]})

    def test_nested_vector_of_objects(self) -> None:
        vector: Dict[str, Any] = tl.parse_tl_type("Vector<messageEntity>", "t")
        self.assertEqual(vector, {"kind": "vector", "elem": {"kind": "object", "type": "messageEntity"}})

    def test_vector_predicate_has_no_params(self) -> None:
        entry: Dict[str, Any] = tl.parse_tl_line("vector#1cb5c415 {t:Type} # [ t ] = Vector t;")
        self.assertEqual(entry["predicate"], "vector")
        self.assertEqual(entry["id"], 0x1CB5C415)
        self.assertEqual(entry["params"], [])
        self.assertEqual(entry["result"], "Vector t")

    def test_dotted_predicate_keeps_namespace(self) -> None:
        entry: Dict[str, Any] = tl.parse_tl_line("account.getPassword#9cd4eaf9 = account.Password;")
        self.assertEqual(entry["predicate"], "account.getPassword")
        self.assertEqual(entry["result"], "account.Password")

    def test_rejects_missing_semicolon(self) -> None:
        with self.assertRaises(ValueError):
            tl.parse_tl_line("boolTrue#997275b5 = Bool")

    def test_rejects_missing_id(self) -> None:
        with self.assertRaises(ValueError):
            tl.parse_tl_line("boolTrue = Bool;")

    def test_rejects_malformed_parameter(self) -> None:
        with self.assertRaises(ValueError):
            tl.parse_tl_line("x#00000002 flagsonly = X;")

    def test_rejects_unsupported_type(self) -> None:
        with self.assertRaises(ValueError):
            tl.parse_tl_type("Vector<int", "t")

    def test_parse_params_adds_names(self) -> None:
        params: List[Dict[str, Any]] = tl.parse_params([{"name": "id", "type": "long"}], "x")
        self.assertEqual(params, [{"kind": "long", "name": "id"}])


class ParseSchemeTextTest(unittest.TestCase):
    def test_splits_constructors_and_functions(self) -> None:
        text: str = "boolTrue#997275b5 = Bool;\n---functions---\nping#7abe77ec ping_id:long = Pong;\n"
        scheme: Dict[str, Any] = tl.parse_scheme_text("// LAYER 232\n" + text)
        self.assertEqual(scheme["layer"], 232)
        self.assertEqual(len(scheme["constructors"]), 1)
        self.assertEqual(len(scheme["methods"]), 1)
        self.assertEqual(scheme["methods"][0]["predicate"], "ping")

    def test_rejects_other_layer(self) -> None:
        with self.assertRaises(ValueError):
            tl.parse_scheme_text("boolTrue#997275b5 = Bool;\n// LAYER 229\n")

    def test_rejects_missing_layer(self) -> None:
        with self.assertRaises(ValueError):
            tl.parse_scheme_text("boolTrue#997275b5 = Bool;\n")


if __name__ == "__main__":
    unittest.main()
