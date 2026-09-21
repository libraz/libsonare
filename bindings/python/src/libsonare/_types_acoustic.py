"""Room-acoustic result shapes: decay measurements, impulse responses, geometry.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TypedDict


@dataclass(frozen=True, slots=True)
class AcousticResult:
    """Room acoustic parameters from a blind recording or a measured
    impulse response (``is_blind`` distinguishes the two).

    Only ``rt60`` (and ``rt60_bands``) is estimated in both modes.
    ``c50``/``c80``/``d50`` and ``edt`` require a known direct-sound arrival
    time, which only a measured impulse response provides, so they are NaN when
    ``is_blind`` is true -- ``edt`` measures the 0 to -10 dB decay and the blind
    estimator only fits the late decay ``rt60`` comes from. ``c50_bands`` and
    ``c80_bands`` are then empty lists (not computed), while ``edt_bands`` stays
    a full-length list of NaNs so it can be indexed by the same band index as
    ``rt60_bands``."""

    rt60: float
    edt: float
    c50: float
    c80: float
    d50: float
    rt60_bands: list[float]
    edt_bands: list[float]
    c50_bands: list[float]
    c80_bands: list[float]
    confidence: float
    is_blind: bool

    @property
    def rt60Bands(self) -> list[float]:  # noqa: N802
        return self.rt60_bands

    @property
    def edtBands(self) -> list[float]:  # noqa: N802
        return self.edt_bands

    @property
    def c50Bands(self) -> list[float]:  # noqa: N802
        return self.c50_bands

    @property
    def c80Bands(self) -> list[float]:  # noqa: N802
        return self.c80_bands

    @property
    def isBlind(self) -> bool:  # noqa: N802
        return self.is_blind


@dataclass(frozen=True, slots=True)
class RirDiagnostic:
    """One diagnostic reported by the RIR synthesizer.

    Args:
        code: Stable machine-readable id, e.g. ``acoustic.rir_length_clamped``.
        message: Human-readable detail.
        severity: ``"info"``, ``"warning"`` or ``"error"``.
    """

    code: str
    message: str
    severity: str


@dataclass(frozen=True, slots=True)
class RirResult:
    """Room impulse response synthesized from shoebox geometry.

    ``error_message`` contains the stable acoustic diagnostic code and detail
    when geometry validation makes the result unusable.

    ``diagnostics`` carries every diagnostic the synthesizer reported, in its own
    order. Warnings appear on SUCCESSFUL calls too and are otherwise invisible: a
    ``max_seconds`` clamp that cut the reverb tail
    (``acoustic.rir_length_clamped``), a ``max_seconds`` shorter than the direct
    sound's own arrival, which is raised to fit it
    (``acoustic.rir_length_floored``), or a request reduced from "early
    reflections + diffuse tail" to early reflections only
    (``acoustic.no_late_tail``). None of them sets ``has_error``, so a truncated
    RIR is indistinguishable from a complete one without reading this field.
    """

    rir: list[float]
    sample_rate: int
    has_error: bool
    error_message: str = ""
    diagnostics: list[RirDiagnostic] = field(default_factory=list)

    @property
    def sampleRate(self) -> int:  # noqa: N802
        return self.sample_rate

    @property
    def hasError(self) -> bool:  # noqa: N802
        return self.has_error


@dataclass(frozen=True, slots=True)
class RoomMorphResult:
    """Morphed audio and what the target-room synthesis had to change to make it.

    Shaped like :class:`RirResult` because the same synthesis runs underneath.
    There is no ``has_error`` / ``error_message`` counterpart: an unusable morph
    raises, so every entry in ``diagnostics`` is a warning. Each says the morph
    went through a room other than the one requested — an image-source order
    reduced to the safe maximum (``acoustic.ism_order_clamped``), a tail cut
    against ``max_seconds`` (``acoustic.rir_length_clamped``), a ``max_seconds``
    shorter than the direct sound's flight time and extended to fit it
    (``acoustic.rir_length_floored``), a request that produced no diffuse tail
    (``acoustic.no_late_tail``) — and is otherwise invisible.

    These are the four codes the synthesis can emit here, so a ``match`` over
    them needs no fall-through case. :func:`room_morph` forwards ``max_seconds``
    unchanged, which is why the floored one reaches a morph at all.
    """

    audio: list[float]
    sample_rate: int
    diagnostics: list[RirDiagnostic] = field(default_factory=list)

    @property
    def sampleRate(self) -> int:  # noqa: N802
        return self.sample_rate


@dataclass(frozen=True, slots=True)
class RoomEstimate:
    """Blind equivalent-room estimate (volume/dimensions/absorption/DRR)."""

    volume: float
    length: float
    width: float
    height: float
    drr_db: float
    confidence: float
    absorption_bands: list[float]
    rt60_bands: list[float]

    @property
    def drrDb(self) -> float:  # noqa: N802
        return self.drr_db

    @property
    def absorptionBands(self) -> list[float]:  # noqa: N802
        return self.absorption_bands

    @property
    def rt60Bands(self) -> list[float]:  # noqa: N802
        return self.rt60_bands


class DereverbClassicalConfig(TypedDict):
    """A complete classical-dereverberator configuration.

    The keys are exactly the keyword arguments of
    :func:`libsonare.mastering_repair_dereverb_classical`, so a value of this
    type is splatted straight into it::

        config = mastering_repair_dereverb_config_for_room(estimate)
        clean = mastering_repair_dereverb_classical(samples, sr, **config)

    Every key is required: this type describes a config that is ready to run,
    never a partial override set.
    """

    threshold: float
    attenuation: float
    n_fft: int
    hop_length: int
    t60_sec: float
    late_delay_ms: float
    over_subtraction: float
    spectral_floor: float
    wpe_enabled: bool
    wpe_iterations: int
    wpe_taps: int
    wpe_strength: float
