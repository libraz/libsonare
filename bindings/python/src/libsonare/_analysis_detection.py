"""Analysis and inspection wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Any, Literal, cast

from ._ffi_types_core import (
    SonareKey,
    SonareKeyCandidate,
    SonareOnsetDetectConfig,
)
from ._runtime import (
    SonareValueError,
    _check,
    _get_lib,
    _guard_buffer,
    _mode_values,
    _out_float_array,
    _profile_value,
    _to_c_float_array,
    _to_c_int_array,
)
from .types import (
    AnalysisBeatObservations,
    AnalysisDynamics,
    AnalysisMelody,
    AnalysisResult,
    AnalysisRhythm,
    AnalysisTimbre,
    BpmHypothesis,
    Chord,
    Key,
    KeyCandidate,
    KeyProfile,
    MelodyPoint,
    Mode,
    PitchClass,
    Section,
    SectionType,
    TimeSignature,
)


@_guard_buffer("samples")
def detect_bpm(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> float:
    """Detect the BPM (tempo) of audio samples.

    One-shot wrapper for raw sample input. When you load from a file or call
    multiple analyses on the same signal, prefer :class:`libsonare.Audio` and
    :meth:`Audio.detect_bpm` to avoid re-copying samples across the FFI.

    Args:
        samples: Mono audio samples (1D, nominally ``[-1.0, 1.0]``). Accepts
            ``list[float]``, ``tuple[float, ...]``, ``array.array``, or a numpy
            1D array of dtype ``float32``.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        Detected BPM as a ``float``. For confidence, use :func:`analyze`.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_bpm = ctypes.c_float()
    rc = lib.sonare_detect_bpm(
        c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate), ctypes.byref(out_bpm)
    )
    _check(rc)
    return float(out_bpm.value)


