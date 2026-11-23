"""Analysis and inspection wrappers for libsonare."""

from __future__ import annotations

import ctypes
import json
from collections.abc import Mapping, Sequence
from typing import Any, cast

from ._errors import _entry, _refuse, _unknown_error
from ._ffi import (
    SonareAnalyzeMelodyOptions,
    SonareBoundaryOptions,
    SonareBoundaryResult,
    SonareChord,
    SonareChordAnalysisResult,
    SonareChordDetectionOptions,
    SonareMelodyResult,
    SonareSectionOptions,
    SonareSectionResult,
    SonareStringArray,
)
from ._runtime import (
    _call_string,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _mapping_keys,
    _measured,
    _mode_value,
    _section_type_token,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int32,
    _to_c_size_t,
    _token_ordinal,
    _tuning_arg,
)
from ._types_analysis import _chord_symbol
from ._types_capabilities import _from_wire
from ._types_engine import _SECTION_NAMES
from ._types_enums import _CHORD_CHROMA_METHODS
from .types import (
    CHORD_QUALITIES,
    Boundary,
    BoundaryResult,
    Capabilities,
    Chord,
    ChordAnalysisResult,
    ChordChromaMethod,
    FunctionalChord,
    Key,
    KeyMode,
    MelodyPoint,
    MelodyResult,
    PitchClass,
    Section,
    SectionAnalysis,
)

_CHORD_QUALITY_ORDINALS = {name: ordinal for ordinal, name in enumerate(CHORD_QUALITIES)}


def _chords_from_c(out: SonareChordAnalysisResult) -> list[Chord]:
    """Copies the chords of a C detection result into :class:`Chord` entries."""
    chords = []
    for i in range(out.chord_count):
        ordinal = int(out.chords[i].quality)
        quality = CHORD_QUALITIES[ordinal] if 0 <= ordinal < len(CHORD_QUALITIES) else "unknown"
        root = PitchClass(out.chords[i].root)
        bass = PitchClass(out.chords[i].bass)
        chords.append(
            Chord(
                root=root,
                quality=quality,
                start_sec=float(out.chords[i].start_sec),
                end_sec=float(out.chords[i].end_sec),
                confidence=float(out.chords[i].confidence),
                bass=bass,
                name=_chord_symbol(root, quality, bass),
            )
        )
    return chords


def _voiced_hz(value: float) -> float | None:
    """A melody point's frequency, or ``None`` for an unvoiced point (0 in C)."""
    return float(value) if value > 0.0 else None


