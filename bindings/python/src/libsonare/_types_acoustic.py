"""Room-acoustic result shapes: decay measurements, impulse responses, geometry.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Literal, NotRequired, TypedDict

if TYPE_CHECKING:
    import numpy as np

MaterialPresetName = Literal["none", "concrete", "wood", "curtain", "carpet", "glass"]
"""Names accepted for ``material_preset`` (the ``SONARE_MATERIAL_PRESET_*`` values)."""
AcousticModeName = Literal["auto", "blind", "impulseResponse"]
"""Names accepted for the room-estimate ``mode`` (the ``SONARE_ACOUSTIC_MODE_*`` values)."""


@dataclass(frozen=True, slots=True, eq=False)
class AcousticResult:
    """Room acoustic parameters from a blind recording or a measured
    impulse response (``is_blind`` distinguishes the two).

    Only ``rt60`` (and ``rt60_bands``) is estimated in both modes.
    ``c50``/``c80``/``d50`` and ``edt`` require a known direct-sound arrival
    time, which only a measured impulse response provides, so they are ``None``
    when ``is_blind`` is true -- ``edt`` measures the 0 to -10 dB decay and the
    blind estimator only fits the late decay ``rt60`` comes from. ``edt_bands``,
    ``c50_bands`` and ``c80_bands`` are ``None`` then too."""

    rt60: float
    """Reverberation time (RT60) in seconds; estimated in both blind and impulse-response modes."""
    edt: float | None
    """Early decay time in seconds; None when ``is_blind`` is true."""
    c50: float | None
    """Clarity C50 in dB; None when ``is_blind`` is true."""
    c80: float | None
    """Clarity C80 in dB; None when ``is_blind`` is true."""
    d50: float | None
    """Definition D50, the early-energy fraction in [0, 1]; None when ``is_blind`` is true."""
    rt60_bands: np.ndarray
    """RT60 per octave band, in seconds."""
    edt_bands: np.ndarray | None
    """Early decay time per octave band, in seconds; None when ``is_blind`` is true."""
    c50_bands: np.ndarray | None
    """Clarity C50 per octave band, in dB; None when ``is_blind`` is true."""
    c80_bands: np.ndarray | None
    """Clarity C80 per octave band, in dB; None when ``is_blind`` is true."""
    confidence: float
    """Confidence of the estimate, in [0, 1]."""
    is_blind: bool
    """True when estimated from a recording rather than a measured impulse response."""


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


@dataclass(frozen=True, slots=True, eq=False)
class RirResult:
    """Room impulse response synthesized from shoebox geometry.

    ``diagnostics`` carries every diagnostic the synthesizer reported, in its own
    order. Warnings appear on SUCCESSFUL calls too and are otherwise invisible: a
    ``max_sec`` clamp that cut the reverb tail
    (``acoustic.rir_length_clamped``), a ``max_sec`` shorter than the direct
    sound's own arrival, which is raised to fit it
    (``acoustic.rir_length_floored``), a ``max_sec`` shorter than the
    longest band's RT60, which cuts that band before it decays by 60 dB so its
    reverberation time cannot be measured from the RIR
    (``acoustic.rir_tail_truncated``), or a request reduced from "early
    reflections + diffuse tail" to early reflections only
    (``acoustic.no_late_tail``). A refused synthesis raises ``SonareError``
    instead, so every entry is a warning, and a truncated RIR is
    indistinguishable from a complete one without reading this field.
    """

    rir: np.ndarray
    """Synthesized impulse response samples."""
    sample_rate: int
    """Sample rate of ``rir`` in Hz."""
    diagnostics: list[RirDiagnostic] = field(default_factory=list)
    """Warnings the synthesizer reported, in its own order; see the class docstring."""


@dataclass(frozen=True, slots=True, eq=False)
class RoomMorphResult:
    """Morphed audio and what the target-room synthesis had to change to make it.

    Shaped like :class:`RirResult` because the same synthesis runs underneath.
    An unusable morph raises, so every entry in ``diagnostics`` is a warning. Each says the morph
    went through a room other than the one requested — an image-source order
    reduced to the safe maximum (``acoustic.ism_order_clamped``), a tail cut
    against ``max_sec`` (``acoustic.rir_length_clamped``), a ``max_sec``
    shorter than the direct sound's flight time and extended to fit it
    (``acoustic.rir_length_floored``), a ``max_sec`` shorter than the
    longest band's RT60, which cuts that band before it decays by 60 dB
    (``acoustic.rir_tail_truncated``), a request that produced no diffuse tail
    (``acoustic.no_late_tail``) — and is otherwise invisible.

    These are the five codes the synthesis can emit here, so a ``match`` over
    them needs no fall-through case. :func:`room_morph` forwards ``max_sec``
    unchanged, which is why the floored one reaches a morph at all.
    """

    audio: np.ndarray
    """Morphed audio samples."""
    sample_rate: int
    """Sample rate of ``audio`` in Hz."""
    diagnostics: list[RirDiagnostic] = field(default_factory=list)
    """Warnings the target-room synthesis reported, in its own order."""


@dataclass(frozen=True, slots=True, eq=False)
class RoomMorphStereoResult:
    """Morphed stereo audio and the target-room synthesis warnings.

    ``left`` and ``right`` have equal length. ``diagnostics`` carries the same
    codes as :class:`RoomMorphResult`.
    """

    left: np.ndarray
    """Left channel of the morphed audio."""
    right: np.ndarray
    """Right channel of the morphed audio."""
    sample_rate: int
    """Sample rate of both channels in Hz."""
    diagnostics: list[RirDiagnostic] = field(default_factory=list)
    """Warnings the target-room synthesis reported, in its own order."""


@dataclass(frozen=True, slots=True, eq=False)
class RoomEstimate:
    """Blind equivalent-room estimate (volume/dimensions/absorption/DRR).

    ``volume``, ``length_m``, ``width_m`` and ``height_m`` (the names
    :func:`synthesize_rir` takes) are NaN, with
    ``confidence`` 0, when the recording has no measurable broadband decay
    (silence, or an RT60 the analyzer could not fit). NaN is the acoustic
    family's "not measurable", as in the RT60 bands.
    """

    volume: float
    """Equivalent room volume in cubic metres; NaN when no broadband decay was measurable."""
    length_m: float
    """Estimated room length in metres; NaN when no broadband decay was measurable."""
    width_m: float
    """Estimated room width in metres; NaN when no broadband decay was measurable."""
    height_m: float
    """Estimated room height in metres; NaN when no broadband decay was measurable."""
    drr_db: float
    """Direct-to-reverberant ratio in dB."""
    confidence: float
    """Confidence of the estimate, in [0, 1]; 0 when nothing was measurable."""
    band_absorption: np.ndarray
    """Mean wall absorption per octave band; same length as ``rt60_bands``."""
    rt60_bands: np.ndarray
    """RT60 per octave band in seconds; NaN where a band's fit failed."""


