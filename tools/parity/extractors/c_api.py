"""Extract the canonical C API surface from sonare_c.h and its generated headers.

The C API is the canonical ABI: it defines the authoritative function NAME,
PARAMETER ORDER and PARAM TYPES. C has no default values.

Parsing strategy: C declarations are not regular, but the libsonare headers are
machine-generated / hand-written in a consistent style: each declaration is a
return-type + ``sonare_<name>(`` + comma-separated params, terminated by ``);``.
We strip comments, join continuation lines, then split on the top-level ``;``.
"""

from __future__ import annotations

import re
from pathlib import Path

from model import Extraction, FunctionSig, Param
from normalize import canonical_key

# Return types we accept as the start of a real declaration.
_RETURN_TYPES = (
    "SonareError",
    "void",
    "int",
    "float",
    "double",
    "size_t",
    "bool",
    "const char*",
    "const float*",
    "const int*",
    "uint32_t",
    "uint64_t",
    "int32_t",
    "int64_t",
)

_DECL_RE = re.compile(
    r"\b(?P<ret>" + "|".join(re.escape(t) for t in _RETURN_TYPES) + r")\s+"
    r"(?P<name>sonare_[A-Za-z0-9_]+)\s*\((?P<args>.*?)\)\s*;",
    re.DOTALL,
)

# Out-pointer / length / scalar params that the TS/py facades fold away or
# represent differently. These are structural, not positional features.
# Matched with re.search so suffixes like ``_count`` / ``_out`` are caught.
_OUT_NAME_RE = re.compile(r"^out(_|$)|_count$|_out$|^out$")

# Pure C-ABI plumbing param names that have no facade analog (callback bridge,
# user-data cookies, length cookies). Folded away by every facade.
_PLUMBING_NAMES = {
    "callback",
    "user_data",
    "userdata",
    "json_out",
    "out_json",
    "param_count",
    "override_count",
    "mode_count",
}


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def _split_args(args: str) -> list[str]:
    """Split a C arg list on top-level commas (no nested templates here)."""
    args = args.strip()
    if not args or args == "void":
        return []
    parts = []
    depth = 0
    cur = []
    for ch in args:
        if ch in "([<":
            depth += 1
        elif ch in ")]>":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def _parse_param(decl: str) -> Param:
    """Parse a single C parameter declaration into a Param."""
    decl = decl.strip()
    # The identifier is the last token (handles ``const SonareConfig* config``,
    # ``float** out``, ``size_t* out_count``). Pointers/qualifiers belong to type.
    m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", decl)
    name = m.group(1) if m else ""
    ctype = decl[: m.start()].strip() if m else decl
    # Reattach pointer stars that sit before the name.
    ptr = decl[len(ctype) : m.start()].strip() if m else ""
    full_type = (ctype + ptr).strip()

    structural = False
    is_ptr = "*" in decl
    lname = name.lower()
    if _OUT_NAME_RE.search(lname):
        structural = True  # out_*, out, *_count, *_out
    elif lname in _PLUMBING_NAMES:
        structural = True  # callback/user_data/json_out/... no facade analog
    elif lname in ("samples", "data", "length", "sample_rate", "size", "len"):
        structural = True  # input-buffer plumbing folded by facades
    elif is_ptr and lname in (
        "audio",
        "self",
        "handle",
        "engine",
        "out_key",
        "out_bpm",
    ):
        structural = True
    elif name == "":
        structural = True

    return Param(
        name=name, raw_name=name, default=None, type=full_type, structural=structural
    )


# Local public-API includes: ``#include "sonare_c_effects.h"`` etc. The monolith
# ``sonare_c.h`` was split into domain headers (commit 38aa15c), so the canonical
# C surface is now spread across the headers it pulls in. We follow those local
# includes transitively to reconstruct the full surface. Internal / helper
# headers are excluded -- they hold private plumbing, not the public ABI.
_LOCAL_INCLUDE_RE = re.compile(r'#include\s+"(sonare_c[A-Za-z0-9_]*\.h)"')


def _is_internal_header(name: str) -> bool:
    return name.endswith("_internal.h") or "_helpers" in name


def _collect_api_headers(root: Path) -> list[Path]:
    """All public C-API headers, starting from sonare_c.h and following its
    local ``sonare_c*.h`` includes transitively (excluding internal/helpers)."""
    src = root / "src"
    out: list[Path] = []
    seen: set[Path] = set()
    queue: list[Path] = [src / "sonare_c.h"]
    while queue:
        path = queue.pop(0)
        if path in seen or not path.exists():
            continue
        seen.add(path)
        out.append(path)
        raw = path.read_text(encoding="utf-8")
        for inc in _LOCAL_INCLUDE_RE.findall(raw):
            if _is_internal_header(inc):
                continue
            child = src / inc
            if child not in seen:
                queue.append(child)
    # Generated headers (when codegen is active) as a backstop.
    for gen in sorted((src / "generated").glob("*_gen.h")):
        if gen not in seen:
            out.append(gen)
    return out


def extract(root: Path) -> Extraction:
    ex = Extraction(surface="c")
    files = _collect_api_headers(root)
    seen: set[str] = set()
    for path in files:
        if not path.exists():
            continue
        raw = path.read_text(encoding="utf-8")
        # Map char offset -> line number for diagnostics.
        text = _strip_comments(raw)
        for m in _DECL_RE.finditer(text):
            name = m.group("name")
            key = canonical_key(name, "c")
            if key in seen:
                continue
            args = m.group("args")
            try:
                parts = _split_args(args)
                params = [_parse_param(p) for p in parts]
            except Exception:  # noqa: BLE001 - record, do not crash
                ex.unparsed += 1
                ex.unparsed_notes.append(f"{path.name}: {name} (arg parse failed)")
                continue
            line = raw.count("\n", 0, raw.find(name)) + 1 if name in raw else 0
            seen.add(key)
            ex.functions.append(
                FunctionSig(
                    key=key,
                    surface="c",
                    raw_name=name,
                    params=params,
                    returns=m.group("ret"),
                    file=str(path.relative_to(root)),
                    line=line,
                )
            )
    return ex
