"""Generates the TL layer 232 to ImpulseM proto mapping for the desktop clients.

The client scheme is Telegram/SourceFiles/mtproto/scheme/api.tl (layer 232). It is
paired by name with the server protos vendored in tools/impulse/protos. Those files
are copied unchanged from the ImpulseM server repository (master, commit
7ba875049cb6c38f5edf40975c304393755fbf44) and stay on layer 229.

Pairing is tolerant. A constructor the server cannot carry gets no proto. A method
the server cannot carry is left out, so the client answers it locally as not mapped.
A parameter the server does not carry is kept and flagged as unmapped, so the
transcoder refuses to send it. Every shared field is checked by wire type, not only
by name.

Usage: python -I tools/impulse/gen_tl_proto_232.py --out <file.json.gz>
"""
import argparse
import gzip
import json
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

TOOLS_DIR: Path = Path(__file__).resolve().parent
REPO_ROOT: Path = TOOLS_DIR.parent.parent
SCHEMA_PATH: Path = REPO_ROOT / "Telegram" / "SourceFiles" / "mtproto" / "scheme" / "api.tl"
PROTOS_DIR: Path = TOOLS_DIR / "protos"

# Isolated mode (python -I) leaves the script directory off sys.path, so add the tools directory explicitly.
sys.path.insert(0, str(TOOLS_DIR))

from tl232.hidden_list import HIDDEN_METHODS  # noqa: E402

LAYER: int = 232
VECTOR_PREDICATE: str = "vector"
WRAPPER_IDS: Dict[str, int] = {
    "invokeAfterMsg": 0xcb9f372d,
    "invokeAfterMsgs": 0x3dc4b4f0,
    "initConnection": 0xc1cd5ea9,
    "invokeWithLayer": 0xda9b0d0d,
    "invokeWithoutUpdates": 0xbf9459b7,
    "invokeWithMessagesRange": 0x365275f2,
    "invokeWithTakeout": 0xaca9fd2e,
    "invokeWithBusinessConnection": 0xdd289f8e,
    "invokeWithGooglePlayIntegrity": 0x1df92984,
    "invokeWithApnsSecret": 0x0dae54f8,
    "invokeWithReCaptcha": 0xadbb0f94,
}
CONSTANT_IDS: Dict[str, int] = {
    "boolFalse": 0xbc799737,
    "boolTrue": 0x997275b5,
    "vector": 0x1cb5c415,
}
EXCLUDED_METHODS: Dict[str, str] = {
    "payments.getStarGiftWithdrawalUrl": "layer 232 replaces password with tmp_password; blocked semantically",
}
CAPTURE_NAMES: Set[str] = {"session_token", "refresh_token"}
PRIMITIVES: Set[str] = {"int", "long", "double", "string", "bytes", "int128", "int256"}
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
PRIMITIVE_LIST_ELEMS: Dict[str, str] = {
    "int": "IntResponse",
    "long": "LongResponse",
    "double": "DoubleResponse",
    "string": "StringResponse",
    "bytes": "BytesResponse",
}

TL_HEAD_RE: re.Pattern = re.compile(r"^([A-Za-z_][\w.]*)#([0-9a-f]+)$")
TL_PARAM_RE: re.Pattern = re.compile(r"^([A-Za-z_]\w*):(\S+)$")
TL_TEMPLATE_RE: re.Pattern = re.compile(r"\{[^{}]*\}")
TL_LAYER_RE: re.Pattern = re.compile(r"^// LAYER (\d+)$")
PROTO_FIELD_RE: re.Pattern = re.compile(r"^(?:(optional|repeated)\s+)?([A-Za-z_][\w.]*(?:<[^>]*>)?)\s+(\w+)\s*=\s*(\d+)\s*(?:\[[^\]]*\])?\s*;$")
PROTO_RPC_RE: re.Pattern = re.compile(r"^rpc\s+(\w+)\s*\(\s*(?:stream\s+)?([\w.]+)\s*\)\s*returns\s*\(\s*(?:stream\s+)?([\w.]+)\s*\)\s*(?:;|\{\s*\})$")

Param = Dict[str, Any]
Message = Dict[str, Any]
Services = Dict[Tuple[str, str], Dict[str, Tuple[str, str]]]


def pascal(s: str) -> str:
    out: List[str] = []
    upNext: bool = True
    for ch in s:
        if ch == "_" or ch == ".":
            upNext = True
            continue
        out.append(ch.upper() if upNext else ch)
        upNext = False
    return "".join(out)


