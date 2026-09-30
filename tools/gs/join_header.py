"""Render the insertion-effect adjudication into a C++ header the tests walk.

``tools/gs/efx-bindings/*.json`` answers, for one printed (type, slot) of the
GS insertion-effect block, what the byte drives: an insert control through a
measured table (translated), an insert control through a designed law
(designed), or which stages are on (enables). ``bindings_header.py`` renders
the rows the runtime walks, one per driven control; this renders one row per
adjudicated (type, slot) with its form, where its law comes from and the byte
range it is printed over, which is what a test needs to sweep every slot and
hold it to the form it claims.

Only the binding files are read. The measurement archive is not needed.

Usage::

    join_header.py [--bindings tools/gs/efx-bindings]
                   [--header tests/midi/gs_efx_join.h] [--check]
"""

from __future__ import annotations

import argparse
import functools
import importlib.util
import json
import operator
import subprocess
import sys
from pathlib import Path

GENERATED_BY = "tools/gs/join_header.py"

# The forms, in the order the header numbers them.
FORMS = ("translated", "designed", "enables")
FORM_CONSTANT = {
    "translated": "kGsEfxJoinTranslated",
    "designed": "kGsEfxJoinDesigned",
    "enables": "kGsEfxJoinEnables",
}
FORM_ROWS_CONSTANT = {
    "translated": "kGsEfxJoinTranslatedRows",
    "designed": "kGsEfxJoinDesignedRows",
    "enables": "kGsEfxJoinEnablesRows",
}
# Where a row's law comes from: measured for this (type, slot), carried from
# another, or invented.
BASES = ("measured", "carried", "invented")
BASIS_CONSTANT = {
    "measured": "kGsEfxJoinMeasured",
    "carried": "kGsEfxJoinCarried",
    "invented": "kGsEfxJoinInvented",
}
MARK_LITERALS = {None: "0", "+": "'+'", "#": "'#'"}