class DereverbClassicalConfig(TypedDict):
    """A complete classical-dereverberator configuration.

    The keys are exactly the keyword arguments of
    :func:`libsonare.mastering_repair_dereverb_classical`, so a value of this
    type is splatted straight into it::

        config = mastering_repair_dereverb_config_for_room(estimate)
        clean = mastering_repair_dereverb_classical(samples, sr, **config).samples

    Every key is required: this type describes a config that is ready to run,
    never a partial override set.
    """

    threshold: float
    """Late-reverberation threshold relative to a bin's own power, in [0, 1]."""
    attenuation: float
    """Share of the computed suppression applied, from 0 (none) to 1 (all)."""
    n_fft: int
    """STFT size in samples; a power of two."""
    hop_length: int
    """Hop in samples, in (0, n_fft / 2]."""
    t60_sec: float
    """Assumed reverberation time in seconds, in (0, 10]."""
    late_delay_ms: float
    """Delay in milliseconds after which reverberation counts as late, from 0 to 500."""
    over_subtraction: float
    """Over-subtraction factor, from 0 to 16."""
    spectral_floor: float
    """Floor multiplier relative to the input power, in [0, 1]."""
    wpe_enabled: bool
    """Whether the WPE stage runs."""
    wpe_iterations: int
    """WPE iterations, from 1 to 16."""
    wpe_taps: int
    """WPE filter taps, from 1 to 32."""
    wpe_strength: float
    """WPE strength, in [0, 1]."""


class RoomGeometry(TypedDict):
    """Room geometry in the shape :func:`libsonare.synthesize_rir` takes.

    Splat it into the call: ``synthesize_rir(**geometry)``. ``absorption`` and
    ``band_absorption`` are present only when the estimate has finite bands; an
    absent key leaves that argument at its default.
    """

    length_m: float
    """Room length in metres."""
    width_m: float
    """Room width in metres."""
    height_m: float
    """Room height in metres."""
    source: tuple[float, float, float]
    """Source position (x, y, z) in metres."""
    listener: tuple[float, float, float]
    """Listener position (x, y, z) in metres."""
    absorption: NotRequired[float]
    """Uniform wall absorption in [0, 0.999]."""
    band_absorption: NotRequired[list[float]]
    """Per-octave-band wall absorption, each entry in [0, 1]."""
