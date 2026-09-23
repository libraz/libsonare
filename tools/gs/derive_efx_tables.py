"""Derive the insertion-effect conversion tables from an archive of unit measurements.

``src/midi/synth/docs/gs.md`` says the manual is a proxy for the machine and that
where the two disagree the machine decides. The EFX parameter block is where that
bites hardest: a Rate, a Time, a Freq or a Level byte is not the quantity it is
printed as, and libsonare has been running most of those bytes into an insert's
compile-time default. This reads an archive of what one individual SC-8850
answered and writes down, once, what each byte is worth -- the fourteen shared
conversions, and which (type, slot) pairs the archive gives one to.

**Nothing here decides anything.** A slot the archive does not reach is left
unreached and counted; a record that falls outside the inference it rests on is
reported rather than dropped or kept quietly. What this does is make the list
finite, the same arrangement `check_unit.py` next to it has.

Two inputs, both under the archive's CC0 dedication: the per-type parameter
records under ``data/units/<unit>/efx-params/``, and the claim files under
``inferences/<unit>/``. Nothing else is read.

The assignment is taken from structured record fields and never from a file name.
Each parameter carries ``printed_values`` -- a conversion-column pointer, or an
explicit byte range -- and that is what picks the table. Which records are the
power-on defaults is read the same way, from each record's own ``prepared`` list
rather than from the suffix on its name.

Usage::

    derive_efx_tables.py --archive <archive root> \\
                         --out tools/gs/efx-tables.json \\
                         --header src/midi/synth/gs_efx_tables.h

    derive_efx_tables.py --from-json tools/gs/efx-tables.json \\
                         --header src/midi/synth/gs_efx_tables.h
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import itertools
import json
import math
import re
import subprocess
import sys
from pathlib import Path

GENERATED_BY = "tools/gs/derive_efx_tables.py"

# Concrete readings shown per aggregated finding on stdout. The tables file
# carries every one of them; this only bounds what a run prints.
SAMPLES = 4

# The EFX parameter block: twenty bytes from the first parameter address. The
# type itself sits at 40 03 00/01 and is not one of them.
FIRST_PARAMETER = 0x400303
PARAMETER_SLOTS = 20

# The stage whose records carry a default per parameter, read from the archive's
# own index rather than by globbing: which records a stage holds is the archive's
# filing and not this tool's business (check_unit.py reads a stage the same way).
PARAMS_STAGE = "efx-params"

# A record measuring the power-on defaults prepares two things and no more: it
# selects the type, and it routes the stimulus part through the effect. A record
# with anything further prepared measured a different condition -- a modulator
# parked, an amp switched on, a stage mixed in -- and its defaults are that
# condition's. Read from the record's own `prepared` list because a name is a
# spelling somebody chose and this is not: forty-four of the hundred and nine
# records here are suffixed, and the suffix is not what makes them one.
PART_ROUTING_ADDRESS = "40 42 22"
TYPE_ADDRESS = "40 03 00"

# The internal rate every independent reading in the archive converges on. It is
# what a delay-time ladder is cut to and what the rotary's acceleration constant
# factors by; it is NOT a rate anything here renders at (docs/gs.md, and the
# design's rule that a coefficient is never stored, only the physical quantity).
UNIT_CLOCK_HZ = 32000

# Preferred numbers, one decade of the third-octave series. The frequency tables
# are the named third octaves between the two ends a slot's own record prints, so
# the series is derived here and checked against the one the archive's claim
# names rather than copied from anywhere.
THIRD_OCTAVE_DECADE = (100, 125, 160, 200, 250, 315, 400, 500, 630, 800)

# The fourteen conversion classes, in the order they are reported and emitted.
# Written out rather than taken from the table below, so that a class dropped
# from the table is a missing key here and not a shorter report.
CLASS_ORDER = (
    "rate",
    "delay_time",
    "freq",
    "gain",
    "level",
    "width",
    "wave",
    "pan",
    "balance",
    "azimuth",
    "accel",
    "post_gain",
    "window",
    "corner",
)

# What picks each class, and what bounds it.
#
# `columns` are the `printed_values` spellings that select the class. A pointer
# into the unit's conversion grid (`*1`-`*14`) names one column and therefore one
# table. An explicit range does not, in general: `00-7F` is the whole byte and is
# printed against three hundred and twenty-one parameters that have nothing to do
# with each other.
#
# A spelling may also map to `{address: table}`, where one spelling names two
# tables and only the address says which -- the equaliser's two corner bytes.
#
# `scoped_by_address` is that distinction made mechanical. Where the printed
# spelling picks the table, the inference's own address list is informational and
# a record outside it is reported (the chorus pre-filter at `40 03 04` is a real
# one). Where the spelling is the bare byte range, the address is what is left to
# select on, so it is part of the selector and a slot outside it is not reachable
# at all rather than a finding.
#
# Every class is intersected with its inference's `about.types` whichever it is.
# An inference's `rests_on` list carries the records that refuted its rivals as
# well as the ones that carry it -- the frequency claim cites a reading taken on
# the phaser's corner byte, and its own `about.types` excludes the phaser -- so
# deriving from `rests_on` without the intersection hands the phaser a table the
# claim says in as many words that it does not have.
CLASSES = {
    "rate": {
        "inference": "rate-the-byte-indexes-a-printed-table",
        "columns": {"*7": "narrow", "*6": "wide"},
        "scoped_by_address": False,
        "quantity": "Hz",
    },
    "delay_time": {
        "inference": "time-the-byte-indexes-a-ladder-cut-to-whole-samples",
        "columns": {
            "*1": "pre_delay",
            "*2": "time1",
            "*3": "time2",
            "*4": "time3",
            "*5": "time4",
        },
        "scoped_by_address": False,
        "quantity": "ms",
    },
    "freq": {
        "inference": "freq-the-byte-indexes-a-third-octave-table",
        "columns": {"*10": "eq", "*9": "pre_filter", "*8": "damping"},
        "scoped_by_address": False,
        "quantity": "Hz",
    },
    "gain": {
        "inference": "gain-the-byte-is-a-decibel-a-step",
        "columns": {"34–4C": "tone"},
        "scoped_by_address": False,
        "quantity": "dB",
    },
    "level": {
        "inference": "level-the-multiplier-is-a-seventh-bit-table",
        "columns": {"00–7F": "output"},
        "scoped_by_address": True,
        "quantity": "multiplier",
    },
    "width": {
        "inference": "width-the-byte-halves-the-printed-table",
        "columns": {"00/01/02/03/04": "section"},
        "scoped_by_address": False,
        "quantity": "octaves at the half-gain points",
    },
    "wave": {
        "inference": "wave-the-byte-picks-one-of-five-fixed-shapes",
        "columns": {"00/01/02/03/04": "modulator"},
        "scoped_by_address": False,
        "quantity": "shape",
    },
    "pan": {
        "inference": "pan-one-multiplier-a-side-under-a-term-added-after-it",
        "columns": {"00–7F": "output"},
        "scoped_by_address": True,
        "quantity": "multiplier",
    },
    "balance": {
        "inference": "balance-two-ramps-that-meet-at-full",
        "columns": {"00–7F": "effect"},
        "scoped_by_address": True,
        "quantity": "multiplier",
    },
    "azimuth": {
        "inference": "azimuth-a-byte-rounded-to-a-quarter-and-clamped",
        "columns": {"*13": "placement"},
        "scoped_by_address": False,
        "quantity": "degrees",
    },
    "accel": {
        "inference": "0122-an-acceleration-byte-is-four-bits-and-a-multiplier",
        "columns": {"*14": "rotor"},
        "scoped_by_address": False,
        "quantity": "divisor",
    },
    "post_gain": {
        "inference": "post-gain-four-fixed-steps-of-six-decibels",
        "columns": {"00/01/02/03": "makeup"},
        "scoped_by_address": True,
        "quantity": "dB",
    },
    # The law is read on one type; `reach_also_by` names claims that measured a
    # further type answering to the same table, which widens reach and not the law.
    "window": {
        "inference": "0160-a-pitch-byte-is-read-out-of-a-window-the-mode-byte-picks",
        "reach_also_by": ("0161-a-feedback-shifters-mode-byte-picks-the-same-five-lengths",),
        "columns": {"00–04": "splice"},
        "scoped_by_address": True,
        "quantity": "ms",
    },
    # `00/01` is every on/off switch's spelling too, so the address is part of the
    # selector, and it is what separates the low corner's table from the high one's.
    "corner": {
        "inference": "0100-a-chain-of-four-sections-and-an-output-gain",
        "columns": {"00/01": {"40 03 03": "low", "40 03 05": "high"}},
        "scoped_by_address": True,
        "quantity": "Hz",
    },
}

WHAT_THIS_CANNOT_SEE = [
    (
        "One unit is one unit. Every number here is what one individual machine "
        "answered, and no second one has been measured. Entries flagged "
        "unit_specific are the ones a second machine is most likely to disagree "
        "with; that a entry is not flagged is not a claim that it would agree."
    ),
    (
        "A slot with no printed_values field is not reached, and nothing here "
        "says whether it carries a quantity at all. Five hundred and thirty of "
        "the unit's parameters are in that state."
    ),
    (
        "Reach is a statement about the archive, not about libsonare. A (type, "
        "slot) pair reached here still has to have somewhere to go in the insert "
        "it maps to before anything is translated, and that count is taken "
        "elsewhere."
    ),
    (
        "The class assignment is taken from the printed spelling a record carries "
        "and from the type list its inference claims. A slot whose spelling "
        "matches a class it is not on would be assigned to it, and nothing here "
        "would notice: what would notice is a reading of that slot."
    ),
    (
        "Where a class is selected by the bare byte range rather than by a "
        "conversion column, the inference's address list is part of the selector. "
        "A slot of one of those types carrying the same quantity at another "
        "address is invisible here rather than reported."
    ),
    (
        "The defaults are the bytes the unit powered up holding. The manual's own "
        "printed statement of each default is not transcribed anywhere in the "
        "archive, so nothing here compares the two."
    ),
    (
        "A ladder or a table is checked against the records that admit a reading. "
        "A setting no record admitted is carried on the law alone, and an entry "
        "flagged approximate is one no reading placed at all."
    ),
    (
        "Whether a slot that returns a constant fraction of its cited column is on "
        "a second law or on the same one read through something else. The fraction "
        "and the shortfall are both reported for a slot this derivation does not "
        "reach, and neither is acted on: separating them wants a reading the unit "
        "can be asked for and this cannot produce."
    ),
]


def spelled_type(code: int) -> str:
    return f"{(code >> 8) & 0xFF:02X} {code & 0xFF:02X}"


def parsed_type(text: str) -> int:
    high, low = (int(b, 16) for b in text.split())
    return (high << 8) | low


def parsed_address(text: str) -> int:
    high, mid, low = (int(b, 16) for b in text.split())
    return (high << 16) | (mid << 8) | low


def spelled_address(addr: int) -> str:
    return f"{(addr >> 16) & 0xFF:02X} {(addr >> 8) & 0xFF:02X} {addr & 0xFF:02X}"


def octaves(hz: float, against: float) -> float:
    return abs(math.log2(hz / against))


def third_octaves(low: float, high: float) -> list[float]:
    """The named third octaves from @p low to @p high inclusive.

    Derived from the preferred-number decade rather than transcribed, and checked
    against the series the frequency claim names before it is used for anything.
    """
    out: list[float] = []
    decade = 0.1
    while decade <= 1e6:
        for step in THIRD_OCTAVE_DECADE:
            value = round(step * decade, 6)
            if low * 0.999 <= value <= high * 1.001:
                out.append(float(value) if value >= 1 else value)
        decade *= 10
    return out


def entry(**fields) -> dict:
    """One entry of a table, carrying the two flags every entry has.

    The flags are in the schema from the first version deliberately: adding them
    later would be a format change, and the two they are for are already known --
    a value this unit alone returns, and a value no reading placed.
    """
    fields.setdefault("unit_specific", False)
    fields.setdefault("approximate", False)
    return fields


# The three values `source` takes. It is the provenance of the conversion law,
# a projection of the two flags above, and says nothing about how a slot is
# bound to an insert -- that is a different axis and a different count.
SOURCE_MEASURED = "measured"
SOURCE_ASSIGNED = "assigned"
SOURCE_UNIT_OVERRIDES_ASSIGNED = "unit_overrides_assigned"
SOURCE_VALUES = (SOURCE_MEASURED, SOURCE_ASSIGNED, SOURCE_UNIT_OVERRIDES_ASSIGNED)


def carries_the_flags(node) -> bool:
    """Whether @p node is one of the dicts `source` is a projection of.

    Both flags have to be real booleans. The `schema` block carries the same two
    keys holding their prose descriptions, and it is a statement about entries
    rather than one of them.
    """
    return (
        isinstance(node, dict)
        and isinstance(node.get("unit_specific"), bool)
        and isinstance(node.get("approximate"), bool)
    )


def fold_source(unit_specific: bool, approximate: bool) -> str:
    """The projection where the flags are a fold over entries.

    A table and a `map` row take the strongest statement any entry under them
    makes, so the two flags there are not exclusive: a table can hold an entry a
    reading overruled beside one no reading placed at all. Reading it as
    overruled is what says the table carries a value this individual unit
    returned, which is the fact a second machine has to be checked against.
    """
    if unit_specific:
        return SOURCE_UNIT_OVERRIDES_ASSIGNED
    if approximate:
        return SOURCE_ASSIGNED
    return SOURCE_MEASURED


def cell_source(unit_specific: bool, approximate: bool, where: str) -> str:
    """The projection where the flags are one entry's own.

    Both at once is refused rather than resolved here: on a single entry it
    reads "no reading placed this, and a reading overruled the law", which is
    not a state either flag can be in, so meeting it means the two no longer
    mean what the schema says and a winner picked here would bury that.
    """
    if unit_specific and approximate:
        sys.exit(
            f"{where} is one entry flagged unit_specific and approximate at once. No reading "
            "placed it and a reading overruled the law cannot both hold; one of the two flags "
            "is being written for something other than what the schema says it means."
        )
    return fold_source(unit_specific, approximate)


def stamp_sources(node, path: str) -> bool:
    """Give every provenance-carrying dict under @p node its `source`.

    One pass over the finished tables rather than a line in each builder: cells
    and the table-level folds over them carry the same two flags, and several
    tables write their dicts inline rather than through `entry`, so a table
    added later would otherwise be the one that quietly emits no `source`.

    Returns whether anything at or under @p node carried the flags, which is
    what separates the two projections: a dict with no flag-carrying descendant
    is one entry and its flags are exclusive, and a dict with one is a fold.
    """
    found = False
    if isinstance(node, dict):
        for key, value in node.items():
            found |= stamp_sources(value, f"{path}.{key}")
        if carries_the_flags(node):
            unit_specific, approximate = node["unit_specific"], node["approximate"]
            node["source"] = (
                fold_source(unit_specific, approximate)
                if found
                else cell_source(unit_specific, approximate, path)
            )
            return True
    elif isinstance(node, list):
        for index, item in enumerate(node):
            found |= stamp_sources(item, f"{path}[{index}]")
    return found


def check_map_sources(map_rows: list[dict]) -> dict[str, int]:
    """The three `source` counts over the map, and the refusal of an empty one.

    A bucket at zero is not reported as a count, because a split that collapsed
    into fewer buckets produces exactly what a block with nothing to distinguish
    produces and neither is readable from the number alone. The counts are
    computed rather than compared against expected ones: what is asserted is
    that the distinction still reaches a verdict, not what the verdicts were.
    """
    counts = dict.fromkeys(SOURCE_VALUES, 0)
    for row in map_rows:
        source = row.get("source")
        if source not in counts:
            sys.exit(
                f"map entry {row.get('type')} {row.get('address')} carries source={source!r}, "
                f"which is not one of {list(SOURCE_VALUES)}."
            )
        counts[source] += 1
    empty = [name for name, count in counts.items() if count == 0]
    if empty:
        sys.exit(
            f"no map entry carries source {', '.join(empty)}. The three-valued split has "
            "collapsed into fewer buckets, which reads exactly like a block whose entries all "
            "have the same provenance."
        )
    return counts


def disagreements(classes: dict) -> list[dict]:
    """The five places the measured law is taken over the printed one.

    `src/midi/synth/docs/gs.md` is the source of record for the list; this is
    what the tables carry of it, and a conformance test reads both, so the two
    cannot part company without something going red. A figure this derivation
    computes is read off the table the run built rather than restated -- the
    third-octave entry is measured here, and only the rotary switch, which
    belongs to none of the fourteen classes, is carried on the doc's word.
    """
    third = classes["freq"]["tables"]["eq"]["entries"][2]
    gain = classes["gain"]["tables"]["tone"]
    level = classes["level"]["tables"]["output"]
    balance = classes["balance"]["tables"]["effect"]
    return [
        {
            "conversion_class": "freq",
            "table": "eq",
            "what": (
                f"the third entry reads {third['hz']} Hz where the third-octave series printed "
                f"beside it says {third['series_hz']}, {third['away_octaves']} octaves away"
            ),
            "the_printed_law_says": third["series_hz"],
            "this_unit_returns": third["hz"],
        },
        {
            "conversion_class": None,
            "table": None,
            "what": ("the rotary speed switch turns over at 63/64 rather than at the printed 7F"),
            "why_it_names_no_class": (
                "A switch, not a conversion: none of the fourteen classes carries it, so nothing "
                "here computes its corner and the figure is the doc's."
            ),
        },
        {
            "conversion_class": "gain",
            "table": "tone",
            "what": (
                f"the gain window is {gain['window'][0]}-{gain['window'][1]} of the byte and "
                "narrower than the range printed against the parameter, and outside it the unit "
                f"returns {gain['outside_the_window']}"
            ),
            "the_printed_law_says": gain["printed_ends"],
            "this_unit_returns": gain["window"],
        },
        {
            "conversion_class": "level",
            "table": "output",
            "what": (
                f"the level curve is a stored table of {len(level['entries'])} numerators over "
                f"{level['denominator']} and not the straight line its printed range reads as"
            ),
            "the_printed_law_says": level["printed_ends"],
        },
        {
            "conversion_class": "balance",
            "table": "effect",
            "what": (
                "the centre of the balance stands over both of its ends, the two ramps meeting "
                "at full at the middle of the byte rather than crossing"
            ),
            "the_printed_law_says": balance["printed_ends"],
            "how_far_over": balance["why_the_middle_stands_over_the_ends"],
        },
    ]


def printed_parameters(root: Path) -> int:
    """How many (type, slot) parameters the unit prints a value for.

    Counted by `tools/gs/coverage.py`'s own enumeration rather than by a second
    one written here: it is the denominator the whole block's coverage is read
    against, and two derivations of it would be two things to keep in step.
    The module is loaded by path because `coverage` is also the name of a widely
    installed package, and an import by name would take whichever the
    interpreter's path offered first.
    """
    spec = importlib.util.spec_from_file_location(
        "gs_coverage", Path(__file__).resolve().parent / "coverage.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    _archive_keyed, canonical = module.load_printed(root)
    total = sum(len(slots) for slots in canonical.values())
    if total != module.EXPECTED_PRINTED:
        sys.exit(
            f"the printed enumeration counts {total} (type, slot) parameters where "
            f"{module.EXPECTED_PRINTED} is recorded. That number is the denominator these "
            "tables are read against; refusing to emit a header carrying a different one."
        )
    return total


def load(path: Path) -> dict:
    if not path.is_file():
        sys.exit(f"{path} is not in the archive; this derivation needs it")
    return json.loads(path.read_text())


def archive_revision(root: Path, inputs: list[Path]) -> dict:
    """What identifies the inputs these tables were derived from.

    A git revision where the archive is a working copy, and a hash over the files
    actually read where it is not. Both are recorded with which one it is, since
    a reader comparing two runs needs to know whether a difference is the archive
    moving or the archive being a different kind of thing.

    A revision only identifies the inputs if the working copy is clean over them,
    so whether it is is carried beside it rather than assumed.
    """
    try:
        rev = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        digest = hashlib.sha256()
        for path in sorted(inputs):
            digest.update(path.relative_to(root).as_posix().encode())
            digest.update(path.read_bytes())
        return {
            "archive_revision": f"sha256:{digest.hexdigest()}",
            "archive_revision_source": "a hash over the files read; the archive is not a git tree",
            "archive_inputs_dirty": False,
        }

    status = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "status",
            "--porcelain",
            "--",
            *[str(p) for p in inputs],
        ],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    return {
        "archive_revision": rev,
        "archive_revision_source": "git rev-parse HEAD",
        "archive_inputs_dirty": bool(status),
    }


def parameter_records(root: Path, unit: str) -> tuple[list[dict], list[dict], list[dict]]:
    """Every efx-params record the archive's index files, split by what it prepared.

    Returns the power-on records, the records taken under some other condition,
    and any record whose two readings of that question disagree -- the name it
    carries against the state it declares. The split is the record's, and the
    name is only checked so that a divergence between the two is a finding rather
    than a silent change of population.
    """
    unit_dir = root / "data" / "units" / unit
    index = load(unit_dir / "index.json")
    listed = [e["file"] for e in index.get("stages", {}).get(PARAMS_STAGE, [])]
    if not listed:
        sys.exit(f"{unit_dir / 'index.json'} lists no stage {PARAMS_STAGE!r}")

    power_on: list[dict] = []
    other: list[dict] = []
    disputed: list[dict] = []
    for name in listed:
        path = unit_dir / name
        record = load(path)
        record["_file"] = name
        record["_path"] = path
        prepared = [(p["address"], p["bytes"]) for p in record.get("prepared", [])]
        minimal = prepared == [
            (TYPE_ADDRESS, record["type"]),
            (PART_ROUTING_ADDRESS, "01"),
        ]
        by_name = Path(name).stem.upper() == record["type"].replace(" ", "-")
        if minimal != by_name:
            disputed.append(
                {
                    "file": name,
                    "prepared_says_power_on": minimal,
                    "the_name_says_power_on": by_name,
                    "prepared": [f"{a}={b}" for a, b in prepared],
                }
            )
        (power_on if minimal else other).append(record)
    return power_on, other, disputed


def defaults_table(power_on: list[dict], other: list[dict]) -> tuple[dict, list, int]:
    """The twenty power-on bytes of every type, and what disagrees with them.

    A record taken under another condition overlaps the power-on one at every slot
    it holds. Those slots are not a second source for a default -- they are the
    detector that the power-on records were read as what they are. A disagreement
    is fatal: the alternative is a table of defaults silently carrying one type's
    parked modulator or another's amp switched on.
    """
    held: dict[int, dict[int, int]] = {}
    for record in power_on:
        code = parsed_type(record["type"])
        slots = held.setdefault(code, {})
        for parameter in record["parameters"]:
            slots[parsed_address(parameter["address"])] = parameter["default"]

    disagreements = []
    for record in other:
        code = parsed_type(record["type"])
        slots = held.get(code)
        if slots is None:
            continue
        for parameter in record["parameters"]:
            addr = parsed_address(parameter["address"])
            if addr in slots and slots[addr] != parameter["default"]:
                disagreements.append(
                    {
                        "type": record["type"],
                        "address": parameter["address"],
                        "power_on_record_holds": slots[addr],
                        "this_record_holds": parameter["default"],
                        "record": record["_file"],
                    }
                )

    complete = sum(1 for slots in held.values() if len(slots) == PARAMETER_SLOTS)
    return held, disagreements, complete


def slot_candidates(power_on: list[dict]) -> list[dict]:
    """Every (type, slot) the power-on records carry, with what its page prints.

    The parameter's own index is kept rather than derived from its address: the
    archive fills the slots in order and says so, and a slot read off a sorted
    list of whatever records happened to be present moves every later parameter
    up by one whenever a record is short.
    """
    out = []
    for record in power_on:
        for parameter in record["parameters"]:
            out.append(
                {
                    "type": record["type"],
                    "type_code": parsed_type(record["type"]),
                    "address": parameter["address"],
                    "parameter": parameter["parameter"],
                    "printed_values": parameter.get("printed_values"),
                    "printed_settings": parameter.get("printed_settings"),
                    "default": parameter["default"],
                }
            )
    return out


def about(inferences: Path, name: str) -> dict:
    data = load(inferences / f"{name}.json")
    inference = data["inference"]
    scope = inference["about"]
    return {
        "file": f"{name}.json",
        "state": inference["state"],
        "rounds": inference.get("rounds"),
        "types": scope["types"],
        "addresses": scope.get("addresses", []),
        "data": data,
    }


def build_map(candidates: list[dict], scopes: dict) -> tuple[list[dict], list[dict]]:
    """Which (type, slot) pairs each class reaches, and which fall outside their claim.

    An address outside the inference's own list is kept and reported. Records
    legitimately sit there -- the chorus's pre-filter corner is at `40 03 04` and
    the frequency claim's address list does not name it -- so dropping them would
    lose reach, and keeping them quietly would lose the one place a reader can see
    that the claim was widened by this derivation rather than by its author.

    A class's `reach_also_by` claims are walked beside its own, and each row names
    the claim that reached it. Two claims reaching one slot is refused: which of
    them a row rests on would then be the order they were listed in.
    """
    reached: list[dict] = []
    outside: list[dict] = []
    for name in CLASS_ORDER:
        spec = CLASSES[name]
        for scope in [scopes[name], *scopes[name]["also"]]:
            for slot in candidates:
                table = spec["columns"].get(slot["printed_values"])
                if isinstance(table, dict):
                    table = table.get(slot["address"])
                if table is None or slot["type"] not in scope["types"]:
                    continue
                in_about = slot["address"] in scope["addresses"]
                if spec["scoped_by_address"] and not in_about:
                    continue
                reached.append(
                    {
                        "type": slot["type"],
                        "address": slot["address"],
                        "parameter": slot["parameter"],
                        "conversion_class": name,
                        "table": table,
                        "printed_values": slot["printed_values"],
                        "rests_on": scope["file"],
                        "inference_state": scope["state"],
                        "address_inside_the_inferences_list": in_about,
                    }
                )
                if not in_about:
                    outside.append(
                        {
                            "type": slot["type"],
                            "address": slot["address"],
                            "conversion_class": name,
                            "printed_values": slot["printed_values"],
                            "rests_on": scope["file"],
                            "the_inference_names": scope["addresses"],
                        }
                    )
    seen: set[tuple[str, str]] = set()
    for row in reached:
        key = (row["type"], row["address"])
        if key in seen:
            sys.exit(
                f"{row['type']} {row['address']} is reached twice, the second time by "
                f"{row['rests_on']} for {row['conversion_class']}. A slot follows one law."
            )
        seen.add(key)
    return reached, outside


def breakpoints(knots: list[tuple[int, float]], key: str) -> list[dict]:
    return [entry(setting=setting, **{key: value}) for setting, value in knots]


def rate_tables(scope: dict) -> dict:
    """The two rate tables, as the settings their step changes at.

    Both are whole steps of a twentieth of a hertz, so a breakpoint pair carries a
    segment exactly and the hundred and twenty-eight entries are not stored.
    """
    return {
        "read_by": "the byte, whole",
        "tables": {
            "narrow": {
                "printed_ends": ["0.05", "6.40"],
                "columns": ["*7"],
                "kind": "breakpoints",
                "interpolate": "linear in the setting",
                "breakpoints": breakpoints([(0, 0.05), (127, 6.40)], "hz"),
                "unit_specific": False,
                "approximate": False,
            },
            "wide": {
                "printed_ends": ["0.05", "10.00"],
                "columns": ["*6"],
                "kind": "breakpoints",
                "interpolate": "linear in the setting",
                "breakpoints": breakpoints(
                    [(0, 0.05), (99, 5.00), (119, 7.00), (125, 10.00), (127, 10.00)],
                    "hz",
                ),
                "why_the_last_two_settings_repeat": (
                    "The table has 126 entries over 128 settings, so 126 and 127 have none of "
                    "their own and return what 125 returns."
                ),
                "unit_specific": False,
                "approximate": False,
            },
        },
        "rests_on": scope["file"],
    }


DELAY_LADDERS = {
    "pre_delay": (
        ["*1"],
        ["0.0", "100"],
        [(0, 0.0), (50, 5.0), (60, 10.0), (100, 50.0), (125, 100.0), (127, 100.0)],
    ),
    "time1": (
        ["*2"],
        ["200", "1000"],
        [(0, 200.0), (70, 550.0), (115, 1000.0), (127, 1000.0)],
    ),
    "time2": (
        ["*3"],
        ["200", "1000"],
        [(0, 200.0), (80, 600.0), (120, 1000.0), (127, 1000.0)],
    ),
    "time3": (
        ["*4"],
        ["0.0", "500"],
        [
            (0, 0.0),
            (50, 5.0),
            (60, 10.0),
            (90, 40.0),
            (116, 300.0),
            (126, 500.0),
            (127, 500.0),
        ],
    ),
    "time4": (["*5"], ["0", "635"], [(0, 0.0), (127, 635.0)]),
}


def delay_tables(scope: dict) -> dict:
    """The five delay-time ladders, as breakpoints, with the clock they are cut to.

    Which ladder a slot is on is the conversion column its record cites and not
    the range its page prints: two slots print the same ends and cite different
    columns, and the two ladders they name are fifty milliseconds apart over a
    third of the byte.
    """
    tables = {}
    for name, (columns, ends, knots) in DELAY_LADDERS.items():
        # The cut happens once, where a value is produced, and the stored knots are
        # the uncut ladder. A knot that was already cut would be cut a second time
        # by the runtime, and a value interpolated between two cut endpoints is not
        # the cut of the value interpolated between the uncut ones. It happens that
        # every knot here is a whole sample already, so the distinction costs
        # nothing today -- which is exactly why it is asserted rather than trusted.
        uncut = [(setting, value) for setting, value in knots if cut_to_clock(value) != value]
        if uncut:
            sys.exit(
                f"The {name} ladder stores knots that are not whole samples of the clock: "
                f"{uncut}. The runtime cuts once at evaluation, so a pre-cut knot would be cut "
                "twice and an interpolation between two of them would not be the cut of the "
                "interpolation."
            )
        tables[name] = {
            "printed_ends": ends,
            "columns": columns,
            "kind": "breakpoints",
            "interpolate": "linear in the setting, then cut",
            "breakpoints": breakpoints(knots, "ms"),
            "unit_specific": False,
            "approximate": False,
        }
    return {
        "read_by": "the byte, whole",
        "tables": tables,
        "cut": {
            "clock_hz": UNIT_CLOCK_HZ,
            "mode": "floor",
            "how": "ms -> floor(ms * clock_hz / 1000) * 1000 / clock_hz",
            "where": (
                "Once, at evaluation, after interpolating between the stored knots. The knots "
                "are the uncut ladder and every one of them is checked to be a whole sample "
                "already, so nothing is cut twice."
            ),
            "how_the_reading_settles_it": (
                "Measured every run under derivation_checks.the_cut_floor_against_round, over "
                "the settings where the two predictions differ at all. Two of the five ladders "
                "separate nothing -- every entry of theirs is five milliseconds or more, which "
                "is already a whole step -- so the settings that decide it are at the short end "
                "of the other three."
            ),
        },
        "rests_on": scope["file"],
    }


def freq_tables(scope: dict, candidates: list[dict]) -> tuple[dict, list[str]]:
    """The three frequency tables, sixteen entries each, indexed by the top four bits.

    The entries are the named third octaves between the two ends the slot's own
    record prints -- derived here from the preferred-number series and checked
    against the series the claim names -- with a reading laid over every entry the
    archive published one for.
    """
    notes: list[str] = []
    data = scope["data"]

    named = data["a_printing_read_after_this_was_made"]["compared"][0]["the_series_the_claim_names"]
    derived = third_octaves(200, 6300)
    if [float(v) for v in named] != [float(v) for v in derived]:
        sys.exit(
            "The third-octave series derived here is not the one the frequency claim names: "
            f"{derived} against {named}. Refusing to build a table on a series the archive "
            "does not recognise."
        )

    ends_for: dict[str, list[str]] = {}
    for slot in candidates:
        table = CLASSES["freq"]["columns"].get(slot["printed_values"])
        if table and slot["printed_settings"]:
            ends_for.setdefault(table, sorted(slot["printed_settings"]))

    readings: dict[str, dict[int, float]] = {"eq": {}, "pre_filter": {}, "damping": {}}
    for index, hz in enumerate(data["a_readout_beside_the_claim"]["entries_hz"]):
        readings["eq"][index] = hz
    for setting, hz in data["a_third_printed_range"]["read_hz"].items():
        readings["damping"][int(setting) // 8] = hz

    spans = {
        "eq": ("200 - 6.3k", 200, 6300, False, 0.0833),
        "pre_filter": ("250 - 8k", 250, 8000, False, 0.0847),
        "damping": ("315 - 8k/Bypass", 315, 8000, True, 0.0847),
    }

    tables = {}
    for name, (printed, low, high, has_bypass, band) in spans.items():
        series = third_octaves(low, high)
        want = 15 if has_bypass else 16
        if len(series) != want:
            sys.exit(f"{printed} brackets {len(series)} third octaves where {want} fill its table")
        entries = []
        for index in range(16):
            if has_bypass and index == 15:
                entries.append(
                    entry(
                        index=index,
                        settings=[index * 8, index * 8 + 7],
                        hz=None,
                        bypass=True,
                        what_placed_it="the range's own last position, which is not a frequency",
                        approximate=False,
                    )
                )
                continue
            nominal = series[index]
            read = readings[name].get(index)
            if read is None:
                entries.append(
                    entry(
                        index=index,
                        settings=[index * 8, index * 8 + 7],
                        hz=nominal,
                        series_hz=nominal,
                        what_placed_it="the series; no reading of this entry is published",
                        approximate=True,
                    )
                )
                continue
            away = octaves(read, nominal)
            entries.append(
                entry(
                    index=index,
                    settings=[index * 8, index * 8 + 7],
                    hz=round(read, 4),
                    series_hz=nominal,
                    away_octaves=round(away, 4),
                    what_placed_it="read",
                    unit_specific=away > band,
                )
            )
        tables[name] = {
            "printed_ends": ends_for.get(name),
            "columns": [col for col, table in CLASSES["freq"]["columns"].items() if table == name],
            "printed_range_the_claim_calls_it": printed,
            "kind": "entries",
            "entries": entries,
            "band_the_reading_separates_octaves": band,
            "unit_specific": any(e["unit_specific"] for e in entries),
            "approximate": any(e["approximate"] for e in entries),
        }

    second = data["another_printed_range"]["the_second_entry_hz"]
    tables["pre_filter"]["checked_against"] = (
        "Eleven settings across the range, a median 0.046 octaves from this series, and its "
        f"second entry read {second['through_the_low_pass']} Hz through the low-pass and "
        f"{second['through_the_high_pass']} through the high-pass -- inside the band this "
        "reading separates. No per-entry readout is published, so the entries are the series."
    )
    notes.append(
        "freq: the 200 - 6.3k table's third entry is the one entry of the three tables whose "
        "reading is further from its own series than the reading separates -- "
        f"{tables['eq']['entries'][2]['hz']} Hz against {tables['eq']['entries'][2]['series_hz']}, "
        f"{tables['eq']['entries'][2]['away_octaves']} octaves. Flagged unit_specific."
    )
    return {
        "read_by": "the top four bits; eight settings share an entry",
        "tables": tables,
        "rests_on": scope["file"],
    }, notes


def gain_tables(scope: dict) -> dict:
    return {
        "read_by": "the byte, whole",
        "tables": {
            "tone": {
                "printed_ends": ["-12", "+12"],
                "columns": ["34–4C"],
                "kind": "rule",
                "rule": "clamp(setting, 52, 76) - 64 decibels",
                "window": [52, 76],
                "offset": 64,
                "db_per_step": 1.0,
                "outside_the_window": "the nearest edge value, not the byte read as decibels",
                "unit_specific": False,
                "approximate": False,
            }
        },
        "rests_on": scope["file"],
    }


def level_tables(scope: dict) -> dict:
    """The output-level multiplier: a stored numerator over 127.

    The denominator is 127 rather than 128 because the byte's range is `00-7F` and
    the layer above already divides by 127. The archive cannot separate the two --
    a constant 0.068 dB, under what its chain resolves -- and says so; this picks
    the one the rest of the code already uses.
    """
    table = scope["data"]["the_table"]
    not_read = set(table.get("entries_not_read", []))
    entries = [
        entry(
            setting=setting,
            numerator=numerator,
            approximate=setting in not_read,
            **(
                {"why": "filled from the straight line the byte gives; the take sits at the floor"}
                if setting in not_read
                else {}
            ),
        )
        for setting, numerator in enumerate(table["entries"])
    ]
    return {
        "read_by": "the byte, whole",
        "tables": {
            "output": {
                "printed_ends": ["0", "127"],
                "columns": ["00–7F"],
                "kind": "entries",
                "denominator": 127,
                "why_this_denominator": (
                    "The reading stands for 127 and 128 alike -- a constant 0.068 dB it cannot "
                    "resolve -- and 127 is the byte's own top and what the surrounding code "
                    "already divides by."
                ),
                "entries": entries,
                "unit_specific": False,
                "approximate": any(e["approximate"] for e in entries),
            }
        },
        "rests_on": scope["file"],
    }


def width_tables(scope: dict) -> dict:
    return {
        "read_by": "the byte, whole",
        "tables": {
            "section": {
                "printed_ends": ["0", "4"],
                "columns": ["00/01/02/03/04"],
                "kind": "entries",
                "entries": [
                    entry(setting=setting, octaves=value)
                    for setting, value in enumerate((0.25, 0.5, 1.0, 2.0, 4.5))
                ],
                "settings_past_the_table_return": 0,
                "what_that_cannot_be_told_from": (
                    "This slot powers up at setting 0, so a setting past the table returning "
                    "entry 0 and a setting past the table doing nothing are the same takes."
                ),
                "unit_specific": False,
                "approximate": False,
            }
        },
        "rests_on": scope["file"],
    }


def wave_tables(scope: dict) -> dict:
    """The five modulation shapes, with what separates the third from a plain sine."""
    published = scope["data"]["the_entries"]
    sine = scope["data"]["the_entry_the_page_prints_sin_against"]["what_the_unit_returns"]
    # That block carries one reading per type plus a row saying how far the two
    # readings sit apart; averaging over the block whole would pull the figure
    # towards the difference, which is not a reading of anything.
    third = [reading["h3"] for name, reading in sine.items() if name.startswith("on ")]
    rising = {}
    read_on = {}
    for index, record in published.items():
        fractions = {t: r["going_up_fraction"] for t, r in record["on_each_type"].items()}
        rising[int(index)] = sum(fractions.values()) / len(fractions)
        read_on[int(index)] = fractions
    shapes = (
        ("Tri", "triangle"),
        ("Sqr", "square"),
        ("Sin", "sine carrying a third harmonic and almost nothing above it"),
        ("Saw1", "sawtooth, rising over most of the cycle"),
        ("Saw2", "sawtooth, rising over little of the cycle"),
    )
    entries = [
        entry(
            setting=setting,
            printed=printed,
            shape=shape,
            rising_fraction=round(rising[setting], 4),
            third_harmonic=round(sum(third) / len(third), 4) if setting == 2 else 0.0,
            rising_fraction_read_on=read_on[setting],
            **({"third_harmonic_read_on": third} if setting == 2 else {}),
        )
        for setting, (printed, shape) in enumerate(shapes)
    ]
    return {
        "read_by": "the byte, whole",
        "tables": {
            "modulator": {
                "printed_ends": ["0", "4"],
                "columns": ["00/01/02/03/04"],
                "kind": "entries",
                "entries": entries,
                "what_the_reading_cannot_separate": (
                    "Which direction Saw1 and Saw2 run. A magnitude-only reading places the "
                    "fraction of the cycle that climbs and not the sign."
                ),
                "unit_specific": False,
                "approximate": False,
            }
        },
        "rests_on": scope["file"],
    }


def pan_tables(scope: dict) -> dict:
    """One multiplier a side, recovered at eighteen settings with the added term removed."""
    sides = load(
        scope["path"].parent.parent / "models" / "pan-a-table-of-one-multiplier-per-side.json"
    )["sides"]
    return {
        "read_by": "the byte, whole",
        "what_it_does_not_carry": (
            "The term added after the pan, which is what stops the far channel short of silence "
            "and is frequency-dependent. It is characterised by its net effect and not decomposed."
        ),
        "tables": {
            "output": {
                "printed_ends": ["L63", "R63"],
                "columns": ["00–7F"],
                "kind": "breakpoints",
                "interpolate": "linear in the setting",
                "sides": {
                    side: [
                        entry(setting=setting, multiplier=round(value, 6))
                        for setting, value in sides[side]
                    ]
                    for side in ("left", "right")
                },
                "recovered_up_to": (
                    "A constant. The table is pinned by putting its quietest entry at silence, "
                    "which bounds the last entry's depth from one side only."
                ),
                "unit_specific": False,
                "approximate": False,
            }
        },
        "rests_on": scope["file"],
    }


def balance_tables(scope: dict) -> dict:
    return {
        "read_by": "the byte, whole",
        "tables": {
            "effect": {
                "printed_ends": ["D>0E", "D0<E"],
                "columns": ["00–7F"],
                "kind": "rule",
                "rule": "min(1, floor((114 - n) * 128 / 50) / 128) past the effect, "
                "min(1, floor((n - 14) * 128 / 50) / 128) through it",
                "corner_past_the_effect": 114,
                "corner_through_the_effect": 14,
                "width": 50,
                "grid": 128,
                "truncated": True,
                "why_the_middle_stands_over_the_ends": (
                    "The two corners sum to 128 and the halves meet at full at setting 64, so the "
                    "middle of the byte carries both at once and stands 3 dB over either end -- "
                    "the opposite sign to a crossfade."
                ),
                "unit_specific": False,
                "approximate": False,
            }
        },
        "rests_on": scope["file"],
    }


def azimuth_tables(scope: dict) -> dict:
    """Thirty-one placements from a byte, by rounding to a quarter and clamping.

    The rule is evaluated here and checked against the column the claim publishes,
    setting span by setting span: a rule that agreed with the archive at the ends
    and not in the middle would otherwise ship.
    """
    positions = {}
    for byte in range(128):
        position = max(-15, min(15, ((byte + 2) >> 2) - 16))
        positions.setdefault(position, []).append(byte)

    published = scope["data"]["the_column_in_full"]
    for row in published:
        settings = positions.get(row["position"])
        if settings is None or [settings[0], settings[-1]] != row["settings"]:
            sys.exit(
                f"The azimuth rule puts position {row['position']} at "
                f"{settings and [settings[0], settings[-1]]} where the archive's column puts it "
                f"at {row['settings']}. Refusing to ship a rule the archive contradicts."
            )

    entries = [
        entry(
            position=row["position"],
            settings=row["settings"],
            degrees=row["degrees"],
        )
        for row in published
    ]
    return {
        "read_by": "two added, shifted right two, recentred, clamped",
        "tables": {
            "placement": {
                "printed_ends": ["L180(=R180)", "R180(=L180)"],
                "columns": ["*13"],
                "kind": "rule",
                "rule": "clamp(((byte + 2) >> 2) - 16, -15, 15), 12 degrees a position",
                "degrees_per_position": 12,
                "clamp": 15,
                "entries": entries,
                "the_two_poles_are_one_place": True,
                "the_table_is_a_mirror": (
                    "A position and its opposite put the same separation with the sign reversed, "
                    "so a renderer needs sixteen magnitudes and a sign."
                ),
                "unit_specific": False,
                "approximate": False,
            }
        },
        "rests_on": scope["file"],
    }


def accel_tables(scope: dict) -> dict:
    """Sixteen divisors from the top four bits, and the one entry that leaves the law.

    Twelve of the fourteen entries a reading placed land on the doubling sequence
    inside their own take's resolution, so the sequence is what is stored. The
    last entry is five times its take's resolution away from the sequence and the
    reading is stored there instead -- flagged, because that is a number this
    individual unit returned and not a law.
    """
    data = scope["data"]["what_the_table_holds"]
    sequence = {int(k): v for k, v in data["the_sequence"].items()}
    measured = {row["entry"]: row for row in data["at_each_entry"]}

    entries = []
    for index in range(16):
        row = measured.get(index)
        if row is None:
            entries.append(
                entry(
                    index=index,
                    settings=[index * 8, index * 8 + 7],
                    divisor=float(sequence[index]),
                    what_placed_it="the sequence; no multiplier is published for this entry",
                    approximate=True,
                )
            )
            continue
        apart = row["times_what_the_take_told_apart"]
        leaves_the_law = apart >= 2.0
        entries.append(
            entry(
                index=index,
                settings=[index * 8, index * 8 + 7],
                divisor=float(row["multiplier"] if leaves_the_law else sequence[index]),
                sequence_says=sequence[index],
                measured_multiplier=row["multiplier"],
                times_the_take_told_apart=apart,
                what_placed_it="this unit's reading" if leaves_the_law else "the sequence",
                unit_specific=leaves_the_law,
            )
        )
    return {
        "read_by": "the top four bits; eight settings share an entry",
        "tables": {
            "rotor": {
                "printed_ends": ["0", "15"],
                "columns": ["*14"],
                "kind": "entries",
                "entries": entries,
                "what_the_divisor_is": (
                    "What the rotor's control loop divides the distance to its target rate by "
                    "before shifting toward it."
                ),
                "to_seconds": {
                    "rule": "divisor * 2^15 / clock_hz",
                    "shift": 32768,
                    "clock_hz": UNIT_CLOCK_HZ,
                    "why_not_the_divisor_itself": (
                        "The divisor is a per-step coefficient, so held as it stands its time "
                        "constant moves with the sample rate. The constant behind it is "
                        "0.9768 Hz, which is the clock over two to the fifteenth."
                    ),
                },
                "unit_specific": any(e["unit_specific"] for e in entries),
                "approximate": any(e["approximate"] for e in entries),
            }
        },
        "rests_on": scope["file"],
    }


# The four settings a post gain prints, and the step between them. The claim
# reads nine steps averaging 5.999 dB over a spread of 0.09 as one law.
POST_GAIN_SETTINGS = 4
POST_GAIN_DB_PER_STEP = 6.0


def post_gain_tables(root: Path, scope: dict) -> dict:
    """Four fixed steps of six decibels, and every step the claim's records read.

    The steps are reported rather than gated, as the rate tables are: the claim
    already says the third step runs a few hundredths short on every set, and a
    gate tight enough to see that would refuse the law its own author stands by.
    What is refused is a run that read no step at all.
    """
    steps = []
    for measurement in scope["data"]["rests_on"]["measurements"]:
        name = measurement["file"]
        if "/efx-orders/" not in name:
            continue
        record = load(root / name)
        heard = {r["value"]: r["heard_db"] for r in record["readings"] if "heard_db" in r}
        for setting in range(1, POST_GAIN_SETTINGS):
            if setting in heard and setting - 1 in heard:
                step = heard[setting] - heard[setting - 1]
                steps.append(
                    {
                        "record": Path(name).name,
                        "type": record["type"],
                        "address": record["address"],
                        "from_setting": setting - 1,
                        "step_db": round(step, 4),
                        "off_the_law_db": round(step - POST_GAIN_DB_PER_STEP, 4),
                    }
                )
    if not steps:
        sys.exit(
            "No efx-orders record of a post gain was read, so its law was checked against nothing."
        )
    worst = max(abs(s["off_the_law_db"]) for s in steps)
    return {
        "read_by": "the byte, whole",
        "tables": {
            "makeup": {
                "printed_ends": ["0", "+18"],
                "columns": ["00/01/02/03"],
                "kind": "rule",
                "rule": "setting * 6 decibels, after the stage the slot is printed beside",
                "settings": POST_GAIN_SETTINGS,
                "db_per_step": POST_GAIN_DB_PER_STEP,
                "settings_past_the_table_return": 0,
                "what_that_rests_on": (
                    "Nothing read past the fourth setting. Entry 0 is what the measured small "
                    "tables return there, and the reader keeps that one convention."
                ),
                "steps_read": steps,
                "worst_step_off_the_law_db": round(worst, 4),
                "unit_specific": False,
                "approximate": False,
            }
        },
        "rests_on": scope["file"],
    }


# The five windows stand as 3 : 4 : 6 : 8 : 12, and the shortest is 1024 steps
# of the unit clock -- a count the claim reads off the stored distance itself.
WINDOW_RATIO = (3, 4, 6, 8, 12)
WINDOW_SHORTEST_STEPS = 1024


def window_tables(scope: dict) -> dict:
    """Five splice windows, the law checked against both claims that read it.

    The claim the law rests on publishes every window in milliseconds and the
    three whole-step counts it could read; a count disagreeing with the law is
    fatal. The claim widening reach to a second type reads the same ratio at a
    constant multiple, and a state departing from it by more than that claim's own
    repeats is fatal too -- that is the refutation it wrote for itself.
    """
    data = scope["data"]
    law_steps = [WINDOW_SHORTEST_STEPS * part / WINDOW_RATIO[0] for part in WINDOW_RATIO]
    read_ms = data["the_round_that_read_the_output_against_itself"]["what_the_reading_returned"][
        "window_ms"
    ]
    whole = data["the_round_that_asked_at_two_more_windows"]["the_figures"][
        "the_window_at_each_state_in_steps_of_32000_hz"
    ]
    for state, count in whole.items():
        if count != law_steps[int(state)]:
            sys.exit(
                f"window: state {state} is {count} steps of the clock in {scope['file']} where the "
                f"law says {law_steps[int(state)]}. Refusing to ship a law the archive contradicts."
            )

    corroborated = []
    for other in scope["also"]:
        figures = other["data"]["the_round_that_took_each_state_four_times"]["the_figures"]
        shifted = figures["at_two_semitones_up"]
        bound = shifted["how_far_apart_four_takes_of_one_state_land_ms"]
        means = shifted["means_ms"]
        for state, mean in enumerate(means):
            predicted = means[0] * WINDOW_RATIO[state] / WINDOW_RATIO[0]
            if abs(mean - predicted) > bound:
                sys.exit(
                    f"window: state {state} of {other['file']} reads {mean} ms where the ratio "
                    f"puts it at {round(predicted, 4)}, further than its own repeats of {bound} ms."
                )
        corroborated.append(
            {
                "file": other["file"],
                "types": other["types"],
                "means_ms": means,
                "repeats_bound_ms": bound,
            }
        )

    entries = []
    for state, steps in enumerate(law_steps):
        ms = steps * 1000.0 / UNIT_CLOCK_HZ
        read = read_ms[str(state)]
        entries.append(
            entry(
                setting=state,
                ms=round(ms, 6),
                steps_of_the_clock=round(steps, 4),
                read_ms=read,
                read_off_the_law_ms=round(read - ms, 4),
                what_placed_it=(
                    "the law, and a whole-step count the claim read"
                    if str(state) in whole
                    else "the law; the reading cannot tell it from the nearest whole count"
                ),
            )
        )
    return {
        "read_by": "the byte, whole",
        "tables": {
            "splice": {
                "printed_ends": ["1", "5"],
                "columns": ["00–04"],
                "kind": "entries",
                "entries": entries,
                "settings_past_the_table_return": 0,
                "what_that_rests_on": (
                    "Nothing read past the fifth setting. Entry 0 is what the measured small "
                    "tables return there, and the reader keeps that one convention."
                ),
                "what_the_window_is": (
                    "The distance the read-out drifts between splices, which is also how far "
                    "apart the two read-outs are stored."
                ),
                "corroborated_by": corroborated,
                "unit_specific": False,
                "approximate": False,
            }
        },
        "rests_on": scope["file"],
    }


def model_of(root: Path, scope: dict) -> tuple[dict, str]:
    """The model file a claim names as the one that reproduces it, and its path."""
    name = scope["data"].get("reproduces", {}).get("model")
    if not name:
        sys.exit(f"{scope['file']} names no model that reproduces it, so it carries no figures")
    return load(root / name), name


def shelves_of(model: dict, where: str) -> dict[str, dict]:
    """The low and the high shelf of a model's chain, refused unless there is one of each."""
    shelves = [section for section in model["chain"] if section.get("kind") == "shelf"]
    by_side = {section["side"]: section for section in shelves}
    if len(shelves) != 2 or set(by_side) != {"low", "high"}:
        sys.exit(f"{where} does not carry exactly one low shelf and one high shelf")
    return by_side


