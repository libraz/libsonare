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

``kGsEfxBindingRows`` holds translated and designed rows as
``GsEfxBindingRow`` (declared in ``gs_efx_convert.h``), each carrying its law
by value -- a designed law resolved from ``tools/gs/efx-designed-laws.json``,
a ratio's endpoints -- and ``kGsEfxEnables`` holds the enables rows.

Usage::

    bindings_header.py [--bindings tools/gs/efx-bindings]
                       [--tables tools/gs/efx-tables.json]
                       [--laws tools/gs/efx-designed-laws.json]
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
ROOT = Path(__file__).resolve().parents[2]
CONVERT_HEADER = ROOT / "src" / "midi" / "synth" / "gs_efx_convert.h"
RANGES_HEADER = ROOT / "src" / "effects" / "common" / "control_ranges.h"

# Controls whose accepted range src/effects/common/control_ranges.h defines once,
# on the inserts that clamp to it. A row reaching past one would be flattened at
# the receiver, so the header is refused instead.
SHARED_LIMIT_STAGES = ("effects.modulation.", "effects.delay.stereo")
SHARED_LIMITS = {
    "feedback": "kMaxFeedback",
    "centerDelayMs": "kMaxModulationPreDelayMs",
    "preDelayMs": "kMaxModulationPreDelayMs",
    "preDelay2Ms": "kMaxModulationPreDelayMs",
}

# Laws no measured table holds, numbered after the classes gs_efx_tables.h
# numbers. A table names the printed unit the endpoints are spelled in: percent
# reaches a control as the fraction, semitone and cent as printed.
BINDING_LAWS = {"ratio": ("percent", "semitone", "cent")}
# A carried law with a reader of its own, numbered after the binding laws.
CARRIED_CLASSES = {"drive": ("gain", "pedal")}
# Which gs_efx_convert.h constant names each extra class; the header's numbers
# are checked against this numbering rather than restated.
CLASS_CONSTANTS = {"ratio": "kGsEfxRowClassRatio", "drive": "kGsEfxRowClassDrive"}

FORM_CONSTANTS = {
    None: "kGsEfxFormNone",
    "linear": "kGsEfxFormLinear",
    "log": "kGsEfxFormLog",
    "db": "kGsEfxFormDb",
    "bipolar": "kGsEfxFormBipolar",
    "enum": "kGsEfxFormEnum",
}
MARK_LITERALS = {None: "0", "+": "'+'", "#": "'#'"}
NAME_INDEX_LIMIT = 0xFFFF

# The spelling of a printed byte range, as the archive's printed_values has it.
BYTE_RANGE_RE = re.compile(r"^([0-9A-F]{2})–([0-9A-F]{2})$")