def _coverage():
    path = Path(__file__).resolve().parent / "coverage.py"
    spec = importlib.util.spec_from_file_location("gs_efx_coverage", path)
    if spec is None or spec.loader is None:
        sys.exit(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Row forms are coverage.py's, so the two read a row alike.
coverage = _coverage()


def load_rows(bindings_dir: Path) -> list[dict]:
    if not bindings_dir.is_dir():
        sys.exit(f"{bindings_dir} is not a directory")
    rows: list[dict] = []
    for path in sorted(bindings_dir.glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, list):
            sys.exit(f"{path} must hold a JSON list of binding rows")
        for index, row in enumerate(data):
            rows.append(dict(row, _file=path.name, _index=index))
    return rows


def parsed_type(text: str) -> int:
    msb, lsb = text.split()
    return (int(msb, 16) << 8) | int(lsb, 16)


def ordinal_mask(row: dict, where: str, form: str) -> int:
    """Bit n set for each same-named stage n the row drives; the slot is still one row."""
    mask = 0
    for ordinal in coverage.ordinals_of(row, where, form):
        if ordinal > 7:
            sys.exit(f"{where}: ordinal {ordinal} does not fit the join's eight-bit mask")
        mask |= 1 << ordinal
    return mask


def collect(rows: list[dict]) -> list[dict]:
    """One entry per adjudicated (type, slot)."""
    out: list[dict] = []
    claimed: set[tuple[int, int]] = set()
    for row in rows:
        where = f"{row['_file']}[{row['_index']}]"
        form = coverage.row_form(row, where)

        gs_type = parsed_type(row["type"])
        slot = int(row["slot"])
        if not 0 <= slot <= 19:
            sys.exit(f"{where}: slot must be 0-19")
        if (gs_type, slot) in claimed:
            sys.exit(f"{where}: (type, slot) already adjudicated by another row")
        claimed.add((gs_type, slot))

        if form == "translated":
            basis = "measured"
        else:
            basis = str(row[form].get("basis"))
            if basis not in coverage.BASES:
                sys.exit(f"{where}: basis {basis!r} is not one of {list(coverage.BASES)}")
        # A translated row that does not copy its printed spelling is read over
        # the whole byte; the test narrows a state list through the protocol layer.
        byte_lo, byte_hi = coverage.byte_domain(row.get("printed_values", ""))

        out.append(
            {
                "type": gs_type,
                "slot": slot,
                "form": form,
                "basis": basis,
                "ordinal_mask": 0
                if form == "enables"
                else functools.reduce(
                    operator.or_,
                    (ordinal_mask(part, where, form) for part in coverage.targets_of(row, where)),
                ),
                "byte": (byte_lo, byte_hi),
                "mark": row.get("printed_mark"),
            }
        )
    out.sort(key=lambda entry: (entry["type"], entry["slot"]))
    return out


def render(entries: list[dict]) -> str:
    counts = {form: sum(1 for e in entries if e["form"] == form) for form in FORMS}
    out: list[str] = []
    w = out.append

    w(f"// Generated by {GENERATED_BY} from tools/gs/efx-bindings/ -- do not edit.")
    w("//")
    w("// What the binding files say about each printed insertion-effect (type,")
    w("// slot): the form it is adjudicated in, where its law comes from and the")
    w("// bytes it is printed over. gs_efx_bindings.h carries what the chain builder")
    w("// needs, one row per driven control; this carries one row per slot, because")
    w("// holding every slot to the form it claims is the test's whole job.")
    w("")
    w("#pragma once")
    w("")
    w("#include <array>")
    w("#include <cstdint>")
    w("")
    w("namespace sonare::midi::synth {")
    w("")
    w("// The forms a binding row takes. Exactly one applies to a row, and the")
    w("// coverage tool adds them up to the printed parameter count.")
    for index, form in enumerate(FORMS):
        w(f"inline constexpr uint8_t {FORM_CONSTANT[form]} = {index};")
    w("")
    w("// Where a row's law comes from.")
    for index, basis in enumerate(BASES):
        w(f"inline constexpr uint8_t {BASIS_CONSTANT[basis]} = {index};")
    w("")
    w("/// One adjudicated insertion-effect parameter.")
    w("struct GsEfxJoinRow {")
    w("  uint16_t type;         ///< The two type bytes, MSB in the high byte.")
    w("  uint8_t slot;          ///< Slot index from the first parameter address.")
    w("  uint8_t form;          ///< One of the kGsEfxJoin* form constants.")
    w("  uint8_t basis;         ///< One of the kGsEfxJoin* basis constants.")
    w("  uint8_t ordinal_mask;  ///< Bit n: the row drives the n-th same-named stage.")
    w("  uint8_t byte_lo;       ///< First printed byte.")
    w("  uint8_t byte_hi;       ///< Last printed byte.")
    w("  uint8_t printed_mark;  ///< 0, '+' or '#'.")
    w("};")
    w("")
    w("// Sorted by (type, slot), so one unit's rows are contiguous.")
    w(f"inline constexpr std::array<GsEfxJoinRow, {len(entries)}> kGsEfxJoin = {{{{")
    for entry in entries:
        w(
            f"    {{0x{entry['type']:04X}, {entry['slot']}, {FORM_CONSTANT[entry['form']]}, "
            f"{BASIS_CONSTANT[entry['basis']]}, 0x{entry['ordinal_mask']:02X}, "
            f"0x{entry['byte'][0]:02X}, 0x{entry['byte'][1]:02X}, {MARK_LITERALS[entry['mark']]}}},"
        )
    w("}};")
    w("")
    w("// The counts the files declare. A test measures its own and requires these,")
    w("// so a rendering that dropped rows reads as a disagreement rather than as a")
    w("// smaller clean run.")
    for form in FORMS:
        w(f"inline constexpr int {FORM_ROWS_CONSTANT[form]} = {counts[form]};")
    w("")
    w("}  // namespace sonare::midi::synth")
    return "\n".join(out) + "\n"


def laid_out(text: str, header: Path) -> str:
    """The header as clang-format lays it out, which is how it is committed."""
    style = Path(__file__).resolve().parents[2] / ".clang-format"
    try:
        done = subprocess.run(
            ["clang-format", f"--style=file:{style}", f"--assume-filename={header}"],
            input=text,
            capture_output=True,
            text=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError) as failure:
        sys.exit(f"clang-format could not lay out {header} ({failure})")
    return done.stdout


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bindings", type=Path, default=Path("tools/gs/efx-bindings"))
    parser.add_argument("--header", type=Path, default=Path("tests/midi/gs_efx_join.h"))
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail when the header on disk differs from what these inputs render",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    entries = collect(load_rows(args.bindings))
    if not entries:
        sys.exit("no binding row was adjudicated; the header would classify nothing")
    rendered = laid_out(render(entries), args.header)

    if args.check:
        try:
            current = args.header.read_text(encoding="utf-8")
        except FileNotFoundError:
            print(f"join header is missing: {args.header}", file=sys.stderr)
            return 1
        if current != rendered:
            print(f"join header is stale: {args.header} (run make gs-efx-join)", file=sys.stderr)
            return 1
        print(f"join header is current: {args.header} ({len(entries)} parameters)")
        return 0

    args.header.write_text(rendered, encoding="utf-8")
    print(f"wrote {args.header} ({len(entries)} parameters)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