def corner_tables(root: Path, scope: dict) -> dict:
    """The equaliser's two corner bytes, two states each, off the model its claim names.

    The page prints two figures per byte and the claim says in as many words that
    which label goes with which state is not measurable, so what is carried is the
    byte's two states and not the page's two figures. A model whose corner is not
    split between byte 0 and every other byte, or whose byte is not the address the
    class selects on, is refused rather than read.

    The model's `*` is what the byte returns from a slot standing in its second
    state; a byte past `01` is not taken at all (STATE_LIST_INFERENCE), so the
    second state is carried at `01` and nowhere above it.
    """
    model, name = model_of(root, scope)
    shelves = shelves_of(model, name)
    tables = {}
    for address, side in CLASSES["corner"]["columns"]["00/01"].items():
        corner = shelves[side]["corner_hz"]
        if corner.get("byte") != address:
            sys.exit(
                f"{name}: the {side} shelf's corner is on {corner.get('byte')!r}, not the "
                f"{address} the corner class selects on."
            )
        states = corner.get("map", {})
        if states.get("kind") != "states" or set(states.get("values", {})) != {"0", "*"}:
            sys.exit(
                f"{name}: the {side} corner is not two states split between byte 0 and the rest."
            )
        tables[side] = {
            "columns": ["00/01"],
            "address": address,
            "kind": "states",
            "order": shelves[side]["order"],
            "entries": [
                entry(settings="00", hz=states["values"]["0"]),
                entry(settings="01", hz=states["values"]["*"]),
            ],
            "what_the_hz_is": (
                "The half-gain point of the shelf's deviation, which is where a first-order "
                "shelf's corner is defined."
            ),
            "what_the_labels_are_not": (
                "Which of the page's two figures names which state is not measurable, so the "
                "byte's states are carried and the figures are not."
            ),
            "what_the_model_says_of_it": states.get("why"),
            "from": states.get("from"),
            "unit_specific": False,
            "approximate": False,
        }
    return {
        "read_by": "byte 0 against byte 1; a byte past 1 is not taken",
        "tables": tables,
        "rests_on": scope["file"],
        "model": name,
    }


