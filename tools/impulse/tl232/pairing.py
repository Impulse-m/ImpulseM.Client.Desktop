"""Pairs TL layer 232 constructors with server proto messages by name and wire kind.

A constructor pairs with the proto message named after it in PascalCase: `peerUser`
with `PeerUser`, `account.foo` with `AccountFoo`. Every shared field is checked by
wire kind as well as by name, because a name-only match sends a field with the wrong
wire type.

Tolerant rules:
- A constructor with no server message, or with a wire-kind mismatch on a shared
  field, gets `proto: None`. Its fields are left unpaired and `problem` says why.
- A TL field the server message does not carry is kept with `discard_only: True`.
  It is never dropped silently.
- The `flags` bitmask field has no proto field. It is kept with `proto_field: None`.
- A `has_<name>` boolean whose `<name>` field is in the same constructor is derived
  from that field's presence. It is kept with `derived_from` set.

Input shapes:
  TL constructor: {"name": str, "id": int, "layer": int,
                   "fields": [{"name": str, "tl_type": str}]}
  Proto messages: {message name: [{"name": str, "number": int, "proto_type": str,
                                   "repeated": bool, "oneof": Optional[str]}]}

Output shape: the TL constructor with `proto` (the message name or None) and
`problem` (None or a reason). Each field carries `name`, `tl_type`, `proto_field`,
`proto_type`, `discard_only` and `derived_from`.
"""
import re
from typing import Any, Dict, List, Optional, Set, Tuple


FLAGS_TYPE: str = "#"
FLAG_PREFIX_RE: re.Pattern = re.compile(r"^flags2?\.\d+\?")
VECTOR_RE: re.Pattern = re.compile(r"^Vector<(.+)>$")
DERIVED_PREFIX: str = "has_"
SCALAR_PROTO_TYPES: Dict[str, Set[str]] = {
    "int": {"int32"},
    "long": {"int64"},
    "double": {"double"},
    "string": {"string"},
    "bytes": {"bytes"},
    "int128": {"bytes"},
    "int256": {"bytes"},
    "Bool": {"bool"},
    "true": {"bool"},
}
BOOL_TYPES: Set[str] = {"Bool", "true"}

TlField = Dict[str, Any]
TlConstructor = Dict[str, Any]
ProtoField = Dict[str, Any]
ProtoMessages = Dict[str, List[ProtoField]]


def pascal(tlName: str) -> str:
    out: List[str] = []
    upNext: bool = True
    for ch in tlName:
        if ch == "_" or ch == ".":
            upNext = True
            continue
        out.append(ch.upper() if upNext else ch)
        upNext = False
    return "".join(out)


def normalize_tl_type(tlType: str) -> str:
    """Drops the `!` marker and the `flags.N?` prefix, which do not change the wire kind."""
    bare: str = tlType[1:] if tlType.startswith("!") else tlType
    return FLAG_PREFIX_RE.sub("", bare)


def normalize_proto_type(protoType: str) -> str:
    return protoType.split(".")[-1]


def vector_element(tlType: str) -> Optional[str]:
    match: Optional[re.Match] = VECTOR_RE.match(tlType)
    if match is None:
        return None
    return match.group(1)


def element_matches(tlType: str, protoType: str) -> bool:
    bare: str = normalize_tl_type(tlType)
    accepted: Optional[Set[str]] = SCALAR_PROTO_TYPES.get(bare)
    if accepted is not None:
        return protoType in accepted
    return normalize_proto_type(protoType) == pascal(bare)


def field_matches(tlType: str, protoField: ProtoField) -> bool:
    """Wire-kind check of one shared field: scalar, message or repeated list."""
    bare: str = normalize_tl_type(tlType)
    protoType: str = protoField["proto_type"]
    repeated: bool = bool(protoField["repeated"])
    element: Optional[str] = vector_element(bare)
    if element is not None:
        return repeated and element_matches(element, protoType)
    return not repeated and element_matches(bare, protoType)


def is_derived_flag(name: str, tlType: str, tlNames: Set[str]) -> bool:
    """A `has_<x>` boolean whose `<x>` field exists in the same constructor."""
    if not name.startswith(DERIVED_PREFIX):
        return False
    if normalize_tl_type(tlType) not in BOOL_TYPES:
        return False
    return name[len(DERIVED_PREFIX):] in tlNames


def make_field(name: str, tlType: str) -> Dict[str, Any]:
    return {
        "name": name,
        "tl_type": tlType,
        "proto_field": None,
        "proto_type": None,
        "discard_only": False,
        "derived_from": None,
    }


def unpaired_fields(tlFields: List[TlField]) -> List[Dict[str, Any]]:
    return [make_field(f["name"], f["tl_type"]) for f in tlFields]


def pair_fields(
    tlFields: List[TlField],
    protoFields: List[ProtoField],
) -> Tuple[List[Dict[str, Any]], Optional[str]]:
    """Returns the paired fields and None, or the unpaired fields and the first mismatch."""
    protoByName: Dict[str, ProtoField] = {p["name"]: p for p in protoFields}
    tlNames: Set[str] = {f["name"] for f in tlFields}
    paired: List[Dict[str, Any]] = []
    for tlField in tlFields:
        name: str = tlField["name"]
        tlType: str = tlField["tl_type"]
        entry: Dict[str, Any] = make_field(name, tlType)
        if tlType == FLAGS_TYPE:
            paired.append(entry)
            continue
        protoField: Optional[ProtoField] = protoByName.get(name)
        if protoField is not None:
            if not field_matches(tlType, protoField):
                label: str = "repeated" if protoField["repeated"] else "singular"
                reason: str = "field %s: TL %s does not match server %s %s" % (
                    name,
                    tlType,
                    label,
                    protoField["proto_type"],
                )
                return unpaired_fields(tlFields), reason
            entry["proto_field"] = int(protoField["number"])
            entry["proto_type"] = normalize_proto_type(protoField["proto_type"])
        elif is_derived_flag(name, tlType, tlNames):
            entry["derived_from"] = name[len(DERIVED_PREFIX):]
        else:
            entry["discard_only"] = True
        paired.append(entry)
    return paired, None


def pair_constructor(ctor: TlConstructor, messages: ProtoMessages) -> Dict[str, Any]:
    protoName: str = pascal(ctor["name"])
    entry: Dict[str, Any] = {
        "name": ctor["name"],
        "id": ctor["id"],
        "layer": ctor["layer"],
        "proto": None,
        "fields": [],
        "problem": None,
    }
    protoFields: Optional[List[ProtoField]] = messages.get(protoName)
    if protoFields is None:
        entry["fields"] = unpaired_fields(ctor["fields"])
        entry["problem"] = "no server message %s" % protoName
        return entry
    fields: List[Dict[str, Any]]
    problem: Optional[str]
    fields, problem = pair_fields(ctor["fields"], protoFields)
    entry["fields"] = fields
    if problem is not None:
        entry["problem"] = problem
        return entry
    entry["proto"] = protoName
    return entry


def pair_constructors(ctors: List[TlConstructor], messages: ProtoMessages) -> List[Dict[str, Any]]:
    return [pair_constructor(ctor, messages) for ctor in ctors]
