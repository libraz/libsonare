"""The core's integer checks a facade reaches without a named domain are counted against a record.

Such a check has no ``numeric-domains.json`` row, so a facade refuses a fraction or a NaN
for that member in the C type's range rather than the core's domain. Only the ``facade``
class of ``tools/api/unnamed_integer_bounds.py`` is held: an enumeration's ordinals are not
a numeric domain, and a member no facade exposes as a number is refused by the core in its
own words. The record moves only in the commit that changes the count, in either direction,
and is held at zero once every reached domain is named.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
from typing import Any

_ROOT = Path(__file__).resolve().parents[3]
_SCANNER = _ROOT / "tools/api/unnamed_integer_bounds.py"

# Sites of the facade class listed by tools/api/unnamed_integer_bounds.py.
_RECORDED = 21


def _scanner() -> Any:
    spec = importlib.util.spec_from_file_location("unnamed_integer_bounds", _SCANNER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_the_facade_reached_unnamed_integer_bounds_match_the_record() -> None:
    rows = _scanner().classified(_ROOT)
    counts = {name: sum(row[5] == name for row in rows) for name in ("enum", "c-only", "facade")}
    assert counts["facade"] == _RECORDED, (
        f"{counts}; {_RECORDED} facade sites recorded: name the new check's domain, or set the "
        "record to the new count in the commit that moved it"
    )


def test_the_classes_follow_the_bounds_and_the_facades() -> None:
    scanner = _scanner()
    exposed = frozenset({"hop_length"})
    assert scanner.classify("0, SONARE_CHORD_QUALITY_COUNT - 1", '"quality"', exposed) == "enum"
    assert scanner.classify("1", '"hop_length"', exposed) == "facade"
    assert scanner.classify("1", '"segment_count"', exposed) == "c-only"
    assert scanner.classify("0, 127", "subject", exposed) == "facade"
    assert not any("channels - 1" in row[3] for row in scanner.sites(_ROOT))
