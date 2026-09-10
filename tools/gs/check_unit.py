#!/usr/bin/env python3
"""Compare the GS address table against what a measured unit actually answered.

``src/midi/synth/docs/gs.md`` names the SC-8850 as the target. Which manual a row
was written from no longer decides what it says -- the SC-88Pro and SC-8850 maps
agree on size, data range and power-on default everywhere they overlap, and the
nine points where they do not are listed there. What is left is the document
against the machine, and this is what closes it: it takes the table as the code
holds it and an archive of what one individual unit answered, and reports where
the two disagree.

**Nothing here decides anything.** A disagreement is a question -- a row
transcribed wrongly, one of the nine points where the machines genuinely differ,
a limit of what the probe could see -- and which of those it is comes from
reading the row and the record, not from this tool. What it does is make the
list finite.

The archive is external and its licence is its own; the diff is committed, so a
fresh clone reads the work list without fetching anything. Same arrangement as
the address census next to it.

Usage::

    check_unit.py --table .cache/gs-address-table.json \\
                  --unit <archive>/data/units/roland-sc8850-01 \\
                  --out tools/gs/unit-diff.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# What this reads, of the couple of hundred records the archive holds.
#
# Two of the three measurements are named by their **stage** rather than by a
# file, because a stage is free to be more than one run and this one is: an
# archive takes a capture two ways where neither way reaches what the other
# does. This unit's power-on state is read once region by region and once an
# offset at a time, and the second is the only one that reaches a live address
# sitting past a silent gap -- which is where PART OUTPUT ASSIGN turned out to
# be. Naming one file here reads whichever half the name landed on and reports
# the other half's addresses as absent from the machine, silently.
META = "meta.json"
POWER_ON_STAGE = "power-on"
WRITE_PROBE_STAGE = "write-probe"
BOUNDARY = "boundary/whole-map.json"

# A write-probe record carrying this key put the machine into a state before
# writing, so its accepted values are that state's rather than the power-on
# machine's and they do not belong in one flat map. The archive declares the
# preparation in the record; nothing here needs to know which records those are.
PREPARED = "prepared"

# The finding naming the blocks that are a window onto another store rather than
# storage of their own. Read by the kind the finding gives itself, so a unit
# whose window sits elsewhere is read correctly without this knowing where.
WINDOW_FINDING = "blocks-that-are-a-window"

# Rows whose default is deliberately not the machine's, with the decision that
# made it so. Declared here rather than left to the reader, for the reason the
# parity allowlist is declared: an exclusion argued only in prose is invisible to
# anything mechanical and reads as an oversight. An entry that stops suppressing
# anything is reported as stale rather than sitting unexamined.
DEFAULT_IS_NOT_THE_MACHINES = {
    "kDrumPlayNote": "no power-on value; a drum set change re-initialises it to the kit's",
    "kDrumLevel": "held at the identity 7F so an unwritten value cannot overrule the kit",
    "kDrumAssignGroup": "a group is a name; 00 takes the note out of every group",
    "kDrumPanpot": "held at the identity 40 so an unwritten value cannot overrule the kit",
    "kDrumReverbSend": "a multiplicand; 7F is its identity",
    "kDrumChorusSend": "a multiplicand; 7F is its identity",
    "kDrumDelaySend": "a multiplicand; 7F is its identity",
    "kDrumRxNoteOff": "ACCEPT; the two voice banks disagree with nothing written",
    "kDrumMapName": "ACCEPT; a name, and the unit powers on holding its own",
    "kUserDrumPlayNote": "a user set the file builds; unwritten means the source kit's",
    "kUserDrumLevel": "held at the identity 7F",
    "kUserDrumAssignGroup": "a group is a name; 00 takes the note out of every group",
    "kUserDrumPanpot": "held at the identity 40",
    "kUserDrumReverbSend": "a multiplicand; 7F is its identity",
    "kUserDrumChorusSend": "a multiplicand; 7F is its identity",
    "kUserDrumDelaySend": "a multiplicand; 7F is its identity",
    "kUserDrumRxNoteOff": "ACCEPT, for the reason kDrumRxNoteOff is",
    "kUserDrumSetName": "ACCEPT; a name the file writes",
    "kUserDrumSourceMap": "ACCEPT; every kit is reachable from the program alone",
    "kUserDrumSourceNote": "a user set the file builds; unwritten selects nothing",
    "kPatchName": "ACCEPT; a name, and the unit powers on holding its own",
}

# Rows whose range is deliberately wider than the machine's, with the decision
# that made it so. Only the widening is excused: a row here that turns out to be
# too *narrow* for the machine is still reported, since nothing about an
# extension licenses a value the hardware takes and libsonare does not.
#
# Declared for the reason above, and stale-checked for it too: an entry that
# suppresses nothing has outlived the divergence it excuses.
RANGE_WIDENED_DELIBERATELY = {
    "kPartEfxAssign": "02-10 select the extra insertion units libsonare adds; the hardware has "
    "one unit and refuses them, which is the property that makes the extension "
    "unreachable from a spec-compliant file (docs/gs.md)",
}

# The write probe's own verdict on each byte it wrote, which is what says whether
# that byte's accepted set is a measurement of the range at all.
#
# `accepts` and `clamps` both bound the range exactly: a clamp reports the nearest
# value the machine would hold, so the accepted set is still the machine's range.
# `refuses out of range` does the same by returning the original. An `unchanging`
# byte is none of these -- the archive's own words are that "a clamp and a refusal
# cannot be told apart" there -- so its accepted set is a single value the byte
# already held, and reading that as a range would report every row over a
# write-only or whole-parameter-only address as too wide by its whole span.
RANGE_IS_MEASURED = frozenset({"accepts", "clamps", "refuses out of range"})
RANGE_UNDECIDED = "unchanging, so a clamp and a refusal cannot be told apart"

# The insertion-effect units libsonare addresses at `40 3u xx` for u = 0..F
# (gs.md, "The extensions libsonare adds"). The property that makes them safe
# without a feature flag is that a spec-compliant file cannot reach them, which
# rests on the hardware having nothing there. gs.md asserts that from the manual;
# here it is checked against the machine. `40 30 xx` is included because the
# extension numbers a unit by its own address nibble, which makes that block a
# second way into unit 0 rather than a hole: it is as much a claim about the
# hardware as the other fifteen.
EXTENSION_ADDRESSES = frozenset(
    (0x40 << 16) | (0x30 + unit) << 8 | low for unit in range(0, 16) for low in range(0x00, 0x20)
)


def advance(addr: int, steps: int) -> int:
    """The address `steps` on from this one, carrying in base 128.

    A GS address byte holds seven bits, so a run that walks off the end of a low
    byte continues at the next mid byte rather than at an eighth bit that is not
    an address (gs_address_table.h says the same about a range spanning two mid
    bytes).
    """
    hi, mid, low = (addr >> 16) & 0x7F, (addr >> 8) & 0x7F, addr & 0x7F
    low += steps
    mid, low = mid + low // 128, low % 128
    hi, mid = hi + mid // 128, mid % 128
    return (hi << 16) | (mid << 8) | low


def submasks(mask: int):
    """Every value the row's variable nibbles can take, the empty one included."""
    if mask == 0:
        yield 0
        return
    sub = mask
    while True:
        yield sub
        if sub == 0:
            return
        sub = (sub - 1) & mask


