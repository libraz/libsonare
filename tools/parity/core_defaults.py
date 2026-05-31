"""Load the canonical-key -> C++ core config-struct default mapping.

Reads ``core_map.toml`` and, for each entry, extracts the numeric / boolean
field initializers of the named C++ struct (via
``extractors.cpp_struct``). The result feeds ``compare._core_default_drift``,
which compares those "design" defaults against the facade-declared defaults.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from extractors.cpp_struct import (
    extract_func_defaults,
    extract_struct_defaults,
    scan_constants,
)

# Headers always scanned for named numeric constants (in addition to every
# header named in core_map.toml, where module-local constants like
# ``chord_constants::*`` live).
_GLOBAL_CONSTANT_HEADERS = ("src/util/constants.h",)


@dataclass
class CoreConfig:
    """One canonical key's link to its C++ core defaults.

    The anchor is either a config STRUCT (``kind='struct'``, ``name`` is the
    struct) or a free FUNCTION's default arguments (``kind='func'``, ``name`` is
    the C++ function). Either way ``fields`` maps a core field/param name to its
    default; ``rename`` maps a facade param spelling to that core name.
    """

    key: str
    header: str
    name: str
    kind: str = "struct"
    fields: dict[str, str] = field(default_factory=dict)
    rename: dict[str, str] = field(default_factory=dict)

    def core_default_for(self, facade_param: str) -> str | None:
        """The core literal default for a facade param name, or None."""
        return self.fields.get(self.rename.get(facade_param, facade_param))


def load(map_path: Path, root: Path) -> dict[str, CoreConfig]:
    """Load ``core_map.toml`` and resolve every struct's literal defaults."""
    if not map_path.exists():
        return {}
    data = tomllib.loads(map_path.read_text(encoding="utf-8"))
    struct_entries = data.get("map", {})
    func_entries = data.get("func", {})
    # Scan constants from the global header(s) plus every mapped header (module-
    # local constants such as ``chord_constants::*`` live in the struct headers;
    # ``kEpsilon`` / ``kDefaultSampleRate`` appear as free-fn default args).
    const_headers = [root / h for h in _GLOBAL_CONSTANT_HEADERS]
    const_headers += [
        root / spec["header"]
        for spec in (*struct_entries.values(), *func_entries.values())
    ]
    constants = scan_constants(const_headers)
    out: dict[str, CoreConfig] = {}
    for key, spec in struct_entries.items():
        struct = spec["struct"]
        fields = extract_struct_defaults(root / spec["header"], struct, constants)
        out[key] = CoreConfig(
            key=key,
            header=spec["header"],
            name=struct,
            kind="struct",
            fields=fields,
            rename=dict(spec.get("rename", {})),
        )
    for key, spec in func_entries.items():
        func = spec["cpp_func"]
        fields = extract_func_defaults(root / spec["header"], func, constants)
        out[key] = CoreConfig(
            key=key,
            header=spec["header"],
            name=func,
            kind="func",
            fields=fields,
            rename=dict(spec.get("rename", {})),
        )
    return out