@_entry("detect_chords")
@_guard_buffer("samples")
def detect_chords(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    min_duration_sec: float = 0.3,
    smoothing_window_sec: float = 2.0,
    threshold: float = 0.5,
    use_triads_only: bool = True,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_beat_sync: bool = True,
    use_hmm: bool = True,
    hmm_beam_width: int = 24,
    use_key_context: bool = False,
    key_root: PitchClass = PitchClass.C,
    key_mode: KeyMode = "major",
    detect_inversions: bool = False,
    chroma_method: ChordChromaMethod = "cqt",
    tuning_cents: float | str = "auto",
) -> ChordAnalysisResult:
    """Detect a continuous chord/N.C. timeline.

    ``threshold`` is a final-template correlation cutoff in ``[0, 1]``;
    rejected intervals use quality ``"unknown"`` and ``Chord.name == "N.C."``.
    ``tuning_cents`` is the recording's tuning offset in cents; it
    must be in ``[-50, 50)`` and 0 is concert A440. ``"auto"`` (default)
    measures it from the audio, and :func:`reference_hz_to_tuning` converts a reference pitch to
    this unit. The value used, given or measured, is the result's ``tuning_cents``.
    ``n_fft`` is the STFT chroma window in samples at 22050 Hz,
    rescaled to the input rate so its duration is the same at every rate.
    ``hop_length`` is the hop in samples of the caller's buffer, at the sample rate.
    ``chroma_method`` is ``"cqt"`` (default), ``"stft"`` or ``"nnls"``; the
    defaults decode triads only, with the HMM on.
    """
    chroma_method_value = _token_ordinal(chroma_method, _CHORD_CHROMA_METHODS, "chroma_method")
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareChordAnalysisResult()
    # The key ordinals go in unconverted: the struct's own narrowing refuses a
    # fraction, which int() truncated into a neighbouring pitch class or mode.
    tuning_value, tuning_auto = _tuning_arg(tuning_cents)
    options = SonareChordDetectionOptions(
        min_duration_sec,
        smoothing_window_sec,
        threshold,
        1 if use_triads_only else 0,
        n_fft,
        hop_length,
        1 if use_beat_sync else 0,
        1 if use_hmm else 0,
        hmm_beam_width,
        1 if use_key_context else 0,
        key_root,
        _mode_value(key_mode, "key_mode"),
        1 if detect_inversions else 0,
        chroma_method_value,
        tuning_value,
        tuning_auto,
    )
    rc = lib.sonare_detect_chords(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(options),
        ctypes.byref(out),
    )
    _check(rc, flattened="options")
    try:
        return ChordAnalysisResult(chords=_chords_from_c(out), tuning_cents=float(out.tuning_cents))
    finally:
        lib.sonare_free_chord_analysis_result(ctypes.byref(out))


@_entry("chord_functional_analysis")
@_guard_buffer("samples")
def chord_functional_analysis(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    key_root: PitchClass,
    key_mode: KeyMode = "major",
    min_duration_sec: float = 0.3,
    smoothing_window_sec: float = 2.0,
    threshold: float = 0.5,
    use_triads_only: bool = True,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_beat_sync: bool = True,
    use_hmm: bool = True,
    hmm_beam_width: int = 24,
    use_key_context: bool = False,
    detect_inversions: bool = False,
    chroma_method: ChordChromaMethod = "cqt",
    tuning_cents: float | str = "auto",
) -> list[FunctionalChord]:
    """Detect chords and label them relative to a key.

    Detects chords with the same algorithm as :func:`detect_chords`, then
    returns the timed entries as :func:`chord_functions` does: every
    :class:`Chord` field plus ``roman_numeral`` and ``function``. ``tuning_cents`` is as for
    :func:`detect_chords`.
    """
    chroma_method_value = _token_ordinal(chroma_method, _CHORD_CHROMA_METHODS, "chroma_method")
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    detected = SonareChordAnalysisResult()
    roman = SonareStringArray()
    functions = SonareStringArray()
    # The key ordinals go in unconverted: the struct's own narrowing refuses a
    # fraction, which int() truncated into a neighbouring pitch class or mode.
    tuning_value, tuning_auto = _tuning_arg(tuning_cents)
    options = SonareChordDetectionOptions(
        min_duration_sec,
        smoothing_window_sec,
        threshold,
        1 if use_triads_only else 0,
        n_fft,
        hop_length,
        1 if use_beat_sync else 0,
        1 if use_hmm else 0,
        hmm_beam_width,
        1 if use_key_context else 0,
        key_root,
        _mode_value(key_mode, "key_mode"),
        1 if detect_inversions else 0,
        chroma_method_value,
        tuning_value,
        tuning_auto,
    )
    rc = lib.sonare_chord_functional_analysis(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(options),
        _to_c_int32(key_root, "key_root"),
        _to_c_int32(_mode_value(key_mode, "key_mode"), "key_mode"),
        ctypes.byref(detected),
        ctypes.byref(roman),
        ctypes.byref(functions),
    )
    _check(rc, flattened="options")
    try:
        return [
            FunctionalChord(
                chord.root,
                chord.quality,
                chord.start_sec,
                chord.end_sec,
                chord.confidence,
                chord.bass,
                chord.name,
                roman.items[i].decode("utf-8"),
                functions.items[i].decode("utf-8"),
            )
            for i, chord in enumerate(_chords_from_c(detected))
        ]
    finally:
        lib.sonare_free_chord_analysis_result(ctypes.byref(detected))
        lib.sonare_free_string_array(ctypes.byref(roman))
        lib.sonare_free_string_array(ctypes.byref(functions))


