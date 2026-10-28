import gzip
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

# Isolated mode (python -I) leaves the script directory off sys.path, so add it explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_tl_proto_232 as gen  # noqa: E402


CONSTRUCTOR_COUNT: int = 1720
FUNCTION_COUNT: int = 859
WRAPPER_COUNT: int = 11


def fake_message(name: str, fields: Dict[str, Tuple[int, str, str]]) -> Dict[str, Any]:
    return {"fields": fields, "oneof": {}, "file": "fake.proto", "name": name}


class SchemeTest(unittest.TestCase):
    scheme: Dict[str, Any]

    @classmethod
    def setUpClass(cls) -> None:
        cls.scheme = gen.load_scheme(gen.SCHEMA_PATH)

    def test_layer_and_counts(self) -> None:
        self.assertEqual(self.scheme["layer"], 232)
        self.assertEqual(len(self.scheme["constructors"]), CONSTRUCTOR_COUNT)
        self.assertEqual(len(self.scheme["methods"]), FUNCTION_COUNT)

    def test_wrapper_functions_are_the_only_undotted_methods(self) -> None:
        undotted: List[str] = [m["predicate"] for m in self.scheme["methods"] if "." not in m["predicate"]]
        self.assertEqual(len(undotted), WRAPPER_COUNT)
        self.assertEqual(sorted(undotted), sorted(gen.WRAPPER_IDS))

    def test_wrapper_ids_equal_scheme_ids(self) -> None:
        byName: Dict[str, int] = {m["predicate"]: m["id"] for m in self.scheme["methods"]}
        for name, wrapperId in gen.WRAPPER_IDS.items():
            self.assertEqual(byName[name], wrapperId, name)

    def test_rejects_other_layer(self) -> None:
        with self.assertRaises(ValueError):
            gen.parse_scheme_text("boolTrue#997275b5 = Bool;\n// LAYER 229\n")

    def test_parses_flags_bang_and_vector_params(self) -> None:
        entry: Dict[str, Any] = gen.parse_tl_line("wrap#00000001 {X:Type} flags:# a:flags.2?true b:Vector<int> q:!X = X;")
        self.assertEqual(entry["predicate"], "wrap")
        self.assertEqual(entry["id"], 1)
        self.assertEqual(entry["result"], "X")
        self.assertEqual([p["name"] for p in entry["params"]], ["flags", "a", "b", "q"])
        flagsParam: Dict[str, Any] = gen.parse_tl_type(entry["params"][0]["type"], "t")
        self.assertEqual(flagsParam, {"kind": "flags"})
        flagged: Dict[str, Any] = gen.parse_tl_type(entry["params"][1]["type"], "t")
        self.assertEqual(flagged, {"kind": "true", "flag": ["flags", 2]})
        vector: Dict[str, Any] = gen.parse_tl_type(entry["params"][2]["type"], "t")
        self.assertEqual(vector, {"kind": "vector", "elem": {"kind": "int"}})
        bang: Dict[str, Any] = gen.parse_tl_type(entry["params"][3]["type"], "t")
        self.assertEqual(bang, {"kind": "bang", "type": "X"})

    def test_vector_constructor_has_no_parameters(self) -> None:
        entry: Dict[str, Any] = gen.parse_tl_line("vector#1cb5c415 {t:Type} # [ t ] = Vector t;")
        self.assertEqual(entry["id"], 0x1cb5c415)
        self.assertEqual(entry["params"], [])

    def test_malformed_parameter_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            gen.parse_tl_line("x#00000001 bad = X;")


class WireTypeCheckTest(unittest.TestCase):
    def test_scalar_wire_types(self) -> None:
        self.assertTrue(gen.field_matches({"kind": "int"}, "int32", ""))
        self.assertFalse(gen.field_matches({"kind": "int"}, "int64", ""))
        self.assertTrue(gen.field_matches({"kind": "long"}, "int64", "optional"))
        self.assertTrue(gen.field_matches({"kind": "true"}, "bool", "optional"))
        self.assertTrue(gen.field_matches({"kind": "Bool"}, "bool", "optional"))
        self.assertFalse(gen.field_matches({"kind": "string"}, "int32", ""))

    def test_repeated_label_must_match_vector(self) -> None:
        vectorInt: Dict[str, Any] = {"kind": "vector", "elem": {"kind": "int"}}
        self.assertTrue(gen.field_matches(vectorInt, "int32", "repeated"))
        self.assertFalse(gen.field_matches(vectorInt, "int32", ""))
        self.assertFalse(gen.field_matches({"kind": "int"}, "int32", "repeated"))

    def test_object_names_are_compared_without_package(self) -> None:
        self.assertTrue(gen.field_matches({"kind": "object", "type": "Peer"}, "v1.Peer", "optional"))
        self.assertFalse(gen.field_matches({"kind": "object", "type": "Peer"}, "string", ""))

    def test_conflicting_shared_field_is_reported(self) -> None:
        message: Dict[str, Any] = fake_message("Fake", {"description": (1, "string", "")})
        params: List[Dict[str, Any]] = gen.parse_params([{"name": "description", "type": "TextWithEntities"}], "test")
        problem: Optional[str] = gen.pair_params(params, message)
        self.assertIsNotNone(problem)
        self.assertNotIn("field", params[0])


