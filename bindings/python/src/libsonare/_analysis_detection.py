"""Analysis and inspection wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Any, cast

import numpy as np

from ._ffi_types_core import (
    SonareKey,
    SonareKeyCandidate,
    SonareOnsetDetectConfig,
)
from ._narrowing import _narrow_double
from ._runtime import (
    SonareValueError,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _key_mode_token,
    _mode_values,
    _out_float_array,
    _profile_value,
    _section_type_token,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int_array,
    _to_c_size_t,
    _token_arg,
    _tuning_arg,
    _utf8_arg,
)
from ._types_analysis import _chord_symbol
from ._types_engine import _SECTION_NAMES
from ._types_enums import _GENRE_HINTS
from .types import (
    CHORD_QUALITIES,
    AnalysisBpmCandidate,
    AnalysisChord,
    AnalysisDynamics,
    AnalysisResult,
    AnalysisRhythm,
    Beat,
    BeatObservations,
    BpmCandidateRelation,
    GenreHint,
    Key,
    KeyCandidate,
    KeyDetection,
    KeyMode,
    KeyProfile,
    MelodyPoint,
    MelodyResult,
    PitchClass,
    Section,
    SectionType,
    TimbreFrame,
    TimeSignature,
)

# The core tempo search range every tempo entry point reads an omitted bound as.
_DEFAULT_BPM_MIN = 60.0
_DEFAULT_BPM_MAX = 200.0


@_guard_buffer("samples")
def detect_bpm(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    *,
    bpm_min: float | None = None,
    bpm_max: float | None = None,
) -> float:
    """Detect the BPM (tempo) of audio samples.

    One-shot wrapper for raw sample input. When you load from a file or call
    multiple analyses on the same signal, prefer :class:`libsonare.Audio` and
    :meth:`Audio.detect_bpm` to avoid re-copying samples across the FFI.

    Args:
        samples: Mono audio samples (1D, nominally ``[-1.0, 1.0]``). Accepts
            ``list[float]``, ``tuple[float, ...]``, ``array.array``, or a numpy
            1D array of dtype ``float32``.
        sample_rate: Sample rate in Hz.
        bpm_min: Lower bound of the tempo search in BPM (default 60).
        bpm_max: Upper bound of the tempo search in BPM (default 200).

    Returns:
        Detected BPM as a ``float``. For confidence, use :func:`analyze`.

    Raises:
        SonareValueError: If the range is non-finite, non-positive or inverted.
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_bpm = ctypes.c_float()
    rc = lib.sonare_detect_bpm(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(_DEFAULT_BPM_MIN if bpm_min is None else bpm_min, "bpm_min"),
        _to_c_float(_DEFAULT_BPM_MAX if bpm_max is None else bpm_max, "bpm_max"),
        ctypes.byref(out_bpm),
    )
    _check(rc)
    return float(out_bpm.value)


