"""Every flat mastering chain key resolves to a descriptor through the catalog's ``chain`` section.

The key population is the one the conformance gate
``tests/conformance/check_mastering_param_surfaces.py`` derives from the chain's
setter, so a key the setter learns without the catalog learning it fails here.
A stage key is described by its stage's processor, a list element by the list's
processor or its own chain descriptor, and an alias spelling by its canonical key.
"""

from __future__ import annotations

import importlib.util
import re
from pathlib import Path
from typing import Any

import pytest

from libsonare import capability_catalog

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

ROOT = Path(__file__).resolve().parents[3]
_GATE_PATH = ROOT / "tests" / "conformance" / "check_mastering_param_surfaces.py"
_SPEC = importlib.util.spec_from_file_location("libsonare_chain_key_gate", _GATE_PATH)
assert _SPEC is not None and _SPEC.loader is not None
gate = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(gate)

# `key == "saturation.tape." jkey` inside an X-macro: the stage prefix of a table-driven family.
_FAMILY_PREFIX_RE = re.compile(r'key\s*==\s*"([^"]+\.)"\s*jkey')
_TABLE_USE_RE = re.compile(r"SONARE_FIELDS_(\w+)\(X\)")
_INDEX = r"(?:0|[1-9][0-9]*)"


def _table_fields(tables: str, table: str) -> list[str]:
    macro = re.search(rf"#define\s+SONARE_FIELDS_{table}\(X\)((?:[^\n]*\\\n)*[^\n]*)", tables)
    return gate._FIELD_RE.findall(macro.group(1)) if macro else []


def _family_keys(source: str, tables: str) -> dict[str, list[str]]:
    """Each table-driven stage prefix with the keys its field table expands to."""
    families: dict[str, list[str]] = {}
    for match in _FAMILY_PREFIX_RE.finditer(source):
        table = _TABLE_USE_RE.search(source, match.end())
        assert table is not None, match.group(1)
        fields = _table_fields(tables, table.group(1))
        families[match.group(1)] = [match.group(1) + field for field in fields]
    return families


def chain_keys() -> list[str]:
    """The gate's literal and indexed keys, with each family prefix expanded to its fields."""
    source = gate.PARAM_SOURCE.read_text()
    tables = gate.FIELD_TABLES.read_text()
    families = _family_keys(source, tables)
    keys: set[str] = set()
    for key in gate.parameter_keys(source) + gate.indexed_keys(tables):
        if key.endswith("."):
            assert families.get(key), f"family prefix {key} expanded to no keys"
            keys.update(families[key])
        else:
            keys.add(key)
    return sorted(keys)


def _alias_pattern(spelling: str) -> re.Pattern[str]:
    return re.compile("^" + re.escape(spelling).replace(re.escape("<i>"), f"({_INDEX})") + "$")


def canonical(section: dict[str, Any], key: str) -> str:
    for entry in section["aliases"]:
        for spelling in entry["aliases"]:
            match = _alias_pattern(spelling).match(key)
            if match:
                return (
                    entry["key"].replace("<i>", match.group(1)) if match.groups() else entry["key"]
                )
    return key


def resolve(catalog: dict[str, Any], key: str) -> dict[str, Any] | None:
    """The descriptor @p key resolves to through the chain section, or None."""
    section = catalog["chain"]
    processors = {entry["id"]: entry for entry in catalog["processors"]}

    def processor_param(processor: str | None, name: str) -> dict[str, Any] | None:
        if processor is None or processor not in processors:
            return None
        return next((p for p in processors[processor]["params"] if p["name"] == name), None)

    own = {entry["name"]: entry for entry in section["params"]}
    key = canonical(section, key)
    if key in own:
        return own[key]
    for entry in section["lists"]:
        match = re.match(rf"^{re.escape(entry['key'])}\.({_INDEX})(?:\.(.+))?$", key)
        if match is None or int(match.group(1)) >= entry["maxItems"]:
            continue
        field = match.group(2)
        if entry["processor"] is None:
            return own.get(entry["key"]) if field is None else None
        return processor_param(entry["processor"], field) if field is not None else None
    stages = sorted(section["stages"], key=lambda stage: len(stage["prefix"]), reverse=True)
    for stage in stages:
        prefix = stage["prefix"] + "."
        if key.startswith(prefix):
            return processor_param(stage["processor"], key[len(prefix) :])
    return None


def test_every_chain_key_resolves_to_a_descriptor() -> None:
    catalog = capability_catalog()
    keys = chain_keys()
    # The population is this test's subject: a key source that stopped parsing reports nothing.
    assert len(keys) > 150
    unresolved = [key for key in keys if resolve(catalog, key) is None]
    assert unresolved == []


def test_every_stage_links_an_existing_processor_and_a_boolean_enable_key() -> None:
    catalog = capability_catalog()
    section = catalog["chain"]
    ids = {entry["id"] for entry in catalog["processors"]}
    assert section["stages"]
    for stage in section["stages"]:
        assert stage["processor"] is None or stage["processor"] in ids, stage
        enabled = resolve(catalog, stage["enabledKey"])
        assert enabled is not None and enabled["type"] == "boolean", stage
    for entry in section["lists"]:
        assert entry["processor"] is None or entry["processor"] in ids, entry


def test_band_fields_are_the_compressor_fields_at_every_band_index() -> None:
    catalog = capability_catalog()
    bands = next(e for e in catalog["chain"]["lists"] if e["key"].endswith(".bands"))
    last = bands["maxItems"] - 1
    for spelling in (
        f"dynamics.multibandComp.bands.{last}.detector",
        f"dynamics.multibandComp.band{last}.detector",
    ):
        descriptor = resolve(catalog, spelling)
        assert descriptor is not None and descriptor["type"] == "enum", spelling
    assert resolve(catalog, f"dynamics.multibandComp.bands.{last + 1}.ratio") is None
    assert resolve(catalog, "dynamics.multibandComp.bands.01.ratio") is None
