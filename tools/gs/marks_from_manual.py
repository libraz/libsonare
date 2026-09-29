"""Transcribe the SC-8850 parameter list's names and ``+``/``#`` marks into binding rows.

The owner's manual prints a name for every insertion-effect parameter and a
one-character mark beside some of them. This copies those two, and nothing
else, into the ``printed_name`` and ``printed_mark`` fields of the binding rows
named on the command line, matching a manual row to
a binding row by its address -- type MSB, type LSB and parameter address --
never by the parameter name, whose spelling differs between manuals. The
manual's ``03 00`` is folded onto ``02 0C`` exactly as ``coverage.py`` folds
it, since that is the number binding files are written against.

This is the one tool here that opens the archive's ``documents/`` tree. Its
effect-list table is read from the parser's output and from the hand-kept rows
the parser refused (``by-hand.json``), which the archive joins with it. The
derivation and ``coverage.py`` do not open ``documents/``; ``coverage.py``
matches its name rules against the transcribed ``printed_name``.

Usage::

    marks_from_manual.py --archive <archive root> [--report] <row file>...
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

MANUAL = "documents/roland-sc-8850-owners-manual-en"
TABLE = "effect-list"
# The same fold coverage.py applies: 02 0C is canonical, 03 00 its alias.
TYPE_ALIASES = {"03 00": "02 0C"}
MARKS = ("+", "#")
# The first parameter address of the insertion-effect block; slot 0 lives here.
FIRST_PARAMETER_LSB = 0x03


def manual_rows(archive: Path) -> list[dict]:
    """Every effect-list parameter row, parsed and hand-kept."""
    manual = archive / MANUAL
    parsed = json.loads((manual / f"{TABLE}.json").read_text(encoding="utf-8"))["rows"]
    by_hand = json.loads((manual / "by-hand.json").read_text(encoding="utf-8"))
    kept = by_hand.get("tables", {}).get(TABLE, [])
    return [row for row in (*parsed, *kept) if "address_lsb" in row]


def by_address(rows: list[dict]) -> dict[tuple[str, int], dict]:
    """Manual rows keyed by canonical type and slot; a disagreeing duplicate is an error."""
    keyed: dict[tuple[str, int], dict] = {}
    for row in rows:
        gs_type = f"{row['msb']} {row['lsb']}".upper()
        gs_type = TYPE_ALIASES.get(gs_type, gs_type)
        slot = int(row["address_lsb"], 16) - FIRST_PARAMETER_LSB
        address = (gs_type, slot)
        seen = keyed.get(address)
        if seen is not None and (
            seen.get("printed_mark") != row.get("printed_mark")
            or seen["parameter"] != row["parameter"]
        ):
            sys.exit(f"the manual prints {gs_type} slot {slot} twice, differently")
        mark = row.get("printed_mark")
        if mark is not None and mark not in MARKS:
            sys.exit(f"{gs_type} slot {slot}: unknown mark {mark!r}")
        keyed[address] = row
    return keyed


def dumped(rows: list[dict]) -> str:
    """A row file's own layout: one row per line."""
    return "[\n" + ",\n".join(json.dumps(row, ensure_ascii=False) for row in rows) + "\n]\n"


def transcribe(path: Path, manual: dict[tuple[str, int], dict]) -> dict[str, int]:
    rows = json.loads(path.read_text(encoding="utf-8"))
    counts = {mark: 0 for mark in MARKS}
    for row in rows:
        printed = manual.get((row["type"], row["slot"]))
        if printed is None:
            sys.exit(f"{path}: {row['type']} slot {row['slot']} is not in the manual's list")
        # Both fields are set from scratch, name first, so a rerun is a no-op.
        row.pop("printed_name", None)
        row.pop("printed_mark", None)
        row["printed_name"] = printed["parameter"]
        mark = printed.get("printed_mark")
        if mark is not None:
            row["printed_mark"] = mark
            counts[mark] += 1
    path.write_text(dumped(rows), encoding="utf-8")
    return counts


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--archive", type=Path, required=True, help="root of a soundings archive")
    ap.add_argument(
        "--report",
        action="store_true",
        help="print which types carry no + and which no #, over the whole manual",
    )
    ap.add_argument("rows", type=Path, nargs="*", help="binding row files to write marks into")
    args = ap.parse_args()

    manual = by_address(manual_rows(args.archive))

    totals = {mark: 0 for mark in MARKS}
    for path in args.rows:
        counts = transcribe(path, manual)
        for mark in MARKS:
            totals[mark] += counts[mark]
        print(f"{path}: + {counts['+']}  # {counts['#']}")
    if args.rows:
        print(f"marks written: + {totals['+']}  # {totals['#']}")

    if args.report:
        types = sorted({gs_type for gs_type, _ in manual})
        for mark in MARKS:
            carrying = {t for (t, _), row in manual.items() if row.get("printed_mark") == mark}
            print(f"types without {mark}: {' '.join(t for t in types if t not in carrying)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
