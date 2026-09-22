"""The last two claims about a voice, and the only two nothing on disk implies.

Every step of the ladder below `settled` is a predicate over a file some tool
already wrote: a capture's timbre count, a profile's rows, a gate's bounds and
their distance from the references' own spread. The last step is not, and that
is deliberate — it is the two questions a comparison cannot ask.

- **The structural residual.** `autofit --diagnose` separates a term no knob
  reaches from a term the fit has already spent. The first is a mechanism the
  model does not have, and no budget of fitting reaches it. That reading exists
  only while the run's output does, and the run is expensive.
- **The musical sign-off.** Someone listened to a take and said it is the
  instrument. No metric produces this and none ever will; the whole harness is
  built to make it a smaller question, not to answer it.

Neither had a home, so `status.py` read both as unknown and the top of the
ladder was unreachable rather than earned. This is that home: one entry per
voice slug, the same key the audition page shows, `calibrations.json` uses and
a render's address carries, so a note about a voice names the voice the file
does.

## An unreachable term is accepted with a reason, or it is open

A diagnosis that finds nothing unreachable is rare and is not what the file is
for. A voice reaches `settled` when every term the probe could not reach is
named here with the reason it is acceptable — a limit of the probe, a mechanism
deliberately not modelled, a measurement that is about the reference rather
than the instrument. That is the same discipline as a capture's `dimensions_na`
and the parity allowlist, for the same reason: an exclusion argued in prose is
invisible to anything mechanical and reads as an oversight however good it is.

## Both claims expire, and the two ways they expire are not the same

A residual measured against one bank says nothing about a later one, so each
record carries the bank generation it was taken at and — for a voice that has
one — the version of its own patch unit. `tools/bank-versions.json` is the
source for both.

- The patch version moved: the voice itself changed, and the record is `stale`.
- A *shared* unit moved: one of the 17 engine and fallback-table units carries
  values this voice may rest on, and nothing can say whether it does, so the
  honest answer is `unverified` rather than either verdict.

The second is deliberately not "the generation moved". The generation moves for
any of the 314 units, so dating a record against it retires every voice in the
bank whenever one patch is touched — and says a shared unit moved when none
did. The registry's own `kind` already separates the two, and a record only
rests on its own patch and the shared set.

Both block `settled`; they are named apart so the next action can say which one
happened. A kit has no single patch unit — its voices are its drum notes — so
the drum kinds stand in for the patch version it does not have, and moving them
makes its record `stale` exactly as a patch bump does. That is read separately
from the shared units rather than as one generation folding both together: with
the two merged, fitting forty-three of the kit's own notes reported as
`unverified` and said a shared unit had moved when none had.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_PATH = HERE / "signoff.json"

#: JSON keys that document the file rather than describing a voice, as in the
#: capture definitions and `calibrations.json`.
DOC_PREFIX = "_"

#: A record is only evidence about the bank it was taken from.
CURRENT = "current"
STALE = "stale"
UNVERIFIED = "unverified"


@dataclass(frozen=True)
class Provenance:
    """Which bank a record was taken against."""

    date: str = ""
    bank_generation: int = 0
    patch_version: int = 0

    def state(self, shared_generation: int, patch_version: int, own_generation: int = 0) -> str:
        """`current`, `stale` or `unverified` against the registry as it stands.

        The first argument is the generation at which a *shared* calibration
        unit last moved, not the registry's current one. A record is evidence
        about its own patch and about the constants under it, and nothing else:
        another voice's patch moving cannot reach this one, so comparing
        against the bare generation retires a diagnosis every time any of the
        297 patch and drum units is touched -- while saying, wrongly, that a
        shared unit had moved.

        `own_generation` is for a voice with no single patch unit to be versioned
        by. A kit's own voices are its drum notes, and forty-three of them moving
        is the voice itself changing -- `stale`, the same as a patch bump. Dating
        it by a generation that folds the drum kinds into the shared ones instead
        makes that read as `unverified` and report a shared unit as having moved
        when none did.
        """
        if patch_version and self.patch_version and patch_version > self.patch_version:
            return STALE
        if own_generation and self.bank_generation and own_generation > self.bank_generation:
            return STALE
        if shared_generation and self.bank_generation and shared_generation > self.bank_generation:
            return UNVERIFIED
        return CURRENT


@dataclass(frozen=True)
class Structure:
    """What `autofit --diagnose` reported, and which of it is accepted."""

    provenance: Provenance
    spec: str = ""
    probe: str = ""
    unreachable: tuple[str, ...] = ()
    accepted: dict[str, str] = None  # type: ignore[assignment]
    note: str = ""

    @property
    def open_terms(self) -> list[str]:
        """Unreachable terms nobody has given a reason for."""
        return [t for t in self.unreachable if t not in (self.accepted or {})]


@dataclass(frozen=True)
class Music:
    """A take somebody listened to and signed."""

    provenance: Provenance
    take: str = ""
    by: str = ""
    note: str = ""


@dataclass(frozen=True)
class Record:
    """One voice's two claims. Either may be absent."""

    structure: Structure | None = None
    music: Music | None = None


