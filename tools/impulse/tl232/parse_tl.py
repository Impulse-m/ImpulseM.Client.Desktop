"""Parser for the TL scheme of the desktop client (layer 232).

Reads Telegram/SourceFiles/mtproto/scheme/api.tl. Constructors are the lines
before the ---functions--- marker, methods are the lines after it. Flags
parameters (flags:#, flags.N?type) and vector and bare types are kept as written.

Usage from the generator: parse_tl.load_scheme(parse_tl.SCHEMA_PATH).
"""
import re
from pathlib import Path
from typing import Any, Dict, List, Optional


REPO_ROOT: Path = Path(__file__).resolve().parents[3]
SCHEMA_PATH: Path = REPO_ROOT / "Telegram" / "SourceFiles" / "mtproto" / "scheme" / "api.tl"

LAYER: int = 232
VECTOR_PREDICATE: str = "vector"
FUNCTIONS_MARKER: str = "---functions---"
PRIMITIVES: List[str] = ["int", "long", "double", "string", "bytes", "int128", "int256"]

TL_HEAD_RE: re.Pattern = re.compile(r"^([A-Za-z_][\w.]*)#([0-9a-f]+)$")
TL_PARAM_RE: re.Pattern = re.compile(r"^([A-Za-z_]\w*):(\S+)$")
TL_TEMPLATE_RE: re.Pattern = re.compile(r"\{[^{}]*\}")
TL_LAYER_RE: re.Pattern = re.compile(r"^// LAYER (\d+)$")
TL_FLAG_RE: re.Pattern = re.compile(r"^(flags2?)\.(\d+)\?(.+)$")
TL_VECTOR_RE: re.Pattern = re.compile(r"^Vector<(.+)>$")
TL_NAME_RE: re.Pattern = re.compile(r"^[A-Za-z_][\w.]*$")


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
            paramMatch: Optional[re.Match] = TL_PARAM_RE.match(token)
            if paramMatch is None:
                raise ValueError("TL parameter %r is malformed: %s" % (token, line))
            params.append({"name": paramMatch.group(1), "type": paramMatch.group(2)})
    entry: Dict[str, Any] = {
        "id": int(head.group(2), 16),
        "params": params,
        "predicate": predicate,
        "result": resultType.strip(),
    }
    return entry


def parse_tl_type(tlType: str, where: str) -> Dict[str, Any]:
    if tlType == "#":
        return {"kind": "flags"}
    flagMatch: Optional[re.Match] = TL_FLAG_RE.match(tlType)
    if flagMatch is not None:
        inner: Dict[str, Any] = parse_tl_type(flagMatch.group(3), where)
        inner["flag"] = [flagMatch.group(1), int(flagMatch.group(2))]
        return inner
    if tlType.startswith("!"):
        return {"kind": "bang", "type": tlType[1:]}
    if tlType == "true" or tlType == "Bool" or tlType in PRIMITIVES:
        return {"kind": tlType}
    vectorMatch: Optional[re.Match] = TL_VECTOR_RE.match(tlType)
    if vectorMatch is not None:
        elem: Dict[str, Any] = parse_tl_type(vectorMatch.group(1), where)
        return {"kind": "vector", "elem": elem}
    if TL_NAME_RE.match(tlType) is None:
        raise ValueError("unsupported TL type %r at %s" % (tlType, where))
    return {"kind": "object", "type": tlType}


def parse_params(tlParams: List[Dict[str, str]], where: str) -> List[Dict[str, Any]]:
    parsed: List[Dict[str, Any]] = []
    for tlParam in tlParams:
        entry: Dict[str, Any] = parse_tl_type(tlParam["type"], where + "." + tlParam["name"])
        entry["name"] = tlParam["name"]
        parsed.append(entry)
    return parsed


def parse_scheme_text(text: str) -> Dict[str, Any]:
    layer: Optional[int] = None
    constructors: List[Dict[str, Any]] = []
    methods: List[Dict[str, Any]] = []
    inFunctions: bool = False
    for rawLine in text.splitlines():
        line: str = rawLine.strip()
        layerMatch: Optional[re.Match] = TL_LAYER_RE.match(line)
        if layerMatch is not None:
            layer = int(layerMatch.group(1))
            continue
        if not line or line.startswith("//"):
            continue
        if line == FUNCTIONS_MARKER:
            inFunctions = True
            continue
        entry: Dict[str, Any] = parse_tl_line(line)
        if inFunctions:
            methods.append(entry)
        else:
            constructors.append(entry)
    if layer != LAYER:
        raise ValueError("scheme layer %s, expected %d" % (layer, LAYER))
    return {"layer": layer, "constructors": constructors, "methods": methods}


def load_scheme(path: Path) -> Dict[str, Any]:
    return parse_scheme_text(path.read_text(encoding="utf-8"))
