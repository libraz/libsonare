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
- Only the generation moved: some unit's values moved, and the dump cannot say
  whether that unit reaches this voice. Shared calibration constants are their
  own unit precisely because they cannot be attributed to the patches that use
  them, so the honest answer is `unverified` rather than either verdict.

Both block `settled`; they are named apart so the next action can say which one
happened. A kit has no single patch unit — its voices are its drum notes — so
only the generation applies to it, and it can never read better than the bank's
own generation says.
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

    def state(self, generation: int, patch_version: int) -> str:
        """`current`, `stale` or `unverified` against the registry as it stands."""
        if patch_version and self.patch_version and patch_version > self.patch_version:
            return STALE
        if generation and self.bank_generation and generation > self.bank_generation:
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
                    f"reason; a term with no reason is open, not accepted")
            unreachable = tuple(str(t) for t in (s.get("unreachable") or []))
            stray = sorted(set(accepted) - set(unreachable))
            if stray:
                raise ValueError(
                    f"{path.name}: {slug}: accepted term(s) {', '.join(stray)} are not in "
                    f"`unreachable`; the diagnosis did not report them")
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


def axis(claim: Structure | Music | None, generation: int, patch_version: int) -> dict | None:
    """One claim as `status.py` records it, or None where nothing is recorded."""
    if claim is None:
        return None
    out: dict = {
        "state": claim.provenance.state(generation, patch_version),
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