# Two shelf pairs no byte selects a corner for, each read off the claim that
# measured it. Which types a pair is put on is the skeleton's, not this file's.
OUTPUT_TONE_INFERENCE = "tone-a-pair-of-fixed-first-order-shelves"
COMBINATION_EQ_INFERENCE = "section-gain-a-shelf-stores-the-cut"

# A combination type's low shelf is read as the standalone equaliser's at its
# first state. The claim puts the two under one band of the twelfth-octave set.
COMBINATION_LOW_AGAINST_THE_FIRST_STATE_OCTAVES = 1.0 / 12.0


def fixed_corner_pairs(root: Path, inferences: Path, corner: dict) -> tuple[dict, list[str]]:
    """The output tone pair and the combination types' pair, and the models read.

    The output pair's corners are the points its model carries, each checked to
    sit inside the band the claim publishes as every corner that fits. The
    combination pair is the claim's own scan; its low corner is checked against
    the standalone equaliser's first state, which is what the claim reads it as.
    """
    tone = about(inferences, OUTPUT_TONE_INFERENCE)
    tone_model, tone_name = model_of(root, tone)
    shelves = shelves_of(tone_model, tone_name)
    fits = tone["data"]["how_well_the_corner_is_determined"]["shared_by_every_record_hz"]
    output = {}
    for side in ("low", "high"):
        hz = shelves[side]["corner_hz"].get("fixed")
        if hz is None:
            sys.exit(f"{tone_name}: the {side} shelf carries no fixed corner")
        low, high = fits[side]
        # The band is published to a tenth of a hertz and the point to full precision.
        if not low - 0.05 <= hz <= high + 0.05:
            sys.exit(
                f"{tone_name}: the {side} corner {hz} Hz is outside the {low}-{high} Hz its "
                f"claim publishes as every corner that fits."
            )
        output[side] = entry(
            hz=round(hz, 4), order=shelves[side]["order"], every_corner_that_fits_hz=[low, high]
        )

    combination = about(inferences, COMBINATION_EQ_INFERENCE)
    read = combination["data"]["read_on_another_types_pair_of_shelves"]
    hinge = read["where_the_shelves_hinge"]
    first_state = corner["tables"]["low"]["entries"][0]["hz"]
    apart = octaves(hinge["the_low_shelf_hz"], first_state)
    if apart > COMBINATION_LOW_AGAINST_THE_FIRST_STATE_OCTAVES:
        sys.exit(
            f"{combination['file']}: the combination low shelf at {hinge['the_low_shelf_hz']} Hz "
            f"is {apart:.3f} octaves from the equaliser's first state, which the claim reads it as."
        )
    pair = {
        "output_tone": {
            "rests_on": tone["file"],
            "state": tone["state"],
            "model": tone_name,
            "measured_on_types": tone["types"],
            "addresses": tone["addresses"],
            "low": output["low"],
            "high": output["high"],
            "what_it_is": (
                "The tone pair the module applies after the effect, whatever the effect is. "
                "Each corner is a point inside a band that fits every record, and the model's."
            ),
            "unit_specific": False,
            "approximate": False,
        },
        "combination_eq": {
            "rests_on": combination["file"],
            "state": combination["state"],
            "measured_on_types": combination["data"]["inference"]["about"]["checked_on_types"],
            "low": entry(
                hz=hinge["the_low_shelf_hz"],
                order=corner["tables"]["low"]["order"],
                octaves_from_the_equalisers_first_state=round(apart, 4),
            ),
            "high": entry(hz=hinge["the_high_shelf_hz"], order=read["the_high_shelf"]["order"]),
            "what_it_is": (
                "The shelves of a combination type's equaliser, which prints two gains and no "
                "corner. The low one is the standalone equaliser's at its first state."
            ),
            "unit_specific": False,
            "approximate": False,
        },
    }
    return pair, [tone_name]