def snake(s: str) -> str:
    out: List[str] = []
    for i, ch in enumerate(s):
        if ch.isupper() and i > 0 and not s[i - 1].isupper():
            out.append("_")
        out.append(ch.lower())
    return "".join(out).replace(".", "_")


def type_message(tlType: str) -> str:
    return pascal(tlType.replace(".", "_"))


def normalize_proto_type(protoType: str) -> str:
    return protoType.split(".")[-1]


def parse_tl_line(line: str) -> Dict[str, Any]:
    body: str = line.strip()
    if not body.endswith(";"):
        raise ValueError("TL line without ';': %s" % line)
    lhs: str
    sep: str
    resultType: str
    lhs, sep, resultType = body[:-1].rpartition(" = ")
    if not sep:
        raise ValueError("TL line without ' = ': %s" % line)
    tokens: List[str] = TL_TEMPLATE_RE.sub(" ", lhs).split()
    head: Optional[re.Match] = TL_HEAD_RE.match(tokens[0])
    if head is None:
        raise ValueError("TL line without id: %s" % line)
    predicate: str = head.group(1)
    params: List[Dict[str, str]] = []
    if predicate != VECTOR_PREDICATE:
        for token in tokens[1:]:
            pm: Optional[re.Match] = TL_PARAM_RE.match(token)
            if pm is None:
                raise ValueError("TL parameter %r is malformed: %s" % (token, line))
            params.append({"name": pm.group(1), "type": pm.group(2)})
    return {
        "id": int(head.group(2), 16),
        "params": params,
        "predicate": predicate,
        "result": resultType.strip(),
    }


def parse_scheme_text(text: str) -> Dict[str, Any]:
    layer: Optional[int] = None
    constructors: List[Dict[str, Any]] = []
    functions: List[Dict[str, Any]] = []
    inFunctions: bool = False
    for rawLine in text.splitlines():
        line: str = rawLine.strip()
        layerMatch: Optional[re.Match] = TL_LAYER_RE.match(line)
        if layerMatch is not None:
            layer = int(layerMatch.group(1))
            continue
        if not line or line.startswith("//"):
            continue
        if line == "---functions---":
            inFunctions = True
            continue
        entry: Dict[str, Any] = parse_tl_line(line)
        if inFunctions:
            functions.append(entry)
        else:
            constructors.append(entry)
    if layer != LAYER:
        raise ValueError("scheme layer %s, expected %d" % (layer, LAYER))
    return {"layer": layer, "constructors": constructors, "methods": functions}


def load_scheme(path: Path) -> Dict[str, Any]:
    return parse_scheme_text(path.read_text(encoding="utf-8"))


def parse_tl_type(tlType: str, where: str) -> Dict[str, Any]:
    if tlType == "#":
        return {"kind": "flags"}
    flagMatch: Optional[re.Match] = re.match(r"^(flags2?)\.(\d+)\?(.+)$", tlType)
    if flagMatch is not None:
        inner: Dict[str, Any] = parse_tl_type(flagMatch.group(3), where)
        inner["flag"] = [flagMatch.group(1), int(flagMatch.group(2))]
        return inner
    if tlType.startswith("!"):
        return {"kind": "bang", "type": tlType[1:]}
    if tlType == "true" or tlType == "Bool" or tlType in PRIMITIVES:
        return {"kind": tlType}
    vectorMatch: Optional[re.Match] = re.match(r"^Vector<(.+)>$", tlType)
    if vectorMatch is not None:
        return {"kind": "vector", "elem": parse_tl_type(vectorMatch.group(1), where)}
    if re.match(r"^[A-Za-z_][\w.]*$", tlType) is None:
        raise ValueError("unsupported TL type %r at %s" % (tlType, where))
    return {"kind": "object", "type": tlType}


def parse_params(tlParams: List[Dict[str, str]], where: str) -> List[Param]:
    parsed: List[Param] = []
    for p in tlParams:
        entry: Param = parse_tl_type(p["type"], where + "." + p["name"])
        entry["name"] = p["name"]
        parsed.append(entry)
    return parsed


def elem_matches(tlEntry: Param, protoType: str) -> bool:
    kind: str = tlEntry["kind"]
    if kind == "object":
        return normalize_proto_type(protoType) == type_message(tlEntry["type"])
    accepted: Optional[Set[str]] = SCALAR_PROTO_TYPES.get(kind)
    return accepted is not None and protoType in accepted