def _key_pair(key: object) -> tuple[object, object]:
    """Reads a key given as an object with ``root`` / ``mode``, a mapping, or a pair."""
    if isinstance(key, Mapping):
        fields = _mapping_keys(key, ("root", "mode"), "key")
        root, mode = fields.get("root"), fields.get("mode")
    elif isinstance(key, tuple | list) and len(key) == 2:
        root, mode = key
    else:
        root, mode = getattr(key, "root", None), getattr(key, "mode", None)
    if root is None or mode is None:
        raise _refuse("key must have a root and a mode")
    return root, mode


@_entry("chord_functions")
def chord_functions(
    chords: ChordAnalysisResult | Sequence[Chord],
    key: Key | Mapping[str, object] | tuple[PitchClass, KeyMode],
) -> list[FunctionalChord]:
    """Label chords that are already known with their harmonic function in a key.

    ``chords`` is :func:`detect_chords`' result (or its ``chords`` list), or the
    same :class:`Chord` entries built by hand; only ``root`` and ``quality`` are
    read for the labels and nothing is re-detected. ``key`` is a :class:`Key`
    (e.g. from :func:`detect_key`), a ``{"root": ..., "mode": ...}`` mapping or a
    ``(root, mode)`` pair.

    Returns the same timed entries as :class:`FunctionalChord`, with ``roman_numeral``
    (``"I"``, ``"V7"``, ``"vi"``; ``"N.C."`` for an unknown chord) and
    ``function`` (``"tonic"``, ``"subdominant"``, ``"dominant"``, ``"chromatic"``
    for a root outside the key's scale, ``"none"`` for an unknown chord) added.
    Every mode other than minor reads the major scale.
    """
    entries = list(chords.chords if isinstance(chords, ChordAnalysisResult) else chords)
    root, mode = _key_pair(key)
    key_root = _to_c_int32(root, "key.root")
    key_mode = _to_c_int32(_mode_value(cast(KeyMode, mode), "key.mode"), "key.mode")
    lib = _get_lib()
    array = (SonareChord * len(entries))()
    for i, chord in enumerate(entries):
        ordinal = _CHORD_QUALITY_ORDINALS.get(chord.quality)
        if ordinal is None:
            raise _refuse(
                f"chords[{i}].quality must be one of {', '.join(_CHORD_QUALITY_ORDINALS)}"
            )
        array[i] = SonareChord(
            _to_c_int32(chord.root, f"chords[{i}].root"),
            ordinal,
            chord.start_sec,
            chord.end_sec,
            chord.confidence,
            _to_c_int32(chord.root if chord.bass is None else chord.bass, f"chords[{i}].bass"),
        )
    roman = SonareStringArray()
    functions = SonareStringArray()
    rc = lib.sonare_chord_functions(
        array if entries else None,
        len(entries),
        key_root,
        key_mode,
        ctypes.byref(roman),
        ctypes.byref(functions),
    )
    _check(rc)
    try:
        return [
            FunctionalChord(
                chord.root,
                chord.quality,
                chord.start_sec,
                chord.end_sec,
                chord.confidence,
                chord.bass,
                chord.name,
                roman.items[i].decode("utf-8"),
                functions.items[i].decode("utf-8"),
            )
            for i, chord in enumerate(entries)
        ]
    finally:
        lib.sonare_free_string_array(ctypes.byref(roman))
        lib.sonare_free_string_array(ctypes.byref(functions))