class TolerantPairingTest(unittest.TestCase):
    def test_param_missing_on_server_is_unmapped(self) -> None:
        message: Dict[str, Any] = fake_message("Fake", {"id": (1, "int32", "")})
        params: List[Dict[str, Any]] = gen.parse_params(
            [{"name": "id", "type": "int"}, {"name": "extra", "type": "string"}],
            "test",
        )
        self.assertIsNone(gen.pair_params(params, message))
        self.assertEqual(params[0]["field"], 1)
        self.assertTrue(params[1]["unmapped"])
        self.assertNotIn("field", params[1])

    def test_true_flag_derives_has_prefix(self) -> None:
        message: Dict[str, Any] = fake_message("Fake", {"url": (1, "string", "")})
        params: List[Dict[str, Any]] = gen.parse_params(
            [{"name": "url", "type": "string"}, {"name": "has_url", "type": "flags.0?true"}],
            "test",
        )
        self.assertIsNone(gen.pair_params(params, message))
        self.assertEqual(params[1]["derived_from"], "url")
        self.assertNotIn("field", params[1])

    def test_constructor_without_server_message_has_no_proto(self) -> None:
        ctor: Dict[str, Any] = {"id": 1, "params": [], "predicate": "fake", "result": "Fake"}
        entry: Dict[str, Any]
        problem: Optional[str]
        entry, problem = gen.build_constructor(ctor, False, {}, set())
        self.assertIsNone(entry["proto"])
        self.assertEqual(problem, "no server message")

    def test_type_conflict_leaves_constructor_unmapped(self) -> None:
        v1: Dict[str, Any] = {"Fake": fake_message("Fake", {"id": (1, "int32", ""), "text": (2, "string", "")})}
        ctor: Dict[str, Any] = {
            "id": 2,
            "params": [{"name": "id", "type": "int"}, {"name": "text", "type": "TextWithEntities"}],
            "predicate": "fake",
            "result": "Fake",
        }
        entry: Dict[str, Any]
        problem: Optional[str]
        entry, problem = gen.build_constructor(ctor, False, v1, set())
        self.assertIsNone(entry["proto"])
        self.assertIsNotNone(problem)
        self.assertNotIn("field", entry["params"][0])

    def test_result_kinds(self) -> None:
        self.assertEqual(gen.result_of("Vector<int>", "IntResponse", None), None)
        self.assertEqual(gen.result_of("int", "IntResponse", None), {"kind": "int"})
        self.assertIsNone(gen.result_of("long", "IntResponse", None))
        self.assertIsNone(gen.result_of("Bool", "BoolResponse", None))