def field_matches(tlEntry: Param, protoType: str, protoLabel: str) -> bool:
    """Wire-type check of one shared field: scalar, message or repeated list."""
    if tlEntry["kind"] == "vector":
        return protoLabel == "repeated" and elem_matches(tlEntry["elem"], protoType)
    return protoLabel != "repeated" and elem_matches(tlEntry, protoType)


def pair_params(params: List[Param], message: Message) -> Optional[str]:
    """Fills field numbers and flags unmapped params in place. Returns the first wire-type conflict, if any."""
    fields: Dict[str, Tuple[int, str, str]] = message["fields"]
    tlNames: Set[str] = {p["name"] for p in params}
    for entry in params:
        if entry["kind"] == "flags":
            continue
        name: str = entry["name"]
        if name in fields:
            number: int
            protoType: str
            label: str
            number, protoType, label = fields[name]
            if not field_matches(entry, protoType, label):
                return "field %s: TL %s, server %s %s" % (name, entry["kind"], label, protoType)
            entry["field"] = number
        elif name.startswith("has_") and entry["kind"] in ("true", "Bool") and name[4:] in tlNames:
            entry["derived_from"] = name[4:]
        else:
            entry["unmapped"] = True
    return None


def capture_fields(message: Message, tlNames: Set[str], overlayFields: Set[Tuple[str, str, str]]) -> List[Dict[str, Any]]:
    captures: List[Dict[str, Any]] = []
    for fieldName, info in message["fields"].items():
        if fieldName in tlNames or fieldName not in CAPTURE_NAMES:
            continue
        if (message["file"], message["name"], fieldName) not in overlayFields:
            continue
        captures.append({"field": info[0], "name": fieldName})
    captures.sort(key=lambda c: int(c["field"]))
    return captures


def resolve_constructor_message(
    predicate: str,
    tlType: str,
    polymorphic: bool,
    v1: Dict[str, Message],
) -> Tuple[Optional[str], Optional[int]]:
    """Returns (proto message, oneof arm number). Both None when the server has no such arm."""
    if not polymorphic:
        name: str = type_message(tlType)
        return (name if name in v1 else None), None
    typeMsg: str = type_message(tlType)
    armMsg: str = pascal(predicate)
    if armMsg == typeMsg:
        armMsg += "Value"
    info: Optional[Message] = v1.get(typeMsg)
    if info is None:
        return None, None
    key: str = snake(predicate)
    if key not in info["oneof"]:
        return None, None
    armNum: int
    armType: str
    armNum, armType = info["oneof"][key]
    if normalize_proto_type(armType) != armMsg or armMsg not in v1:
        return None, None
    return armMsg, armNum


def build_constructor(
    ctor: Dict[str, Any],
    polymorphic: bool,
    v1: Dict[str, Message],
    overlayFields: Set[Tuple[str, str, str]],
) -> Tuple[Dict[str, Any], Optional[str]]:
    where: str = "constructor " + ctor["predicate"]
    tlNames: Set[str] = {p["name"] for p in ctor["params"]}
    entry: Dict[str, Any] = {
        "arm": None,
        "capture": [],
        "id": ctor["id"],
        "params": parse_params(ctor["params"], where),
        "predicate": ctor["predicate"],
        "proto": None,
        "type": ctor["result"],
    }
    protoName: Optional[str]
    armNum: Optional[int]
    protoName, armNum = resolve_constructor_message(ctor["predicate"], ctor["result"], polymorphic, v1)
    if protoName is None:
        return entry, "no server message"
    problem: Optional[str] = pair_params(entry["params"], v1[protoName])
    if problem is not None:
        entry["params"] = parse_params(ctor["params"], where)
        return entry, problem
    entry["proto"] = protoName
    entry["arm"] = armNum
    entry["capture"] = capture_fields(v1[protoName], tlNames, overlayFields)
    return entry, None


def response_message(
    messages: Dict[str, Dict[str, Message]],
    package: str,
    rpcReturn: str,
) -> Optional[Message]:
    """Resolves an rpc return type as protoc does: in the service's package first, then in v1."""
    if rpcReturn.startswith("v1."):
        return messages["v1"].get(rpcReturn[3:])
    return messages.get(package, {}).get(rpcReturn) or messages["v1"].get(rpcReturn)