@_guard_buffer("samples")
def detect_key(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 4096,
    hop_length: int = 512,
    use_hpss: bool = False,
    loudness_weighted: bool = False,
    high_pass_hz: float = 0.0,
    modes: Sequence[Mode | str] | str | None = None,
    profile: KeyProfile | str | None = None,
    genre_hint: str | None = None,
) -> Key:
    """Detect the musical key of audio samples.

    Args:
        samples: Mono audio samples (1D float). See :func:`detect_bpm` for
            accepted types.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT size used for chroma analysis.
        hop_length: Hop length used for chroma analysis.
        use_hpss: Use harmonic-percussive separation before chroma analysis.
        loudness_weighted: Weight chroma frames by RMS loudness.
        high_pass_hz: Optional high-pass cutoff before chroma analysis.
        profile: Optional key-profile family.
        genre_hint: Optional genre hint (``"auto"``, ``"edm"``, ``"pop"``,
            ``"classical"``, or ``"jazz"``).

    Returns:
        :class:`libsonare.Key` with ``root`` (:class:`PitchClass`), ``mode``
        (:class:`Mode`), and ``confidence`` (``float`` in ``[0, 1]``).

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    mode_values = _mode_values(modes)
    mode_array, mode_count = _to_c_int_array(mode_values) if mode_values else (None, 0)
    out_key = SonareKey()
    rc = lib.sonare_detect_key_with_extended_options(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(1 if use_hpss else 0),
        ctypes.c_int(1 if loudness_weighted else 0),
        ctypes.c_float(high_pass_hz),
        mode_array,
        ctypes.c_size_t(mode_count),
        ctypes.c_int32(_profile_value(profile)),
        genre_hint.encode("utf-8") if genre_hint else None,
        ctypes.byref(out_key),
    )
    _check(rc)
    return Key(
        root=PitchClass(out_key.root),
        mode=Mode(out_key.mode),
        confidence=float(out_key.confidence),
    )


@_guard_buffer("samples")
def detect_key_candidates(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 4096,
    hop_length: int = 512,
    use_hpss: bool = False,
    loudness_weighted: bool = False,
    high_pass_hz: float = 0.0,
    modes: Sequence[Mode | str] | str | None = None,
    profile: KeyProfile | str | None = None,
    genre_hint: str | None = None,
) -> list[KeyCandidate]:
    """Return ranked musical key candidates for ambiguous material."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    mode_values = _mode_values(modes)
    mode_array, mode_count = _to_c_int_array(mode_values) if mode_values else (None, 0)
    out_candidates = ctypes.POINTER(SonareKeyCandidate)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_detect_key_candidates_with_extended_options(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(1 if use_hpss else 0),
        ctypes.c_int(1 if loudness_weighted else 0),
        ctypes.c_float(high_pass_hz),
        mode_array,
        ctypes.c_size_t(mode_count),
        ctypes.c_int32(_profile_value(profile)),
        genre_hint.encode("utf-8") if genre_hint else None,
        ctypes.byref(out_candidates),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        return [
            KeyCandidate(
                key=Key(
                    root=PitchClass(out_candidates[i].key.root),
                    mode=Mode(out_candidates[i].key.mode),
                    confidence=float(out_candidates[i].key.confidence),
                ),
                correlation=float(out_candidates[i].correlation),
            )
            for i in range(out_count.value)
        ]
    finally:
        if out_candidates and out_count.value > 0:
            lib.sonare_free_key_candidates(out_candidates)


@_guard_buffer("samples")
def detect_beats(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Detect beat positions in audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of beat times in seconds.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out_times, out_count):
        rc = lib.sonare_detect_beats(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            ctypes.byref(out_times),
            ctypes.byref(out_count),
        )
        _check(rc)
        count = out_count.value
        return [float(out_times[i]) for i in range(count)]


@_guard_buffer("samples")
def detect_downbeats(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Detect downbeat positions in audio samples."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out_times, out_count):
        rc = lib.sonare_detect_downbeats(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            ctypes.byref(out_times),
            ctypes.byref(out_count),
        )
        _check(rc)
        count = out_count.value
        return [float(out_times[i]) for i in range(count)]


@_guard_buffer("samples")
def detect_onsets(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    n_fft: int = 2048,
    hop_length: int = 512,
    threshold: float = 0.0,
    pre_max: int = 1,
    post_max: int = 1,
    pre_avg: int = 3,
    post_avg: int = 4,
    delta: float = 0.06,
    wait: int = 1,
    backtrack: bool = False,
    backtrack_range: int = 10,
) -> list[float]:
    """Detect onset positions in audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of onset times in seconds.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out_times, out_count):
        config = SonareOnsetDetectConfig(
            n_fft=int(n_fft),
            hop_length=int(hop_length),
            threshold=float(threshold),
            pre_max=int(pre_max),
            post_max=int(post_max),
            pre_avg=int(pre_avg),
            post_avg=int(post_avg),
            delta=float(delta),
            wait=int(wait),
            backtrack=int(backtrack),
            backtrack_range=int(backtrack_range),
        )
        rc = lib.sonare_detect_onsets_ex(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            ctypes.byref(config),
            ctypes.byref(out_times),
            ctypes.byref(out_count),
        )
        _check(rc)
        count = out_count.value
        return [float(out_times[i]) for i in range(count)]


# camelCase → snake_case quality name table (mirrors detect_chords).
_QUALITY_NAMES: dict[int, str] = {
    0: "major",
    1: "minor",
    2: "diminished",
    3: "augmented",
    4: "dominant7",
    5: "major7",
    6: "minor7",
    7: "sus2",
    8: "sus4",
    9: "unknown",
    10: "add9",
    11: "minorAdd9",
    12: "dim7",
    13: "halfDim7",
    14: "major9",
    15: "dominant9",
    16: "sus2Add4",
}

# Derived from the table rather than written out, so renumbering the mirror
# cannot leave the fallback pointing at a different quality.
_UNKNOWN_QUALITY_ORDINAL: int = next(
    ordinal for ordinal, name in _QUALITY_NAMES.items() if name == "unknown"
)


def _section_type_from_ordinal(raw: Any) -> SectionType:
    """Convert a raw ``sections[].type`` ordinal into a :class:`SectionType`.

    The analyzer always writes this field, and its ordinals are a mirror of
    ``sonare::SectionType``. Both facts are load-bearing, so neither a missing
    field nor an ordinal outside the enum is treated as a value to substitute
    for: a silent fallback would turn a mirror that has drifted apart into
    plausible-looking analysis output. Raising names the ordinal instead.

    :raises SonareValueError: the field is absent or names no known section type.
    """
    if raw is None:
        raise SonareValueError("analysis result section is missing its type field")
    ordinal = int(raw)
    try:
        return SectionType(ordinal)
    except ValueError as exc:
        raise SonareValueError(f"unknown analysis section type ordinal: {ordinal}") from exc


def _parse_analysis_json(data: dict[str, Any]) -> AnalysisResult:
    """Parse the camelCase JSON dict returned by ``sonare_analyze_json`` into
    an :class:`AnalysisResult`.

    This is an internal helper shared by :func:`analyze` and
    :func:`analyze_with_progress`.
    """
    # Key
    key_d = data.get("key", {})
    key = Key(
        root=PitchClass(int(key_d.get("root", 0))),
        mode=Mode(int(key_d.get("mode", 0))),
        confidence=float(key_d.get("confidence", 0.0)),
    )

    # Time signature (top-level)
    ts_d = data.get("timeSignature", {})
    time_signature = TimeSignature(
        numerator=int(ts_d.get("numerator", 4)),
        denominator=int(ts_d.get("denominator", 4)),
        confidence=float(ts_d.get("confidence", 0.0)),
    )

    bpm_candidates: list[BpmHypothesis] = []
    for candidate in data.get("bpmCandidates", []):
        relation = str(candidate.get("relation", "other"))
        if relation not in {"primary", "half", "double", "other"}:
            relation = "other"
        bpm_candidates.append(
            BpmHypothesis(
                value=float(candidate.get("value", 0.0)),
                confidence=float(candidate.get("confidence", 0.0)),
                relation=cast(Literal["primary", "half", "double", "other"], relation),
            )
        )

    time_signature_candidates = [
        TimeSignature(
            numerator=int(candidate.get("numerator", 4)),
            denominator=int(candidate.get("denominator", 4)),
            confidence=float(candidate.get("confidence", 0.0)),
        )
        for candidate in data.get("timeSignatureCandidates", [])
    ]

    # Beats (list of {time, strength})
    beats_raw = data.get("beats", [])
    beat_times = [float(b.get("time", 0.0)) for b in beats_raw]
    beat_strengths = [float(b.get("strength", 0.0)) for b in beats_raw]

    # Downbeats are indices into ``beats``, not a parallel array.
    downbeat_indices = [int(i) for i in data.get("downbeatIndices", [])]
    downbeat_phase = int(data.get("downbeatPhase", 0))

    # Beat-level evidence the downbeat and meter pass scores. Each stream is
    # beat-indexed; an empty one means the analysis could not produce it rather
    # than that every beat scored zero.
    obs_d = data.get("beatObservations", {})
    beat_observations = (
        AnalysisBeatObservations(
            onset_strength=[float(v) for v in obs_d.get("onsetStrength", [])],
            low_frequency_energy=[float(v) for v in obs_d.get("lowFrequencyEnergy", [])],
            chord_change=[float(v) for v in obs_d.get("chordChange", [])],
        )
        if obs_d
        else None
    )

    # Chords
    chord_quality_str: dict[str, str] = {
        "major": "major",
        "minor": "minor",
        "diminished": "diminished",
        "augmented": "augmented",
        "dominant7": "dominant7",
        "major7": "major7",
        "minor7": "minor7",
        "sus2": "sus2",
        "sus4": "sus4",
        "unknown": "unknown",
        "add9": "add9",
        "minorAdd9": "minorAdd9",
        "dim7": "dim7",
        "halfDim7": "halfDim7",
        "major9": "major9",
        "dominant9": "dominant9",
        "sus2Add4": "sus2Add4",
    }
    chords: list[Chord] = []
    for c in data.get("chords", []):
        q_raw = c.get("quality", _UNKNOWN_QUALITY_ORDINAL)
        # quality field may be int (enum value) or str (name from JSON)
        if isinstance(q_raw, int):
            # "unknown" is a quality the analyzer genuinely reports, so it has an
            # ordinal of its own. An ordinal outside the table is something else
            # -- the mirror has drifted -- and folding the two together would
            # present that as a detection result.
            if q_raw not in _QUALITY_NAMES:
                raise SonareValueError(f"unknown analysis chord quality ordinal: {q_raw}")
            quality_str = _QUALITY_NAMES[q_raw]
        else:
            quality_str = chord_quality_str.get(str(q_raw), "unknown")
        chords.append(
            Chord(
                root=PitchClass(int(c.get("root", 0))),
                bass=PitchClass(int(c.get("bass", c.get("root", 0)))),
                quality=quality_str,
                start=float(c.get("start", 0.0)),
                end=float(c.get("end", 0.0)),
                confidence=float(c.get("confidence", 0.0)),
                canonical_name=str(c.get("name", "")),
            )
        )

    # Sections
    sections: list[Section] = []
    for s in data.get("sections", []):
        sections.append(
            Section(
                type=_section_type_from_ordinal(s.get("type")),
                start=float(s.get("start", 0.0)),
                end=float(s.get("end", 0.0)),
                energy_level=float(s.get("energyLevel", 0.0)),
                confidence=float(s.get("confidence", 0.0)),
                canonical_name=str(s.get("name", "")),
            )
        )

    # Timbre
    timbre_d = data.get("timbre", {})
    timbre = (
        AnalysisTimbre(
            brightness=float(timbre_d.get("brightness", 0.0)),
            warmth=float(timbre_d.get("warmth", 0.0)),
            density=float(timbre_d.get("density", 0.0)),
            roughness=float(timbre_d.get("roughness", 0.0)),
            complexity=float(timbre_d.get("complexity", 0.0)),
        )
        if timbre_d
        else None
    )

    # Dynamics
    dyn_d = data.get("dynamics", {})
    dynamics = (
        AnalysisDynamics(
            dynamic_range_db=float(dyn_d.get("dynamicRangeDb", 0.0)),
            peak_db=float(dyn_d.get("peakDb", 0.0)),
            rms_db=float(dyn_d.get("rmsDb", 0.0)),
            crest_factor=float(dyn_d.get("crestFactor", 0.0)),
            loudness_range_db=float(dyn_d.get("loudnessRangeDb", 0.0)),
            is_compressed=bool(dyn_d.get("isCompressed", False)),
        )
        if dyn_d
        else None
    )

    # Rhythm
    rhy_d = data.get("rhythm", {})
    if rhy_d:
        rts_d = rhy_d.get("timeSignature", ts_d)
        rhythm = AnalysisRhythm(
            time_signature=TimeSignature(
                numerator=int(rts_d.get("numerator", 4)),
                denominator=int(rts_d.get("denominator", 4)),
                confidence=float(rts_d.get("confidence", 0.0)),
            ),
            syncopation=float(rhy_d.get("syncopation", 0.0)),
            groove_type=str(rhy_d.get("grooveType", "straight")),
            pattern_regularity=float(rhy_d.get("patternRegularity", 0.0)),
            tempo_stability=float(rhy_d.get("tempoStability", 0.0)),
        )
    else:
        rhythm = None

    # Melody
    mel_d = data.get("melody", {})
    if mel_d:
        pitches = [
            MelodyPoint(
                time=float(p.get("time", 0.0)),
                frequency=float(p.get("frequency", 0.0)),
                confidence=float(p.get("confidence", 0.0)),
            )
            for p in mel_d.get("pitches", [])
        ]
        melody = AnalysisMelody(
            pitch_range_octaves=float(mel_d.get("pitchRangeOctaves", 0.0)),
            pitch_stability=float(mel_d.get("pitchStability", 0.0)),
            mean_frequency=float(mel_d.get("meanFrequency", 0.0)),
            vibrato_rate=float(mel_d.get("vibratoRate", 0.0)),
            pitches=pitches,
        )
    else:
        melody = None

    return AnalysisResult(
        bpm=float(data.get("bpm", 0.0)),
        bpm_confidence=float(data.get("bpmConfidence", 0.0)),
        key=key,
        time_signature=time_signature,
        beat_times=beat_times,
        beat_strengths=beat_strengths,
        downbeat_indices=downbeat_indices,
        downbeat_phase=downbeat_phase,
        bpm_candidates=bpm_candidates,
        time_signature_candidates=time_signature_candidates,
        chords=chords,
        sections=sections,
        timbre=timbre,
        dynamics=dynamics,
        rhythm=rhythm,
        melody=melody,
        beat_observations=beat_observations,
        form=str(data.get("form", "")),
    )
