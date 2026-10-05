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

## A musical claim is checked against the recording it rests on

A block `heard.py --signoff` writes carries `evidence`: the comparison, the
request, asset and build ids, the bank registry digest the library embedded,
the unit versions under that registry and the comparison boundary.
`claim_eligibility` is the one check of it, run when the block is written and
again whenever it is read. A block without `evidence` was dated by hand and is
`unverified` unless its provenance already says `stale`. Everything here up to
`claim_eligibility` imports only the standard library, so `heard.py` can share
it; re-deriving the boundary imports `boundary`, and does so lazily.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_PATH = HERE / "signoff.json"
REPO_ROOT = HERE.parents[1]
BANK_VERSIONS = REPO_ROOT / "tools" / "bank-versions.json"

#: `boundary.SCOPE_*` and the matched status, repeated because `boundary` needs
#: numpy and `heard.py` does not; `test_signoff.py` holds the two in step.
SCOPE_INSTRUMENT = "instrument"
SCOPE_PRODUCT = "product"
STATUS_MATCHED = "matched"

#: The source each scope's default playback is rendered as; every other model
#: source on a page is a candidate carrying overrides.
SHIPPED_SOURCE = {SCOPE_PRODUCT: "model", SCOPE_INSTRUMENT: "model-di"}

#: What a written claim's `evidence` must carry before anything else is checked.
EVIDENCE_FIELDS = (
    "comparison_id",
    "scope",
    "set",
    "set_generation",
    "take",
    "judged",
    "program",
    "request_id",
    "asset_id",
    "build_id",
    "bank_registry_digest",
    "units",
    "counterpart",
    "boundary",
)

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
    """A take somebody listened to and signed; `evidence` is None on a hand-dated block."""

    provenance: Provenance
    take: str = ""
    evidence: dict | None = None


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
            evidence = m.get("evidence")
            music = Music(
                provenance=_provenance(m.get("provenance") or {}),
                take=str(m.get("take", "")).strip(),
                evidence=evidence if isinstance(evidence, dict) else None,
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
    claim: Structure | Music | None,
    dating: int,
    patch_version: int,
    own_generation: int = 0,
    registry: Path | None = None,
) -> dict | None:
    """One claim as `status.py` records it, or None where nothing is recorded.

    `dating` is the generation at which a shared unit last moved, and the pair
    after it says what the claim's own voice is versioned by: a patch version
    where it has one, a generation over the kinds standing in for it where it
    does not. See `Provenance.state`. A musical claim is then held to its
    recording by `music_state`; `registry` is the bank registry it is read against.
    """
    if claim is None:
        return None
    state = claim.provenance.state(dating, patch_version, own_generation)
    reasons: list[str] = []
    if isinstance(claim, Music):
        state, reasons = music_state(claim, state, registry or BANK_VERSIONS)
    out: dict = {"state": state, "date": claim.provenance.date}
    if isinstance(claim, Structure):
        out["unreachable"] = list(claim.unreachable)
        out["accepted"] = sorted(claim.accepted or {})
        out["open"] = claim.open_terms
    else:
        out["take"] = claim.take
    if reasons:
        out["reasons"] = reasons
    return out


def music_state(claim: Music, dated: str, registry: Path) -> tuple[str, list[str]]:
    """A musical claim's state: `claim_eligibility` where it carries evidence.

    A hand-dated block has nothing to check but its date: the date's own
    verdict stands where it is not `current`, and `current` becomes `unverified`.
    """
    if claim.evidence is None:
        if dated != CURRENT:
            return dated, []
        return UNVERIFIED, [
            "hand-dated claim without recording evidence: re-sign it with heard.py --signoff"
        ]
    return claim_eligibility(
        claim.evidence, claim.provenance, scope=SCOPE_PRODUCT, registry=registry
    )


def registry_digest(path: Path) -> str | None:
    """SHA-256 of the registry's raw bytes -- the digest a library embeds -- or None."""
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return None


#: Registries found by digest in this process, and the paths whose history was read.
_REGISTRIES: dict[str, dict] = {}
_SCANNED: set[str] = set()


