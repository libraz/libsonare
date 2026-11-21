"""The core's integer checks still written with constant bounds are counted against a record.

Each such check has no named domain, so it has no ``numeric-domains.json`` row and Python
refuses a fraction for that member in the C type's range rather than the core's domain.
The record moves only in the commit that changes the count, in either direction, and is
held at zero once every domain is named.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[3]
_SCANNER = _ROOT / "tools/api/unnamed_integer_bounds.py"

# Sites listed by tools/api/unnamed_integer_bounds.py.
_RECORDED = 447


def _sites() -> list[tuple[str, int, str, str, str]]:
    spec = importlib.util.spec_from_file_location("unnamed_integer_bounds", _SCANNER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    found: list[tuple[str, int, str, str, str]] = module.sites(_ROOT)
    return found


def test_the_unnamed_integer_bounds_match_the_record() -> None:
    found = _sites()
    assert len(found) == _RECORDED, (
        f"{len(found)} unnamed integer bounds, {_RECORDED} recorded: name the new check's "
        "domain, or set the record to the new count in the commit that moved it"
    )


def test_the_scanner_sees_a_constant_bound_and_skips_a_relation() -> None:
    found = {(path, subject) for path, _, _, _, subject in _sites()}
    assert ("src/analysis/bpm_analyzer.h", '"hop_length"') in found
    assert not any("channels - 1" in bounds for _, _, _, bounds, _ in _sites())
