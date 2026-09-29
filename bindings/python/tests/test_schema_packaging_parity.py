"""Every schema the WASM package exports must ship in the Python package too.

bindings/wasm/package.json's `exports` map names each JSON Schema the WASM
package publishes under a `./schemas/*.schema.json` subpath. pyproject.toml's
wheel and sdist `force-include` tables are the Python package's own manifest of
which of those schemas it bundles -- there is no build step deriving one from
the other, so nothing else catches a schema added to one and forgotten in the
other short of building both packages and diffing the file lists.
"""

from __future__ import annotations

import re
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
_SCHEMA_EXPORT = re.compile(r'"\./schemas/([\w.-]+\.schema\.json)"\s*:')


def _wasm_exported_schemas() -> set[str]:
    text = (ROOT / "bindings/wasm/package.json").read_text(encoding="utf-8")
    names = set(_SCHEMA_EXPORT.findall(text))
    assert names, "no schema exports found: the regex above stopped matching package.json"
    return names


def _pyproject_force_include_schemas(table: str) -> set[str]:
    data = tomllib.loads((ROOT / "bindings/python/pyproject.toml").read_text(encoding="utf-8"))
    force_include = data["tool"]["hatch"]["build"]["targets"][table]["force-include"]
    names = set()
    for source in force_include:
        if source.startswith("../../schemas/") and source.endswith(".schema.json"):
            names.add(Path(source).name)
    return names


def test_wheel_force_include_ships_every_wasm_exported_schema() -> None:
    wasm_schemas = _wasm_exported_schemas()
    wheel_schemas = _pyproject_force_include_schemas("wheel")
    assert wasm_schemas <= wheel_schemas, wasm_schemas - wheel_schemas


def test_sdist_force_include_ships_every_wasm_exported_schema() -> None:
    wasm_schemas = _wasm_exported_schemas()
    sdist_schemas = _pyproject_force_include_schemas("sdist")
    assert wasm_schemas <= sdist_schemas, wasm_schemas - sdist_schemas
