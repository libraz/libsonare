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

from extractors.cpp_struct import extract_struct_defaults, scan_constants

# Headers always scanned for named numeric constants (in addition to every
# header named in core_map.toml, where module-local constants like
# ``chord_constants::*`` live).
_GLOBAL_CONSTANT_HEADERS = ("src/util/constants.h",)


@dataclass
class CoreConfig:
    """One canonical key's link to its C++ core config struct."""

    key: str
    header: str
    struct: str
    # struct_field -> literal default (numeric / boolean only).
    fields: dict[str, str] = field(default_factory=dict)
    # facade_param -> struct_field, when the facade spells a field differently.
    rename: dict[str, str] = field(default_factory=dict)

    def core_default_for(self, facade_param: str) -> str | None:
        """The core literal default for a facade param name, or None."""
        return self.fields.get(self.rename.get(facade_param, facade_param))


def load(map_path: Path, root: Path) -> dict[str, CoreConfig]:
    """Load ``core_map.toml`` and resolve every struct's literal defaults."""
    if not map_path.exists():
        return {}
    data = tomllib.loads(map_path.read_text(encoding="utf-8"))
    entries = data.get("map", {})
    # Scan constants from the global header(s) plus every mapped struct's header
    # (module-local constants such as ``chord_constants::*`` are defined there).
    const_headers = [root / h for h in _GLOBAL_CONSTANT_HEADERS]
    const_headers += [root / spec["header"] for spec in entries.values()]
    constants = scan_constants(const_headers)
    out: dict[str, CoreConfig] = {}
    for key, spec in entries.items():
        header = spec["header"]
        struct = spec["struct"]
        rename = dict(spec.get("rename", {}))
        fields = extract_struct_defaults(root / header, struct, constants)
        out[key] = CoreConfig(
            key=key, header=header, struct=struct, fields=fields, rename=rename
        )
    return out