@_guard_buffer("samples")
def detect_key(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_hpss: bool = False,
    loudness_weighted: bool = False,
    chroma_high_pass_hz: float = 80.0,
    modes: Sequence[KeyMode] | str | None = None,
    profile: KeyProfile | None = None,
    genre_hint: GenreHint | None = None,
    tuning_cents: float | str = "auto",
) -> KeyDetection:
    """Detect the musical key of audio samples.

    The chroma is shifted by the tuning measured from the audio unless
    ``tuning_cents`` says otherwise; pass 0 to read it at concert A440.

    Args:
        samples: Mono audio samples (1D float). See :func:`detect_bpm` for
            accepted types.
        sample_rate: Sample rate in Hz.
        n_fft: Chroma window in samples at 22050 Hz, rescaled to the analysis rate.
        hop_length: Hop in samples of the analysed signal: the input resampled to
            22050 Hz when its rate is above 22050 Hz, the input itself otherwise.
        use_hpss: Use harmonic-percussive separation before chroma analysis.
        loudness_weighted: Weight chroma frames by RMS loudness.
        chroma_high_pass_hz: Optional high-pass cutoff before chroma analysis.
        profile: Key-profile family to pin; it outranks ``genre_hint``. ``None``
            lets ``genre_hint`` choose it, or the chroma under the default
            ``"auto"`` hint.
        genre_hint: Optional genre hint (``"auto"``, ``"edm"``, ``"pop"``,
            ``"classical"``, or ``"jazz"``).
        tuning_cents: Tuning offset of the recording in cents, in
            ``[-50, 50)``; 0 is concert A440. ``"auto"`` (default) measures it
            from the audio, and :func:`reference_hz_to_tuning` converts a reference pitch
            to this unit.

    Returns:
        :class:`libsonare.KeyDetection`, a :class:`Key` with ``root``
        (:class:`PitchClass`), ``mode`` (a :data:`KeyMode` token), and ``confidence``
        (``float`` in ``[0, 1]``), plus ``tuning_cents``: the tuning used, given or
        measured, in cents.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    mode_values = _mode_values(modes)
    mode_array, mode_count = _to_c_int_array(mode_values, "modes") if mode_values else (None, 0)
    out_key = SonareKey()
    out_tuning = ctypes.c_float()
    tuning_value, tuning_auto = _tuning_arg("detect_key", tuning_cents)
    rc = lib.sonare_detect_key_with_tuning(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        ctypes.c_int(1 if use_hpss else 0),
        ctypes.c_int(1 if loudness_weighted else 0),
        _to_c_float(chroma_high_pass_hz, "chroma_high_pass_hz"),
        mode_array,
        _to_c_size_t(mode_count, "mode_count"),
        ctypes.c_int32(_profile_value(profile)),
        None
        if genre_hint is None
        else _utf8_arg(_token_arg(genre_hint, _GENRE_HINTS, "genre_hint"), "genre_hint"),
        _to_c_float(tuning_value, "tuning_cents"),
        _to_c_int(tuning_auto, "tuning_auto"),
        ctypes.byref(out_key),
        ctypes.byref(out_tuning),
    )
    _check(rc)
    return KeyDetection(
        root=PitchClass(out_key.root),
        mode=_key_mode_token(out_key.mode),
        confidence=float(out_key.confidence),
        tuning_cents=float(out_tuning.value),
    )


@_guard_buffer("samples")
def detect_key_candidates(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_hpss: bool = False,
    loudness_weighted: bool = False,
    chroma_high_pass_hz: float = 80.0,
    modes: Sequence[KeyMode] | str | None = None,
    profile: KeyProfile | None = None,
    genre_hint: GenreHint | None = None,
    tuning_cents: float | str = "auto",
) -> list[KeyCandidate]:
    """Return ranked musical key candidates for ambiguous material.

    ``tuning_cents`` is as for :func:`detect_key`: a tuning offset in cents in
    ``[-50, 50)`` (0 is concert A440) or ``"auto"`` to measure it.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    mode_values = _mode_values(modes)
    mode_array, mode_count = _to_c_int_array(mode_values, "modes") if mode_values else (None, 0)
    out_candidates = ctypes.POINTER(SonareKeyCandidate)()
    out_count = ctypes.c_size_t()
    tuning_value, tuning_auto = _tuning_arg("detect_key_candidates", tuning_cents)
    rc = lib.sonare_detect_key_candidates_with_tuning(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        ctypes.c_int(1 if use_hpss else 0),
        ctypes.c_int(1 if loudness_weighted else 0),
        _to_c_float(chroma_high_pass_hz, "chroma_high_pass_hz"),
        mode_array,
        _to_c_size_t(mode_count, "mode_count"),
        ctypes.c_int32(_profile_value(profile)),
        None
        if genre_hint is None
        else _utf8_arg(_token_arg(genre_hint, _GENRE_HINTS, "genre_hint"), "genre_hint"),
        _to_c_float(tuning_value, "tuning_cents"),
        _to_c_int(tuning_auto, "tuning_auto"),
        ctypes.byref(out_candidates),
        ctypes.byref(out_count),
        None,
    )
    _check(rc)
    try:
        return [
            KeyCandidate(
                key=Key(
                    root=PitchClass(out_candidates[i].key.root),
                    mode=_key_mode_token(out_candidates[i].key.mode),
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
    sample_rate: int,
    *,
    bpm_min: float | None = None,
    bpm_max: float | None = None,
) -> np.ndarray:
    """Detect beat positions in audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz.
        bpm_min: Lower bound of the tempo search in BPM (default 60).
        bpm_max: Upper bound of the tempo search in BPM (default 200).

    Returns:
        A float32 array of beat times in seconds.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out_times, out_count):
        rc = lib.sonare_detect_beats(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            _to_c_float(_DEFAULT_BPM_MIN if bpm_min is None else bpm_min, "bpm_min"),
            _to_c_float(_DEFAULT_BPM_MAX if bpm_max is None else bpm_max, "bpm_max"),
            ctypes.byref(out_times),
            ctypes.byref(out_count),
        )
        _check(rc)
        count = out_count.value
        return _from_c_float_array(out_times, count)


@_guard_buffer("samples")
def detect_downbeats(
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> np.ndarray:
    """Detect downbeat positions in audio samples as a float32 array of seconds."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out_times, out_count):
        rc = lib.sonare_detect_downbeats(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(out_times),
            ctypes.byref(out_count),
        )
        _check(rc)
        count = out_count.value
        return _from_c_float_array(out_times, count)


@_guard_buffer("samples")
def detect_onsets(
    samples: Sequence[float] | list[float],
    sample_rate: int,
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
) -> np.ndarray:
    """Detect onset positions in audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz.

    Returns:
        A float32 array of onset times in seconds.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out_times, out_count):
        # Counts go in unconverted: the struct's own narrowing refuses a
        # fraction, which int() truncated into a legal frame count instead.
        config = SonareOnsetDetectConfig(
            n_fft=n_fft,
            hop_length=hop_length,
            threshold=_narrow_double(threshold, "threshold"),
            pre_max=pre_max,
            post_max=post_max,
            pre_avg=pre_avg,
            post_avg=post_avg,
            delta=_narrow_double(delta, "delta"),
            wait=wait,
            backtrack=int(backtrack),
            backtrack_range=backtrack_range,
        )
        rc = lib.sonare_detect_onsets_ex(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.byref(out_times),
            ctypes.byref(out_count),
        )
        _check(rc)
        count = out_count.value
        return _from_c_float_array(out_times, count)


_UNKNOWN_QUALITY_ORDINAL: int = CHORD_QUALITIES.index("unknown")

_BPM_RELATIONS = frozenset({"primary", "half", "double", "other"})


def _bpm_relation(raw: Any) -> BpmCandidateRelation:
    """Spell a candidate's relation token; an unrecognized one reads ``"other"``."""
    token = str(raw) if raw is not None else "other"
    return cast(BpmCandidateRelation, token) if token in _BPM_RELATIONS else "other"


def _section_type_from_ordinal(raw: Any) -> SectionType:
    """Convert a raw ``sections[].type`` ordinal into a :data:`SectionType` token.

    The analyzer always writes this field, and its ordinals are a mirror of
    ``sonare::SectionType``. Both facts are load-bearing, so neither a missing
    field nor an ordinal outside the enum is treated as a value to substitute
    for: a silent fallback would turn a mirror that has drifted apart into
    plausible-looking analysis output. Raising names the ordinal instead.

    :raises SonareValueError: the field is absent or names no known section type.
    """
    if raw is None:
        raise SonareValueError("analysis result section is missing its type field")
    return _section_type_token(int(raw))


def _beat_from_wire(record: dict[str, Any]) -> Beat:
    return Beat(
        time_sec=float(record.get("timeSec", 0.0)), strength=float(record.get("strength", 0.0))
    )


def _parse_analysis_json(data: dict[str, Any]) -> AnalysisResult:
    """Parse the camelCase JSON dict returned by ``sonare_analyze_json`` into
    an :class:`AnalysisResult`.

    This is an internal helper of :func:`analyze`.
    """
    # Key
    key_d = data.get("key", {})
    key = Key(
        root=PitchClass(int(key_d.get("root", 0))),
        mode=_key_mode_token(int(key_d.get("mode", 0))),
        confidence=float(key_d.get("confidence", 0.0)),
    )

    # Time signature (top-level)
    ts_d = data.get("timeSignature", {})
    time_signature = TimeSignature(
        numerator=int(ts_d.get("numerator", 4)),
        denominator=int(ts_d.get("denominator", 4)),
        confidence=float(ts_d.get("confidence", 0.0)),
    )

    bpm_candidates = [
        AnalysisBpmCandidate(
            bpm=float(candidate.get("bpm", 0.0)),
            confidence=float(candidate.get("confidence", 0.0)),
            relation=_bpm_relation(candidate.get("relation")),
        )
        for candidate in data.get("bpmCandidates", [])
    ]

    time_signature_candidates = [
        TimeSignature(
            numerator=int(candidate.get("numerator", 4)),
            denominator=int(candidate.get("denominator", 4)),
            confidence=float(candidate.get("confidence", 0.0)),
        )
        for candidate in data.get("timeSignatureCandidates", [])
    ]

    # Beats and downbeats are both lists of {timeSec, strength} beat records.
    beats = [_beat_from_wire(b) for b in data.get("beats", [])]
    downbeats = [_beat_from_wire(b) for b in data.get("downbeats", [])]
    downbeat_phase = int(data.get("downbeatPhase", 0))

    # Beat-level evidence the downbeat and meter pass scores. Each stream is
    # beat-indexed; an empty one means the analysis could not produce it rather
    # than that every beat scored zero.
    obs_d = data.get("beatObservations", {})
    beat_observations = (
        BeatObservations(
            onset_strength=np.asarray(obs_d.get("onsetStrength", []), dtype=np.float32),
            low_frequency_energy=np.asarray(obs_d.get("lowFrequencyEnergy", []), dtype=np.float32),
            chord_change=np.asarray(obs_d.get("chordChange", []), dtype=np.float32),
        )
        if obs_d
        else None
    )

    # Beat-indexed like the streams above, and likewise empty rather than absent
    # when it was not produced -- here because the caller did not ask for it.
    beat_local_bpm = np.asarray(data.get("beatLocalBpm", []), dtype=np.float32)

    # Chords
    chords: list[AnalysisChord] = []
    for c in data.get("chords", []):
        q_raw = c.get("quality", _UNKNOWN_QUALITY_ORDINAL)
        # quality field may be int (enum value) or str (name from JSON)
        if isinstance(q_raw, int):
            # "unknown" is a quality the analyzer genuinely reports, so it has an
            # ordinal of its own. An ordinal outside the table is something else
            # -- the mirror has drifted -- and folding the two together would
            # present that as a detection result.
            if not 0 <= q_raw < len(CHORD_QUALITIES):
                raise SonareValueError(f"unknown analysis chord quality ordinal: {q_raw}")
            quality_str = CHORD_QUALITIES[q_raw]
        else:
            quality_str = str(q_raw) if str(q_raw) in CHORD_QUALITIES else "unknown"
        root = PitchClass(int(c.get("root", 0)))
        bass = PitchClass(int(c.get("bass", c.get("root", 0))))
        chords.append(
            AnalysisChord(
                root=root,
                bass=bass,
                quality=quality_str,
                start_sec=float(c.get("startSec", 0.0)),
                end_sec=float(c.get("endSec", 0.0)),
                confidence=float(c.get("confidence", 0.0)),
                name=str(c.get("name", "")) or _chord_symbol(root, quality_str, bass),
                roman_numeral=str(c.get("romanNumeral", "")),
            )
        )

    # Sections
    sections: list[Section] = []
    for s in data.get("sections", []):
        section_type = _section_type_from_ordinal(s.get("type"))
        sections.append(
            Section(
                type=section_type,
                start_sec=float(s.get("startSec", 0.0)),
                end_sec=float(s.get("endSec", 0.0)),
                energy_level=float(s.get("energyLevel", 0.0)),
                confidence=float(s.get("confidence", 0.0)),
                group=int(s["group"]),
                name=str(s.get("name", "")) or _SECTION_NAMES[section_type],
            )
        )

    # Timbre, dynamics, rhythm and melody are always written by the core.
    timbre_d = data["timbre"]
    timbre = TimbreFrame(
        brightness=float(timbre_d["brightness"]),
        warmth=float(timbre_d["warmth"]),
        density=float(timbre_d["density"]),
        roughness=float(timbre_d["roughness"]),
        complexity=float(timbre_d["complexity"]),
    )
    dyn_d = data["dynamics"]
    dynamics = AnalysisDynamics(
        dynamic_range_db=float(dyn_d["dynamicRangeDb"]),
        peak_db=float(dyn_d["peakDb"]),
        rms_db=float(dyn_d["rmsDb"]),
        crest_factor=float(dyn_d["crestFactor"]),
        loudness_range_lu=float(dyn_d["loudnessRangeLu"]),
        is_compressed=bool(dyn_d["isCompressed"]),
    )
    rhy_d = data["rhythm"]
    rts_d = rhy_d["timeSignature"]
    rhythm = AnalysisRhythm(
        time_signature=TimeSignature(
            numerator=int(rts_d["numerator"]),
            denominator=int(rts_d["denominator"]),
            confidence=float(rts_d["confidence"]),
        ),
        syncopation=float(rhy_d["syncopation"]),
        groove_type=str(rhy_d["grooveType"]),
        pattern_regularity=float(rhy_d["patternRegularity"]),
        tempo_stability=float(rhy_d["tempoStability"]),
    )
    mel_d = data["melody"]
    melody = MelodyResult(
        points=[
            MelodyPoint(
                time_sec=float(p["timeSec"]),
                # Omitted for an unvoiced point.
                frequency_hz=float(p["frequencyHz"]) if "frequencyHz" in p else None,
                confidence=float(p["confidence"]),
            )
            for p in mel_d["pitches"]
        ],
        pitch_range_octaves=float(mel_d["pitchRangeOctaves"]),
        pitch_stability=float(mel_d["pitchStability"]),
        mean_frequency_hz=float(mel_d["meanFrequencyHz"]),
        vibrato_rate_hz=float(mel_d["vibratoRateHz"]),
    )

    return AnalysisResult(
        bpm=float(data.get("bpm", 0.0)),
        bpm_confidence=float(data.get("bpmConfidence", 0.0)),
        key=key,
        time_signature=time_signature,
        beats=beats,
        downbeats=downbeats,
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
        beat_local_bpm=beat_local_bpm,
        form=str(data.get("form", "")),
        tuning_cents=float(data["tuningCents"]),
    )
