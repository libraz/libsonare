"""The comparison boundary: which side of a rig a reference and a render stand on.

A capture stores inputs only — its `rig` and `room` classes and, where the rig
class was argued, the `rig_evidence` behind it. A comparison stores its `scope`
(`instrument` or `product`) and the `RenderRequest` the model side was rendered
from. Everything that follows from those — the boundary, whether the two sides
are comparable, and whether the result may drive a fit or a sign-off — is
derived here by `assess` and never written into a manifest, so a corrected
input changes every reading at once.

Evidence is asked only of a family where a rig is possible (`capture.rig_capable`);
anywhere else it is `not_applicable`, which keeps the rule `corpus.check_rig`
already applies. `canonical_digest` is the one hash every request, source,
build and asset identity is taken with.
"""

from __future__ import annotations

import dataclasses
import enum
import hashlib
import json
import math
from collections.abc import Mapping
from dataclasses import dataclass

from capture import (
    RIG_BAKED,
    RIG_NONE,
    RIG_UNCLASSIFIED,
    RIG_VALUES,
    ROOM_NONE,
    ROOM_PRESENT,
    ROOM_UNCLASSIFIED,
    ROOM_VALUES,
    rig_capable,
)

#: What a capture's `rig_evidence` may answer, strongest first. `verified` is a
#: measured A/B with the rig bypassed, `declared` a product or manual statement,
#: `inferred` an argument from spectral shape. Absent is `unknown`.
EVIDENCE_VERIFIED = "verified"
EVIDENCE_DECLARED = "declared"
EVIDENCE_INFERRED = "inferred"
EVIDENCE_UNKNOWN = "unknown"
EVIDENCE_VALUES = (EVIDENCE_VERIFIED, EVIDENCE_DECLARED, EVIDENCE_INFERRED, EVIDENCE_UNKNOWN)
#: Reported for a family where no rig is possible, so no evidence is owed.
EVIDENCE_NOT_APPLICABLE = "not_applicable"
#: The evidence a rig class must rest on before a fit or a sign-off may use it.
_GROUNDED = frozenset({EVIDENCE_VERIFIED, EVIDENCE_DECLARED, EVIDENCE_INFERRED})

SCOPE_INSTRUMENT = "instrument"
SCOPE_PRODUCT = "product"
SCOPE_VALUES = (SCOPE_INSTRUMENT, SCOPE_PRODUCT)

BOUNDARY_INSTRUMENT = "instrument"
BOUNDARY_RIGGED = "rigged"
BOUNDARY_UNKNOWN = "unknown"

STATUS_MATCHED = "matched"
STATUS_CONTEXT_ONLY = "context_only"
STATUS_UNVERIFIED = "unverified"
STATUS_UNAVAILABLE = "unavailable"

#: CC91/93/94 left at the GS power-on values: the default playback.
SENDS_POWER_ON: tuple[None, None, None] = (None, None, None)
#: CC91/93/94 written to zero: the system effects off, as a dry reference was taken.
SENDS_DRY = (0, 0, 0)


