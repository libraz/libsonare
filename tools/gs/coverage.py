"""Report how much of the GS insertion-effect parameter block is adjudicated.

The SC-8850 prints 770 (type, slot) parameters across its insertion-effect
block. This reads the archive's own record of which of those 770 carry a
printed value, and the hand-written binding files that say what each one
does -- translated into an insert control, driven through a designed law,
switching stages on, or one of the older forms a row may still be written
in. Every printed row is adjudicated exactly once, in one of seven forms: the
first line counts ``translated + state + unmapped + unreadable + builder``,
the second ``translated + designed + enables``, and the run fails unless all
seven add to printed.

Of the archive, **only** ``data/units/*/efx-params/*.json`` is read. Only the
mark transcription script ``marks_from_manual.py`` opens ``documents/``;
this script and the table derivation do not -- that tree carries a licence
this repo does not have (see ``tools/gs/docs/efx-tables.md``). The printed
parameter name a name rule is matched against is the row's own
``printed_name``, which that script transcribes. Binding files are read from ``--bindings``; a missing directory or an empty
one is zero rows.

Usage::

    coverage.py --archive <archive root> [--bindings tools/gs/efx-bindings]
                [--rule-report]
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

# The binding-row forms, keyed by the field that names each one. A row carries
# exactly one; a `stage` with no `designed` beside it is a translated row.
FORM_KEYS = ("designed", "enables", "state", "unmapped", "unreadable", "builder")
OLD_TERMS = ("translated", "state", "unmapped", "unreadable", "builder")
NEW_TERMS = ("translated", "designed", "enables")
ALL_TERMS = ("translated", "designed", "enables", "state", "unmapped", "unreadable", "builder")

TOOLS = Path(__file__).resolve().parent
DEFAULT_LAWS = TOOLS / "efx-designed-laws.json"
DEFAULT_TABLES = TOOLS / "efx-tables.json"

BASES = ("carried", "invented")
# How the soundings drift check recognises that a carried or invented law has
# been superseded by a measurement; a row names at least one.
REPLACED_WHEN_KEYS = ("claim_names", "model_binding", "stage_passed", "covers")
PRINTED_MARKS = ("+", "#")
LAW_FORMS = ("linear", "log", "db", "bipolar", "enum")
# Laws the binding layer reads that no measured table holds. Their endpoints
# are the row's own printed range, so only a translated row may name one.
RATIO_LAWS = ("ratio.percent", "ratio.semitone", "ratio.cent")
ENABLES_LAW = "enables"
ENUM_PLACEHOLDER = "<n>"
# The most stages one enables row can name: the width of GsEfxEnable::stages.
MAX_ENABLE_STAGES = 4

TYPE_RE = re.compile(r"^[0-9A-Fa-f]{2} [0-9A-Fa-f]{2}$")
ADDRESS_RE = re.compile(r"^[0-9A-F]{2} [0-9A-F]{2} [0-9A-F]{2}$")
STATE_LIST_RE = re.compile(r"^[0-9A-F]{2}(/[0-9A-F]{2})+$")
MARKER_RE = re.compile(r"^\*(\d+)$")
RANGE_RE = re.compile(r"^[0-9A-F]{2}–[0-9A-F]{2}$")
WHOLE_BYTE = "00–7F"

# Where a row's identification of its slot came from, when it was not the archive.
NAMED_BY = ("the parameter list",)

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


def check_class_against_printed(row: dict, values: str, gs_class: str | None = None) -> None:
    """Success condition 5, as far as it decides mechanically today.

    Three shapes are checked: a *N marker whose class disagrees with the
    marker's own family, an explicit small enumeration carrying a continuous
    class, and a ratio row on anything but a range printed with a unit.
    Anything else -- a marker with no known family, say -- is left alone
    rather than given an invented rule.
    """
    gs_class = gs_class or row.get("class")
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


def load_laws(path: Path) -> dict:
    """The designed laws and name rules, with every law and pattern well-formed."""
    laws = load(path)
    for law_id, law in laws["laws"].items():
        if not law_id.startswith("d."):
            sys.exit(f"{path}: designed law {law_id!r} must start with 'd.'")
        if law.get("form") not in LAW_FORMS or law["form"] == "enum":
            sys.exit(f"{path}: {law_id} has form {law.get('form')!r}; enum laws are the family")
        if not all(isinstance(law.get(end), (int, float)) for end in ("lo", "hi")):
            sys.exit(f"{path}: {law_id} needs numeric lo and hi")
        if law["form"] == "log" and (law["lo"] <= 0 or law["hi"] <= 0):
            sys.exit(f"{path}: {law_id} is a log law and needs both ends positive")
    for index, rule in enumerate(laws["name_rules"]):
        if "law" not in rule or not ({"name", "printed"} & set(rule)):
            sys.exit(f"{path}: name_rules[{index}] needs a law and a name or printed pattern")
        for field in ("name", "printed", "unless"):
            if field in rule:
                try:
                    re.compile(rule[field])
                except re.error as failure:
                    sys.exit(f"{path}: name_rules[{index}].{field} does not compile ({failure})")
    return laws


def measured_laws(tables_path: Path) -> set[str]:
    """Every measured ``class.table`` the committed tables declare."""
    classes = load(tables_path)["classes"]
    return {f"{name}.{table}" for name, body in classes.items() for table in body["tables"]}


def enum_states(laws: dict, law_id: str) -> int | None:
    """The state count of a ``d.enum<n>`` id, or None for any other id."""
    family = laws["enum_family"]
    rest = law_id[len(family["prefix"]) :] if law_id.startswith(family["prefix"]) else ""
    if not rest.isdigit() or rest != str(int(rest)):
        return None
    count = int(rest)
    return count if 2 <= count <= family["max_states"] else None


def designed_law(laws: dict, law_id: str) -> dict | None:
    """An invented law resolved to the values a generated row holds, or None."""
    count = enum_states(laws, law_id)
    if count is not None:
        return {"form": "enum", "lo": 0, "hi": count - 1, "n_states": count}
    law = laws["laws"].get(law_id)
    if law is None:
        return None
    return {"form": law["form"], "lo": law["lo"], "hi": law["hi"], "n_states": 0}


def carried_laws(laws: dict, measured: set[str]) -> set[str]:
    return measured | set(laws["carried"])


def check_rules(laws: dict, measured: set[str]) -> None:
    """Every law a name rule gives has to be one a row could name."""
    known = carried_laws(laws, measured) | set(RATIO_LAWS) | set(laws["laws"]) | {ENABLES_LAW}
    enum_rule = laws["enum_family"]["prefix"] + ENUM_PLACEHOLDER
    for index, rule in enumerate(laws["name_rules"]):
        if rule["law"] != enum_rule and rule["law"] not in known:
            sys.exit(f"name_rules[{index}] gives {rule['law']!r}, which no row could name")


def state_count(values: str) -> int | None:
    """How many values a printed spelling names; None for a column pointer."""
    if STATE_LIST_RE.match(values):
        return len(values.split("/"))
    if RANGE_RE.match(values):
        lo, hi = (int(end, 16) for end in values.split("–"))
        return hi - lo + 1
    return None


def byte_domain(values: str) -> tuple[int, int]:
    """The first and last printed byte. A column pointer spans the whole byte."""
    if STATE_LIST_RE.match(values):
        states = values.split("/")
        return int(states[0], 16), int(states[-1], 16)
    if RANGE_RE.match(values):
        lo, hi = (int(end, 16) for end in values.split("–"))
        return lo, hi
    return 0x00, 0x7F


def rule_matches(laws: dict, name: str, values: str) -> list[str]:
    """The law each name rule gives a printed (name, values), for every rule that matches."""
    found: list[str] = []
    enum_rule = laws["enum_family"]["prefix"] + ENUM_PLACEHOLDER
    for rule in laws["name_rules"]:
        if "name" in rule and not re.fullmatch(rule["name"], name):
            continue
        if "printed" in rule and not re.fullmatch(rule["printed"], values):
            continue
        if "unless" in rule and re.fullmatch(rule["unless"], name):
            continue
        law = rule["law"]
        if law == enum_rule:
            law = laws["enum_family"]["prefix"] + str(state_count(values))
        found.append(law)
    return found


def row_form(row: dict, where: str) -> str:
    """Which form a row is written in. Shared with the two header generators."""
    present = [key for key in FORM_KEYS if key in row]
    if "stage" in row and "designed" not in row:
        present.append("translated")
    if len(present) != 1:
        sys.exit(
            f"{where}: must carry exactly one of {sorted((*FORM_KEYS, 'stage'))}, has {present}"
        )
    form = present[0]
    if form == "designed" and "stage" not in row:
        sys.exit(f"{where}: a designed row names the stage and key it drives")
    return form


def check_replaced_when(where: str, value: object) -> None:
    if not isinstance(value, dict) or not value:
        sys.exit(f"{where}: needs replaced_when naming one of {list(REPLACED_WHEN_KEYS)}")
    for key, spec in value.items():
        if key not in REPLACED_WHEN_KEYS:
            sys.exit(f"{where}: replaced_when key {key!r} is not one of {list(REPLACED_WHEN_KEYS)}")
        if key in ("claim_names", "model_binding"):
            if not (
                isinstance(spec, list)
                and len(spec) == 2
                and isinstance(spec[0], str)
                and TYPE_RE.match(spec[0])
                and isinstance(spec[1], str)
                and ADDRESS_RE.match(spec[1])
            ):
                sys.exit(f'{where}: replaced_when.{key} must be ["MM LL", "40 03 XX"]')
        elif key == "stage_passed":
            if not (isinstance(spec, str) and TYPE_RE.match(spec)):
                sys.exit(f'{where}: replaced_when.stage_passed must be a "MM LL" type')
        elif not (isinstance(spec, str) and spec):
            sys.exit(f"{where}: replaced_when.covers must name the item it covers")


def check_designed(row: dict, values: str, laws: dict, measured: set[str]) -> str:
    """A designed row's law against the vocabulary and the printed shape; returns its basis."""
    where = row_label(row)
    designed = row["designed"]
    if not isinstance(designed, dict):
        sys.exit(f"{where}: designed must be an object")
    basis = designed.get("basis")
    law = designed.get("law")
    if basis not in BASES:
        sys.exit(f"{where}: designed.basis {basis!r} is not one of {list(BASES)}")
    if not isinstance(law, str):
        sys.exit(f"{where}: designed.law must name a law")
    check_replaced_when(where, designed.get("replaced_when"))
    for field in ("class", "table", "range"):
        if field in row:
            sys.exit(f"{where}: a designed row names its law in designed.law, not {field!r}")
    names = row["keys"] if "keys" in row else [row.get("key")]
    if not names or any(not isinstance(name, str) for name in names):
        sys.exit(f"{where}: a designed row needs 'key' or 'keys'")
    if "printed_values" not in row:
        sys.exit(f"{where}: a designed row needs printed_values, its copy of the printed range")

    if basis == "carried":
        if not isinstance(designed.get("from"), str) or not designed["from"]:
            sys.exit(f"{where}: a carried law names the claim it comes from in designed.from")
        if law in RATIO_LAWS:
            sys.exit(f"{where}: {law} reads the row's own printed ends, so it is translated")
        if law not in carried_laws(laws, measured):
            sys.exit(f"{where}: carried law {law!r} is not a measured class.table or carried law")
        check_class_against_printed(row, values, law.split(".")[0])
        return basis

    if designed_law(laws, law) is None:
        sys.exit(f"{where}: designed law {law!r} is not in {DEFAULT_LAWS.name}")
    count = state_count(values)
    states = enum_states(laws, law)
    if states is not None:
        if count is None or count > laws["enum_family"]["max_states"]:
            sys.exit(f"{where}: {law} is an enum law and {values!r} is continuous")
        if count != states:
            sys.exit(f"{where}: {law} names {states} states and {values!r} prints {count}")
    elif STATE_LIST_RE.match(values):
        sys.exit(f"{where}: {values!r} is a state list and {law} is not an enum law")
    return basis


