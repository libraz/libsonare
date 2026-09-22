"""Render the hand-written insertion-effect bindings into a C++ header.

``tools/gs/efx-bindings/*.json`` says, for one (type, slot) of the GS
insertion-effect block, which insert control the byte drives and which
conversion law reads it. This turns the rows carrying a ``stage`` into a
table ``gs_layer.cpp`` walks, so a binding is a data row rather than a
branch someone remembered to write.

Only the two committed inputs are read -- the binding files and
``tools/gs/efx-tables.json``, the latter for the order its classes file
their tables in, which is the same order ``gs_efx_tables.h`` numbers them
in. The measurement archive is NOT needed: a clone can regenerate this
header, which is why it does not live in ``derive_efx_tables.py``.

One law is the binding layer's rather than the archive's: ``ratio``, a byte
read between the printed endpoints a row carries in ``range``. It is admitted
only where those endpoints force a whole step per byte, and the header is
refused otherwise, because a rounded step is a conversion nobody measured.

Usage::

    bindings_header.py [--bindings tools/gs/efx-bindings]
                       [--tables tools/gs/efx-tables.json]
                       [--header src/midi/synth/gs_efx_bindings.h] [--check]
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path

GENERATED_BY = "tools/gs/bindings_header.py"

# Laws no measured table holds, numbered after the classes gs_efx_tables.h
# numbers. A table names the printed unit the endpoints are spelled in.
BINDING_LAWS = {"ratio": ("percent", "semitone")}

# The spelling of a printed byte range, as the archive's printed_values has it.
BYTE_RANGE_RE = re.compile(r"^([0-9A-F]{2})–([0-9A-F]{2})$")

NO_RANGE = 0xFF


def class_order(derive_path: Path) -> tuple[str, ...]:
    """The class numbering ``gs_efx_tables.h`` emits, read from its own source.

    Imported rather than restated: a second copy of the order would put the
    two headers' class numbers silently out of step, and nothing downstream
    compares them.
    """
    spec = importlib.util.spec_from_file_location("derive_efx_tables", derive_path)
    if spec is None or spec.loader is None:
        sys.exit(f"could not load {derive_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return tuple(module.CLASS_ORDER)


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


def keys_of(row: dict) -> list[str]:
    """A row names one control or several; both spellings reach here as a list."""
    if "keys" in row:
        keys = row["keys"]
        if not isinstance(keys, list) or not keys:
            sys.exit(f'{row["_file"]}[{row["_index"]}]: "keys" must be a non-empty list')
        return [str(key) for key in keys]
    return [str(row["key"])]


def printed_range(row: dict, where: str) -> tuple[int, int, int, int]:
    """A ratio row's byte endpoints and unit endpoints, refused unless they force a step.

    The byte endpoints are the row's own copy of the archive's printed_values,
    which ``coverage.py`` holds to the archive; the unit endpoints are ``range``.
    """
    match = BYTE_RANGE_RE.match(str(row.get("printed_values", "")))
    if match is None:
        sys.exit(f'{where}: a ratio row needs "printed_values" spelled as a byte range')
    lo_byte, hi_byte = (int(end, 16) for end in match.groups())
    ends = row.get("range")
    if not (isinstance(ends, list) and len(ends) == 2 and all(isinstance(e, int) for e in ends)):
        sys.exit(f'{where}: a ratio row needs "range" as two integer endpoints')
    lo_unit, hi_unit = ends
    steps = hi_byte - lo_byte
    if steps <= 0 or (hi_unit - lo_unit) % steps != 0:
        sys.exit(
            f"{where}: {lo_unit}..{hi_unit} over bytes {lo_byte}..{hi_byte} is not a whole "
            "step per byte, so the endpoints do not decide the conversion"
        )
    return lo_byte, hi_byte, lo_unit, hi_unit


def collect(rows: list[dict], classes: dict, order: tuple[str, ...]) -> list[dict]:
    """One emitted entry per (type, slot, key) a binding row translates."""
    laws = {name: list(body["tables"]) for name, body in classes.items()}
    laws.update({name: list(tables) for name, tables in BINDING_LAWS.items()})
    numbering = list(order) + list(BINDING_LAWS)
    out: list[dict] = []
    for row in rows:
        if "stage" not in row:
            continue
        where = f"{row['_file']}[{row['_index']}]"
        gs_class = row.get("class")
        table = row.get("table")
        if gs_class not in numbering:
            sys.exit(f"{where}: class {gs_class!r} is not one the tables declare")
        tables = laws[gs_class]
        if table not in tables:
            sys.exit(f"{where}: {gs_class!r} has no table {table!r}")
        if gs_class in BINDING_LAWS:
            ends = printed_range(row, where)
        elif "range" in row:
            sys.exit(f'{where}: "range" is read by the ratio law only')
        else:
            ends = None
        for key in keys_of(row):
            out.append(
                {
                    "type": parsed_type(row["type"]),
                    "slot": int(row["slot"]),
                    "conversion_class": numbering.index(gs_class),
                    "table": tables.index(table),
                    "stage": str(row["stage"]),
                    "key": key,
                    "range": ends,
                    "label": f"{gs_class}.{table}",
                }
            )
    # Sorted so a type's rows are contiguous and the walk over one unit's
    # twenty slots visits them in slot order.
    out.sort(key=lambda entry: (entry["type"], entry["slot"], entry["key"]))
    duplicates = set()
    seen = set()
    for entry in out:
        address = (entry["type"], entry["slot"], entry["key"])
        if address in seen:
            duplicates.add(address)
        seen.add(address)
    if duplicates:
        spelled = ", ".join(
            f"{type_:#06x} slot {slot} {key}" for type_, slot, key in sorted(duplicates)
        )
        sys.exit(f"two binding rows drive the same control: {spelled}")
    return out


def camel(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def render(entries: list[dict], order: tuple[str, ...]) -> str:
    stages = sorted({entry["stage"] for entry in entries})
    keys = sorted({entry["key"] for entry in entries})
    ranges = sorted({entry["range"] for entry in entries if entry["range"] is not None})
    if len(stages) >= 0xFF or len(keys) >= 0xFF or len(ranges) >= NO_RANGE:
        sys.exit("the name tables outgrew their index width")
    out: list[str] = []
    w = out.append

    w(f"// Generated by {GENERATED_BY} from tools/gs/efx-bindings/ -- do not edit.")
    w("//")
    w("// Which insert control each insertion-effect parameter byte drives. One row")
    w("// per (type, slot, key) a binding file translates; the rows a binding file")
    w("// leaves as a documented state, as unmapped or as the skeleton's own")
    w("// business are absent, which is what makes a missing row mean something.")
    w("//")
    w("// The conversion law is named rather than carried: `conversion_class` and")
    w("// `table` index the same tables gs_efx_tables.h numbers, so a byte read")
    w("// here and a byte read through kGsEfxSlotConversions read alike.")
    w("")
    w("#pragma once")
    w("")
    w("#include <array>")
    w("#include <cstdint>")
    w("#include <string_view>")
    w("")
    w("namespace sonare::midi::synth {")
    w("")
    w("/// One insert control an insertion-effect byte drives.")
    w("struct GsEfxBinding {")
    w("  uint16_t type;             ///< The two type bytes, MSB in the high byte.")
    w("  uint8_t slot;              ///< Slot index from the first parameter address.")
    w("  uint8_t conversion_class;  ///< One of the kGsEfxClass* values.")
    w("  uint8_t table;             ///< Which table of that class.")
    w("  uint8_t stage;             ///< Index into kGsEfxBindingStages.")
    w("  uint8_t key;               ///< Index into kGsEfxBindingKeys.")
    w("  uint8_t range;  ///< Index into kGsEfxBindingRanges, or kGsEfxBindingNoRange.")
    w("};")
    w("")
    w("/// The printed endpoints a ratio row is read between: the byte range the")
    w("/// archive records and the unit range beside it. The generator refuses a pair")
    w("/// that does not put a whole unit step on every byte.")
    w("struct GsEfxBindingRange {")
    w("  uint8_t lo_byte;")
    w("  uint8_t hi_byte;")
    w("  int16_t lo_unit;")
    w("  int16_t hi_unit;")
    w("};")
    w("")
    for index, name in enumerate(BINDING_LAWS, start=len(order)):
        w("/// A law no measured table holds, numbered after the kGsEfxClass* values")
        w("/// gs_efx_tables.h emits; the row carries its own endpoints.")
        w(f"inline constexpr uint8_t kGsEfxClass{camel(name)} = {index};")
    w(f"inline constexpr uint8_t kGsEfxBindingNoRange = 0x{NO_RANGE:02X};")
    w("")
    w(f"inline constexpr std::array<GsEfxBindingRange, {len(ranges)}> kGsEfxBindingRanges = {{{{")
    for lo_byte, hi_byte, lo_unit, hi_unit in ranges:
        w(f"    {{0x{lo_byte:02X}, 0x{hi_byte:02X}, {lo_unit}, {hi_unit}}},")
    w("}};")
    w("")
    w("// The insert names and control names the rows below point at, each spelled")
    w("// once. A name is an index so the table stays a plain array of integers.")
    w(f"inline constexpr std::array<std::string_view, {len(stages)}> kGsEfxBindingStages = {{{{")
    for stage in stages:
        w(f'    "{stage}",')
    w("}};")
    w("")
    w(f"inline constexpr std::array<std::string_view, {len(keys)}> kGsEfxBindingKeys = {{{{")
    for key in keys:
        w(f'    "{key}",')
    w("}};")
    w("")
    w("// Sorted by (type, slot, key), so one unit's rows are contiguous.")
    w(f"inline constexpr std::array<GsEfxBinding, {len(entries)}> kGsEfxBindings = {{{{")
    stage_index = {name: i for i, name in enumerate(stages)}
    key_index = {name: i for i, name in enumerate(keys)}
    range_index = {ends: i for i, ends in enumerate(ranges)}
    for entry in entries:
        spelled_range = (
            "kGsEfxBindingNoRange" if entry["range"] is None else str(range_index[entry["range"]])
        )
        w(
            f"    {{0x{entry['type']:04X}, {entry['slot']}, {entry['conversion_class']}, "
            f"{entry['table']}, {stage_index[entry['stage']]}, {key_index[entry['key']]}, "
            f"{spelled_range}}},"
            f"  // {entry['label']} -> {entry['stage']}.{entry['key']}"
        )
    w("}};")
    w("")
    w("}  // namespace sonare::midi::synth")
    return "\n".join(out) + "\n"


def laid_out(text: str, header: Path) -> str:
    """The header as clang-format lays it out, which is how it is committed.

    The repository formats every header, so an unformatted rendering would
    differ from the committed one over layout alone and report as drift.
    Run here rather than raced with ``make format``, and with the style named
    explicitly: a style found by proximity would lay a scratch copy out
    differently from the committed one.
    """
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
    parser.add_argument("--tables", type=Path, default=Path("tools/gs/efx-tables.json"))
    parser.add_argument("--derive", type=Path, default=Path("tools/gs/derive_efx_tables.py"))
    parser.add_argument("--header", type=Path, default=Path("src/midi/synth/gs_efx_bindings.h"))
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail when the header on disk differs from what these inputs render",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    order = class_order(args.derive)
    classes = json.loads(args.tables.read_text(encoding="utf-8"))["classes"]
    entries = collect(load_rows(args.bindings), classes, order)
    if not entries:
        sys.exit("no binding row carries a stage; the header would bind nothing")
    rendered = laid_out(render(entries, order), args.header)

    if args.check:
        try:
            current = args.header.read_text(encoding="utf-8")
        except FileNotFoundError:
            print(f"binding header is missing: {args.header}", file=sys.stderr)
            return 1
        if current != rendered:
            print(
                f"binding header is stale: {args.header} (run make gs-efx-bindings)",
                file=sys.stderr,
            )
            return 1
        print(f"binding header is current: {args.header} ({len(entries)} controls)")
        return 0

    args.header.write_text(rendered, encoding="utf-8")
    print(f"wrote {args.header} ({len(entries)} controls)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