@_entry("analyze_sections")
@_guard_buffer("samples")
def analyze_sections(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    min_section_sec: float = 4.0,
    threshold: float | None = None,
) -> SectionAnalysis:
    """Detect song-structure sections (intro/verse/chorus/...) and the form they spell.

    Args:
        samples: Mono audio samples (1D float).
        sample_rate: Sample rate in Hz.
        n_fft: FFT window size used for the structural features.
        hop_length: Hop in samples of the analysed signal: the input resampled to
            22050 Hz when its rate is above 22050 Hz, the input itself otherwise.
        min_section_sec: Minimum section duration in seconds.
        threshold: Relative boundary threshold, as in :func:`detect_boundaries`;
            ``None`` keeps the library default, which is lower than the boundary
            layer's because sections snap to downbeats and keep a minimum length.

    Returns:
        A :class:`SectionAnalysis`: the sections in time order and the form label,
        one letter per section (``""`` when none).
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    options = SonareSectionOptions()
    options.n_fft = _to_c_int(n_fft, "n_fft")
    options.hop_length = _to_c_size_t(hop_length, "hop_length")
    options.min_section_sec = _to_c_float(min_section_sec, "min_section_sec")
    if threshold is not None:
        options.threshold = _to_c_float(threshold, "threshold")
    out = SonareSectionResult()
    rc = lib.sonare_analyze_sections(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(options),
        ctypes.byref(out),
    )
    _check(rc, flattened="options")
    try:
        sections = [
            Section(
                type=_section_type_token(int(out.sections[i].type)),
                start_sec=float(out.sections[i].start_sec),
                end_sec=float(out.sections[i].end_sec),
                energy_level=float(out.sections[i].energy_level),
                confidence=float(out.sections[i].confidence),
                group=int(out.sections[i].group),
                name=_SECTION_NAMES[_section_type_token(int(out.sections[i].type))],
            )
            for i in range(out.section_count)
        ]
        form = out.form.decode("utf-8") if out.form else ""
        return SectionAnalysis(sections=sections, form=form)
    finally:
        lib.sonare_free_section_result(ctypes.byref(out))


@_entry("detect_boundaries")
@_guard_buffer("samples")
def detect_boundaries(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    *,
    n_fft: int = 2048,
    hop_length: int = 512,
    kernel_size: int = 64,
    threshold: float = 0.3,
    absolute_threshold: float = 0.005,
    n_mfcc: int = 13,
    n_chroma: int = 12,
    peak_distance_sec: float = 2.0,
    reference_window_sec: float = 60.0,
    use_mfcc: bool = True,
    use_chroma: bool = True,
) -> BoundaryResult:
    """Detect structural boundaries and return the novelty curve behind them.

    This is the unlabelled layer :func:`analyze_sections` is built on, not a
    coarser view of its output: sections are labelled spans, these are the
    transitions plus the continuous curve they were picked from, so a caller
    applying its own threshold needs this and cannot derive it from a section
    list.

    Args:
        samples: Mono audio samples (1D float).
        sample_rate: Sample rate in Hz. Rates above 22050 are
            analyzed at 22050, so boundary times are stable across source rates.
        n_fft: FFT window size used for the structural features.
        hop_length: Hop in samples of the analysed signal: the input resampled to
            22050 Hz when its rate is above 22050 Hz, the input itself otherwise.
        kernel_size: Checkerboard kernel size in frames.
        threshold: Relative novelty threshold: a peak must reach this fraction of
            the largest raw novelty event within ``reference_window_sec`` of it, a
            lone dominant event (one no other event in the window comes within
            30% of) set aside. Selects how prominent a peak is *among
            its neighbours*; it says nothing about how much the features actually
            changed.
        absolute_threshold: Novelty floor applied to the raw response before that
            scaling, asking whether anything changed at all. Set to 0 to gate on
            ``threshold`` alone -- but note that self-scaling turns residual
            fluctuation into peaks of 1.0, so a stationary input then segments
            anyway. Lowering the floor does not recover level-only structure; a
            level change turns the feature vector about five times less than a
            comparable pitch change, landing below what steady noise produces, so
            the noise is admitted first.
        n_mfcc: Number of MFCC coefficients.
        n_chroma: Number of chroma bins.
        peak_distance_sec: Minimum spacing between peaks, in seconds.
        reference_window_sec: One-sided span in seconds (default 60) over which the
            local novelty maximum that ``threshold`` is measured against is taken.
            Must be finite and at least 0; 0 makes each frame its own
            reference, which disables the relative threshold. A value at least
            the track length makes the reference whole-track, still with a lone
            dominant event set aside.
        use_mfcc: Use MFCC features.
        use_chroma: Use chroma features.

    Returns:
        A :class:`BoundaryResult`.

    Raises:
        SonareValueError: If both ``use_mfcc`` and ``use_chroma`` are false --
            the two streams are combined frame-for-frame, so with neither enabled
            there is nothing to combine.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    options = SonareBoundaryOptions(
        n_fft=_to_c_int(n_fft, "n_fft"),
        hop_length=_to_c_size_t(hop_length, "hop_length"),
        kernel_size=_to_c_int(kernel_size, "kernel_size"),
        threshold=_to_c_float(threshold, "threshold"),
        absolute_threshold=_to_c_float(absolute_threshold, "absolute_threshold"),
        n_mfcc=_to_c_int(n_mfcc, "n_mfcc"),
        n_chroma=_to_c_int(n_chroma, "n_chroma"),
        peak_distance_sec=_to_c_float(peak_distance_sec, "peak_distance_sec"),
        reference_window_sec=_to_c_float(reference_window_sec, "reference_window_sec"),
        use_mfcc=1 if use_mfcc else 0,
        use_chroma=1 if use_chroma else 0,
    )
    out = SonareBoundaryResult()
    rc = lib.sonare_detect_boundaries(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(options),
        ctypes.byref(out),
    )
    _check(rc, flattened="options")
    try:
        return BoundaryResult(
            boundaries=[
                Boundary(
                    time_sec=float(out.boundaries[i].time_sec),
                    frame=int(out.boundaries[i].frame),
                    strength=float(out.boundaries[i].strength),
                )
                for i in range(out.boundary_count)
            ],
            novelty_curve=_from_c_float_array(out.novelty_curve, out.novelty_length),
            novelty_peak=float(out.novelty_peak) if out.novelty_peak > 0.0 else None,
            sample_rate=int(out.sample_rate),
            hop_length=int(out.hop_length),
            frame_stride=int(out.frame_stride),
        )
    finally:
        lib.sonare_free_boundary_result(ctypes.byref(out))