def check_enables(row: dict, values: str) -> str:
    where = row_label(row)
    enables = row["enables"]
    if not isinstance(enables, dict):
        sys.exit(f"{where}: enables must be an object")
    if enables.get("basis") != "invented":
        sys.exit(f"{where}: an enables row is invented, not {enables.get('basis')!r}")
    check_replaced_when(where, enables.get("replaced_when"))
    modes = [mode for mode in ("stages", "select") if mode in enables]
    if len(modes) != 1:
        sys.exit(f"{where}: enables carries exactly one of 'stages' and 'select', has {modes}")
    refs = enables[modes[0]]
    if not isinstance(refs, list) or not 1 <= len(refs) <= MAX_ENABLE_STAGES:
        sys.exit(f"{where}: enables.{modes[0]} names 1-{MAX_ENABLE_STAGES} stages")
    for ref in refs:
        ordinal = ref.get("ordinal", 0) if isinstance(ref, dict) else None
        if (
            not isinstance(ref, dict)
            or set(ref) - {"stage", "ordinal"}
            or not isinstance(ref.get("stage"), str)
            or not isinstance(ordinal, int)
            or not 0 <= ordinal <= 0xFF
        ):
            sys.exit(f'{where}: an enables stage is {{"stage": name, "ordinal": n}}')
    if modes[0] == "select":
        if "on_states" in enables:
            sys.exit(f"{where}: a select row picks by state and carries no on_states")
        if state_count(values) != len(refs) or not STATE_LIST_RE.match(values):
            sys.exit(
                f"{where}: select names {len(refs)} stages and {values!r} is not that many states"
            )
        return "invented"
    on_states = enables.get("on_states")
    if (
        not isinstance(on_states, list)
        or not on_states
        or len(set(on_states)) != len(on_states)
        or not all(isinstance(b, int) and 0 <= b <= 0x7F for b in on_states)
    ):
        sys.exit(f"{where}: enables.on_states must be distinct bytes 0-127")
    if STATE_LIST_RE.match(values):
        printed_bytes = {int(state, 16) for state in values.split("/")}
        if not set(on_states) <= printed_bytes:
            sys.exit(f"{where}: on_states {on_states} names a byte {values!r} does not print")
    return "invented"