def spelled(addr: int) -> str:
    return f"{(addr >> 16) & 0xFF:02X} {(addr >> 8) & 0xFF:02X} {addr & 0xFF:02X}"


def parsed(text: str) -> int:
    hi, mid, low = (int(b, 16) for b in text.split())
    return (hi << 16) | (mid << 8) | low


def row_addresses(row: dict) -> list[int]:
    """Every concrete address a row claims: its variable nibbles times its size."""
    out = []
    for bits in submasks(row["mask"]):
        base = row["addr"] | bits
        out.extend(advance(base, i) for i in range(row["size"]))
    return sorted(set(out))


def range_addresses(entry: dict) -> list[int]:
    """Every concrete address an undefined range covers.

    A range is written to stay inside one mid byte, so walking the low byte is
    the whole of it; the variable nibbles multiply it as they do a row.
    """
    out = []
    span = entry["hi_addr"] - entry["lo_addr"] + 1
    for bits in submasks(entry["mask"]):
        base = entry["lo_addr"] | bits
        out.extend(advance(base, i) for i in range(span))
    return sorted(set(out))


def load(unit: Path, name: str) -> dict:
    path = unit / name
    if not path.is_file():
        sys.exit(f"{path} is not in the archive; this comparison needs it{elsewhere(unit, name)}")
    return json.loads(path.read_text())