def registry_at(digest: str, path: Path) -> dict | None:
    """The registry a library was built against, found by its digest.

    The file as it stands answers first; otherwise every committed revision of
    it is hashed. A registry that was never committed cannot be found, which is
    None -- unknown, not changed.
    """
    try:
        raw = path.read_bytes()
    except OSError:
        return None
    if hashlib.sha256(raw).hexdigest() == digest:
        return json.loads(raw)
    try:
        rel = path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return None
    if rel not in _SCANNED:
        _SCANNED.add(rel)
        git = ["git", "-C", str(REPO_ROOT)]
        try:
            revs = subprocess.run(
                [*git, "log", "--format=%H", "--", rel], capture_output=True, check=True, text=True
            ).stdout.split()
        except (OSError, subprocess.CalledProcessError):
            revs = []
        for rev in revs:
            try:
                blob = subprocess.run(
                    [*git, "show", f"{rev}:{rel}"], capture_output=True, check=True
                ).stdout
                _REGISTRIES.setdefault(hashlib.sha256(blob).hexdigest(), json.loads(blob))
            except (OSError, subprocess.CalledProcessError, ValueError):
                continue
    return _REGISTRIES.get(digest)


def _versions(raw: dict) -> dict[str, int]:
    return {
        name: int(u.get("version", 0) or 0)
        for name, u in (raw.get("units") or {}).items()
        if isinstance(u, dict)
    }


def _shared_moved(raw: dict) -> int:
    """The generation at which a `shared` unit last moved; see `moved_generation`."""
    return max(
        (
            int(h.get("generation", 0) or 0)
            for u in (raw.get("units") or {}).values()
            if isinstance(u, dict) and u.get("kind") == "shared"
            for h in (u.get("history") or [])
            if isinstance(h, dict)
        ),
        default=0,
    )


def _worst_state(states: list[str]) -> str:
    for state in (STALE, UNVERIFIED):
        if state in states:
            return state
    return CURRENT