def is_bool_message(message: Optional[Message]) -> bool:
    """A TL Bool result travels as a message that holds only `optional bool value = 1`."""
    return message is not None and not message["oneof"] and message["fields"] == {"value": (1, "bool", "optional")}


def result_of(
    tlResult: str,
    rpcReturn: str,
    rpcMessage: Optional[Message],
) -> Optional[Dict[str, Any]]:
    """The result kind of a method, or None when the server return does not carry it."""
    if tlResult == "Bool":
        return {"kind": "bool"} if is_bool_message(rpcMessage) else None
    if rpcReturn == "IntResponse" or rpcReturn == "LongResponse":
        kind: str = "int" if rpcReturn == "IntResponse" else "long"
        return {"kind": kind} if tlResult == kind else None
    vectorMatch: Optional[re.Match] = re.match(r"^Vector<(.+)>$", tlResult)
    if vectorMatch is not None:
        elem: Param = parse_tl_type(vectorMatch.group(1), tlResult)
        expected: str
        if elem["kind"] == "object":
            expected = "v1." + type_message(elem["type"]) + "List"
        elif elem["kind"] in PRIMITIVE_LIST_ELEMS:
            expected = PRIMITIVE_LIST_ELEMS[elem["kind"]] + "List"
        else:
            return None
        return {"kind": "list", "elem": elem} if rpcReturn == expected else None
    expectedObject: str = "v1." + type_message(tlResult)
    return {"kind": "object", "type": tlResult} if rpcReturn == expectedObject else None


def build_method(
    method: Dict[str, Any],
    messages: Dict[str, Dict[str, Message]],
    services: Services,
    overlayFields: Set[Tuple[str, str, str]],
) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    name: str = method["predicate"]
    ns: str
    rest: str
    ns, rest = name.split(".", 1)
    package: str = "v1." + ns
    service: str = pascal(ns)
    rpcName: str = pascal(rest)
    reqName: str = rpcName + "Request"
    pkgMessages: Dict[str, Message] = messages.get(package, {})
    rpcs: Dict[str, Tuple[str, str]] = services.get((package, service), {})
    if reqName not in pkgMessages:
        return None, "no request message %s.%s" % (package, reqName)
    if rpcName not in rpcs or rpcs[rpcName][0] != reqName:
        return None, "no rpc %s/%s" % (service, rpcName)
    declaredRes: str = rpcs[rpcName][1]
    params: List[Param] = parse_params(method["params"], "method " + name)
    problem: Optional[str] = pair_params(params, pkgMessages[reqName])
    if problem is not None:
        return None, problem
    result: Optional[Dict[str, Any]] = result_of(method["result"], declaredRes, response_message(messages, package, declaredRes))
    if result is None:
        return None, "result %s does not match %s" % (method["result"], declaredRes)
    entry: Dict[str, Any] = {
        "id": method["id"],
        "method": name,
        "params": params,
        "path": "/%s.%s/%s" % (package, service, rpcName),
        "result": result,
    }
    return entry, None


