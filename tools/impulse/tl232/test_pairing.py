import sys
import unittest
from pathlib import Path
from typing import Any, Dict, List

# Isolated mode (python -I) leaves the script directory off sys.path, so add it explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import pairing as pr  # noqa: E402


def tl_ctor(name: str, fields: List[Dict[str, str]]) -> Dict[str, Any]:
    return {"name": name, "id": 1, "layer": 232, "fields": fields}


def tl_field(name: str, tlType: str) -> Dict[str, str]:
    return {"name": name, "tl_type": tlType}


def proto_field(name: str, number: int, protoType: str, repeated: bool = False) -> Dict[str, Any]:
    return {"name": name, "number": number, "proto_type": protoType, "repeated": repeated, "oneof": None}


def find_field(entry: Dict[str, Any], name: str) -> Dict[str, Any]:
    for field in entry["fields"]:
        if field["name"] == name:
            return field
    raise AssertionError("no field " + name)


class PairByNameTest(unittest.TestCase):
    def test_pairs_by_name_and_fills_field_numbers(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {
            "PeerUser": [proto_field("user_id", 1, "int64")],
        }
        ctor: Dict[str, Any] = tl_ctor("peerUser", [tl_field("user_id", "long")])
        entry: Dict[str, Any] = pr.pair_constructor(ctor, messages)
        self.assertEqual(entry["proto"], "PeerUser")
        self.assertIsNone(entry["problem"])
        field: Dict[str, Any] = find_field(entry, "user_id")
        self.assertEqual(field["proto_field"], 1)
        self.assertEqual(field["proto_type"], "int64")
        self.assertFalse(field["discard_only"])

    def test_dotted_tl_name_maps_to_pascal_case(self) -> None:
        self.assertEqual(pr.pascal("account.foo"), "AccountFoo")
        self.assertEqual(pr.pascal("inputPeerUser"), "InputPeerUser")

    def test_no_server_message_gives_null_proto(self) -> None:
        entry: Dict[str, Any] = pr.pair_constructor(tl_ctor("ghostThing", [tl_field("x", "int")]), {})
        self.assertIsNone(entry["proto"])
        self.assertEqual(entry["problem"], "no server message GhostThing")
        self.assertIsNone(find_field(entry, "x")["proto_field"])

    def test_output_keeps_constructor_identity(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {"Ping": [proto_field("ping_id", 1, "int64")]}
        entry: Dict[str, Any] = pr.pair_constructor(tl_ctor("ping", [tl_field("ping_id", "long")]), messages)
        self.assertEqual(entry["name"], "ping")
        self.assertEqual(entry["id"], 1)
        self.assertEqual(entry["layer"], 232)


class WireKindTest(unittest.TestCase):
    def test_name_match_with_wrong_wire_kind_nulls_the_constructor(self) -> None:
        # Same field name, but the server sends a message where TL has a plain string.
        messages: Dict[str, List[Dict[str, Any]]] = {
            "BotVerification": [proto_field("description", 1, "TextWithEntities")],
        }
        ctor: Dict[str, Any] = tl_ctor("botVerification", [tl_field("description", "string")])
        entry: Dict[str, Any] = pr.pair_constructor(ctor, messages)
        self.assertIsNone(entry["proto"])
        self.assertIn("description", entry["problem"])
        self.assertIsNone(find_field(entry, "description")["proto_field"])

    def test_scalar_type_mismatch_nulls_the_constructor(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {"Thing": [proto_field("count", 1, "int64")]}
        entry: Dict[str, Any] = pr.pair_constructor(tl_ctor("thing", [tl_field("count", "int")]), messages)
        self.assertIsNone(entry["proto"])
        self.assertIn("count", entry["problem"])

    def test_message_field_matches_by_pascal_name(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {
            "Holder": [proto_field("peer", 1, "PeerUser")],
            "PeerUser": [proto_field("user_id", 1, "int64")],
        }
        entry: Dict[str, Any] = pr.pair_constructor(tl_ctor("holder", [tl_field("peer", "peerUser")]), messages)
        self.assertEqual(entry["proto"], "Holder")
        self.assertEqual(find_field(entry, "peer")["proto_type"], "PeerUser")

    def test_versioned_proto_type_is_normalised(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {
            "Holder": [proto_field("peer", 1, "v1.PeerUser")],
        }
        messages["PeerUser"] = []
        entry: Dict[str, Any] = pr.pair_constructor(tl_ctor("holder", [tl_field("peer", "peerUser")]), messages)
        self.assertEqual(entry["proto"], "Holder")
        self.assertEqual(find_field(entry, "peer")["proto_type"], "PeerUser")

    def test_bool_matches_bool_only(self) -> None:
        ok: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("flagsBox", [tl_field("on", "Bool")]),
            {"FlagsBox": [proto_field("on", 1, "bool")]},
        )
        bad: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("flagsBox", [tl_field("on", "Bool")]),
            {"FlagsBox": [proto_field("on", 1, "int32")]},
        )
        self.assertEqual(ok["proto"], "FlagsBox")
        self.assertIsNone(bad["proto"])

    def test_int128_and_int256_are_bytes(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {
            "Keys": [proto_field("a", 1, "bytes"), proto_field("b", 2, "bytes")],
        }
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("keys", [tl_field("a", "int128"), tl_field("b", "int256")]),
            messages,
        )
        self.assertEqual(entry["proto"], "Keys")

    def test_one_mismatch_among_good_fields_nulls_the_whole_constructor(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {
            "Pair": [proto_field("good", 1, "string"), proto_field("bad", 2, "int64")],
        }
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("pair", [tl_field("good", "string"), tl_field("bad", "string")]),
            messages,
        )
        self.assertIsNone(entry["proto"])
        self.assertIsNone(find_field(entry, "good")["proto_field"])


class VectorTest(unittest.TestCase):
    def test_vector_of_scalars_needs_repeated(self) -> None:
        ok: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("ids", [tl_field("ids", "Vector<long>")]),
            {"Ids": [proto_field("ids", 1, "int64", repeated=True)]},
        )
        bad: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("ids", [tl_field("ids", "Vector<long>")]),
            {"Ids": [proto_field("ids", 1, "int64", repeated=False)]},
        )
        self.assertEqual(ok["proto"], "Ids")
        self.assertEqual(find_field(ok, "ids")["proto_field"], 1)
        self.assertIsNone(bad["proto"])

    def test_scalar_field_rejects_repeated_server_field(self) -> None:
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("one", [tl_field("id", "long")]),
            {"One": [proto_field("id", 1, "int64", repeated=True)]},
        )
        self.assertIsNone(entry["proto"])

    def test_vector_of_objects_matches_repeated_message(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {
            "Batch": [proto_field("peers", 1, "InputPeer", repeated=True)],
        }
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("batch", [tl_field("peers", "Vector<InputPeer>")]),
            messages,
        )
        self.assertEqual(entry["proto"], "Batch")

    def test_vector_of_strings_matches_repeated_string(self) -> None:
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("words", [tl_field("words", "Vector<string>")]),
            {"Words": [proto_field("words", 1, "string", repeated=True)]},
        )
        self.assertEqual(entry["proto"], "Words")