@_entry("analyze_melody")
@_guard_buffer("samples")
def analyze_melody(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    f0_min_hz: float = 65.0,
    f0_max_hz: float = 2093.0,
    frame_length: int = 2048,
    hop_length: int = 256,
    threshold: float = 0.1,
    use_pyin: bool = False,
    center: bool = True,
) -> MelodyResult:
    """Extract the melody contour from monophonic audio.

    Args:
        samples: Mono audio samples (1D float).
        sample_rate: Sample rate in Hz.
        f0_min_hz: Lowest detectable F0 in Hz.
        f0_max_hz: Highest detectable F0 in Hz.
        frame_length: Analysis frame length in samples at 22050 Hz, rescaled to
            ``sample_rate``.
        hop_length: Hop length in samples of the input buffer.
        threshold: YIN/pYIN absolute threshold.
        use_pyin: When ``True``, use pYIN (probabilistic YIN with Viterbi
            smoothing) instead of plain YIN.
        center: When ``True`` (default), apply librosa-style center padding
            so the first frame is centred on sample 0.

    Returns:
        A :class:`MelodyResult` with the contour points and summary stats.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareMelodyResult()
    options = SonareAnalyzeMelodyOptions._given(
        use_pyin=1 if use_pyin else 0, center=1 if center else 0
    )
    rc = lib.sonare_analyze_melody(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(f0_min_hz, "f0_min_hz"),
        _to_c_float(f0_max_hz, "f0_max_hz"),
        _to_c_size_t(frame_length, "frame_length"),
        _to_c_size_t(hop_length, "hop_length"),
        _to_c_float(threshold, "threshold"),
        ctypes.byref(options),
        ctypes.byref(out),
    )
    _check(rc, flattened="options")
    try:
        return MelodyResult(
            points=[
                MelodyPoint(
                    time_sec=float(out.points[i].time_sec),
                    frequency_hz=_voiced_hz(out.points[i].frequency_hz),
                    confidence=float(out.points[i].confidence),
                )
                for i in range(out.point_count)
            ],
            pitch_range_octaves=_measured(out.pitch_range_octaves),
            pitch_stability=_measured(out.pitch_stability),
            mean_frequency_hz=_measured(out.mean_frequency_hz),
            vibrato_rate_hz=_measured(out.vibrato_rate_hz),
        )
    finally:
        lib.sonare_free_melody_result(ctypes.byref(out))


@_entry("version")
def version() -> str:
    """Return the libsonare version string."""
    lib = _get_lib()
    v = lib.sonare_version()
    return v.decode("utf-8") if v else ""


@_entry("system_info")
def system_info() -> dict[str, Any]:
    """Return the host this library runs on: CPU cores, memory and parallelism.

    Keys are ``cpu`` (``logical_cores``, ``physical_cores``), ``memory``
    (``total_gb``, ``available_gb``) and ``parallel`` (``enabled``, ``workers``,
    ``strategy``). A fact the platform cannot report is absent from its object.

    Returns:
        The parsed document, keyed as the C ABI spells it.
    """
    document = json.loads(_call_string(_get_lib().sonare_system_info_json))
    if not isinstance(document, dict):
        raise _unknown_error("libsonare returned a non-object system info document")
    return document


def _capabilities_document() -> dict[str, Any]:
    """The library's capabilities document as parsed JSON, keyed as the C ABI spells it."""
    lib = _get_lib()
    descriptor = json.loads(_call_string(lib.sonare_capabilities_json))
    if not isinstance(descriptor, dict):
        raise _unknown_error("libsonare returned a non-object capabilities descriptor")
    return descriptor