def parse_proto_file(path: Path, root: Path) -> Tuple[str, Dict[str, Message], Services]:
    package: str = ""
    messages: Dict[str, Message] = {}
    services: Services = {}
    # Open scopes: ("message", name), ("oneof", name) or ("service", name).
    stack: List[Tuple[str, str]] = []
    relFile: str = path.relative_to(root).as_posix()
    text: str = re.sub(r"/\*.*?\*/", "", path.read_text(encoding="utf-8"), flags=re.S)
    for rawLine in text.split("\n"):
        line: str = re.sub(r"\s*//.*$", "", rawLine.strip()).strip()
        if not line:
            continue
        packageMatch: Optional[re.Match] = re.match(r"^package\s+([\w.]+)\s*;$", line)
        if packageMatch is not None:
            package = packageMatch.group(1)
            continue
        if re.match(r"^(syntax|option|import|reserved)\b", line) and line.endswith(";"):
            continue
        inlineMatch: Optional[re.Match] = re.match(r"^message\s+(\w+)\s*\{(.*)\}$", line)
        if inlineMatch is not None:
            inlineName: str = inlineMatch.group(1)
            messages[inlineName] = {"fields": {}, "oneof": {}, "file": relFile, "name": inlineName}
            for statement in inlineMatch.group(2).split(";"):
                statement = statement.strip()
                if not statement:
                    continue
                fieldMatch: Optional[re.Match] = PROTO_FIELD_RE.match(statement + ";")
                if fieldMatch is None:
                    raise ValueError("cannot parse inline field in %s: %s" % (path, line))
                messages[inlineName]["fields"][fieldMatch.group(3)] = (int(fieldMatch.group(4)), fieldMatch.group(2), fieldMatch.group(1) or "")
            continue
        openMessage: Optional[re.Match] = re.match(r"^message\s+(\w+)\s*\{$", line)
        if openMessage is not None:
            openName: str = openMessage.group(1)
            messages[openName] = {"fields": {}, "oneof": {}, "file": relFile, "name": openName}
            stack.append(("message", openName))
            continue
        openOneof: Optional[re.Match] = re.match(r"^oneof\s+(\w+)\s*\{$", line)
        if openOneof is not None:
            stack.append(("oneof", openOneof.group(1)))
            continue
        openService: Optional[re.Match] = re.match(r"^service\s+(\w+)\s*\{$", line)
        if openService is not None:
            services[(package, openService.group(1))] = {}
            stack.append(("service", openService.group(1)))
            continue
        if line == "}":
            if not stack:
                raise ValueError("unbalanced } in " + str(path))
            stack.pop()
            continue
        if not stack:
            raise ValueError("unexpected top-level line in %s: %s" % (path, line))
        scopeKind: str
        scopeName: str
        scopeKind, scopeName = stack[-1]
        if scopeKind == "service":
            rpcMatch: Optional[re.Match] = PROTO_RPC_RE.match(line)
            if rpcMatch is None:
                raise ValueError("cannot parse rpc in %s: %s" % (path, line))
            services[(package, scopeName)][rpcMatch.group(1)] = (rpcMatch.group(2), rpcMatch.group(3))
            continue
        fieldMatch = PROTO_FIELD_RE.match(line)
        if fieldMatch is None:
            raise ValueError("cannot parse line in %s: %s" % (path, line))
        label: str = fieldMatch.group(1) or ""
        fieldType: str = fieldMatch.group(2)
        fieldName: str = fieldMatch.group(3)
        number: int = int(fieldMatch.group(4))
        if scopeKind == "oneof":
            owner: str = [n for k, n in stack if k == "message"][-1]
            messages[owner]["oneof"][fieldName] = (number, fieldType)
        else:
            messages[scopeName]["fields"][fieldName] = (number, fieldType, label)
    if stack:
        raise ValueError("unterminated block in " + str(path))
    return package, messages, services


def parse_protos(root: Path) -> Tuple[Dict[str, Dict[str, Message]], Services]:
    """Parses every .proto in root and root/v1. Messages are keyed by package."""
    allMessages: Dict[str, Dict[str, Message]] = {}
    allServices: Services = {}
    files: List[Path] = sorted(root.glob("*.proto")) + sorted((root / "v1").glob("*.proto"))
    for f in files:
        package: str
        messages: Dict[str, Message]
        services: Services
        package, messages, services = parse_proto_file(f, root)
        target: Dict[str, Message] = allMessages.setdefault(package, {})
        for name, info in messages.items():
            if name in target:
                raise ValueError("duplicate message %s.%s" % (package, name))
            target[name] = info
        allServices.update(services)
    return allMessages, allServices


def load_overlay_fields(path: Path) -> Set[Tuple[str, str, str]]:
    """(file, message, field) for every kind=field overlay entry: ImpulseM extension fields."""
    doc: Dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    return {(e["file"], e["message"], e["name"]) for e in doc["entries"] if e["kind"] == "field"}


def check_wrappers(methods: List[Dict[str, Any]]) -> None:
    """Every method without a namespace must be one of the literal wrapper ids."""
    found: Dict[str, int] = {}
    for m in methods:
        if "." not in m["predicate"]:
            found[m["predicate"]] = m["id"]
    if set(found) != set(WRAPPER_IDS):
        raise ValueError("wrapper set %s, expected %s" % (sorted(found), sorted(WRAPPER_IDS)))
    for name, wrapperId in WRAPPER_IDS.items():
        if found[name] != wrapperId:
            raise ValueError("wrapper %s id %08x, expected %08x" % (name, found[name], wrapperId))