class MappingTest(unittest.TestCase):
    mapping: Dict[str, Any]
    report: Dict[str, Any]
    gzBytes: bytes
    tmp: tempfile.TemporaryDirectory

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        outPath: Path = Path(cls.tmp.name) / "tl232.json.gz"
        mapping: Dict[str, Any]
        report: Dict[str, Any]
        mapping, report = gen.generate(outPath)
        cls.mapping = mapping
        cls.report = report
        cls.gzBytes = outPath.read_bytes()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def constructor(self, predicate: str) -> Dict[str, Any]:
        for c in self.mapping["constructors"]:
            if c["predicate"] == predicate:
                return c
        self.fail("constructor not in mapping: " + predicate)

    def method(self, name: str) -> Dict[str, Any]:
        for m in self.mapping["methods"]:
            if m["method"] == name:
                return m
        self.fail("method not in mapping: " + name)

    def param(self, entry: Dict[str, Any], name: str) -> Dict[str, Any]:
        for p in entry["params"]:
            if p["name"] == name:
                return p
        self.fail("param not found: " + name)

    def test_file_is_layer_232_with_constants(self) -> None:
        decoded: Dict[str, Any] = json.loads(gzip.decompress(self.gzBytes).decode("utf-8"))
        self.assertEqual(decoded["layer"], 232)
        self.assertEqual(decoded["wrappers"], gen.WRAPPER_IDS)
        self.assertEqual(decoded["constants"], gen.CONSTANT_IDS)

    def test_constructor_count_and_ids(self) -> None:
        self.assertEqual(len(self.mapping["constructors"]), CONSTRUCTOR_COUNT)
        ids: List[int] = [c["id"] for c in self.mapping["constructors"]]
        self.assertEqual(len(set(ids)), CONSTRUCTOR_COUNT)
        for constructorId in ids:
            self.assertTrue(0 <= constructorId <= 0xFFFFFFFF)

    def test_method_accounting(self) -> None:
        dotted: int = len([m for m in self.scheme_methods() if "." in m["predicate"]])
        self.assertEqual(self.report["mappedMethods"] + len(self.report["absentMethods"]), dotted)
        names: List[str] = [m["method"] for m in self.mapping["methods"]]
        self.assertEqual(len(set(names)), len(names))
        for name in gen.WRAPPER_IDS:
            self.assertNotIn(name, names)

    def scheme_methods(self) -> List[Dict[str, Any]]:
        return gen.load_scheme(gen.SCHEMA_PATH)["methods"]

    def test_message_constructor_shape(self) -> None:
        message: Dict[str, Any] = self.constructor("message")
        self.assertEqual(message["proto"], "MessageValue")
        self.assertEqual(message["arm"], 2)
        self.assertEqual(self.param(message, "id")["field"], 16)
        self.assertEqual(self.param(message, "peer_id")["field"], 20)
        out: Dict[str, Any] = self.param(message, "out")
        self.assertEqual(out["kind"], "true")
        self.assertEqual(out["flag"], ["flags", 1])
        self.assertEqual(out["field"], 1)

    def test_code_settings_shape(self) -> None:
        code: Dict[str, Any] = self.constructor("codeSettings")
        self.assertIsNone(code["arm"])
        self.assertEqual(self.param(code, "token")["field"], 8)
        sandbox: Dict[str, Any] = self.param(code, "app_sandbox")
        self.assertEqual(sandbox["kind"], "Bool")
        self.assertEqual(sandbox["field"], 9)
        logout: Dict[str, Any] = self.param(code, "logout_tokens")
        self.assertEqual(logout["kind"], "vector")
        self.assertEqual(logout["elem"]["kind"], "bytes")
        self.assertEqual(logout["field"], 7)

    def test_login_token_captures(self) -> None:
        auth: Dict[str, Any] = self.constructor("auth.authorization")
        captures: Dict[str, int] = {c["name"]: c["field"] for c in auth["capture"]}
        self.assertEqual(captures, {"session_token": 6, "refresh_token": 7})

    def test_type_check_leaves_bot_verification_unmapped(self) -> None:
        self.assertIsNone(self.constructor("botVerification")["proto"])
        self.assertIsNone(self.constructor("botVerifierSettings")["proto"])
        self.assertIn("botVerification", " ".join(self.report["unmappedConstructors"]))

    def test_tl_only_constructors_are_unmapped(self) -> None:
        self.assertIsNone(self.constructor("messageEntityTonAddress")["proto"])
        self.assertIsNone(self.constructor("textTonAddress")["proto"])

    def test_derived_flag_has_no_field(self) -> None:
        answer: Dict[str, Any] = self.constructor("messages.botCallbackAnswer")
        hasUrl: Dict[str, Any] = self.param(answer, "has_url")
        self.assertEqual(hasUrl["derived_from"], "url")
        self.assertNotIn("field", hasUrl)

    def test_method_shapes(self) -> None:
        getUsers: Dict[str, Any] = self.method("users.getUsers")
        self.assertEqual(getUsers["path"], "/v1.users.Users/GetUsers")
        self.assertEqual(getUsers["result"], {"kind": "list", "elem": {"kind": "object", "type": "User"}})
        self.assertEqual(self.method("account.updateStatus")["result"], {"kind": "bool"})
        self.assertEqual(self.method("messages.reportMessagesDelivery")["result"], {"kind": "bool"})

    def test_hidden_features_are_absent_from_methods(self) -> None:
        names: Set[str] = {m["method"] for m in self.mapping["methods"]}
        for name in ("wallet.getState", "messages.setShareTheme", "payments.getStarGiftWithdrawalUrl"):
            self.assertNotIn(name, names)
            self.assertIn(name, self.report["absentMethods"])

    def test_output_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as tmpName:
            second: Path = Path(tmpName) / "second.json.gz"
            gen.generate(second)
            self.assertEqual(second.read_bytes(), self.gzBytes)


if __name__ == "__main__":
    unittest.main()
