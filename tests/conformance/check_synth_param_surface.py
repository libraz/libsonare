"""Hold the automatable NativeSynth parameters equal to the patch's own continuous fields.

Two files each claim to mirror the other and nothing compared them.
``native_synth_params.cpp`` says its JSON keys "mirror the bindings' SynthPatch
fields exactly"; the C ABI says the automatable names are "the continuous
SonareSynthPatch fields spelled exactly as the bindings spell them". Both were
false, in both directions at once: ``pitchOffsetCents`` was automatable on four
surfaces while existing as a patch field on none, and every block appended to
the patch after the table was written -- the series highpass, then the two
converter halves -- reached the patch and not the table.

Neither half shows up anywhere else. ``make parity`` compares signatures, and a
name present in a parameter table and absent from a struct is not a signature
disagreement; a binding test sees only what its own surface declares. So the
comparison has to be made here or not at all.

What counts as a continuous field is decided by the struct's own section
markers rather than by a list kept here: an engine block is read only by a
patch voicing that engine, so it is not part of the wrapper every patch shares
and the table is deliberately wrapper-only. A new engine block is therefore
excluded the day it is added, and a new *wrapper* float is reported until it is
either automatable or named in EXPECTED_ABSENT with a reason.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "include" / "sonare" / "sonare_c_types_analysis.h"
TABLE = ROOT / "src" / "midi" / "synth" / "native_synth_params.cpp"

#: Struct sections holding fields one engine reads rather than every patch. The
#: automation table covers the wrapper, so a field here is absent on purpose.
ENGINE_SECTIONS = ("sample engine",)

#: Wrapper floats that are deliberately not automatable, each with the reason.
#: A bare exclusion is how the two files drifted apart in the first place.
EXPECTED_ABSENT: dict[str, str] = {}

#: Floors below which the scan has stopped reading what it thinks it reads. Both
#: sides shrank to zero would otherwise agree perfectly and pass.
MIN_PATCH_FLOATS = 20
MIN_TABLE_ENTRIES = 20


def camel(name: str) -> str:
    head, *rest = name.split("_")
    return head + "".join(word.title() for word in rest)


def patch_float_fields(text: str) -> dict[str, str]:
    """Every ``float`` member of SonareSynthPatch, mapped to its section name."""
    end = text.index("} SonareSynthPatch;")
    start = text.rindex("typedef struct {", 0, end)
    section = ""
    fields: dict[str, str] = {}
    for line in text[start:end].splitlines():
        marker = re.search(r"/\*\s*---\s*(.+?)\s*---\s*\*/", line)
        if marker:
            section = marker.group(1)
            continue
        member = re.match(r"\s*float\s+(\w+)\s*;", line)
        if member:
            fields[member.group(1)] = section
    return fields


def table_names(text: str) -> list[str]:
    return re.findall(r'\{"(\w+)",\s*NativeSynthParamId::', text)


def main() -> int:
    header = HEADER.read_text()
    table = TABLE.read_text()

    fields = patch_float_fields(header)
    names = table_names(table)

    problems: list[str] = []
    if len(fields) < MIN_PATCH_FLOATS:
        problems.append(
            f"read only {len(fields)} float fields from {HEADER.name}; the struct is larger than "
            f"that, so the parse stopped matching rather than the struct shrinking"
        )
    if len(names) < MIN_TABLE_ENTRIES:
        problems.append(
            f"read only {len(names)} entries from {TABLE.name}; the table is larger than that, so "
            f"the parse stopped matching rather than the table shrinking"
        )
    if len(names) != len(set(names)):
        duplicated = sorted({n for n in names if names.count(n) > 1})
        problems.append(f"the parameter table names a key twice: {', '.join(duplicated)}")

    wrapper = {
        camel(field): field
        for field, section in fields.items()
        if not any(section.startswith(s) for s in ENGINE_SECTIONS)
    }
    engine = {
        camel(field): section for field, section in fields.items() if camel(field) not in wrapper
    }

    for key in sorted(set(names) - set(wrapper)):
        if key in engine:
            problems.append(
                f"{key} is automatable but lives in the patch's '{engine[key]}' section, which "
                f"only a patch voicing that engine reads"
            )
        else:
            problems.append(
                f"{key} is automatable but is not a continuous SonareSynthPatch field; every "
                f"other automatable name is one, and a caller reading either doc expects to set "
                f"it in a patch too"
            )

    for key in sorted(set(wrapper) - set(names)):
        if key in EXPECTED_ABSENT:
            continue
        problems.append(
            f"{key} is a continuous patch field and is not automatable; add it to the parameter "
            f"table, or to EXPECTED_ABSENT here with the reason it cannot be driven over time"
        )

    for key, reason in sorted(EXPECTED_ABSENT.items()):
        if key not in wrapper:
            problems.append(
                f"EXPECTED_ABSENT names {key} ({reason}) and the patch has no such wrapper field, "
                f"so the exclusion cannot fire"
            )
        elif key in names:
            problems.append(f"EXPECTED_ABSENT names {key} and it is automatable after all")

    if problems:
        print("the synth patch and its automation table disagree:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    print(
        f"the automation table and the patch agree: {len(names)} automatable names against "
        f"{len(wrapper)} continuous wrapper fields, {len(engine)} engine-block fields excluded by "
        f"section"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