# A parameter whose page prints a list of states takes no byte outside it and
# keeps the state it was in. The rule is the renderer's to apply at the write,
# so what is carried is which slots print such a list and how long it is.
STATE_LIST_INFERENCE = "states-a-value-the-page-prints-no-state-for-is-not-taken"

# A list of states is spelled 00/01/..., one entry per state from nought. The
# `00/7F` spelling uses the same slash and is not one: the claim reads it as a
# function of the byte, every value taken.
STATE_LIST_RE = re.compile(r"^00(?:/[0-9A-F]{2})+$")


def printed_states(spelling: str | None) -> int | None:
    """How many states a printed spelling lists, or None where it lists none."""
    if spelling is None or not STATE_LIST_RE.match(spelling):
        return None
    entries = [int(entry, 16) for entry in spelling.split("/")]
    return len(entries) if entries == list(range(len(entries))) else None


def state_lists(inferences: Path, candidates: list[dict]) -> dict:
    """Every (type, slot) that prints a list of states, and the claim that reads them.

    Every slot the claim asked is checked to be here with the length it asked it
    at, so a spelling this reads differently from the claim is refused rather
    than carried as a list the claim never measured.
    """
    scope = about(inferences, STATE_LIST_INFERENCE)
    if scope["state"] != "standing":
        sys.exit(f"{scope['file']} is {scope['state']}; no write can be refused on it")
    data = scope["data"]
    # The first round's five slots are all two-state switches, by the claim's own
    # account of them.
    asked = [(s["type"], s["address"], 2) for s in data["the_slots_asked_and_where_each_powers_up"]]
    asked += [
        (s["type"], s["address"], s["states_printed"])
        for s in data["the_round_that_asked_a_longer_list"]["slots"]
    ]
    asked += [
        (s["type"], s["address"], s["states_printed"])
        for s in data["the_round_that_closed_the_printed_list_lengths"][
            "every_length_the_page_prints"
        ]
    ]
    read = {(type_, address) for type_, address, _ in asked}
    slots = {}
    for slot in candidates:
        states = printed_states(slot["printed_values"])
        if states is not None:
            slots[(slot["type"], slot["address"])] = {
                "type": slot["type"],
                "address": slot["address"],
                "parameter": slot["parameter"],
                "printed_values": slot["printed_values"],
                "states": states,
                "unit_specific": False,
                # A slot the claim never asked is carried on its rule alone.
                "approximate": (slot["type"], slot["address"]) not in read,
            }
    for type_, address, states in asked:
        row = slots.get((type_, address))
        if row is None or row["states"] != states:
            sys.exit(
                f"{scope['file']} asked {type_} {address} as a list of {states} states and "
                f"the records here read it as {row['printed_values'] if row else 'no list'}."
            )
    return {
        "rests_on": scope["file"],
        "state": scope["state"],
        "what_it_is": (
            "Every slot whose page prints a list of states, with the list's length. A byte "
            "past the list is not taken and the slot keeps the state it was in."
        ),
        "spelling_read": "00/01/..., consecutive from nought",
        "spelling_not_read": {
            "00/7F": "A function of the byte, every value taken; the claim refutes one map for both."
        },
        "checked_against_the_claim": len(read),
        "slots": sorted(slots.values(), key=lambda r: (r["type"], r["address"])),
        "unit_specific": False,
        "approximate": any(row["approximate"] for row in slots.values()),
    }


