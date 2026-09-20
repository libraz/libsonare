"""Which layer a bank slot is aimed at, as the one reader of `policy.json`.

`policy.json` records that a slot naming a real instrument takes a modern
recording while a slot naming a sound the machine invented takes the machine,
and a capture's `source_class` records which of those a recording came from.
Nothing brought the two together: the layer was read only where a page was
rendered, so the resolver that picks which capture answers a voice could not
consult it and picked by filename order instead.

The mapping from `source_class` to a layer lives here rather than at either
call site, because a second copy of it is a second answer to `is this reference
the kind of thing the slot wants` and the two would disagree silently.
"""

from __future__ import annotations

import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
POLICY_PATH = HERE / "policy.json"

#: The `source_class` of a capture read out of the machine's own recordings.
#: Every other class is a recording of an instrument, whatever product made it.
MACHINE_SOURCE = "module"


def load(path: Path | None = None) -> dict:
    """The policy as data, or an empty dict where it cannot be read.

    A missing policy leaves every slot unaimed rather than mis-aimed, which is
    what the callers want: an unreadable file must not silently promote one
    layer over the other.
    """
    try:
        return json.loads((path or POLICY_PATH).read_text())
    except (OSError, json.JSONDecodeError):
        return {}


def wanted_layer(policy: dict, program: int, kit: bool, bank: int = 0) -> dict:
    """Which kind of reference this slot is aimed at, and why.

    Two branches are selected by a flag rather than by a program number, because
    both share the program space with something else and the number cannot tell
    them apart. A kit takes the kit branch whatever its number, or a kit selected
    by a program some branch also names would be answered as that melodic voice.
    A GS variation takes the variation branch whenever its bank is non-zero,
    because a variation carries its capital's program: dispatching it by number
    resolves it to `default`, which wants an instrument, and the caller's
    off-target test then cannot fire on any variation at all. Otherwise a branch
    naming this program wins, and `default` takes everything left.
    """
    branches = policy.get("reference_layer")
    if not isinstance(branches, dict):
        return {}
    named = {k: v for k, v in branches.items()
             if isinstance(v, dict) and not k.startswith("_")}
    chosen = ""
    if kit and "kits" in named:
        chosen = "kits"
    elif bank and "variations" in named:
        chosen = "variations"
    for name, branch in named.items():
        if not chosen and program in (branch.get("programs") or []):
            chosen = name
    if not chosen:
        chosen = "default"
    branch = named.get(chosen)
    if not branch:
        return {}
    return {
        "branch": chosen,
        "timbre": branch.get("timbre") or "",
        "behaviour": branch.get("behaviour") or "",
        "reason": branch.get("reason") or "",
    }


def answers_layer(source_class: str | None, want: dict) -> bool:
    """Whether a capture of this class answers the layer a slot is aimed at.

    Unclassified is never an answer. A slot aimed at the machine and fitted
    against a library's idea of the sound is indistinguishable from a finished
    one afterwards, so a capture saying nothing about where it came from cannot
    be read as saying the right thing — the same reason `Capture.source_class`
    refuses a default.
    """
    timbre = want.get("timbre") or ""
    if not timbre or not source_class:
        return False
    if timbre == "machine":
        return source_class == MACHINE_SOURCE
    return source_class != MACHINE_SOURCE