def check_name(row: dict, form: str, values: str, laws: dict) -> None:
    """The one law the row's printed name gives, against what the row wrote."""
    where = row_label(row)
    name = row.get("printed_name")
    if not isinstance(name, str) or not name:
        sys.exit(f"{where}: a {form} row needs printed_name, transcribed by marks_from_manual.py")
    found = rule_matches(laws, name, values)
    if not found:
        sys.exit(f"{where}: no name rule matches printed name {name!r} ({values})")
    if len(found) > 1:
        sys.exit(f"{where}: {len(found)} name rules match printed name {name!r}: {found}")
    written = ENABLES_LAW if form == "enables" else row["designed"]["law"]
    if written != found[0]:
        sys.exit(
            f"{where}: written as {written!r}, and printed name {name!r} ({values}) follows "
            f"{found[0]!r}"
        )


def tally(
    rows: list[dict],
    printed: dict[str, dict[int, str]],
    laws: dict,
    measured: set[str],
) -> dict:
    check_rules(laws, measured)
    counts = {term: 0 for term in ALL_TERMS}
    basis = {name: 0 for name in BASES}
    per_msb_forms: dict[str, dict[str, int]] = {}
    declared_keys: set[tuple[str, str]] = set()
    claimed: set[tuple[str, int]] = set()
    names_checked = 0

    for row in rows:
        gs_type = row.get("type")
        slot = row.get("slot")
        if not isinstance(gs_type, str) or not TYPE_RE.match(gs_type):
            sys.exit(f'{row_label(row)}: type must be an "MM LL" hex pair')
        if not isinstance(slot, int) or not (0 <= slot <= 19):
            sys.exit(f"{row_label(row)}: slot must be an integer 0-19")

        form = row_form(row, row_label(row))

        slots = printed.get(gs_type)
        values = slots.get(slot) if slots else None
        if values is None:
            sys.exit(f"{row_label(row)}: names a (type, slot) with no printed value")
        # A row's copy of the spelling is what a generator reads without the
        # archive, so it is held to the archive here.
        if "named_by" in row and row["named_by"] not in NAMED_BY:
            sys.exit(f"{row_label(row)}: named_by {row['named_by']!r} is not one of {NAMED_BY}")
        if "printed_values" in row and row["printed_values"] != values:
            sys.exit(
                f"{row_label(row)}: printed_values {row['printed_values']!r} is not the "
                f"archive's {values!r}"
            )
        ordinal = row.get("ordinal", 0)
        if not isinstance(ordinal, int) or isinstance(ordinal, bool) or not 0 <= ordinal <= 0xFF:
            sys.exit(f"{row_label(row)}: ordinal must be an integer 0-255")
        if "printed_name" in row and not (
            isinstance(row["printed_name"], str) and row["printed_name"]
        ):
            sys.exit(f"{row_label(row)}: printed_name must be a non-empty string")
        if "printed_mark" in row and row["printed_mark"] not in PRINTED_MARKS:
            sys.exit(f"{row_label(row)}: printed_mark must be one of {PRINTED_MARKS}")

        key = (gs_type, slot)
        if key in claimed:
            sys.exit(f"{row_label(row)}: (type, slot) already claimed by another binding row")
        claimed.add(key)

        counts[form] += 1
        msb_forms = per_msb_forms.setdefault(gs_type.split()[0], {t: 0 for t in ALL_TERMS})
        msb_forms[form] += 1

        if form in ("translated", "designed"):
            stage = row["stage"]
            keys = row["keys"] if "keys" in row else [row.get("key")]
            if not keys or any(n is None for n in keys):
                sys.exit(f"{row_label(row)}: an assigned row needs 'key' or 'keys'")
            for name in keys:
                declared_keys.add((stage, name))
        if form == "translated":
            check_class_against_printed(row, values)
        elif form == "designed":
            basis[check_designed(row, values, laws, measured)] += 1
        elif form == "enables":
            basis[check_enables(row, values)] += 1

        # Rows still in the older forms are not held to the name rules.
        if form in ("designed", "enables"):
            check_name(row, form, values, laws)
            names_checked += 1

    total_printed = sum(len(slots) for slots in printed.values())
    unadjudicated = total_printed - len(claimed)
    return {
        "counts": counts,
        "basis": basis,
        "per_msb_forms": per_msb_forms,
        "declared_keys": len(declared_keys),
        "unadjudicated": unadjudicated,
        "printed": total_printed,
        "names_checked": names_checked,
    }


