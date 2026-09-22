"""Render the insertion-effect adjudication into a C++ header the tests walk.

``tools/gs/efx-bindings/*.json`` answers, for one printed (type, slot) of the
GS insertion-effect block, which of five things happens to the byte: it is
assigned to an insert control, left as a documented state, unmapped because
the type realises no chain, assembled by the chain skeleton, or unreadable.
``bindings_header.py`` renders the assigned rows for the runtime; this renders
**every** adjudicated row for the test, which is the side that has to be able
to tell a form apart from the four it is not.

A row carries its reason string and, where a state row names the control its
insert does not have, that (stage, key) pair -- so the claim "the limiter
insert has no ratio" fails the moment the limiter grows one.

Only the binding files are read. The measurement archive is not needed.

Usage::

    join_header.py [--bindings tools/gs/efx-bindings]
                   [--header tests/midi/gs_efx_join.h] [--check]
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

GENERATED_BY = "tools/gs/join_header.py"

# The five forms, in the order the header numbers them and the coverage
# equation adds them up.
FORMS = ("stage", "state", "unmapped", "builder", "unreadable")
FORM_CONSTANT = {
    "stage": "kGsEfxJoinAssigned",
    "state": "kGsEfxJoinState",
    "unmapped": "kGsEfxJoinUnmapped",
    "builder": "kGsEfxJoinBuilder",
    "unreadable": "kGsEfxJoinUnreadable",
}
FORM_ROWS_CONSTANT = {
    "stage": "kGsEfxJoinAssignedRows",
    "state": "kGsEfxJoinStateRows",
    "unmapped": "kGsEfxJoinUnmappedRows",
    "builder": "kGsEfxJoinBuilderRows",
    "unreadable": "kGsEfxJoinUnreadableRows",
}

NO_NAME = 0xFF
NO_REASON = 0xFFFF


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
    if "keys" in row:
        return [str(key) for key in row["keys"]]
    return [str(row["key"])]


def absent_of(row: dict, where: str, form: str) -> tuple[str, str] | None:
    """The control a state row says its insert does not have.

    Optional, and deliberately so: a claim naming a key no insert anywhere
    spells could never go red, so a row whose missing control has no
    established spelling carries prose alone.
    """
    if "absent" not in row:
        return None
    if form != "state":
        sys.exit(f'{where}: only a state row may carry "absent"')
    absent = row["absent"]
    if not isinstance(absent, dict) or set(absent) != {"stage", "key"}:
        sys.exit(f'{where}: "absent" must be an object with exactly "stage" and "key"')
    return str(absent["stage"]), str(absent["key"])


def collect(rows: list[dict]) -> tuple[list[dict], int]:
    """One entry per adjudicated (type, slot), plus the declared control count."""
    out: list[dict] = []
    declared_keys = 0
    claimed: set[tuple[int, int]] = set()
    for row in rows:
        where = f"{row['_file']}[{row['_index']}]"
        present = [form for form in FORMS if form in row]
        if len(present) != 1:
            sys.exit(f"{where}: must carry exactly one of {list(FORMS)}, has {present}")
        form = present[0]

        gs_type = parsed_type(row["type"])
        slot = int(row["slot"])
        if not 0 <= slot <= 19:
            sys.exit(f"{where}: slot must be 0-19")
        if (gs_type, slot) in claimed:
            sys.exit(f"{where}: (type, slot) already adjudicated by another row")
        claimed.add((gs_type, slot))

        if form == "stage":
            declared_keys += len(keys_of(row))
            reason = None
        else:
            reason = str(row[form])

        out.append(
            {
                "type": gs_type,
                "slot": slot,
                "form": form,
                "reason": reason,
                "absent": absent_of(row, where, form),
            }
        )
    out.sort(key=lambda entry: (entry["type"], entry["slot"]))
    return out, declared_keys


def render(entries: list[dict], declared_keys: int) -> str:
    reasons = sorted({entry["reason"] for entry in entries if entry["reason"] is not None})
    stages = sorted({entry["absent"][0] for entry in entries if entry["absent"]})
    keys = sorted({entry["absent"][1] for entry in entries if entry["absent"]})
    counts = {form: sum(1 for e in entries if e["form"] == form) for form in FORMS}
    if len(reasons) > NO_REASON or max(len(stages), len(keys), 0) >= NO_NAME:
        sys.exit("the name tables outgrew their index width")

    out: list[str] = []
    w = out.append

    w(f"// Generated by {GENERATED_BY} from tools/gs/efx-bindings/ -- do not edit.")
    w("//")
    w("// What the binding files say about each printed insertion-effect (type,")
    w("// slot): the byte reaches a control, or it does not and the row says why.")
    w("// gs_efx_bindings.h carries the assigned rows alone, because that is all")
    w("// the chain builder needs; this carries all five forms, because telling a")
    w("// form apart from the four it is not is the test's whole job. A byte")
    w("// counted as a state has to be inert, and one counted as unmapped has to")
    w("// belong to a type that realises nothing -- neither is checkable from a")
    w("// table that only lists what did reach a control.")
    w("")
    w("#pragma once")
    w("")
    w("#include <array>")
    w("#include <cstdint>")
    w("#include <string_view>")
    w("")
    w("namespace sonare::midi::synth {")
    w("")
    w("// The five forms a binding row takes. Exactly one applies to a row, and")
    w("// the coverage equation adds these five up to the printed parameter count.")
    for index, form in enumerate(FORMS):
        w(f"inline constexpr uint8_t {FORM_CONSTANT[form]} = {index};")
    w("")
    w("/// No name at this position (an assigned row, or a state row that names no")
    w("/// missing control).")
    w(f"inline constexpr uint8_t kGsEfxJoinNoName = 0x{NO_NAME:02X};")
    w("/// No reason at this position, which is what an assigned row carries.")
    w(f"inline constexpr uint16_t kGsEfxJoinNoReason = 0x{NO_REASON:04X};")
    w("")
    w("/// One adjudicated insertion-effect parameter.")
    w("struct GsEfxJoinRow {")
    w("  uint16_t type;         ///< The two type bytes, MSB in the high byte.")
    w("  uint8_t slot;          ///< Slot index from the first parameter address.")
    w("  uint8_t form;          ///< One of the kGsEfxJoin* form constants.")
    w("  uint8_t absent_stage;  ///< Index into kGsEfxJoinStages, or kGsEfxJoinNoName.")
    w("  uint8_t absent_key;    ///< Index into kGsEfxJoinKeys, or kGsEfxJoinNoName.")
    w("  uint16_t reason;       ///< Index into kGsEfxJoinReasons, or kGsEfxJoinNoReason.")
    w("};")
    w("")
    w("// The inserts and controls a state row names as missing. A state row that")
    w("// says an insert has no X names the X here, so the claim expires when the")
    w("// insert grows one rather than standing as prose nothing reads.")
    w(f"inline constexpr std::array<std::string_view, {len(stages)}> kGsEfxJoinStages = {{{{")
    for stage in stages:
        w(f'    "{stage}",')
    w("}};")
    w("")
    w(f"inline constexpr std::array<std::string_view, {len(keys)}> kGsEfxJoinKeys = {{{{")
    for key in keys:
        w(f'    "{key}",')
    w("}};")
    w("")
    w("// Every reason spelled once; rows sharing a reason share its index.")
    w(f"inline constexpr std::array<std::string_view, {len(reasons)}> kGsEfxJoinReasons = {{{{")
    for reason in reasons:
        w(f'    "{escaped(reason)}",')
    w("}};")
    w("")
    w("// Sorted by (type, slot), so one unit's rows are contiguous.")
    w(f"inline constexpr std::array<GsEfxJoinRow, {len(entries)}> kGsEfxJoin = {{{{")
    stage_index = {name: i for i, name in enumerate(stages)}
    key_index = {name: i for i, name in enumerate(keys)}
    reason_index = {text: i for i, text in enumerate(reasons)}
    for entry in entries:
        absent = entry["absent"]
        absent_stage = "kGsEfxJoinNoName" if absent is None else str(stage_index[absent[0]])
        absent_key = "kGsEfxJoinNoName" if absent is None else str(key_index[absent[1]])
        reason = (
            "kGsEfxJoinNoReason" if entry["reason"] is None else str(reason_index[entry["reason"]])
        )
        w(
            f"    {{0x{entry['type']:04X}, {entry['slot']}, {FORM_CONSTANT[entry['form']]}, "
            f"{absent_stage}, {absent_key}, {reason}}},"
        )
    w("}};")
    w("")
    w("// The counts the files declare. A test measures its own and requires these,")
    w("// so a rendering that dropped rows reads as a disagreement rather than as a")
    w("// smaller clean run.")
    for form in FORMS:
        w(f"inline constexpr int {FORM_ROWS_CONSTANT[form]} = {counts[form]};")
    w("")
    w("/// Controls the assigned rows declare, which is one per key and so larger")
    w("/// than the assigned row count as soon as a row names several.")
    w(f"inline constexpr int kGsEfxJoinDeclaredKeys = {declared_keys};")
    w("")
    w("}  // namespace sonare::midi::synth")
    return "\n".join(out) + "\n"


def escaped(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')


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
    entries, declared_keys = collect(load_rows(args.bindings))
    if not entries:
        sys.exit("no binding row was adjudicated; the header would classify nothing")
    rendered = laid_out(render(entries, declared_keys), args.header)

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