@_entry("capabilities")
def capabilities() -> Capabilities:
    """Return the native library's build, platform, and decoder capabilities.

    The descriptor describes the loaded native library, rather than the Python
    package that happens to call it.
    """
    return _from_wire(Capabilities, _capabilities_document())


@_entry("abi_version")
def abi_version() -> int:
    """Return the aggregate libsonare C ABI version.

    Folds every subsystem ABI macro into a single 32-bit value (see
    ``sonare_c.h``), letting a prebuilt binding detect an incompatible native
    library.
    """
    lib = _get_lib()
    return int(lib.sonare_abi_version())


@_entry("engine_abi_version")
def engine_abi_version() -> int:
    """Return the realtime engine ABI version used for binding compatibility checks."""
    return int(_get_lib().sonare_engine_abi_version())


@_entry("voice_changer_abi_version")
def voice_changer_abi_version() -> int | None:
    """Return the realtime voice-changer ABI version.

    ``None`` when libsonare was built without the voice changer; presence is
    ``capabilities().features.voice_changer``.

    Bindings that rely on the flat POD ``SonareRealtimeVoiceChangerConfig``
    layout (Rust FFI, raw C consumers) should call this at attach time and
    compare against the compile-time constant exported by the host library.
    JSON-based callers (this binding) do not need to gate on this; it is
    exposed for parity with the C/Node/WASM surfaces.
    """
    return int(_get_lib().sonare_voice_changer_abi_version()) or None