def summary_lines(result: dict, per_msb: dict[str, int]) -> list[str]:
    counts = result["counts"]
    basis = result["basis"]
    coverage_line = (
        "GS EFX coverage: printed={printed} translated={translated} state={state} "
        "unmapped={unmapped} unreadable={unreadable} builder={builder}".format(
            printed=result["printed"], **counts
        )
    )
    forms_line = (
        f"GS EFX forms: translated={counts['translated']} designed={counts['designed']} "
        f"enables={counts['enables']} carried={basis['carried']} invented={basis['invented']}"
    )
    lines = [
        coverage_line,
        forms_line,
        "per-MSB: " + " ".join(f"{msb}={per_msb[msb]}" for msb in sorted(per_msb)),
    ]
    for msb in sorted(set(per_msb) | set(result["per_msb_forms"])):
        forms = result["per_msb_forms"].get(msb, {t: 0 for t in ALL_TERMS})
        lines.append(f"forms[{msb}]: " + " ".join(f"{t}={forms[t]}" for t in ALL_TERMS))
    lines.append(f"declared_keys={result['declared_keys']}")
    lines.append(f"unadjudicated={result['unadjudicated']}")
    lines.append(f"name_rules: checked={result['names_checked']}")
    return lines


def rule_report(
    rows: list[dict],
    printed: dict[str, dict[int, str]],
    laws: dict,
) -> list[str]:
    """The law each printed name gives every row not written as translated.

    A dry run over rows still in the old forms: it names the rows the rules do
    not decide rather than stopping at the first.
    """
    lines: list[str] = []
    tallies = {"one": 0, "none": 0, "several": 0}
    for row in sorted(rows, key=lambda r: (r["type"], r["slot"])):
        if row_form(row, row_label(row)) == "translated":
            continue
        values = printed[row["type"]][row["slot"]]
        name = row.get("printed_name")
        found = rule_matches(laws, name, values) if name is not None else []
        verdict = "one" if len(found) == 1 else "none" if not found else "several"
        tallies[verdict] += 1
        lines.append(f"rule {row['type']} slot {row['slot']} {name!r} {values} -> {found}")
    lines.append(
        f"name_rules dry-run: rows={sum(tallies.values())} one={tallies['one']} "
        f"none={tallies['none']} several={tallies['several']}"
    )
    return lines


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
    ap.add_argument("--laws", type=Path, default=DEFAULT_LAWS, help="designed laws and name rules")
    ap.add_argument("--tables", type=Path, default=DEFAULT_TABLES, help="measured class tables")
    ap.add_argument(
        "--rule-report",
        action="store_true",
        help="print the law each printed name gives every row not written as translated",
    )
    args = ap.parse_args()

    archive = Path(args.archive)
    archive_keyed, canonical = load_printed(archive)

    # The archive-keyed view catches the archive's own filing moving (a new
    # 02-0C.json appearing, say); the canonical view is what binding files
    # are written against and is what gets printed below.
    assert_expected("archive-keyed", archive_keyed, EXPECTED_PER_MSB_ARCHIVE)
    assert_expected("canonical", canonical, EXPECTED_PER_MSB_CANONICAL)

    laws = load_laws(args.laws)
    measured = measured_laws(args.tables)
    rows = load_bindings(Path(args.bindings))

    if args.rule_report:
        print("\n".join(rule_report(rows, canonical, laws)))
        return 0

    result = tally(rows, canonical, laws, measured)
    print("\n".join(summary_lines(result, per_msb_of(canonical))))

    # Old and new forms may sit side by side; either way every printed row is
    # adjudicated exactly once.
    equation_holds = sum(result["counts"].values()) == result["printed"]
    if not equation_holds:
        sys.exit(1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