def elsewhere(unit: Path, name: str) -> str:
    """What the archive's own index has under that record's stage, if anything.

    A record moves with the archive's filing rather than with anything libsonare
    decides, so the useful thing to say when one is missing is where its stage's
    records are now -- otherwise the reader goes hunting through a directory of a
    couple of hundred files for a name that no longer exists.
    """
    index = unit / "index.json"
    if not index.is_file():
        return ""
    stage = Path(name).parent.name
    files = [e["file"] for e in json.loads(index.read_text()).get("stages", {}).get(stage, [])]
    if not files:
        return f". The index lists no stage {stage!r}"
    return f". The index lists under {stage!r}: {', '.join(files)}"


def stage_records(unit: Path, stage: str) -> list[str]:
    """Every record the archive's own index files under @p stage, in its order.

    The index is the only thing that knows what a stage holds now, so a stage
    that grows a second capture is read whole without this being told.
    """
    index = unit / "index.json"
    if not index.is_file():
        sys.exit(f"{index} is not in the archive; this comparison needs it to read a stage whole")
    files = [e["file"] for e in json.loads(index.read_text()).get("stages", {}).get(stage, [])]
    if not files:
        sys.exit(f"{index} lists no stage {stage!r}, so there is nothing to compare against")
    return files


def agreed_values(unit: Path, files: list[str]) -> tuple[dict, dict, list]:
    """Every address a stage's records answered, and the value where they agree.

    Two captures of one power-on state need not agree, and this unit's do not at
    256 addresses: a region read and a single-byte read reach different bytes of
    a block, so where both answered they are two measurements rather than one
    repeated. Resolving that here by taking whichever record was read first
    would decide something -- a default comparison against the losing value
    manufactures a finding or hides one. So a disagreement is reported, and the
    address carries no value into the comparison while keeping its place among
    the addresses a read answered, which is not what is in doubt.
    """
    answered: dict[str, str] = {}
    disputed: dict[str, dict[str, str]] = {}
    for name in files:
        for address, value in load(unit, name).get("values", {}).items():
            held = answered.setdefault(address, value)
            if held != value:
                disputed.setdefault(address, {}).update({name: value})
    reports = [
        {"address": address, "readings": sorted(set([answered[address], *by_record.values()]))}
        for address, by_record in sorted(disputed.items())
    ]
    values = {a: v for a, v in answered.items() if a not in disputed}
    return answered, values, reports


def window_blocks(unit: Path, files: list[str]) -> frozenset:
    """The high bytes a stage's records call a window, from the finding naming itself.

    Searched across the stage rather than read from one file: which of a stage's
    records carries a finding is the archive's filing, not this tool's business.
    """
    for name in files:
        for finding in load(unit, name).get("findings", []):
            if finding.get("kind") == WINDOW_FINDING:
                return frozenset(int(b, 16) for b in finding["blocks"])
    sys.exit(
        f"No record of {unit} carries a {WINDOW_FINDING!r} finding. Every address in "
        "a window mirrors another store, so counting them as answered would invent "
        "thousands of gaps. Refusing to guess."
    )


