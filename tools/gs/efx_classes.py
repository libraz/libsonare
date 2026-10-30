"""The EFX conversion classes and the address/type spellings the tables use.

Shared by the derivation, the header emitter and the reports; nothing in here
reads an archive.
"""

from __future__ import annotations

GENERATED_BY = "tools/gs/derive_efx_tables.py"

# Concrete readings shown per aggregated finding on stdout. The tables file
# carries every one of them; this only bounds what a run prints.
SAMPLES = 4

# The internal rate every independent reading in the archive converges on. It is
# what a delay-time ladder is cut to and what the rotary's acceleration constant
# factors by; it is NOT a rate anything here renders at (docs/gs.md, and the
# design's rule that a coefficient is never stored, only the physical quantity).
UNIT_CLOCK_HZ = 32000

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

# The three values `source` takes. It is the provenance of the conversion law,
# a projection of the two flags above, and says nothing about how a slot is
# bound to an insert -- that is a different axis and a different count.
SOURCE_MEASURED = "measured"
SOURCE_ASSIGNED = "assigned"
SOURCE_UNIT_OVERRIDES_ASSIGNED = "unit_overrides_assigned"
SOURCE_VALUES = (SOURCE_MEASURED, SOURCE_ASSIGNED, SOURCE_UNIT_OVERRIDES_ASSIGNED)


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