def canonical_digest(obj: object) -> str:
    """SHA-256 over the canonical JSON of `obj`.

    Keys are sorted, separators fixed, floats written in their shortest
    round-trip form, enums as their value, dataclasses as their fields, and
    bytes as the hex SHA-256 of their content. A type this cannot place is
    refused rather than stringified, because a digest that silently depends on
    an object's repr is not an identity.
    """
    text = json.dumps(
        _canonical(obj), sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False
    )
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _canonical(obj: object) -> object:
    if obj is None or isinstance(obj, (bool, int, str)):
        return obj
    if isinstance(obj, enum.Enum):
        return _canonical(obj.value)
    if isinstance(obj, float):
        if not math.isfinite(obj):
            raise ValueError(f"a non-finite float has no canonical form: {obj!r}")
        return obj
    if isinstance(obj, (bytes, bytearray)):
        return hashlib.sha256(bytes(obj)).hexdigest()
    if dataclasses.is_dataclass(obj) and not isinstance(obj, type):
        return {f.name: _canonical(getattr(obj, f.name)) for f in dataclasses.fields(obj)}
    if isinstance(obj, Mapping):
        if not all(isinstance(k, str) for k in obj):
            raise TypeError("canonical JSON needs string keys")
        return {k: _canonical(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_canonical(v) for v in obj]
    raise TypeError(f"no canonical form for {type(obj).__name__}")


@dataclass(frozen=True)
class RenderRequest:
    """Everything that decides what one model render sounds like.

    Notes, velocities, gate, tail, controllers and preroll reach the render only
    through the SMF, so the bytes actually written are their identity. `sends`
    duplicates CC91/93/94 from those bytes because the boundary reads it.
    """

    # source
    program: int
    seconds: float
    smf: bytes = b""
    bank: int = 0
    #: Zero-based MIDI channel (9 = the GM drum channel).
    channel: int = 0
    #: A catalogue entry bound as the patch; "" renders through the GM bank.
    preset: str = ""
    #: True keeps the bank's default rig (the product path); False clears it.
    rig: bool = True
    #: `SONARE_TUNING_OVERRIDES` text; "" is the library as built.
    overrides: str = ""
    #: CC91/93/94 written at tick 0; None leaves the GS power-on value.
    sends: tuple[int | None, int | None, int | None] = SENDS_POWER_ON
    #: `--allow-rigged-oracle`: fit against a reference whose rig class forbids it.
    allow_rigged_oracle: bool = False
    # stimulus
    sample_rate: int = 48000
    # reference
    capture: str = ""
    timbre: str = ""
    #: (sounding note, key) pairs, applied to the reference side only.
    key_map: tuple[tuple[int, int], ...] = ()
    keyswitch: int = 0

    def __post_init__(self) -> None:
        # Fixed types, so `1` and `1.0` or a list and a tuple cannot hash apart.
        set_ = object.__setattr__
        set_(self, "program", int(self.program))
        set_(self, "seconds", float(self.seconds))
        set_(self, "smf", bytes(self.smf))
        set_(self, "bank", int(self.bank))
        set_(self, "channel", int(self.channel))
        set_(self, "sample_rate", int(self.sample_rate))
        set_(self, "keyswitch", int(self.keyswitch))
        if len(self.sends) != 3:
            raise ValueError(f"sends is (reverb, chorus, delay), not {self.sends!r}")
        set_(self, "sends", tuple(None if s is None else int(s) for s in self.sends))
        set_(self, "key_map", tuple(sorted((int(n), int(k)) for n, k in self.key_map)))

    @property
    def frames(self) -> int:
        """The render length in frames, rounded the way `render_model` rounds it."""
        return round(self.seconds * self.sample_rate)

    @property
    def sends_override(self) -> bool:
        """Whether the sends are neither the power-on values nor the dry comparison."""
        return self.sends not in (SENDS_POWER_ON, SENDS_DRY)

    def fingerprint(self) -> str:
        """The request identity: `canonical_digest` over every field."""
        return canonical_digest(self)


@dataclass(frozen=True)
class Reference:
    """The boundary classes a capture answered, read from its raw definition."""

    capture: str
    rig: str = RIG_UNCLASSIFIED
    room: str = ROOM_UNCLASSIFIED
    rig_evidence: str = EVIDENCE_UNKNOWN

    @classmethod
    def from_capture(cls, raw: Mapping) -> Reference:
        """Absent and unrecognised values all read as the unanswered class."""
        rig = raw.get("rig")
        room = raw.get("room")
        return cls(
            capture=str(raw.get("id", "")),
            rig=rig if rig in RIG_VALUES else RIG_UNCLASSIFIED,
            room=room if room in ROOM_VALUES else ROOM_UNCLASSIFIED,
            rig_evidence=rig_evidence(raw),
        )


def rig_evidence(raw: Mapping) -> str:
    """A capture's `rig_evidence`; absent or unrecognised is `unknown`."""
    value = raw.get("rig_evidence")
    return value if value in EVIDENCE_VALUES else EVIDENCE_UNKNOWN


@dataclass(frozen=True)
class Assessment:
    """What a reference and a render may be used for together. Derived, never stored."""

    scope: str
    boundary: str
    status: str
    evidence: str
    may_fit: bool
    may_sign_off: bool
    reasons: tuple[str, ...]

    @property
    def reason(self) -> str:
        return self.reasons[0]

    @property
    def may_adopt(self) -> bool:
        """Whether a fit's values may be written back: a fit on a matched comparison only."""
        return self.may_fit and self.status == STATUS_MATCHED


#: (rig class, evidence grounded) -> boundary, for a rig-capable family.
_BOUNDARY = {
    (RIG_NONE, True): BOUNDARY_INSTRUMENT,
    (RIG_BAKED, True): BOUNDARY_RIGGED,
    (RIG_NONE, False): BOUNDARY_UNKNOWN,
    (RIG_BAKED, False): BOUNDARY_UNKNOWN,
    (RIG_UNCLASSIFIED, True): BOUNDARY_UNKNOWN,
    (RIG_UNCLASSIFIED, False): BOUNDARY_UNKNOWN,
}

#: (boundary, scope) -> (status, may_fit, may_sign_off, reason), before the
#: request is checked against it.
_BASE = {
    (BOUNDARY_INSTRUMENT, SCOPE_INSTRUMENT): (
        STATUS_MATCHED,
        True,
        True,
        "reference at the instrument's boundary, compared with the instrument",
    ),
    (BOUNDARY_INSTRUMENT, SCOPE_PRODUCT): (
        STATUS_MATCHED,
        False,
        True,
        "reference without a rig, compared with a product path without one",
    ),
    (BOUNDARY_RIGGED, SCOPE_INSTRUMENT): (
        STATUS_CONTEXT_ONLY,
        False,
        False,
        "the reference carries a rig: an acceptance target, never a fit target",
    ),
    (BOUNDARY_RIGGED, SCOPE_PRODUCT): (
        STATUS_MATCHED,
        False,
        True,
        (
            "rigged reference against the product path; the difference is not split "
            "between voice and amplifier"
        ),
    ),
    (BOUNDARY_UNKNOWN, SCOPE_INSTRUMENT): (
        STATUS_CONTEXT_ONLY,
        False,
        False,
        "the reference's rig is unclassified or rests on no evidence",
    ),
    (BOUNDARY_UNKNOWN, SCOPE_PRODUCT): (
        STATUS_CONTEXT_ONLY,
        False,
        False,
        "the reference's rig is unclassified or rests on no evidence",
    ),
}

#: Which sends each reference room is compared with in the product scope. An
#: unclassified room is compared dry, the way a dry capture was taken.
_PRODUCT_SENDS = {
    ROOM_PRESENT: SENDS_POWER_ON,
    ROOM_NONE: SENDS_DRY,
    ROOM_UNCLASSIFIED: SENDS_DRY,
}


@dataclass(frozen=True)
class _Facts:
    scope: str
    capable: bool
    boundary: str
    room: str
    request: RenderRequest
    product_rig: bool | None


#: Request checks, in order: (applies, reason). Each one that applies demotes a
#: matched comparison to context_only, which removes fit and sign-off with it.
_MISMATCHES = (
    (
        lambda f: f.scope == SCOPE_INSTRUMENT and f.capable and f.request.rig,
        "the model keeps the bank rig while the reference stops at the instrument",
    ),
    (
        lambda f: f.scope == SCOPE_INSTRUMENT and f.request.sends != SENDS_DRY,
        "the instrument comparison renders with the system sends at zero",
    ),
    (
        lambda f: f.scope == SCOPE_PRODUCT and not f.request.rig,
        "the model's bank rig is cleared, so this is not the product path",
    ),
    (
        lambda f: (
            f.scope == SCOPE_PRODUCT
            and f.capable
            and f.boundary == BOUNDARY_INSTRUMENT
            and f.product_rig is not False
        ),
        (
            "a direct reference against a product path that carries, or may carry, a rig "
            "is diagnostic, not a ranking"
        ),
    ),
    (
        lambda f: f.scope == SCOPE_PRODUCT and f.request.sends != _PRODUCT_SENDS[f.room],
        "the model's system sends do not match the reference's room",
    ),
)

#: Request checks that leave the comparison standing but withhold sign-off.
_UNSIGNABLE = (
    (
        lambda f: bool(f.request.overrides),
        "tuning overrides: a candidate, not the shipped voice",
    ),
)


def assess(
    reference: Reference | None,
    scope: str,
    request: RenderRequest,
    *,
    product_rig: bool | None = None,
) -> Assessment:
    """Derive what one comparison may be used for.

    `product_rig` says whether the voice's default playback carries a bank rig
    (None: not established). It matters only in the product scope against a
    reference at the instrument's boundary. A missing reference makes fidelity
    unavailable; a musical judgement without one is outside this function.
    """
    if scope not in SCOPE_VALUES:
        raise ValueError(f"scope is one of {', '.join(SCOPE_VALUES)}, not {scope!r}")
    capable = rig_capable(request.program)
    if reference is None:
        return Assessment(
            scope,
            BOUNDARY_UNKNOWN,
            STATUS_UNAVAILABLE,
            EVIDENCE_NOT_APPLICABLE if not capable else EVIDENCE_UNKNOWN,
            False,
            False,
            ("no reference: fidelity is unavailable; a musical judgement is not assessed here",),
        )

    if capable:
        evidence = reference.rig_evidence
        boundary = _BOUNDARY[(reference.rig, evidence in _GROUNDED)]
    else:
        evidence = EVIDENCE_NOT_APPLICABLE
        boundary = BOUNDARY_RIGGED if reference.rig == RIG_BAKED else BOUNDARY_INSTRUMENT
    status, may_fit, may_sign_off, base_reason = _BASE[(boundary, scope)]
    if scope == SCOPE_PRODUCT:
        may_fit = False
    facts = _Facts(scope, capable, boundary, reference.room, request, product_rig)

    mismatches = [r for applies, r in _MISMATCHES if applies(facts)]
    unsignable = [r for applies, r in _UNSIGNABLE if applies(facts)]
    # The decisive reason comes first: the reference's own, unless only the request spoilt it.
    if status == STATUS_MATCHED and mismatches:
        status, may_fit, may_sign_off = STATUS_CONTEXT_ONLY, False, False
        reasons = [*mismatches, base_reason]
    else:
        reasons = [base_reason, *mismatches]
    if unsignable:
        may_sign_off = False
        reasons += unsignable
    if request.allow_rigged_oracle:
        status = STATUS_UNVERIFIED
        may_fit = scope == SCOPE_INSTRUMENT
        may_sign_off = False
        reasons.insert(0, "--allow-rigged-oracle: usable for a fit, never for adoption or sign-off")
    return Assessment(scope, boundary, status, evidence, may_fit, may_sign_off, tuple(reasons))