def check_delay_ladders(
    root: Path, scope: dict, reached: list[dict], candidates: list[dict]
) -> tuple[list[dict], list[dict]]:
    """The ladders against every efx-time record the claim rests on.

    A setting whose residual runs to hundreds of milliseconds is carried out in
    full, with the alternative reading the record itself published beside it. The
    stimulus is a looping sample and puts a copy of itself in the output at its
    loop length, which a cepstrum cannot tell from a delay; the archive says so
    about this very run and names the three settings. Listing them is not
    excusing them -- a reader can see which settings they are and what the record
    holds there, which a median cannot say and a worst says wrongly.

    A record of a slot this derivation does **not** reach is read too and returned
    apart. That is what the type intersection costs, measured rather than argued:
    the one slot it removes from this class is the slot whose own reading departs
    from the column its page cites.
    """
    assigned = {(r["type"], r["address"]): r["table"] for r in reached}
    columns = CLASSES["delay_time"]["columns"]
    printed = {(c["type"], c["address"]): c["printed_values"] for c in candidates}
    defaults = {(c["type"], c["address"]): c["default"] for c in candidates}
    checks: list[dict] = []
    not_reached: list[dict] = []
    for measurement in scope["data"]["rests_on"]["measurements"]:
        name = measurement["file"] if isinstance(measurement, dict) else measurement
        if "/efx-time/" not in name:
            continue
        record = load(root / name)
        key = (record["type"], record["address"])
        table = assigned.get(key) or columns.get(printed.get(key))
        if table is None:
            continue
        ladder = evaluate_ladder(table)
        rows = [r for r in record["readings"] if r.get("admitted")]
        if not rows:
            continue
        errors = sorted(abs(r["ms"] - ladder[r["value"]]) for r in rows)
        row = {
            "record": Path(name).name,
            "type": record["type"],
            "address": record["address"],
            "ladder": table,
            "settings": len(errors),
            "median_ms": round(errors[len(errors) // 2], 4),
            "worst_ms": round(errors[-1], 4),
            "settings_over_1ms": sum(1 for e in errors if e > 1.0),
            "the_settings_over_1ms": [
                {
                    "setting": r["value"],
                    "read_ms": r["ms"],
                    "the_ladder_says_ms": round(ladder[r["value"]], 4),
                    "the_record_also_read_ms": r.get("also_ms"),
                }
                for r in rows
                if abs(r["ms"] - ladder[r["value"]]) > 1.0
            ],
            "the_reading_is_quantised_to_ms": record.get("quefrency_step_ms"),
            "the_run_measured_its_own_floor_ms": record.get("floor_ms"),
        }
        if key not in assigned:
            # What the slot returns *as a shape*, not just how far off it is. A
            # constant fraction of the column and a constant number of
            # milliseconds short are different mechanisms, and a residual in
            # milliseconds alone cannot tell them apart. Reported because that is
            # a question the archive can put to the unit and this cannot.
            #
            # Split at the byte the slot powers up holding, which is where the
            # archive says a slot of this kind changes regime and is the one
            # threshold available without choosing one. Taken from the parameter
            # record rather than from the shape of the readings: a split chosen by
            # looking at the numbers would find a break in anything.
            power_on = defaults.get(key)
            row["its_power_on_byte"] = power_on
            row["as_a_fraction_of_the_ladder"] = {
                "over_every_setting": spread(
                    sorted(r["ms"] / ladder[r["value"]] for r in rows if ladder[r["value"]] > 0),
                    6,
                ),
                "at_or_below_its_power_on_byte": spread(
                    sorted(
                        r["ms"] / ladder[r["value"]]
                        for r in rows
                        if ladder[r["value"]] > 0 and r["value"] <= power_on
                    ),
                    6,
                ),
                "above_it": spread(
                    sorted(
                        r["ms"] / ladder[r["value"]]
                        for r in rows
                        if ladder[r["value"]] > 0 and r["value"] > power_on
                    ),
                    6,
                ),
            }
            row["ms_short_of_the_ladder"] = {
                "over_every_setting": spread(sorted(ladder[r["value"]] - r["ms"] for r in rows), 4),
                "at_or_below_its_power_on_byte": spread(
                    sorted(ladder[r["value"]] - r["ms"] for r in rows if r["value"] <= power_on),
                    4,
                ),
                "above_it": spread(
                    sorted(ladder[r["value"]] - r["ms"] for r in rows if r["value"] > power_on),
                    4,
                ),
            }
        (checks if key in assigned else not_reached).append(row)
    if not checks:
        sys.exit(
            "No efx-time record of a reached delay slot was read, so the ladders were checked "
            "against nothing. A derivation that checks nothing reports exactly what a clean one "
            "does."
        )
    return checks, not_reached


def spread(values: list[float], places: int) -> dict:
    return {
        "n": len(values),
        "median": round(values[len(values) // 2], places),
        "min": round(values[0], places),
        "max": round(values[-1], places),
    }


def check_the_cut(root: Path, scope: dict, reached: list[dict]) -> dict:
    """Floor against round, over the settings where the two differ at all.

    The claim's word is "cut back" and the transcription of it says `round`, so
    which of the two the part does is read off the records rather than taken from
    either. Only the settings where the two predictions differ carry any
    information: on an entry of five milliseconds or more they agree exactly, and
    counting those would drown the answer in agreement neither reading earned.
    """
    assigned = {(r["type"], r["address"]): r["table"] for r in reached}
    floor_closer = round_closer = separating = 0
    per_record = []
    for measurement in scope["data"]["rests_on"]["measurements"]:
        name = measurement["file"] if isinstance(measurement, dict) else measurement
        if "/efx-time/" not in name:
            continue
        record = load(root / name)
        table = assigned.get((record["type"], record["address"]))
        if table is None:
            continue
        _, _, knots = DELAY_LADDERS[table]
        rows = []
        for r in record["readings"]:
            if not r.get("admitted"):
                continue
            raw = interpolate(knots, r["value"])
            low = math.floor(raw * UNIT_CLOCK_HZ / 1000.0) * 1000.0 / UNIT_CLOCK_HZ
            near = round(raw * UNIT_CLOCK_HZ / 1000.0) * 1000.0 / UNIT_CLOCK_HZ
            if low != near:
                rows.append((abs(r["ms"] - low), abs(r["ms"] - near)))
        if not rows:
            continue
        separating += len(rows)
        floor_closer += sum(1 for f, n in rows if f < n)
        round_closer += sum(1 for f, n in rows if n < f)
        per_record.append(
            {
                "record": Path(name).name,
                "ladder": table,
                "settings_where_the_two_differ": len(rows),
                "floor_closer": sum(1 for f, n in rows if f < n),
                "round_closer": sum(1 for f, n in rows if n < f),
                "median_ms_floor": round(sorted(f for f, _ in rows)[len(rows) // 2], 4),
                "median_ms_round": round(sorted(n for _, n in rows)[len(rows) // 2], 4),
            }
        )
    if separating == 0:
        sys.exit(
            "No admitted setting of any reached delay slot separates floor from round, so the "
            "cut this file ships is undetermined by the records. A check that reaches no verdict "
            "reports exactly what a check that agrees reports."
        )
    if round_closer >= floor_closer:
        sys.exit(
            f"The records put round closer than floor at {round_closer} of {separating} "
            f"separating settings against floor's {floor_closer}. These tables ship floor and "
            "the conversion layer reads them as floor; one of the three is wrong."
        )
    return {
        "settings_where_the_two_differ": separating,
        "floor_closer": floor_closer,
        "round_closer": round_closer,
        "per_record": per_record,
    }


def evaluate_ladder(name: str) -> list[float]:
    _, _, knots = DELAY_LADDERS[name]
    return [cut_to_clock(interpolate(knots, setting)) for setting in range(128)]


def interpolate(knots: list[tuple[int, float]], setting: int) -> float:
    for (lo, low), (hi, high) in itertools.pairwise(knots):
        if lo <= setting <= hi:
            if hi == lo:
                return low
            return low + (high - low) * (setting - lo) / (hi - lo)
    return knots[-1][1]


def cut_to_clock(ms: float) -> float:
    return math.floor(ms * UNIT_CLOCK_HZ / 1000.0) * 1000.0 / UNIT_CLOCK_HZ


def idle_tables(classes: dict, reach_by_table: dict, candidates: list[dict], scopes: dict) -> list:
    """Every table no reached slot uses, with why each candidate for it was let go.

    A table is kept when it loses its last user -- it is measured, and it is part
    of its class -- but a table sitting there with nothing pointing at it and no
    account of why is indistinguishable from one the derivation stopped filling.
    So each one carries the slots whose own printed spelling names its column and
    the filter that removed each: a table left idle by the type intersection and a
    table the unit simply has no slot for are different findings.
    """
    out = []
    for key, using in reach_by_table.items():
        if using:
            continue
        name, table_name = key.split(".", 1)
        scope = scopes[name]
        wanted = {
            column
            for column, table in CLASSES[name]["columns"].items()
            if table == table_name or (isinstance(table, dict) and table_name in table.values())
        }
        let_go = [
            {
                "type": slot["type"],
                "address": slot["address"],
                "printed_values": slot["printed_values"],
                "the_type_is_in_the_inferences_list": slot["type"] in scope["types"],
                "the_address_is_in_the_inferences_list": slot["address"] in scope["addresses"],
            }
            for slot in candidates
            if slot["printed_values"] in wanted
        ]
        out.append(
            {
                "table": key,
                "columns": sorted(wanted),
                "slots_whose_printed_spelling_names_it": let_go,
                "kind": (
                    "no slot on this unit carries the column"
                    if not let_go
                    else "every slot carrying the column was let go by the intersection"
                ),
            }
        )
    return out


def check_rate_tables(root: Path, scope: dict, reached: list[dict], tables: dict) -> list[dict]:
    """The two rate tables against every efx-rate record the claim rests on."""
    assigned = {(r["type"], r["address"]): r["table"] for r in reached}
    checks = []
    for measurement in scope["data"]["rests_on"]["measurements"]:
        name = measurement["file"] if isinstance(measurement, dict) else measurement
        if "/efx-rate/" not in name:
            continue
        record = load(root / name)
        table = assigned.get((record["type"], record["address"]))
        if table is None:
            continue
        knots = [(e["setting"], e["hz"]) for e in tables[table]["breakpoints"]]
        errors = sorted(
            abs(r["rate_hz"] - interpolate(knots, r["value"]))
            for r in record["readings"]
            if r.get("rate_hz") is not None
        )
        if not errors:
            continue
        checks.append(
            {
                "record": Path(name).name,
                "type": record["type"],
                "address": record["address"],
                "table": table,
                "settings": len(errors),
                "median_hz": round(errors[len(errors) // 2], 4),
                "worst_hz": round(errors[-1], 4),
            }
        )
    if not checks:
        sys.exit(
            "No efx-rate record of a reached rate slot was read, so the rate tables were checked "
            "against nothing."
        )
    return checks


def derive(root: Path, unit: str) -> dict:
    inferences = root / "inferences" / unit
    if not inferences.is_dir():
        sys.exit(f"{inferences} is not in the archive; this derivation needs the claim files")

    power_on, other, name_disputes = parameter_records(root, unit)
    defaults, default_disagreements, complete = defaults_table(power_on, other)
    if default_disagreements:
        for row in default_disagreements:
            print(
                f"default disagreement: type {row['type']} {row['address']} powers up at "
                f"{row['power_on_record_holds']} and {row['record']} holds "
                f"{row['this_record_holds']}",
                file=sys.stderr,
            )
        count = len(default_disagreements)
        sys.exit(
            f"{count} slot{'' if count == 1 else 's'} where a record taken under another "
            "condition disagrees with the power-on record about the default. One of the two was "
            "read as something it is not; refusing to write a defaults table over it."
        )

    scopes = {}
    for name in CLASS_ORDER:
        scope = about(inferences, CLASSES[name]["inference"])
        scope["path"] = inferences / scope["file"]
        scope["also"] = [
            about(inferences, other) for other in CLASSES[name].get("reach_also_by", ())
        ]
        scopes[name] = scope

    candidates = slot_candidates(power_on)
    reached, outside = build_map(candidates, scopes)

    notes: list[str] = []
    classes = {
        "rate": rate_tables(scopes["rate"]),
        "delay_time": delay_tables(scopes["delay_time"]),
        "gain": gain_tables(scopes["gain"]),
        "level": level_tables(scopes["level"]),
        "width": width_tables(scopes["width"]),
        "wave": wave_tables(scopes["wave"]),
        "pan": pan_tables(scopes["pan"]),
        "balance": balance_tables(scopes["balance"]),
        "azimuth": azimuth_tables(scopes["azimuth"]),
        "accel": accel_tables(scopes["accel"]),
        "post_gain": post_gain_tables(root, scopes["post_gain"]),
        "window": window_tables(scopes["window"]),
        "corner": corner_tables(root, scopes["corner"]),
    }
    fixed_corners, fixed_models = fixed_corner_pairs(root, inferences, classes["corner"])
    lists = state_lists(inferences, candidates)
    classes["freq"], freq_notes = freq_tables(scopes["freq"], candidates)
    notes.extend(freq_notes)

    reach = {name: 0 for name in CLASS_ORDER}
    for row in reached:
        reach[row["conversion_class"]] += 1

    # Per table as well as per class. A class holds more than one table -- two
    # rate ranges, five delay ladders, three frequency columns -- so a table that
    # stops being fed leaves the class's own count untouched, which is exactly the
    # shape the class guard was written against. Carried into the file and into
    # the header rather than printed, because a fact that only reaches stdout is
    # gone the moment nobody is reading the run.
    reach_by_table = {}
    for name in CLASS_ORDER:
        for table_name, table_body in classes[name]["tables"].items():
            using = sum(
                1
                for row in reached
                if row["conversion_class"] == name and row["table"] == table_name
            )
            table_body["entries_using_it"] = using
            reach_by_table[f"{name}.{table_name}"] = using

    empty = [name for name in CLASS_ORDER if reach[name] == 0]
    if empty:
        sys.exit(
            f"{', '.join(empty)} reached no (type, slot) pair. A class that reaches nothing "
            "produces exactly what a class that was never wired produces, so this is a failure "
            "rather than a count."
        )

    for row in reached:
        table = classes[row["conversion_class"]]["tables"][row["table"]]
        row["unit_specific"] = bool(table["unit_specific"])
        row["approximate"] = bool(table["approximate"])

    parked = [row for row in reached if row["inference_state"] != "standing"]

    ladder_checks, ladder_not_reached = check_delay_ladders(
        root, scopes["delay_time"], reached, candidates
    )
    for row in ladder_not_reached:
        low = row["as_a_fraction_of_the_ladder"]["at_or_below_its_power_on_byte"]
        high = row["as_a_fraction_of_the_ladder"]["above_it"]
        short = row["ms_short_of_the_ladder"]["above_it"]
        notes.append(
            f"delay: {row['type']} {row['address']} cites the {row['ladder']} column and is not "
            f"reached, its type being outside the claim's list. Read against that column at "
            f"{row['settings']} settings it does not return it, and what it returns has a shape: "
            f"at or below the byte it powers up at ({row['its_power_on_byte']}) it is a median "
            f"{low['median']} of the column over {low['n']} settings, spread "
            f"{low['min']}-{low['max']}, where 127/128 is {round(127 / 128, 6)}; above that byte "
            f"it is {high['median']} over {high['n']}, a further median {short['median']} ms "
            "short. So a scaling plus a constant subtraction above a threshold, rather than the "
            "single constant a residual in milliseconds reports. **Nothing here decides whether "
            "that is a second law of this slot's or a shape a few points fall into** -- the "
            "archive's own claim reads the shortfall as an artifact of which direction the sweep "
            "approached from, and separating the two wants a reading of the unit that this "
            "cannot produce. Recorded so the observation is not lost; not acted on, and the slot "
            "stays unreached either way, since the column unscaled is wrong under both readings."
        )

    checks = {
        "delay_ladders_against_the_records": ladder_checks,
        "delay_records_of_slots_not_reached": ladder_not_reached,
        "the_cut_floor_against_round": check_the_cut(root, scopes["delay_time"], reached),
        "rate_tables_against_the_records": check_rate_tables(
            root, scopes["rate"], reached, classes["rate"]["tables"]
        ),
        "tables_no_reached_slot_uses": idle_tables(classes, reach_by_table, candidates, scopes),
        "records_whose_name_and_prepared_state_disagree": name_disputes,
        "the_inference_each_class_rests_on": [
            {
                "conversion_class": name,
                "file": scope["file"],
                "state": scope["state"],
                "rounds": scope["rounds"],
                **({"widens_reach_only": True} if scope is not scopes[name] else {}),
            }
            for name in CLASS_ORDER
            for scope in [scopes[name], *scopes[name]["also"]]
        ],
    }

    printed = printed_parameters(root)

    meta = load(root / "data" / "units" / unit / "meta.json")
    # Everything a run reads, which is what the revision has to identify. The
    # stage index is in here because it decides which records are read at all: an
    # archive that refiled the stage and nothing else would produce a different
    # table under an unchanged revision if the index were left out.
    unit_dir = root / "data" / "units" / unit
    inputs = [unit_dir / "index.json", unit_dir / "meta.json"]
    inputs += [record["_path"] for record in power_on + other]
    inputs += sorted(inferences.glob("*.json"))
    inputs += sorted((root / "inferences" / "models").glob("pan-*.json"))
    inputs += [root / classes["corner"]["model"], *(root / name for name in fixed_models)]
    inputs += [
        root / measurement["file"]
        for name in ("delay_time", "rate")
        for measurement in scopes[name]["data"]["rests_on"]["measurements"]
        if f"/efx-{'time' if name == 'delay_time' else 'rate'}/" in measurement["file"]
    ]
    inputs += [
        root / measurement["file"]
        for measurement in scopes["post_gain"]["data"]["rests_on"]["measurements"]
        if "/efx-orders/" in measurement["file"]
    ]
    # The printed enumeration reads every unit's parameter records rather than
    # this one's stage index, so whatever it adds joins the list too.
    inputs += [
        path
        for path in sorted((root / "data" / "units").glob("*/efx-params/*.json"))
        if path not in inputs
    ]

    tables = {
        "generated_by": GENERATED_BY,
        "unit_id": meta.get("unit_id", unit),
        "model": meta.get("model"),
        **archive_revision(root, inputs),
        "what_this_is": (
            "The byte-to-physical-quantity conversions the GS insertion-effect parameter block "
            "uses, and which (type, slot) pairs an archive of what one individual unit answered "
            "gives one to. Committed so that a clone reads them without the archive."
        ),
        "what_this_cannot_see": WHAT_THIS_CANNOT_SEE,
        "schema": {
            "every_entry_carries": ["unit_specific", "approximate", "source"],
            "unit_specific": (
                "This unit returned a value the class's own law does not account for, by more "
                "than the reading separates. A second unit is free to differ here."
            ),
            "approximate": "No reading placed this entry; it is carried on the law alone.",
            "source": (
                "The provenance of the conversion law, as the three values the two flags above "
                "project onto: unit_overrides_assigned where a reading overruled the law, "
                "assigned where no reading placed it, measured where a reading placed it and it "
                "agrees. The two flags are exclusive on one entry; on a table or a map row they "
                "are a fold over entries and both can hold, where overruled is what it reads as. "
                "Not the provenance of a binding, which is a different axis and is counted by "
                "tools/gs/coverage.py."
            ),
            "what_placed_it": (
                "Where a cell says so, which reading or which law put the value there. Prose "
                "beside source rather than under it, since source is mechanical."
            ),
        },
        "reach": {
            "printed": printed,
            "measured": len(reached),
            "reached": len(reached),
            "translatable": None,
            "translated": None,
            "what_the_first_two_are": (
                "Printed is every (type, slot) the unit's own records carry a printed value for, "
                "counted by tools/gs/coverage.py's enumeration -- the denominator the block's "
                "coverage is read against. Measured is how many of them these tables give a "
                "conversion to; reached is the same count under the name the schema opened with."
            ),
            "why_two_are_null": (
                "Translatable asks whether the insert a slot maps to has a control of the same "
                "physical unit, and translated asks whether the GS layer emits the key. Neither "
                "is a property of the archive, so neither is derived here."
            ),
        },
        "reach_by_class": {name: reach[name] for name in CLASS_ORDER},
        "reach_by_table": reach_by_table,
        "defaults": {
            "types": len(defaults),
            "slots_per_type": PARAMETER_SLOTS,
            "types_with_complete_defaults": complete,
            "what_they_are": (
                "The bytes the unit was measured holding at power-on, per type. Not the manual's "
                "printed defaults, which the archive does not transcribe."
            ),
            "by_type": {
                spelled_type(code): [slots.get(FIRST_PARAMETER + i) for i in range(PARAMETER_SLOTS)]
                for code, slots in sorted(defaults.items())
            },
        },
        "classes": classes,
        "fixed_corners": fixed_corners,
        "state_lists": lists,
        "map": sorted(reached, key=lambda r: (r["type"], r["address"])),
        "disagreement": disagreements(classes),
        "records_outside_their_inferences_addresses": outside,
        "entries_resting_on_a_parked_inference": parked,
        "derivation_checks": checks,
        "notes": notes,
    }

    stamp_sources(tables["classes"], "classes")
    stamp_sources(tables["fixed_corners"], "fixed_corners")
    stamp_sources(tables["state_lists"], "state_lists")
    # A `map` row's flags are the fold its table already made, so it is stamped
    # as a fold rather than let through the walker, which would read a row with
    # no flag-carrying children as one entry and refuse the two accel slots.
    for row in tables["map"]:
        row["source"] = fold_source(row["unit_specific"], row["approximate"])
    tables["reach"]["by_source"] = check_map_sources(tables["map"])
    return tables


def cpp_float(value: float) -> str:
    """A float literal C++ will take. `5f` is not one; `5.0f` is."""
    text = f"{value:.6g}"
    if "." not in text and "e" not in text and "E" not in text:
        text += ".0"
    return text + "f"


def emit_header(tables: dict, path: Path) -> None:
    """Render the committed JSON into the header the C++ side includes.

    The reach counts are named constants and not an array, because the test that
    reads them enumerates the fourteen classes by hand: a header that offered the
    list would let a derivation that dropped a class pass by not counting it.
    """
    classes = tables["classes"]
    reach = tables["reach_by_class"]
    out: list[str] = []
    w = out.append

    w(f"// Generated by {GENERATED_BY} from tools/gs/efx-tables.json -- do not edit.")
    w("//")
    w("// The byte-to-physical-quantity conversions the GS insertion-effect parameter")
    w("// block uses, derived from an archive of what one individual SC-8850 answered.")
    w("// Nothing here is a sample-rate coefficient: every quantity is Hz, ms, seconds,")
    w("// decibels or a ratio, and a coefficient is built in prepare().")
    w("//")
    w("// tools/gs/docs/efx-tables.md says how the tables are read and what they")
    w("// cannot see. tools/gs/efx-tables.json carries the provenance of each entry.")
    w("")
    w("#pragma once")
    w("")
    w("#include <array>")
    w("#include <cstdint>")
    w("#include <string_view>")
    w("")
    w("namespace sonare::midi::synth {")
    w("")
    w(f'inline constexpr std::string_view kGsEfxUnitId = "{tables["unit_id"]}";')
    w(f'inline constexpr std::string_view kGsEfxArchiveRevision = "{tables["archive_revision"]}";')
    w("")
    w("/// A (setting, value) knot of a piecewise-linear table.")
    w("struct GsEfxBreakpoint {")
    w("  uint8_t setting;")
    w("  float value;")
    w("};")
    w("")
    w("/// One (type, slot) pair the archive gives a conversion to.")
    w("struct GsEfxSlotConversion {")
    w("  uint16_t type;             ///< The two type bytes, MSB in the high byte.")
    w("  uint8_t parameter;         ///< Slot index from the first parameter address.")
    w("  uint8_t conversion_class;  ///< One of the kGsEfxClass* values below.")
    w("  uint8_t table;             ///< Which table of that class; see the JSON.")
    w("};")
    w("")
    w("/// A slot whose page prints a list of states, and how many it prints.")
    w("struct GsEfxStateList {")
    w("  uint16_t type;       ///< The two type bytes, MSB in the high byte.")
    w("  uint8_t parameter;   ///< Slot index from the first parameter address.")
    w("  uint8_t states;      ///< Bytes 0 to states - 1 are the list.")
    w("};")
    w("")
    w("/// The twenty bytes a type powers up holding, and which of them were measured.")
    w("struct GsEfxTypeDefaults {")
    w("  uint16_t type;")
    w("  uint32_t measured;  ///< Bit i set where slot i carries a measured byte.")
    w("  std::array<uint8_t, 20> params;")
    w("};")
    w("")

    w("// How far the archive reaches, per conversion class. A class reaching zero is a")
    w("// failure of the derivation and not a property of the unit.")
    for index, name in enumerate(CLASS_ORDER):
        w(f"inline constexpr int kGsEfxReach{camel(name)} = {reach[name]};")
        w(f"inline constexpr uint8_t kGsEfxClass{camel(name)} = {index};")
    w("")
    w("// The two numbers the block's coverage is read as: every (type, slot) the unit")
    w("// prints a value for, and how many of those these tables give a conversion to.")
    w("// Held apart because the numerator alone reads as an amount understood, which")
    w("// it is not, and because one number cannot say which of the two moved.")
    w(f"inline constexpr int kGsEfxPrinted = {tables['reach']['printed']};")
    w(f"inline constexpr int kGsEfxMeasured = {tables['reach']['measured']};")
    w("")
    w("// The same count per table. A class holds more than one -- two rate ranges, five")
    w("// delay ladders, three frequency columns -- so a table that stops being fed leaves")
    w("// its class's count untouched, which is the shape the class guard was written")
    w("// against. Named constants rather than an array, for the reason the class counts are:")
    w("// a check that reads a list the generator wrote passes by not looking.")
    for key, using in tables["reach_by_table"].items():
        name, table_name = key.split(".", 1)
        w(f"inline constexpr int kGsEfxTableUse{camel(name)}{camel(table_name)} = {using};")
    w("")

    rate = classes["rate"]["tables"]
    for table_name in ("narrow", "wide"):
        knots = rate[table_name]["breakpoints"]
        w(f"/// Rate, printed {' - '.join(rate[table_name]['printed_ends'])}.")
        w(
            f"inline constexpr std::array<GsEfxBreakpoint, {len(knots)}> "
            f"kGsEfxRate{table_name.capitalize()} = {{{{"
        )
        for k in knots:
            w(f"    {{{k['setting']}, {cpp_float(k['hz'])}}},")
        w("}};")
        w("")

    w("/// The rate the unit's own delay ladders are cut to. Not a rendering rate.")
    w(f"inline constexpr int kGsEfxUnitClockHz = {UNIT_CLOCK_HZ};")
    w("")
    for table_name, table in classes["delay_time"]["tables"].items():
        knots = table["breakpoints"]
        w(f"/// Delay time, printed {' - '.join(table['printed_ends'])}.")
        w(
            f"inline constexpr std::array<GsEfxBreakpoint, {len(knots)}> "
            f"kGsEfxDelay{camel(table_name)} = {{{{"
        )
        for k in knots:
            w(f"    {{{k['setting']}, {cpp_float(k['ms'])}}},")
        w("}};")
        w("")

    w("/// The 16th entry of a frequency column the unit prints a bypass against.")
    w("inline constexpr float kGsEfxFreqBypass = 0.0f;")
    for table_name, table in classes["freq"]["tables"].items():
        w(f"/// Frequency, printed {table['printed_range_the_claim_calls_it']}.")
        w(f"inline constexpr std::array<float, 16> kGsEfxFreq{camel(table_name)} = {{{{")
        for e in table["entries"]:
            value = "kGsEfxFreqBypass" if e["hz"] is None else cpp_float(e["hz"])
            w(f"    {value},")
        w("}};")
        w("")

    gain = classes["gain"]["tables"]["tone"]
    w("// Gain: decibels a step over a window, nearest edge outside it.")
    w(f"inline constexpr uint8_t kGsEfxGainWindowLo = {gain['window'][0]};")
    w(f"inline constexpr uint8_t kGsEfxGainWindowHi = {gain['window'][1]};")
    w(f"inline constexpr int kGsEfxGainOffset = {gain['offset']};")
    w(f"inline constexpr float kGsEfxGainDbPerStep = {cpp_float(gain['db_per_step'])};")
    w("")

    level = classes["level"]["tables"]["output"]
    w("// Output level: a stored numerator over 127. No closed form crosses this curve.")
    w(f"inline constexpr uint8_t kGsEfxLevelDenominator = {level['denominator']};")
    w("inline constexpr std::array<uint8_t, 128> kGsEfxLevelNumerator = {{")
    numerators = [e["numerator"] for e in level["entries"]]
    for start in range(0, 128, 16):
        w("    " + " ".join(f"{n}," for n in numerators[start : start + 16]))
    w("}};")
    w("")

    width = classes["width"]["tables"]["section"]
    w("/// Section width at the half-gain points. Settings past the table return entry 0.")
    w("inline constexpr std::array<float, 5> kGsEfxWidth = {{")
    w("    " + " ".join(f"{cpp_float(e['octaves'])}," for e in width["entries"]))
    w("}};")
    w("")

    wave = classes["wave"]["tables"]["modulator"]
    w("// Modulation shapes. The fraction of the cycle that climbs separates the two saws;")
    w("// the third harmonic is what separates entry 2 from a plain sine.")
    w("inline constexpr std::array<float, 5> kGsEfxWaveRisingFraction = {{")
    w("    " + " ".join(f"{cpp_float(e['rising_fraction'])}," for e in wave["entries"]))
    w("}};")
    w("inline constexpr std::array<float, 5> kGsEfxWaveThirdHarmonic = {{")
    w("    " + " ".join(f"{cpp_float(e['third_harmonic'])}," for e in wave["entries"]))
    w("}};")
    w("")

    for side in ("left", "right"):
        knots = classes["pan"]["tables"]["output"]["sides"][side]
        w(f"/// Output pan, the {side} side's multiplier, the added term taken off.")
        w(
            f"inline constexpr std::array<GsEfxBreakpoint, {len(knots)}> "
            f"kGsEfxPan{side.capitalize()} = {{{{"
        )
        for k in knots:
            w(f"    {{{k['setting']}, {cpp_float(k['multiplier'])}}},")
        w("}};")
        w("")

    balance = classes["balance"]["tables"]["effect"]
    w("// Balance: two truncated ramps offset so they meet at full rather than crossing.")
    w(f"inline constexpr int kGsEfxBalanceCornerEffect = {balance['corner_past_the_effect']};")
    w(f"inline constexpr int kGsEfxBalanceCornerDirect = {balance['corner_through_the_effect']};")
    w(f"inline constexpr int kGsEfxBalanceWidth = {balance['width']};")
    w(f"inline constexpr int kGsEfxBalanceGrid = {balance['grid']};")
    w("")

    azimuth = classes["azimuth"]["tables"]["placement"]
    w("// Azimuth: two added, shifted right two, recentred, clamped. 31 places, two of them one.")
    w(f"inline constexpr int kGsEfxAzimuthClamp = {azimuth['clamp']};")
    w(f"inline constexpr int kGsEfxAzimuthDegreesPerPosition = {azimuth['degrees_per_position']};")
    w("")

    accel = classes["accel"]["tables"]["rotor"]
    w("// Rotary acceleration: the divisor the rotor's loop divides its rate gap by, from the")
    w("// top four bits. Held as a divisor and turned into seconds by the shift below, never")
    w("// as a per-step coefficient -- that would move the time constant with the sample rate.")
    w(f"inline constexpr int kGsEfxAccelShift = {accel['to_seconds']['shift']};")
    w("inline constexpr std::array<float, 16> kGsEfxAccelDivisor = {{")
    w("    " + " ".join(f"{cpp_float(e['divisor'])}," for e in accel["entries"]))
    w("}};")
    w("")

    post_gain = classes["post_gain"]["tables"]["makeup"]
    w("// Post gain: fixed steps after the stage it is printed beside. Past the table, entry 0.")
    w(f"inline constexpr uint8_t kGsEfxPostGainSettings = {post_gain['settings']};")
    w(f"inline constexpr float kGsEfxPostGainDbPerStep = {cpp_float(post_gain['db_per_step'])};")
    w("")

    window = classes["window"]["tables"]["splice"]
    w("/// Splice window: how far the read-out drifts between splices. Past the table, entry 0.")
    w(f"inline constexpr std::array<float, {len(window['entries'])}> kGsEfxWindowMs = {{{{")
    w("    " + " ".join(f"{cpp_float(e['ms'])}," for e in window["entries"]))
    w("}};")
    w("")

    corner = classes["corner"]["tables"]
    orders = {corner[side]["order"] for side in ("low", "high")}
    orders |= {
        pair[side]["order"] for pair in tables["fixed_corners"].values() for side in ("low", "high")
    }
    if len(orders) != 1:
        sys.exit(f"the shelves these tables carry are of orders {sorted(orders)}, not one order")
    w("/// Every shelf the tables carry is of this order: one pole and one zero a section.")
    w(f"inline constexpr int kGsEfxShelfOrder = {orders.pop()};")
    w("")
    w("/// Equaliser corner: byte 0 selects the first state and byte 1 the second.")
    for side in ("low", "high"):
        states = ", ".join(cpp_float(e["hz"]) for e in corner[side]["entries"])
        w(f"inline constexpr std::array<float, 2> kGsEfxCorner{camel(side)} = {{{{{states}}}}};")
    w("")
    w("// Shelf pairs no byte selects a corner for: the tone pair after every effect, and")
    w("// the pair a combination type's equaliser prints gains and no corner for.")
    for name, pair in tables["fixed_corners"].items():
        for side in ("low", "high"):
            w(
                f"inline constexpr float kGsEfx{camel(name)}{camel(side)}Hz = {cpp_float(pair[side]['hz'])};"
            )
    w("")

    slots = tables["map"]
    w("/// Every (type, slot) pair the archive gives a conversion to.")
    w(
        f"inline constexpr std::array<GsEfxSlotConversion, {len(slots)}> "
        "kGsEfxSlotConversions = {{"
    )
    for row in slots:
        index = CLASS_ORDER.index(row["conversion_class"])
        table_index = list(classes[row["conversion_class"]]["tables"]).index(row["table"])
        w(
            f"    {{0x{parsed_type(row['type']):04X}, {row['parameter']}, "
            f"{index}, {table_index}}},  // {row['conversion_class']}.{row['table']}"
        )
    w("}};")
    w("")

    lists = tables["state_lists"]["slots"]
    w("/// Every slot printing a list of states. A byte past the list is not taken and")
    w("/// the slot keeps the state it was in. Sorted by (type, parameter).")
    w(f"inline constexpr std::array<GsEfxStateList, {len(lists)}> kGsEfxStateLists = {{{{")
    for row in lists:
        w(f"    {{0x{parsed_type(row['type']):04X}, {row['parameter']}, {row['states']}}},")
    w("}};")
    w("")

    by_type = tables["defaults"]["by_type"]
    w("/// What every type powers up holding. `measured` is clear where a slot was refused.")
    w(
        f"inline constexpr int kGsEfxTypesWithCompleteDefaults = "
        f"{tables['defaults']['types_with_complete_defaults']};"
    )
    w(f"inline constexpr std::array<GsEfxTypeDefaults, {len(by_type)}> kGsEfxTypeDefaults = {{{{")
    for spelled, params in by_type.items():
        mask = sum(1 << i for i, value in enumerate(params) if value is not None)
        bytes_out = ", ".join(str(value if value is not None else 0) for value in params)
        w(f"    {{0x{parsed_type(spelled):04X}, 0x{mask:05X}, {{{bytes_out}}}}},")
    w("}};")
    w("")
    w("}  // namespace sonare::midi::synth")
    path.write_text("\n".join(out) + "\n")

    # The header is a `.h` under `src/`, so `make format` owns its layout and would
    # rewrite whatever this emitted -- which would leave the committed file and a
    # regenerated one differing over nothing, and the check target red for it. So
    # the formatter runs here instead of being raced with: a missing one is a
    # failure rather than a quietly different rendering.
    # Named explicitly rather than left to the search up from the file: the check
    # target renders into a scratch directory, and a style found by proximity
    # would lay that copy out differently from the committed one and report the
    # difference as drift in the tables.
    style = Path(__file__).resolve().parents[2] / ".clang-format"
    try:
        subprocess.run(["clang-format", f"--style=file:{style}", "-i", str(path)], check=True)
    except (OSError, subprocess.CalledProcessError) as failure:
        sys.exit(
            f"clang-format could not lay out {path} ({failure}). The repository formats every "
            "header, so an unformatted one would differ from a regenerated one over layout alone."
        )


def camel(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def report(tables: dict) -> None:
    reach = tables["reach"]
    print(f"{reach['printed']:>8}  printed")
    print(f"{reach['measured']:>8}  measured")
    print(f"{reach['reached']:>8}  reached")
    print(f"{'-':>8}  translatable   (filled in once the insert side is wired)")
    print(f"{'-':>8}  translated     (filled in once the GS layer emits the keys)")
    print()
    for name in CLASS_ORDER:
        print(f"{tables['reach_by_class'][name]:>8}  {name.replace('_', ' ')}")
    print()
    for name in SOURCE_VALUES:
        print(f"{reach['by_source'][name]:>8}  map entries whose law is {name}")
    print()
    print(f"{tables['defaults']['types']:>8}  types with a power-on record")
    complete = tables["defaults"]["types_with_complete_defaults"]
    print(f"{complete:>8}  of them complete over 20 slots")
    print()
    print(f"archive_revision  {tables['archive_revision']}")
    print(f"                  {tables['archive_revision_source']}")
    if tables["archive_inputs_dirty"]:
        print("                  the working copy is dirty over the files read, so the")
        print("                  revision does not identify them")
    print()

    idle = tables["derivation_checks"]["tables_no_reached_slot_uses"]
    print(f"tables no reached slot uses: {len(idle)}")
    for row in idle:
        print(f"    {row['table']}  columns {' '.join(row['columns'])}  -- {row['kind']}")
        for slot in row["slots_whose_printed_spelling_names_it"]:
            print(
                f"        {slot['type']} {slot['address']}  type in the claim's list: "
                f"{slot['the_type_is_in_the_inferences_list']}"
            )

    outside = tables["records_outside_their_inferences_addresses"]
    print(f"records outside their inference's about.addresses: {len(outside)}")
    for row in outside:
        print(
            f"    {row['type']} {row['address']}  {row['conversion_class']}"
            f"  printed {row['printed_values']}  ({row['rests_on']})"
        )

    parked = tables["entries_resting_on_a_parked_inference"]
    print(f"entries resting on a parked inference: {len(parked)}")
    for row in parked:
        print(f"    {row['type']} {row['address']}  {row['conversion_class']}  ({row['rests_on']})")
    states = tables["derivation_checks"]["the_inference_each_class_rests_on"]
    standing = sum(1 for row in states if row["state"] == "standing")
    print(f"    of {len(states)} class inferences examined, {standing} standing")

    disputes = tables["derivation_checks"]["records_whose_name_and_prepared_state_disagree"]
    print(f"records whose name and prepared state disagree: {len(disputes)}")
    for row in disputes:
        print(f"    {row['file']}  prepared says power-on: {row['prepared_says_power_on']}")

    cut = tables["derivation_checks"]["the_cut_floor_against_round"]
    print()
    print(
        f"the cut, floor against round: {cut['settings_where_the_two_differ']} settings separate "
        f"them, floor closer at {cut['floor_closer']}, round at {cut['round_closer']}"
    )
    for row in cut["per_record"]:
        separating = row["settings_where_the_two_differ"]
        print(
            f"    {row['ladder']:<10} {row['record'][:34]:<36} n={separating:>3}"
            f"  floor {row['floor_closer']:>3} ({row['median_ms_floor']:.4f} ms)"
            f"  round {row['round_closer']:>3} ({row['median_ms_round']:.4f} ms)"
        )

    print()
    for key in (
        "delay_ladders_against_the_records",
        "delay_records_of_slots_not_reached",
        "rate_tables_against_the_records",
    ):
        checks = tables["derivation_checks"][key]
        unit = "ms" if "delay" in key else "hz"
        print(f"{key.replace('_', ' ')}: {len(checks)} records")
        for row in checks:
            over = f"  {row['settings_over_1ms']} over 1 ms" if "settings_over_1ms" in row else ""
            print(
                f"    {row['type']} {row['address']}  {row.get('ladder') or row.get('table'):<10}"
                f"  n={row['settings']:>3}  median {row[f'median_{unit}']:.4f}"
                f"  worst {row[f'worst_{unit}']:.4f} {unit}{over}"
            )
            misses = row.get("the_settings_over_1ms", [])
            for miss in misses[:SAMPLES]:
                print(
                    f"        setting {miss['setting']:>3}  read {miss['read_ms']}"
                    f"  ladder {miss['the_ladder_says_ms']}"
                    f"  the record also read {miss['the_record_also_read_ms']}"
                )
            if len(misses) > SAMPLES:
                print(f"        and {len(misses) - SAMPLES} more, in full in the tables file")
    for note in tables["notes"]:
        print()
        print(note)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--archive", help="an archive root holding data/ and inferences/")
    ap.add_argument("--unit", default="roland-sc8850-01", help="which unit of the archive to read")
    ap.add_argument("--from-json", help="skip the archive and render a committed table file")
    ap.add_argument("--out", help="where to write the tables; omitted prints the summary only")
    ap.add_argument("--header", help="where to write the C++ header")
    args = ap.parse_args()

    if args.from_json:
        tables = json.loads(Path(args.from_json).read_text())
    elif args.archive:
        root = Path(args.archive)
        if not (root / "data").is_dir() or not (root / "inferences").is_dir():
            sys.exit(f"{root} holds no data/ and inferences/; this is not a measurement archive")
        tables = derive(root, args.unit)
    else:
        sys.exit("one of --archive or --from-json is required")

    report(tables)

    if args.out:
        Path(args.out).write_text(json.dumps(tables, indent=2, ensure_ascii=False) + "\n")
        print(f"\nwrote {args.out}")
    if args.header:
        emit_header(tables, Path(args.header))
        print(f"wrote {args.header}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