class FlagsAndDerivedTest(unittest.TestCase):
    def test_flags_bitmask_has_no_proto_field(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {"Opt": [proto_field("title", 1, "string")]}
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("opt", [tl_field("flags", "#"), tl_field("title", "flags.0?string")]),
            messages,
        )
        self.assertEqual(entry["proto"], "Opt")
        flags: Dict[str, Any] = find_field(entry, "flags")
        self.assertIsNone(flags["proto_field"])
        self.assertFalse(flags["discard_only"])
        self.assertEqual(find_field(entry, "title")["proto_field"], 1)

    def test_flagged_vector_is_still_repeated(self) -> None:
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("opt", [tl_field("ids", "flags.2?Vector<int>")]),
            {"Opt": [proto_field("ids", 1, "int32", repeated=True)]},
        )
        self.assertEqual(entry["proto"], "Opt")

    def test_flagged_bool_true_is_a_bool(self) -> None:
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("opt", [tl_field("pinned", "flags.1?true")]),
            {"Opt": [proto_field("pinned", 1, "bool")]},
        )
        self.assertEqual(entry["proto"], "Opt")
        self.assertEqual(find_field(entry, "pinned")["proto_type"], "bool")

    def test_has_flag_is_derived_from_its_field(self) -> None:
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("opt", [tl_field("has_title", "flags.0?true"), tl_field("title", "flags.0?string")]),
            {"Opt": [proto_field("title", 1, "string")]},
        )
        self.assertEqual(entry["proto"], "Opt")
        has: Dict[str, Any] = find_field(entry, "has_title")
        self.assertEqual(has["derived_from"], "title")
        self.assertIsNone(has["proto_field"])
        self.assertFalse(has["discard_only"])

    def test_has_flag_without_its_field_is_discard_only(self) -> None:
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("opt", [tl_field("has_title", "Bool")]),
            {"Opt": []},
        )
        self.assertEqual(entry["proto"], "Opt")
        self.assertTrue(find_field(entry, "has_title")["discard_only"])