def _module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        sys.exit(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Row forms, law resolution and printed-domain reading are coverage.py's; the
# generator reads them from there so the two agree by construction.
coverage = _module("gs_efx_coverage", Path(__file__).resolve().parent / "coverage.py")


def class_order(derive_path: Path) -> tuple[str, ...]:
    """The class numbering ``gs_efx_tables.h`` emits, read from its own source.

    Imported rather than restated: a second copy of the order would put the
    two headers' class numbers silently out of step, and nothing downstream
    compares them.
    """
    return tuple(_module("derive_efx_tables", derive_path).CLASS_ORDER)


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


def out_of(key: str) -> str:
    """Which quantity a row's control takes from a table that yields two."""
    spelled = key.lower()
    if spelled.endswith("undershoothz"):
        return "GsEfxOut::kUndershootHz"
    if spelled.endswith("deceltaus"):
        return "GsEfxOut::kDecelTau"
    if spelled.endswith("acceltaus"):
        return "GsEfxOut::kAccelTau"
    return "GsEfxOut::kValue"


def row_class_numbering(order: tuple[str, ...]) -> list[str]:
    """Measured classes, then the binding laws, then the carried classes with readers."""
    return list(order) + list(BINDING_LAWS) + list(CARRIED_CLASSES)


def check_convert_constants(order: tuple[str, ...], header: Path) -> None:
    """The extra class numbers and the enable capacity here and in gs_efx_convert.h agree."""
    text = header.read_text(encoding="utf-8")
    numbering = row_class_numbering(order)
    for name, constant in CLASS_CONSTANTS.items():
        match = re.search(rf"\b{constant}\s*=\s*(\d+)\s*;", text)
        if match is None or int(match.group(1)) != numbering.index(name):
            sys.exit(f"{header}: {constant} is not {numbering.index(name)}, this class's number")
    match = re.search(r"\bkGsEfxEnableMaxStages\s*=\s*(\d+)\s*;", text)
    if match is None or int(match.group(1)) != coverage.MAX_ENABLE_STAGES:
        sys.exit(f"{header}: kGsEfxEnableMaxStages is not {coverage.MAX_ENABLE_STAGES}")


def shared_limits(header: Path) -> dict[str, float]:
    """The float constants the shared range header defines, by name."""
    text = header.read_text(encoding="utf-8")
    found = {
        name: float(value)
        for name, value in re.findall(r"inline constexpr float (k\w+) = (-?[0-9.]+)f;", text)
    }
    for name in set(SHARED_LIMITS.values()):
        if name not in found:
            sys.exit(f"{header}: {name} is not defined")
    return found


def reach(entry: dict, classes: dict) -> float | None:
    """The largest magnitude a row's byte can produce, or None where nothing here spells it."""
    if entry["law"] is not None:
        return max(abs(float(entry["law"]["lo"])), abs(float(entry["law"]["hi"])))
    gs_class, _, table = entry["label"].partition(".")
    if gs_class == "ratio":
        scale = 100.0 if table == "percent" else 1.0
        return max(abs(unit) for unit in entry["unit"]) / scale
    body = classes.get(gs_class, {}).get("tables", {}).get(table, {})
    if body.get("kind") != "breakpoints":
        return None
    values = [
        abs(float(value))
        for knot in body["breakpoints"]
        for name, value in knot.items()
        if name != "setting" and isinstance(value, (int, float)) and not isinstance(value, bool)
    ]
    return max(values) if values else None


def check_shared_limits(entries: list[dict], classes: dict, limits: dict[str, float]) -> None:
    """Refuse a row whose byte reaches past the shared limit of the control it drives."""
    for e in entries:
        constant = SHARED_LIMITS.get(e["key"])
        if constant is None or not e["stage"].startswith(SHARED_LIMIT_STAGES):
            continue
        where = f"{e['type']:04X} slot {e['slot']} ({e['label']} -> {e['stage']}.{e['key']})"
        reached = reach(e, classes)
        if reached is None:
            sys.exit(f"{where}: no reach can be read for a control limited by {constant}")
        if reached > limits[constant] + 1e-6:
            sys.exit(
                f"{where}: reaches {reached:g}, past {constant} = {limits[constant]:g} in "
                f"{RANGES_HEADER.relative_to(ROOT)}; the receiver would flatten the top of it"
            )


def collect_rows(
    rows: list[dict], classes: dict, order: tuple[str, ...], laws: dict
) -> tuple[list[dict], list[dict]]:
    """One GsEfxBindingRow per (type, slot, key) translated or designed, and every enables row."""
    tables_of = {name: list(body["tables"]) for name, body in classes.items()}
    tables_of.update({name: list(tables) for name, tables in BINDING_LAWS.items()})
    tables_of.update({name: list(tables) for name, tables in CARRIED_CLASSES.items()})
    numbering = row_class_numbering(order)
    entries: list[dict] = []
    enables: list[dict] = []
    for source in rows:
        where = f"{source['_file']}[{source['_index']}]"
        form = coverage.row_form(source, where)
        if form == "enables":
            enables.append(enable_entry(source, where, form))
            continue
        # A row with alternatives is one generated row per target stage.
        for row in coverage.targets_of(source, where):
            entry = {
                "type": parsed_type(row["type"]),
                "slot": int(row["slot"]),
                "mark": row.get("printed_mark"),
                "law": None,
                "byte": (0, 0),
                "unit": (0, 0),
            }
            if form == "translated":
                entry["kind"] = "kGsEfxRowTranslated"
                gs_class, table = row.get("class"), row.get("table")
                if gs_class in BINDING_LAWS:
                    lo_byte, hi_byte, lo_unit, hi_unit = printed_range(row, where)
                    entry["byte"], entry["unit"] = (lo_byte, hi_byte), (lo_unit, hi_unit)
            elif form == "designed":
                entry["kind"] = "kGsEfxRowDesigned"
                if "printed_values" not in row:
                    sys.exit(f"{where}: a designed row needs printed_values")
                entry["byte"] = coverage.byte_domain(row["printed_values"])
                law = row["designed"]["law"]
                if row["designed"]["basis"] == "invented":
                    entry["law"] = coverage.designed_law(laws, law)
                    if entry["law"] is None:
                        sys.exit(f"{where}: designed law {law!r} is not in the designed-law file")
                    gs_class, table = None, None
                else:
                    gs_class, table = law.split(".", 1)
            else:
                continue
            if gs_class is None:
                entry["class"], entry["table"], label = 0, 0, row["designed"]["law"]
            else:
                if gs_class not in numbering or table not in tables_of[gs_class]:
                    sys.exit(f"{where}: {gs_class}.{table} is not a class this header numbers")
                entry["class"] = numbering.index(gs_class)
                entry["table"] = tables_of[gs_class].index(table)
                label = f"{gs_class}.{table}"
            # A row reaching several same-named stages is one generated row per stage.
            for ordinal in coverage.ordinals_of(row, where, form):
                for key in keys_of(row):
                    entries.append(
                        dict(entry, stage=str(row["stage"]), ordinal=ordinal, key=key, label=label)
                    )
    entries.sort(key=lambda e: (e["type"], e["slot"], e["stage"], e["key"], e["ordinal"]))
    enables.sort(key=lambda e: (e["type"], e["slot"]))
    return entries, enables


def enable_entry(row: dict, where: str, form: str) -> dict:
    """One GsEfxEnable: the row's address and the stages its byte turns on."""
    return {
        "type": parsed_type(row["type"]),
        "slot": int(row["slot"]),
        "ordinal": coverage.ordinals_of(row, where, form)[0],
        **enable_of(row["enables"], where),
    }


def enable_of(enables: dict, where: str) -> dict:
    """An enables row's stages and the bytes that turn them on."""
    mode = "select" if "select" in enables else "stages"
    refs = enables[mode]
    limit = coverage.MAX_SELECT_STAGES if mode == "select" else coverage.MAX_ENABLE_STAGES
    if not isinstance(refs, list) or not 1 <= len(refs) <= limit:
        sys.exit(f"{where}: enables.{mode} names 1-{limit} stages")
    mask = [0, 0, 0, 0]
    for byte in enables.get("on_states", []) if mode == "stages" else []:
        mask[byte >> 5] |= 1 << (byte & 31)
    return {
        "mode": "kGsEfxEnableSelect" if mode == "select" else "kGsEfxEnableStages",
        "stages": [str(ref["stage"]) for ref in refs],
        "ordinals": [int(ref.get("ordinal", 0)) for ref in refs],
        "mask": mask,
    }


def float_literal(value: float) -> str:
    return f"{float(value)!r}f"


def law_constant(law_id: str) -> str:
    """``d.cutoff_hz`` -> ``kGsEfxLawCutoffHz``; ``none`` -> ``kGsEfxLawNone``."""
    return "kGsEfxLaw" + camel(law_id.removeprefix("d."))


def camel(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def render(rows: list[dict], enables: list[dict]) -> str:
    out: list[str] = []
    w = out.append

    w(f"// Generated by {GENERATED_BY} from tools/gs/efx-bindings/ -- do not edit.")
    w("//")
    w("// Which insert control each insertion-effect parameter byte drives, and which")
    w("// stages a switch or selector byte turns on. Every printed (type, slot) is")
    w("// one row here: translated through a measured table, driven through a")
    w("// designed law, or an enables row.")
    w("//")
    w("// A measured row names its law: `conv_class` and `table` index the same")
    w("// tables gs_efx_tables.h numbers, so a byte read here and a byte read through")
    w("// kGsEfxSlotConversions read alike.")
    w("")
    w("#pragma once")
    w("")
    w("#include <array>")
    w("#include <cstdint>")
    w("#include <string_view>")
    w("")
    w('#include "midi/synth/gs_efx_convert.h"')
    w("")
    w("namespace sonare::midi::synth {")
    w("")
    render_rows(w, rows, enables)
    w("}  // namespace sonare::midi::synth")
    return "\n".join(out) + "\n"


def render_rows(w, rows: list[dict], enables: list[dict]) -> None:
    """kGsEfxBindingRows and kGsEfxEnables, with the name tables they index."""
    stages = sorted({e["stage"] for e in rows} | {s for e in enables for s in e["stages"]})
    keys = sorted({e["key"] for e in rows})
    if max(len(stages), len(keys)) > NAME_INDEX_LIMIT:
        sys.exit("the row name tables outgrew their index width")
    stage_index = {name: i for i, name in enumerate(stages)}
    key_index = {name: i for i, name in enumerate(keys)}

    w("// The stages and controls kGsEfxBindingRows and kGsEfxEnables point at.")
    w(f"inline constexpr std::array<std::string_view, {len(stages)}> kGsEfxRowStages = {{{{")
    for stage in stages:
        w(f'    "{stage}",')
    w("}};")
    w("")
    w(f"inline constexpr std::array<std::string_view, {len(keys)}> kGsEfxRowKeys = {{{{")
    for key in keys:
        w(f'    "{key}",')
    w("}};")
    w("")
    # Each designed law the rows use, spelled once and copied into a row by name.
    laws = {"none": {"form": None, "lo": 0, "hi": 0, "n_states": 0}}
    for e in rows:
        if e["law"] is not None:
            laws[e["label"]] = e["law"]
    w("// The designed laws the rows below hold by value, one name per law id.")
    for law_id, law in laws.items():
        w(
            f"inline constexpr GsEfxDesignedLaw {law_constant(law_id)} = "
            f"{{{FORM_CONSTANTS[law['form']]}, {float_literal(law['lo'])}, "
            f"{float_literal(law['hi'])}, {law['n_states']}}};"
        )
    w("")
    w("// Translated and designed rows alike, read through gs_efx_binding_value. A")
    w("// designed row holds its law by value; a ratio row holds its printed ends.")
    w("// One row per (type, slot, key, ordinal), sorted in that order.")
    w(f"inline constexpr std::array<GsEfxBindingRow, {len(rows)}> kGsEfxBindingRows = {{{{")
    for e in rows:
        spelled_law = law_constant(e["label"] if e["law"] is not None else "none")
        w(f"    // {e['label']} -> {e['stage']}.{e['key']}")
        w(
            f"    {{0x{e['type']:04X}, {e['slot']}, {e['kind']}, {e['class']}, {e['table']}, "
            f"{spelled_law}, 0x{e['byte'][0]:02X}, 0x{e['byte'][1]:02X}, {e['unit'][0]}, "
            f"{e['unit'][1]}, {out_of(e['key'])}, {stage_index[e['stage']]}, {e['ordinal']}, "
            f"{key_index[e['key']]}, {MARK_LITERALS[e['mark']]}}},"
        )
    w("}};")
    w("")
    w("// The switches and selectors: which stages a byte turns on. Sorted by (type, slot).")
    if not enables:
        w("inline constexpr std::array<GsEfxEnable, 0> kGsEfxEnables = {};")
    else:
        w(f"inline constexpr std::array<GsEfxEnable, {len(enables)}> kGsEfxEnables = {{{{")
    for e in enables:
        padding = coverage.MAX_ENABLE_STAGES - len(e["stages"])
        spelled_stages = ", ".join(str(stage_index[s]) for s in e["stages"]) + ", 0" * padding
        spelled_ordinals = ", ".join(str(o) for o in e["ordinals"]) + ", 0" * padding
        spelled_mask = ", ".join(f"0x{word:X}u" for word in e["mask"])
        w(
            f"    {{0x{e['type']:04X}, {e['slot']}, {e['mode']}, {{{spelled_stages}}}, "
            f"{{{spelled_ordinals}}}, {len(e['stages'])}, {{{spelled_mask}}}}},"
        )
    if enables:
        w("}};")
    w("")


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
    parser.add_argument("--laws", type=Path, default=Path("tools/gs/efx-designed-laws.json"))
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
    check_convert_constants(order, CONVERT_HEADER)
    rows = load_rows(args.bindings)
    row_entries, enables = collect_rows(rows, classes, order, coverage.load_laws(args.laws))
    if not row_entries:
        sys.exit("no binding row carries a stage; the header would bind nothing")
    check_shared_limits(row_entries, classes, shared_limits(RANGES_HEADER))
    rendered = laid_out(render(row_entries, enables), args.header)

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
        print(f"binding header is current: {args.header} ({len(row_entries)} rows)")
        return 0

    args.header.write_text(rendered, encoding="utf-8")
    print(f"wrote {args.header} ({len(row_entries)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
