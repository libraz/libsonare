"""Report how much of the GS insertion-effect parameter block is adjudicated.

The SC-8850 prints 770 (type, slot) parameters across its insertion-effect
block. This reads the archive's own record of which of those 770 carry a
printed value, and the hand-written binding files that say what each one
does -- translated into an insert control, left as a documented state, or
one of the other reasons a row does not translate. It answers one equation:
``translated + state + unmapped + unreadable + builder == printed``.

**Only** ``<archive>/data/units/*/efx-params/*.json`` is read. Nothing under
``<archive>/documents/`` is opened -- that tree carries a licence this repo
does not have (see ``tools/gs/docs/efx-tables.md``), and this script never
crosses it. Binding files are read from ``--bindings``; a missing directory
or an empty one is zero rows, not an error, since no unit has one yet.

Usage::

    coverage.py --archive <archive root> [--bindings tools/gs/efx-bindings]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# What today's archive gives when every efx-params record is read and the
# (type, slot) rows carrying a printed_values are unioned across a type's
# base, condition-variant and -parked files. A drift here is the whole
# design's denominator moving, so it is asserted rather than trusted. Two
# views are asserted: as the archive files it, and after TYPE_ALIASES folds
# Rotary Multi's second type number onto its canonical one.
EXPECTED_PRINTED = 770
EXPECTED_CANONICAL_TYPES = 64
EXPECTED_PER_MSB_ARCHIVE = {
    "01": 322,
    "02": 131,
    "03": 18,
    "04": 139,
    "05": 20,
    "11": 140,
}
EXPECTED_PER_MSB_CANONICAL = {"01": 322, "02": 149, "04": 139, "05": 20, "11": 140}

# Rotary Multi is one effect the manual prints under two type numbers. This
# tree treats 02 0C as canonical (src/midi/synth/gs_layer.cpp) but the
# archive files its records only under the second number, 03 00 -- there is
# no 02-0C.json. A binding file is written against the canonical number, so
# the printed set is folded through this before it is matched against one.
TYPE_ALIASES = {"03 00": "02 0C"}

# The five binding-row forms, keyed by the field that names each one, and the
# equation term each contributes to. A row must carry exactly one of these.
FORM_TERM = {
    "stage": "translated",
    "state": "state",
    "unmapped": "unmapped",
    "unreadable": "unreadable",
    "builder": "builder",
}
TERM_ORDER = ("translated", "state", "unmapped", "unreadable", "builder")

TYPE_RE = re.compile(r"^[0-9A-Fa-f]{2} [0-9A-Fa-f]{2}$")
MARKER_RE = re.compile(r"^\*(\d+)$")
RANGE_RE = re.compile(r"^[0-9A-F]{2}–[0-9A-F]{2}$")
WHOLE_BYTE = "00–7F"

# Which conversion-class family each *N marker belongs to, read off the
# columns table in derive_efx_tables.py's CLASSES. *11 (LPF) and *12
# (Manual) have no measured table and so no established family -- a row
# citing either is not mechanically checkable and is left alone rather than
# guessed at.
MARKER_FAMILY = {
    "1": "delay_time",
    "2": "delay_time",
    "3": "delay_time",
    "4": "delay_time",
    "5": "delay_time",
    "6": "rate",
    "7": "rate",
    "8": "freq",
    "9": "freq",
    "10": "freq",
    "13": "azimuth",
    "14": "accel",
}

# Classes whose printed column is a continuous byte range (00-7F or a
# decibel window) rather than a small named enumeration. An enumeration
# printed_values naming one of these is a contradiction.
CONTINUOUS_CLASSES = {"gain", "level", "pan", "balance"}


def load(path: Path) -> dict:
    return json.loads(path.read_text())


def _merge_slots(
    into: dict[str, dict[int, str]], key: str, slot: int, values: str, source: str
) -> None:
    slots = into.setdefault(key, {})
    existing = slots.get(slot)
    if existing is not None and existing != values:
        sys.exit(
            f"conflicting printed_values for type {key} slot {slot}: "
            f"{existing!r} against {values!r} (seen in {source})"
        )
    slots[slot] = values


def load_printed(
    archive: Path,
) -> tuple[dict[str, dict[int, str]], dict[str, dict[int, str]]]:
    """Every (type, slot) with a printed value, archive-keyed and canonical.

    Archive-keyed unions across a type's base, condition-variant and
    ``-parked`` files, keyed by the record's own ``type`` field rather than
    by file name. Canonical additionally folds any ``TYPE_ALIASES`` entry
    onto its target -- what a binding file is written against. Two files
    disagreeing on the same (type, slot)'s printed value is a hard error in
    either view.
    """
    params_dir = archive / "data" / "units"
    if not params_dir.is_dir():
        sys.exit(f"{params_dir} does not exist; --archive must point at a soundings archive root")
    files = sorted(params_dir.glob("*/efx-params/*.json"))
    if not files:
        sys.exit(f"no efx-params records under {params_dir}")

    archive_keyed: dict[str, dict[int, str]] = {}
    for path in files:
        record = load(path)
        gs_type = record["type"]
        for parameter in record["parameters"]:
            values = parameter.get("printed_values")
            if not values:
                continue
            _merge_slots(archive_keyed, gs_type, parameter["parameter"], values, path.name)
    archive_keyed = {t: s for t, s in archive_keyed.items() if s}

    canonical: dict[str, dict[int, str]] = {}
    for gs_type, slots in archive_keyed.items():
        target = TYPE_ALIASES.get(gs_type, gs_type)
        for slot, values in slots.items():
            _merge_slots(canonical, target, slot, values, f"the {gs_type} alias")

    return archive_keyed, canonical


def load_bindings(bindings_dir: Path) -> list[dict]:
    """Every binding row, tagged with which file it came from.

    A missing directory or one with no ``*.json`` files is zero rows -- no
    binding files exist yet, and that is today's expected state rather than
    a configuration mistake.
    """
    if not bindings_dir.is_dir():
        return []
    rows: list[dict] = []
    for path in sorted(bindings_dir.glob("*.json")):
        data = load(path)
        if not isinstance(data, list):
            sys.exit(f"{path} must hold a JSON list of binding rows")
        for index, row in enumerate(data):
            row = dict(row)
            row["_file"] = path.name
            row["_index"] = index
            rows.append(row)
    return rows


def row_label(row: dict) -> str:
    return f"{row.get('type')!r} slot {row.get('slot')!r} ({row['_file']}[{row['_index']}])"


def check_class_against_printed(row: dict, values: str) -> None:
    """Success condition 5, as far as it decides mechanically today.

    Three shapes are checked: a *N marker whose class disagrees with the
    marker's own family, an explicit small enumeration carrying a continuous
    class, and a ratio row on anything but a range printed with a unit.
    Anything else -- a marker with no known family, say -- is left alone
    rather than given an invented rule.
    """
    gs_class = row.get("class")
    if not gs_class:
        return
    marker = MARKER_RE.match(values)
    if marker:
        family = MARKER_FAMILY.get(marker.group(1))
        if family is not None and gs_class != family:
            sys.exit(
                f"{row_label(row)}: class {gs_class!r} contradicts printed marker "
                f"{values!r}, which belongs to the {family!r} family"
            )
        return
    if "/" in values and gs_class in CONTINUOUS_CLASSES:
        sys.exit(
            f"{row_label(row)}: class {gs_class!r} is continuous but printed_values "
            f"{values!r} is an explicit small enumeration"
        )
    # The ratio law reads a byte between the endpoints of a range printed with a
    # unit. The whole byte carries none, and its measured law is not linear.
    if gs_class == "ratio" and (not RANGE_RE.match(values) or values == WHOLE_BYTE):
        sys.exit(
            f"{row_label(row)}: class 'ratio' needs a byte range printed with a unit, "
            f"and printed_values is {values!r}"
        )


def tally(rows: list[dict], printed: dict[str, dict[int, str]]) -> dict:
    counts = {term: 0 for term in TERM_ORDER}
    declared_keys: set[tuple[str, str]] = set()
    claimed: set[tuple[str, int]] = set()

    for row in rows:
        gs_type = row.get("type")
        slot = row.get("slot")
        if not isinstance(gs_type, str) or not TYPE_RE.match(gs_type):
            sys.exit(f'{row_label(row)}: type must be an "MM LL" hex pair')
        if not isinstance(slot, int) or not (0 <= slot <= 19):
            sys.exit(f"{row_label(row)}: slot must be an integer 0-19")

        forms_present = [key for key in FORM_TERM if key in row]
        if len(forms_present) != 1:
            sys.exit(
                f"{row_label(row)}: must carry exactly one of "
                f"{sorted(FORM_TERM)}, has {forms_present}"
            )
        form = forms_present[0]

        slots = printed.get(gs_type)
        values = slots.get(slot) if slots else None
        if values is None:
            sys.exit(f"{row_label(row)}: names a (type, slot) with no printed value")
        # A row's copy of the spelling is what a generator reads without the
        # archive, so it is held to the archive here.
        if "printed_values" in row and row["printed_values"] != values:
            sys.exit(
                f"{row_label(row)}: printed_values {row['printed_values']!r} is not the "
                f"archive's {values!r}"
            )

        key = (gs_type, slot)
        if key in claimed:
            sys.exit(f"{row_label(row)}: (type, slot) already claimed by another binding row")
        claimed.add(key)

        counts[FORM_TERM[form]] += 1

        if form == "stage":
            stage = row["stage"]
            names = row["keys"] if "keys" in row else [row.get("key")]
            if not names or any(n is None for n in names):
                sys.exit(f"{row_label(row)}: an assigned row needs 'key' or 'keys'")
            for name in names:
                declared_keys.add((stage, name))
            check_class_against_printed(row, values)

    total_printed = sum(len(slots) for slots in printed.values())
    unadjudicated = total_printed - len(claimed)
    return {
        "counts": counts,
        "declared_keys": len(declared_keys),
        "unadjudicated": unadjudicated,
        "printed": total_printed,
    }


def per_msb_of(printed: dict[str, dict[int, str]]) -> dict[str, int]:
    per_msb: dict[str, int] = {}
    for gs_type, slots in printed.items():
        msb = gs_type.split()[0]
        per_msb[msb] = per_msb.get(msb, 0) + len(slots)
    return per_msb


def assert_expected(label: str, printed: dict[str, dict[int, str]], expected_per_msb: dict) -> None:
    total = sum(len(slots) for slots in printed.values())
    types = len(printed)
    per_msb = per_msb_of(printed)
    if (
        total != EXPECTED_PRINTED
        or types != EXPECTED_CANONICAL_TYPES
        or per_msb != expected_per_msb
    ):
        sys.exit(
            f"{label} printed enumeration drifted from the recorded expectation:\n"
            f"  computed: printed={total} types={types} per_msb={per_msb}\n"
            f"  expected: printed={EXPECTED_PRINTED} types={EXPECTED_CANONICAL_TYPES} "
            f"per_msb={expected_per_msb}"
        )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--archive", required=True, help="root of a soundings measurement archive")
    ap.add_argument(
        "--bindings",
        default="tools/gs/efx-bindings",
        help="directory of hand-written binding files, one *.json per MSB",
    )
    args = ap.parse_args()

    archive = Path(args.archive)
    archive_keyed, canonical = load_printed(archive)

    # The archive-keyed view catches the archive's own filing moving (a new
    # 02-0C.json appearing, say); the canonical view is what binding files
    # are written against and is what gets printed below.
    assert_expected("archive-keyed", archive_keyed, EXPECTED_PER_MSB_ARCHIVE)
    assert_expected("canonical", canonical, EXPECTED_PER_MSB_CANONICAL)

    rows = load_bindings(Path(args.bindings))
    result = tally(rows, canonical)
    counts = result["counts"]
    per_msb = per_msb_of(canonical)

    print(
        "GS EFX coverage: printed={printed} translated={translated} state={state} "
        "unmapped={unmapped} unreadable={unreadable} builder={builder}".format(
            printed=result["printed"], **counts
        )
    )
    print("per-MSB: " + " ".join(f"{msb}={per_msb[msb]}" for msb in sorted(per_msb)))
    print(f"declared_keys={result['declared_keys']}")
    print(f"unadjudicated={result['unadjudicated']}")

    equation_holds = sum(counts.values()) == result["printed"]
    if not equation_holds:
        sys.exit(1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
