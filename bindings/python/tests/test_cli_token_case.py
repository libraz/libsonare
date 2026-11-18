"""A token flag reads any letter case, so no set may hold two members differing only by case.

Both CLIs resolve a value to the member it names case-insensitively. Two members
spelled alike up to case would make that lookup ambiguous, so every set either
front-end can resolve against is checked: the option domains each CLI publishes
and every name catalog the library lists at run time.
"""

from __future__ import annotations

import inspect
import json
import os
import subprocess
from collections.abc import Iterable
from pathlib import Path

import pytest

import libsonare
from libsonare import cli
from libsonare._cli_inventory import build_cli_contract

from ._helpers import LIB_AVAILABLE

_ROOT = Path(__file__).resolve().parents[3]


def _case_collisions(members: Iterable[str]) -> list[tuple[str, str]]:
    seen: dict[str, str] = {}
    collisions: list[tuple[str, str]] = []
    for member in members:
        folded = member.lower()
        if folded in seen and seen[folded] != member:
            collisions.append((seen[folded], member))
        seen.setdefault(folded, member)
    return collisions


def _domain_sets(contract: dict[str, object]) -> dict[str, list[str]]:
    sets: dict[str, list[str]] = {}
    for command in contract["commands"]:  # type: ignore[attr-defined]
        for option in command.get("options", []):
            domain = option.get("domain")
            if domain and domain["choices"]:
                sets[f"{command['path']} --{option['name']}"] = list(domain["choices"])
    return sets


def _native_cli() -> Path | None:
    for candidate in (os.environ.get("SONARE_NATIVE_CLI"), str(_ROOT / "build/bin/sonare-cli")):
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


def test_a_pair_differing_only_by_case_is_a_collision() -> None:
    assert _case_collisions(["hipHop", "pop", "HipHop"]) == [("hipHop", "HipHop")]
    assert _case_collisions(["hipHop", "pop", "hipHop"]) == []


def test_python_option_domains_have_no_members_differing_only_by_case() -> None:
    sets = _domain_sets(build_cli_contract(cli._build_parser()))
    assert sets, "the Python CLI published no closed option set"
    for name, members in sets.items():
        assert not _case_collisions(members), name


def test_native_option_domains_have_no_members_differing_only_by_case() -> None:
    binary = _native_cli()
    if binary is None:
        pytest.skip("native sonare-cli binary not found; set SONARE_NATIVE_CLI")
    dumped = subprocess.run(
        [str(binary), "--dump-cli-contract"], capture_output=True, text=True, check=True
    )
    sets = _domain_sets(json.loads(dumped.stdout))
    assert sets, "the native CLI published no closed option set"
    for name, members in sets.items():
        assert not _case_collisions(members), name


@pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")
def test_library_name_catalogs_have_no_members_differing_only_by_case() -> None:
    catalogs = {}
    for name in libsonare.__all__:
        function = getattr(libsonare, name)
        if not name.endswith("_names") or not callable(function):
            continue
        required = [
            parameter
            for parameter in inspect.signature(function).parameters.values()
            if parameter.default is inspect.Parameter.empty
        ]
        if not required:
            catalogs[name] = function()
    assert "mastering_preset_names" in catalogs and "synth_preset_names" in catalogs
    for name, members in catalogs.items():
        assert not _case_collisions(members), name