def bank_state(
    digest: str | None,
    units: list[str],
    registry: Path,
    *,
    then_versions: dict[str, int] | None = None,
    then_generation: int | None = None,
) -> tuple[str, list[str], dict]:
    """Whether a recording's bank still stands: `(state, reasons, then)`.

    `digest` is the registry digest the rendering library embedded. Equal to
    the current registry's, the recording is `current`. Otherwise the registry
    it names -- `then_versions`/`then_generation` where a claim recorded them,
    else found by `registry_at` -- is compared unit by unit: one of `units`
    moving is `stale`, and a shared unit moving after it is `unverified`, the
    policy `Provenance.state` applies. `then` is that registry's generation and
    the versions of `units` in it.
    """
    if not digest:
        return UNVERIFIED, ["the library embedded no bank registry digest"], {}
    try:
        now = json.loads(registry.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return UNVERIFIED, ["no bank registry to compare against"], {}
    if then_versions is None:
        found = registry_at(digest, registry)
        if found is None:
            return (
                UNVERIFIED,
                [f"bank registry {digest[:12]} is neither the current one nor a committed one"],
                {},
            )
        then_versions = _versions(found)
        then_generation = int(found.get("bank_generation", 0) or 0)
    then = {
        "bank_generation": int(then_generation or 0),
        "units": {u: then_versions[u] for u in units if u in then_versions},
    }
    if digest == registry_digest(registry):
        return CURRENT, [], then
    now_versions = _versions(now)
    states, reasons = [], []
    if not units:
        states.append(UNVERIFIED)
        reasons.append("the registry moved and no versioned unit is attributable to this voice")
    for unit in units:
        before, after = then_versions.get(unit), now_versions.get(unit)
        if before is None or after is None:
            states.append(UNVERIFIED)
            reasons.append(f"{unit} is not in both registries")
        elif before != after:
            states.append(STALE)
            reasons.append(f"{unit} moved from v{before} to v{after} since the recording")
    shared = _shared_moved(now)
    if shared > then["bank_generation"]:
        states.append(UNVERIFIED)
        reasons.append(
            f"a shared unit moved at generation {shared}, after the recording's "
            f"{then['bank_generation']}"
        )
    return _worst_state(states), reasons, then


def reference_raw(capture_id: str) -> dict | None:
    """A capture's definition, overlay folded in, or None when this tree has none."""
    import bank

    for cap in bank.captures():
        if cap.id == capture_id:
            return cap.raw
    return None


def capture_of_timbre(timbre: str, prefer: str = "") -> str | None:
    """The capture a timbre id belongs to, `prefer` first; None when no capture has it."""
    import bank

    found = [c.id for c in bank.captures() if timbre in {t.get("id") for t in c.timbres}]
    if prefer in found:
        return prefer
    return found[0] if found else None


def assess_boundary(scope: str, program: int, raw: dict | None, product_rig: bool | None):
    """`boundary.assess` for a default-playback request of this scope: `(reference, assessment)`.

    The manifest records request ids, not the sends a page rendered with, so the
    request here takes the sends the scope asks for and the page's own recorded
    status is what witnesses the sends (see `claim_eligibility`).
    """
    import boundary

    ref = boundary.Reference.from_capture(raw) if raw is not None else None
    if ref is not None:
        sends = boundary.expected_sends(scope, ref.room)
    else:
        sends = boundary.SENDS_DRY if scope == SCOPE_INSTRUMENT else boundary.SENDS_POWER_ON
    request = boundary.RenderRequest(
        program=program, seconds=0.0, rig=scope == SCOPE_PRODUCT, sends=sends
    )
    return ref, boundary.assess(ref, scope, request, product_rig=product_rig)


def boundary_record(
    capture: str,
    raw: dict | None,
    scope: str,
    program: int,
    product_rig: bool | None,
    page_status: str,
    page_may_sign_off: bool,
) -> dict:
    """The `boundary` a claim records, from the same derivation `claim_eligibility` repeats."""
    ref, verdict = assess_boundary(scope, program, raw, product_rig)
    return {
        "capture": capture,
        "rig": ref.rig if ref else None,
        "room": ref.room if ref else None,
        "rig_evidence": verdict.evidence,
        "product_rig": product_rig,
        "status": verdict.status,
        "may_sign_off": verdict.may_sign_off,
        "page_status": page_status,
        "page_may_sign_off": bool(page_may_sign_off),
        "reasons": list(verdict.reasons),
    }


def claim_eligibility(
    evidence: dict, provenance: Provenance, *, scope: str, registry: Path | None = None
) -> tuple[str, list[str]]:
    """Whether a written claim may stand for the voice: `(state, reasons)`.

    The one check of a claim, run by `heard.py --signoff` before it prints one
    and by `axis` whenever one is read. A claim stands when it is about the
    scope's default playback with no overrides, its bank is `current` by
    `bank_state`, its reference's boundary classes are the ones recorded, the
    page found the comparison matched, and `boundary.assess` re-derived now
    still allows a sign-off. A moved unit or reference is `stale`; anything
    that cannot be established is `unverified`.
    """
    registry = registry or BANK_VERSIONS
    missing = [k for k in EVIDENCE_FIELDS if evidence.get(k) in (None, "", [], {})]
    if missing:
        return UNVERIFIED, [f"no recording evidence: {', '.join(missing)} not recorded"]
    if evidence["scope"] != scope:
        return UNVERIFIED, [f"a {evidence['scope']} judgement cannot stand as a {scope} claim"]
    if evidence["judged"] != SHIPPED_SOURCE.get(scope):
        return UNVERIFIED, [
            f"judged {evidence['judged']}, not the default playback ({SHIPPED_SOURCE.get(scope)})"
        ]
    units = evidence["units"] if isinstance(evidence["units"], dict) else {}
    states, reasons = [], []
    state, why, _ = bank_state(
        evidence["bank_registry_digest"],
        sorted(units),
        registry,
        then_versions={str(k): int(v) for k, v in units.items()},
        then_generation=provenance.bank_generation,
    )
    states.append(state)
    reasons += why

    recorded = evidence["boundary"] if isinstance(evidence["boundary"], dict) else {}
    capture = str((evidence["counterpart"] or {}).get("capture") or recorded.get("capture") or "")
    raw = reference_raw(capture) if capture else None
    if raw is None:
        return UNVERIFIED, [
            *reasons,
            f"reference capture {capture or '(none)'} is not in this tree",
        ]
    ref, verdict = assess_boundary(
        scope, int(evidence["program"]), raw, recorded.get("product_rig")
    )
    moved = [
        f"{name} {recorded.get(name)} -> {now}"
        for name, now in (("rig", ref.rig), ("room", ref.room), ("rig_evidence", verdict.evidence))
        if recorded.get(name) != now
    ]
    if moved:
        states.append(STALE)
        reasons.append(f"the reference's boundary moved: {', '.join(moved)}")
    if recorded.get("page_status") != STATUS_MATCHED or not recorded.get("page_may_sign_off"):
        states.append(UNVERIFIED)
        reasons.append(
            f"the page found this comparison {recorded.get('page_status') or 'unrecorded'}"
            + ("" if recorded.get("page_may_sign_off") else ", not signable")
        )
    if not (verdict.status == STATUS_MATCHED and verdict.may_sign_off):
        states.append(UNVERIFIED)
        reasons.append(f"comparison {verdict.status}: {verdict.reason}")
    return _worst_state(states), reasons


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