class UnmappedTest(unittest.TestCase):
    def test_tl_field_missing_on_server_is_discard_only(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {"Msg": [proto_field("id", 1, "int32")]}
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("msg", [tl_field("id", "int"), tl_field("extra", "string")]),
            messages,
        )
        self.assertEqual(entry["proto"], "Msg")
        extra: Dict[str, Any] = find_field(entry, "extra")
        self.assertTrue(extra["discard_only"])
        self.assertIsNone(extra["proto_field"])
        self.assertEqual(extra["tl_type"], "string")

    def test_server_only_fields_are_not_listed(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {
            "Msg": [proto_field("id", 1, "int32"), proto_field("server_only", 2, "string")],
        }
        entry: Dict[str, Any] = pr.pair_constructor(tl_ctor("msg", [tl_field("id", "int")]), messages)
        self.assertEqual([f["name"] for f in entry["fields"]], ["id"])

    def test_field_order_follows_tl(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {
            "Ord": [proto_field("a", 1, "int32"), proto_field("b", 2, "int32")],
        }
        entry: Dict[str, Any] = pr.pair_constructor(
            tl_ctor("ord", [tl_field("b", "int"), tl_field("a", "int")]),
            messages,
        )
        self.assertEqual([f["name"] for f in entry["fields"]], ["b", "a"])
        self.assertEqual(entry["fields"][0]["proto_field"], 2)


class BatchTest(unittest.TestCase):
    def test_pair_constructors_keeps_input_order(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {"First": [proto_field("x", 1, "int32")]}
        ctors: List[Dict[str, Any]] = [
            tl_ctor("first", [tl_field("x", "int")]),
            tl_ctor("second", [tl_field("x", "int")]),
        ]
        out: List[Dict[str, Any]] = pr.pair_constructors(ctors, messages)
        self.assertEqual([e["name"] for e in out], ["first", "second"])
        self.assertEqual(out[0]["proto"], "First")
        self.assertIsNone(out[1]["proto"])

    def test_pairing_does_not_mutate_input(self) -> None:
        messages: Dict[str, List[Dict[str, Any]]] = {"Keep": [proto_field("x", 1, "int32")]}
        ctor: Dict[str, Any] = tl_ctor("keep", [tl_field("x", "int")])
        pr.pair_constructor(ctor, messages)
        self.assertEqual(ctor["fields"], [tl_field("x", "int")])
        self.assertNotIn("proto", ctor)


if __name__ == "__main__":
    unittest.main()