def _provenance(raw: dict) -> Provenance:
    return Provenance(
        date=str(raw.get("date", "")).strip(),
        bank_generation=int(raw.get("bank_generation", 0) or 0),
        patch_version=int(raw.get("patch_version", 0) or 0),
    )


def load(path: Path | None = None) -> dict[str, Record]:
    """The recorded claims, by voice slug. An absent file is an empty one."""
    path = DEFAULT_PATH if path is None else path
    if not path.exists():
        return {}
    raw = json.loads(path.read_text())
    table: dict[str, Record] = {}
    for slug, entry in raw.items():
        if slug.startswith(DOC_PREFIX):
            continue
        structure = None
        if entry.get("structure"):
            s = entry["structure"]
            accepted = {str(k): str(v).strip() for k, v in (s.get("accepted") or {}).items()}
            blank = sorted(k for k, v in accepted.items() if not v)
            if blank:
                raise ValueError(
                    f"{path.name}: {slug}: accepted term(s) {', '.join(blank)} carry no "
                    f"reason; a term with no reason is open, not accepted"
                )
            unreachable = tuple(str(t) for t in (s.get("unreachable") or []))
            stray = sorted(set(accepted) - set(unreachable))
            if stray:
                raise ValueError(
                    f"{path.name}: {slug}: accepted term(s) {', '.join(stray)} are not in "
                    f"`unreachable`; the diagnosis did not report them"
                )
            structure = Structure(
                provenance=_provenance(s.get("provenance") or {}),
                spec=str(s.get("spec", "")).strip(),
                probe=str(s.get("probe", "")).strip(),
                unreachable=unreachable,
                accepted=accepted,
                note=str(s.get("note", "")).strip(),
            )
        music = None
        if entry.get("music"):
            m = entry["music"]
            music = Music(
                provenance=_provenance(m.get("provenance") or {}),
                take=str(m.get("take", "")).strip(),
                by=str(m.get("by", "")).strip(),
                note=str(m.get("note", "")).strip(),
            )
        if structure or music:
            table[slug] = Record(structure=structure, music=music)
    return table


def unknown_voices(table: dict[str, Record], slugs: set[str]) -> list[str]:
    """Keys that match no voice in the bank.

    A typo is silent in exactly the wrong direction: the voice it was meant for
    goes on reporting its claims as unrecorded, which is what the file was
    written to stop.
    """
    return sorted(slug for slug in table if slug not in slugs)


def bank_versions(path: Path) -> tuple[int, dict[str, int]]:
    """The registry's generation and each unit's version.

    Read rather than derived: `tools/bank-versions.json` is itself generated
    from what the library reports it consulted, so this is the same number the
    bump rules are enforced against.
    """
    if not path.is_file():
        return 0, {}
    raw = json.loads(path.read_text())
    units = {name: int(u.get("version", 0) or 0) for name, u in (raw.get("units") or {}).items()}
    return int(raw.get("bank_generation", 0) or 0), units


def moved_generation(path: Path, kinds: set[str]) -> int:
    """The generation at which a unit of one of these kinds last moved.

    Read from the registry's own `kind`, which already separates the 17 engine
    and fallback-table units from the 169 patch and 128 drum ones. That split
    is what lets a record be held against the units it actually rests on: a
    voice with a patch of its own is dated by the shared kinds, since its own
    patch version covers the rest, and a kit -- whose voices are its drum
    notes and which therefore has no single patch unit -- by those and the
    drum kinds together.
    """
    if not path.is_file():
        return 0
    raw = json.loads(path.read_text())
    return max(
        (
            int(h.get("generation", 0) or 0)
            for u in (raw.get("units") or {}).values()
            if u.get("kind") in kinds
            for h in (u.get("history") or [])
        ),
        default=0,
    )


def axis(
    claim: Structure | Music | None, dating: int, patch_version: int, own_generation: int = 0
) -> dict | None:
    """One claim as `status.py` records it, or None where nothing is recorded.

    `dating` is the generation at which a shared unit last moved, and the pair
    after it says what the claim's own voice is versioned by: a patch version
    where it has one, a generation over the kinds standing in for it where it
    does not. See `Provenance.state`.
    """
    if claim is None:
        return None
    out: dict = {
        "state": claim.provenance.state(dating, patch_version, own_generation),
        "date": claim.provenance.date,
    }
    if isinstance(claim, Structure):
        out["unreachable"] = list(claim.unreachable)
        out["accepted"] = sorted(claim.accepted or {})
        out["open"] = claim.open_terms
    else:
        out["take"] = claim.take
    return out


def settled(structure: dict | None, music: dict | None) -> bool:
    """Whether the two claims together earn the last step.

    Both have to be current, and every term the diagnosis could not reach has
    to be accepted. A recorded diagnosis that still has an open term raises
    nothing by itself, which is the point of recording it: it turns "nobody has
    looked" into a named measurement with no mechanism behind it.
    """
    if not structure or not music:
        return False
    if structure["state"] != CURRENT or music["state"] != CURRENT:
        return False
    return not structure["open"]