def provenance(unit: Path, names: tuple[str, ...]) -> dict:
    """Which records this was derived from, and what each says about itself.

    Copied whole rather than summarised. A record that cannot say when it was
    taken says so in its own words, and which fields it is missing differs by
    record -- one kept by hand has no stage either. Restating any of that here
    would be a second copy of the archive's own account, free to drift from it.
    """
    out = {}
    for name in names:
        data = json.loads((unit / name).read_text())
        out[name] = data.get("record", "predates the archive's record envelope")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--table", required=True, help="JSON from dump_address_table")
    ap.add_argument("--unit", required=True, help="a unit directory in a measurement archive")
    ap.add_argument("--out", help="where to write the diff; omitted prints the summary only")
    ap.add_argument(
        "--samples", type=int, default=6, help="concrete addresses shown per aggregated finding"
    )
    args = ap.parse_args()

    unit = Path(args.unit)
    table = json.loads(Path(args.table).read_text())
    meta = load(unit, META)
    power_on_files = stage_records(unit, POWER_ON_STAGE)
    write_probe_files = stage_records(unit, WRITE_PROBE_STAGE)
    boundary = load(unit, BOUNDARY)

    windows = window_blocks(unit, power_on_files)
    power_on_answered, power_on_agreed, power_on_disputes = agreed_values(unit, power_on_files)

    # What the unit answered with a value, less the windows, which are not
    # storage of their own. This is the hard set: each of these returned a byte.
    answered = {parsed(a) for a in power_on_answered if (parsed(a) >> 16) not in windows}

    # How far a read actually got, which is further than the map says and further
    # than `answered`. The power-on sweep reads each region to its mapped end,
    # and the boundary probe then asks past it: `40 00 00` is mapped as four bytes
    # and answers three more, which is where MASTER VOLUME, KEY SHIFT and PAN sit.
    # Without this every one of them reads as absent from the machine.
    #
    # The extent is taken as contiguous because the count is all the record keeps.
    # It bounds what a read reached; it is not a claim that each address inside it
    # answered individually.
    reached = set(answered)
    for region in boundary["regions"]:
        start = parsed(region["address"])
        if (start >> 16) in windows:
            continue
        span = region["mapped_size"] + region["answered_beyond_the_mapped_end"]
        reached.update(advance(start, i) for i in range(span))

    # What the table claims, and which row claims it.
    claimed: dict[int, int] = {}
    for i, row in enumerate(table["rows"]):
        for addr in row_addresses(row):
            claimed.setdefault(addr, i)
    undefined = {a for entry in table["undefined_ranges"] for a in range_addresses(entry)}

    # What the unit accepted, per address, over every write-probe record that
    # wrote to the power-on machine. The probes that ask an address the whole-map
    # run never reached are the reason this is a stage rather than a file; the
    # ones taken under a prepared state are left out, their accepted values
    # belonging to that state.
    accepted: dict[int, dict] = {}
    for name in write_probe_files:
        record = load(unit, name)
        if PREPARED in record:
            continue
        for region in record.get("regions", []):
            for byte in region.get("bytes", []):
                accepted.setdefault(parsed(byte["address"]), byte)

    findings: dict[str, list] = {
        "addresses_with_no_row": [],
        "rows_no_read_reached": [],
        "default_disagreements": [],
        "defaults_one_row_cannot_express": [],
        "range_disagreements": [],
        "ranges_the_probe_could_not_decide": [],
        "blanket_rows_not_compared": [],
        "stale_default_exclusions": [],
        "stale_range_exclusions": [],
        "power_on_reads_disagree": power_on_disputes,
    }

    # A row that stands for a whole high-byte block rather than for a parameter.
    # gs.md: "Each is one row over its whole block rather than a mirror of the
    # block it shadows: the statement being made is that a group is absent, and
    # that is one statement however many parameters would have sat inside it."
    # Its lo, hi and def are not claims about any parameter, so comparing them to
    # a machine would produce one finding per address inside the block.
    #
    # The test is the row's own reach: every mid byte and every low byte. A
    # per-note drum row reaches every low byte too, but its mid byte carries the
    # parameter in one nibble and only the map varies, so it is not a block.
    for row in table["rows"]:
        whole_mid = row["mask"] & 0x007F00 == 0x007F00
        whole_low = row["mask"] & 0x00007F == 0x00007F or row["size"] >= 128
        if whole_mid and whole_low:
            findings["blanket_rows_not_compared"].append(
                {
                    "param": row["param"],
                    "address": row["address"],
                    "addresses": len(row_addresses(row)),
                    "level": row["level"],
                    "why": row["why"],
                }
            )
    blanket = {r["param"] for r in findings["blanket_rows_not_compared"]}

    # 1. Addresses the unit answers that the table does not name. gs.md calls an
    #    address with no row a defect; measured against the machine rather than
    #    against a corpus, this is that count.
    orphans = sorted(answered - set(claimed) - undefined)
    by_block: dict[str, list[int]] = {}
    for addr in orphans:
        by_block.setdefault(f"{(addr >> 16) & 0xFF:02X} {(addr >> 8) & 0xFF:02X}", []).append(addr)
    for block, addrs in sorted(by_block.items()):
        findings["addresses_with_no_row"].append(
            {
                "block": block,
                "addresses": len(addrs),
                "sample": [spelled(a) for a in addrs[: args.samples]],
            }
        )

    # 2. Rows no read reached. **This is not an absence claim.** A single-byte
    #    read that answers nothing leaves an address unproven either way -- the
    #    archive's own boundary probe says so, since a block read starting earlier
    #    reaches addresses a direct read does not. What the list is good for is
    #    aiming: a row here is one the machine has not been asked about. The
    #    families the SC-8850 does not have are named in gs.md, so an entry
    #    inside one of them is expected and one outside them is the interesting
    #    kind.
    for row in table["rows"]:
        addrs = row_addresses(row)
        if any(a in reached for a in addrs):
            continue
        findings["rows_no_read_reached"].append(
            {
                "param": row["param"],
                "level": row["level"],
                "address": row["address"],
                "addresses": len(addrs),
                "sample": [spelled(a) for a in addrs[: args.samples]],
                "why": row["why"],
            }
        )

    # 3. Reset defaults against the state the unit powers up in. Only the first
    #    byte of a row carries a default the table models (gs_address_table.h).
    #    A row's instances need not share one -- a part powers on listening to its
    #    own receive channel -- so the expected value is taken per address from
    #    `reset_not_def`, which the dump fills by calling gs_reset_default rather
    #    than by restating its rule here. Where the machine's instances disagree
    #    with each other and the table names no exception, no single byte can be
    #    right for all of them, and that is a statement about the row's shape
    #    rather than about its value. Reported apart, because both halves are
    #    findings and only one is a value to correct.
    power_on_values = {parsed(a): int(v, 16) for a, v in power_on_agreed.items()}
    disputed_addresses = {parsed(d["address"]) for d in power_on_disputes}
    excused: set[str] = set()
    defaults_undecided: set[str] = set()
    for row in table["rows"]:
        if row["param"] in blanket:
            continue
        expected_at = row["reset_not_def"]
        held: list[tuple[int, int]] = []
        for bits in submasks(row["mask"]):
            addr = row["addr"] | bits
            if addr in disputed_addresses:
                defaults_undecided.add(row["param"])
                continue
            value = power_on_values.get(addr)
            if value is not None:
                held.append((addr, value))
        if not held:
            continue
        distinct = {v for _, v in held}
        disagreeing = [(a, v) for a, v in held if v != expected_at.get(spelled(a), row["def"])]
        if not disagreeing:
            continue
        entry = {
            "param": row["param"],
            "address": row["address"],
            "table_default": row["def"],
            "table_names_exceptions": len(expected_at),
            "instances_read": len(held),
            "disagreeing_addresses": len(disagreeing),
            "sample": [
                {
                    "address": spelled(a),
                    "table_expects": expected_at.get(spelled(a), row["def"]),
                    "unit_holds": v,
                }
                for a, v in disagreeing[: args.samples]
            ],
        }
        if row["param"] in DEFAULT_IS_NOT_THE_MACHINES:
            excused.add(row["param"])
            continue
        if len(distinct) == 1:
            findings["default_disagreements"].append(entry)
        else:
            entry["distinct_values_on_the_unit"] = len(distinct)
            findings["defaults_one_row_cannot_express"].append(entry)

    # An exclusion that suppressed nothing is reported rather than left in place:
    # it keeps asserting a reviewed decision about a row that no longer needs one.
    #
    # Stale means the row was compared and agreed, never that it was not compared.
    # A row every one of whose instances landed among the readings two captures
    # disagree about produced no verdict for the exclusion to suppress, and calling
    # that stale would retire a reviewed decision on the strength of a measurement
    # that was withheld -- the same shape as scoring an empty set as perfect.
    findings["stale_default_exclusions"] = [
        {"param": p, "reason": DEFAULT_IS_NOT_THE_MACHINES[p]}
        for p in sorted(set(DEFAULT_IS_NOT_THE_MACHINES) - excused - defaults_undecided)
    ]

    # 4. Accepted ranges against what the unit took. A probed value the unit
    #    accepted from outside the row's range says the range is too narrow; a
    #    probed value inside it that the unit would not take says it is too wide.
    #    A value the probe never sent says nothing either way, which is why both
    #    halves are drawn from `wrote_read` rather than from the whole 0..7F.
    #
    #    A byte the probe could not decide is excluded rather than counted as
    #    agreement, and reported on its own: the archive says outright that a
    #    clamp and a refusal are indistinguishable there, so its accepted set is
    #    the one value the byte already held and comparing a range to it would
    #    manufacture a disagreement the size of the row.
    widening_excused: set[str] = set()
    for row in table["rows"]:
        if row["param"] in blanket:
            continue
        too_narrow: list[dict] = []
        too_wide: list[dict] = []
        undecided: list[str] = []
        for bits in submasks(row["mask"]):
            for i in range(row["size"]):
                addr = advance(row["addr"] | bits, i)
                probe = accepted.get(addr)
                if probe is None:
                    continue
                if probe["classification"] not in RANGE_IS_MEASURED:
                    undecided.append(spelled(addr))
                    continue
                took = {int(v, 16) for v in probe["accepted"]}
                sent = {int(pair[0], 16) for pair in probe["wrote_read"]}
                outside = sorted(v for v in took if not row["lo"] <= v <= row["hi"])
                refused = sorted(v for v in sent - took if row["lo"] <= v <= row["hi"])
                if outside:
                    too_narrow.append({"address": spelled(addr), "unit_accepted": outside})
                if refused:
                    too_wide.append({"address": spelled(addr), "unit_would_not_take": refused})
        if undecided:
            findings["ranges_the_probe_could_not_decide"].append(
                {
                    "param": row["param"],
                    "address": row["address"],
                    "table_range": [row["lo"], row["hi"]],
                    "undecided_at": len(undecided),
                    "sample": undecided[: args.samples],
                    "why": RANGE_UNDECIDED,
                }
            )
        if too_wide and row["param"] in RANGE_WIDENED_DELIBERATELY:
            widening_excused.add(row["param"])
            too_wide = []
        if too_narrow or too_wide:
            findings["range_disagreements"].append(
                {
                    "param": row["param"],
                    "address": row["address"],
                    "table_range": [row["lo"], row["hi"]],
                    "too_narrow_at": len(too_narrow),
                    "too_wide_at": len(too_wide),
                    "sample_too_narrow": too_narrow[: args.samples],
                    "sample_too_wide": too_wide[: args.samples],
                }
            )

    findings["stale_range_exclusions"] = [
        {"param": p, "reason": RANGE_WIDENED_DELIBERATELY[p]}
        for p in sorted(set(RANGE_WIDENED_DELIBERATELY) - widening_excused)
    ]

    # The extension's safety property, checked rather than asserted. Taken
    # against everything a read reached rather than against what answered, since
    # the question is whether the hardware has anything there at all.
    collisions = sorted(EXTENSION_ADDRESSES & reached)
    extension = {
        "addresses": len(EXTENSION_ADDRESSES),
        "reached_by_a_read": len(collisions),
        "sample_reached": [spelled(a) for a in collisions[: args.samples]],
        "what_it_means": (
            "The extra insertion-effect units live at 40 3u xx because the hardware has "
            "nothing there, which is what makes them unreachable from a spec-compliant "
            "file. Any address answered here is a collision and the extension is not safe "
            "without a flag."
        ),
    }

    diff = {
        "generated_by": "tools/gs/check_unit.py",
        "unit": {
            "unit_id": meta.get("unit_id"),
            "model": meta.get("model"),
            "identity_reply": meta.get("identity_reply"),
        },
        "derived_from": provenance(unit, (META, BOUNDARY, *power_on_files, *write_probe_files)),
        "what_the_comparison_cannot_see": [
            "The records are not shown to be one state of the machine. Several do not "
            "hold the moment they were taken, so the ordering available for those is the "
            "day each was published, under derived_from, and that bounds them from above "
            "rather than placing them. A setting changed between two runs would read "
            "here as a property of the unit.",
            "Where two captures of the power-on state disagree, neither is preferred: the "
            "address is carried under power_on_reads_disagree and no default verdict is "
            "taken from it. It still counts among the addresses a read answered, which is "
            "not what the two disagree about.",
            "No row is shown to be absent from the machine. A read that answers nothing "
            "leaves the address unproven, because a block read starting earlier reaches "
            "addresses a single-byte read does not -- which is the boundary probe's own "
            "finding about this unit.",
            "The extent a read reached is taken as contiguous, the count being all the "
            "boundary probe keeps. It bounds the reach; it does not say each address "
            "inside it answered on its own.",
            "The window blocks are excluded, so nothing is said about them at all.",
            "A range is compared only against the values the write probe actually sent; a "
            "value it never tried is neither inside nor outside as far as this is concerned.",
            "A range is compared only where the probe reached a verdict. A byte it read "
            "back unchanged for every value is carried apart, since the machine's answer "
            "there is one held value rather than a range.",
            "Whether the machine clamps an out-of-range value or refuses it is not "
            "compared. It bounds the range either way, which is what is being read here, "
            "and which of the two libsonare does is a decision gs.md takes rather than a "
            "property the table records.",
            "Levels are libsonare's own promises about its implementation and are not a "
            "property of the machine, so they are reported and never compared.",
            "One unit is one unit. A disagreement is between this table and this machine, "
            "and a second SC-8850 has not been measured.",
        ],
        "summary": {
            "table_rows": len(table["rows"]),
            "addresses_the_table_claims": len(claimed),
            "addresses_the_unit_answered": len(answered),
            "addresses_a_read_reached": len(reached),
            "addresses_with_no_row": len(orphans),
            "rows_no_read_reached": len(findings["rows_no_read_reached"]),
            "rows_whose_default_disagrees": len(findings["default_disagreements"]),
            "rows_one_default_byte_cannot_express": len(
                findings["defaults_one_row_cannot_express"]
            ),
            "rows_whose_range_disagrees": len(findings["range_disagreements"]),
            "rows_the_probe_could_not_decide": len(findings["ranges_the_probe_could_not_decide"]),
            "blanket_rows_not_compared": len(findings["blanket_rows_not_compared"]),
            "defaults_excused_by_a_decision": len(excused),
            "stale_default_exclusions": len(findings["stale_default_exclusions"]),
            "ranges_widened_by_a_decision": len(widening_excused),
            "stale_range_exclusions": len(findings["stale_range_exclusions"]),
            "power_on_reads_disagree": len(findings["power_on_reads_disagree"]),
            "extension_addresses_reached": len(collisions),
        },
        "extension_at_40_3u_xx": extension,
        **findings,
    }

    for key, value in diff["summary"].items():
        print(f"{value:>8}  {key.replace('_', ' ')}")

    if args.out:
        Path(args.out).write_text(json.dumps(diff, indent=2) + "\n")
        print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