def build_mapping(scheme: Dict[str, Any], protosDir: Path) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    messages, services = parse_protos(protosDir)
    if "v1" not in messages:
        raise ValueError("no v1 package under " + str(protosDir))
    v1: Dict[str, Message] = messages["v1"]
    overlayFields: Set[Tuple[str, str, str]] = load_overlay_fields(protosDir / "overlays.json")
    check_wrappers(scheme["methods"])

    typeCounts: Dict[str, int] = {}
    for c in scheme["constructors"]:
        typeCounts[c["result"]] = typeCounts.get(c["result"], 0) + 1

    constructors: List[Dict[str, Any]] = []
    unmappedConstructors: List[str] = []
    typeArms: Dict[str, Dict[str, int]] = {}
    for c in scheme["constructors"]:
        polymorphic: bool = typeCounts[c["result"]] > 1
        entry: Dict[str, Any]
        entry, problem = build_constructor(c, polymorphic, v1, overlayFields)
        if problem is not None:
            unmappedConstructors.append(c["predicate"] + " (" + problem + ")")
        elif polymorphic:
            typeArms.setdefault(c["result"], {})[str(entry["arm"])] = entry["id"]
        constructors.append(entry)

    methods: List[Dict[str, Any]] = []
    absentMethods: Dict[str, str] = {}
    for m in scheme["methods"]:
        if "." not in m["predicate"]:
            continue
        if m["predicate"] in EXCLUDED_METHODS:
            absentMethods[m["predicate"]] = EXCLUDED_METHODS[m["predicate"]]
            continue
        if m["predicate"] in HIDDEN_METHODS:
            absentMethods[m["predicate"]] = "hidden list: mapping omission, not a UI hide"
            continue
        methodEntry: Optional[Dict[str, Any]]
        methodProblem: Optional[str]
        methodEntry, methodProblem = build_method(m, messages, services, overlayFields)
        if methodEntry is None:
            absentMethods[m["predicate"]] = methodProblem or "unknown"
        else:
            methods.append(methodEntry)

    types: List[Dict[str, Any]] = []
    seenTypes: Set[str] = set()
    for c in scheme["constructors"]:
        tlType: str = c["result"]
        if tlType in seenTypes:
            continue
        seenTypes.add(tlType)
        types.append({
            "arms": typeArms.get(tlType, {}),
            "name": tlType,
            "polymorphic": typeCounts[tlType] > 1,
            "proto": type_message(tlType),
        })

    unmappedParams: int = 0
    for entry in constructors + methods:
        unmappedParams += len([p for p in entry["params"] if p.get("unmapped")])

    mapping: Dict[str, Any] = {
        "constants": dict(CONSTANT_IDS),
        "constructors": constructors,
        "layer": LAYER,
        "methods": methods,
        "types": types,
        "wrappers": dict(WRAPPER_IDS),
    }
    report: Dict[str, Any] = {
        "absentMethods": absentMethods,
        "constructors": len(scheme["constructors"]),
        "functions": len(scheme["methods"]),
        "mappedConstructors": len([c for c in constructors if c["proto"] is not None]),
        "mappedMethods": len(methods),
        "unmappedConstructors": unmappedConstructors,
        "unmappedParams": unmappedParams,
    }
    return mapping, report


def generate(
    outPath: Path,
    schemaPath: Path = SCHEMA_PATH,
    protosDir: Path = PROTOS_DIR,
) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    mapping, report = build_mapping(load_scheme(schemaPath), protosDir)
    data: bytes = json.dumps(mapping, sort_keys=True, separators=(",", ":")).encode("utf-8")
    outPath.parent.mkdir(parents=True, exist_ok=True)
    outPath.write_bytes(gzip.compress(data, compresslevel=9, mtime=0))
    return mapping, report


def main() -> None:
    parser: argparse.ArgumentParser = argparse.ArgumentParser(description="Generate the TL layer 232 proto mapping.")
    parser.add_argument("--out", required=True, type=Path, help="gzip JSON file to write")
    args: argparse.Namespace = parser.parse_args()
    mapping: Dict[str, Any]
    report: Dict[str, Any]
    mapping, report = generate(args.out)
    print("layer: %d" % mapping["layer"])
    print("constructors: %d (mapped %d)" % (report["constructors"], report["mappedConstructors"]))
    print("functions: %d (mapped %d)" % (report["functions"], report["mappedMethods"]))
    print("unmapped params: %d" % report["unmappedParams"])
    print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
