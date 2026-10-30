"""Take alignment and analysis-to-chord/key conversions for projects."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from dataclasses import dataclass

import numpy as np

from ._analysis_music import _CHORD_QUALITY_ORDINALS
from ._errors import _not_supported
from ._runtime import (
    SonareProjectWarpAnchor,
    SonareTakeAlignConfig,
    SonareTakeAlignment,
    SonareValueError,
    _check,
    _get_lib,
    _guard_buffer,
    _to_c_float_array,
    _to_c_int,
    _to_c_int32,
    _to_c_size_t,
)
from .types import Chord, Mode


@dataclass(frozen=True)
class TakeAlignment:
    """How well an :func:`align_take_to_reference` alignment was conditioned.

    Every field is descriptive: none of them makes the call fail, and a caller
    deciding what is acceptable supplies its own threshold.

    ``mean_residual_frames`` is the mean absolute frame residual of the alignment
    path around its diagonal trend -- a coarse indicator of how far the alignment
    strayed from a constant rate, not an error bound. ``reference_frames`` /
    ``take_frames`` are the chroma frames each signal produced, named for the
    arguments the caller passed, so their ratio is the overall rate difference
    the anchors encode.
    """

    mean_residual_frames: float
    reference_frames: int
    take_frames: int


@_guard_buffer("reference", "take")
def align_take_to_reference(
    reference: Sequence[float] | list[float] | np.ndarray,
    take: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    *,
    take_sample_rate: int | None = None,
    hop_length: int | None = None,
    bins_per_octave: int | None = None,
    validate: bool = True,
) -> tuple[list[tuple[float, float]], TakeAlignment]:
    """Align a take to a reference timeline and return anchors placing it under it.

    A chromagram is measured for each signal and the two are aligned, then the
    alignment is reduced to anchors :meth:`Project.set_warp_map` accepts: at
    least two finite, strictly increasing pairs. The reduction is needed rather
    than decorative -- an alignment path advances one axis at a time, so the raw
    correspondence repeats a coordinate wherever one signal carries more frames
    than the other, and those pairs are refused as a warp map.

    **The anchors are oriented for the take's own clip.** The first element of
    each pair is a position on the REFERENCE timeline and the second the
    corresponding position in the TAKE, which is the direction a clip whose
    source is that take needs. Pass them straight to
    :meth:`Project.set_warp_map` for the take's clip, and bind the clip to the
    resulting warp map with :meth:`Project.set_clip_warp_ref`.

    ``sample_rate`` is the reference's rate and ``take_sample_rate`` the take's
    (``None`` means the same rate). A take at another rate is resampled to the
    reference rate before measurement, so both signals share one chroma grid.

    **Anchors are in reference-rate samples on both axes**, unrounded, and
    ``hop_length`` is in reference-rate samples. To use them in a project whose
    rate differs, multiply both axes by ``project_rate / reference_rate`` before
    :meth:`Project.set_warp_map`, which reads project-rate samples.

    Args:
        reference: The reference timeline -- the guide take, or the backing track
            the takes were sung against.
        take: The signal to be placed under it. Its length is independent of
            ``reference``'s; a take running at a different rate is the case this
            exists for.
        sample_rate: Sample rate of ``reference`` in Hz. It has to carry the whole
            chroma grid, whose top bin must sit under Nyquist: at the default
            resolution 8 kHz is enough, and a finer ``bins_per_octave`` raises
            that bin and the rate it needs -- 24 bins per octave already refuses
            8 kHz.
        take_sample_rate: Sample rate of ``take`` in Hz; ``None`` reads it at
            ``sample_rate``. Refused like ``sample_rate`` when not positive.
        hop_length: Chroma hop in samples, which sets the time resolution of the
            anchors. ``None`` keeps the library value, and so does ``0``: the
            field has no meaning at 0, so there is no separate default to fill
            in first.
        bins_per_octave: Chroma bins per octave, ``None`` / ``0`` as above. Must
            be a positive **multiple of 12**, because the chroma folds the CQT
            grid onto twelve pitch classes: 12, 24 and 36 are in domain and 13 or
            18 are refused.
        validate: Reject empty / NaN / Inf input (default True).

    Returns:
        ``(anchors, alignment)`` -- the ``(warp_sample, source_sample)`` pairs in
        increasing order, and a :class:`TakeAlignment` describing how well the
        alignment was conditioned, its frame counts being hop frames at the
        reference rate (``take_frames`` counts the resampled take). The metadata
        is how a caller tells a take the reference genuinely fits from one it
        does not.

    Raises:
        SonareValueError: If either buffer is empty or carries a non-finite
            sample, or if a configuration value does not fit its C field.
        SonareValueError: For a non-positive ``sample_rate`` or
            ``take_sample_rate``, a resampled take over the buffer size limit, a
            rate too low to carry the chroma grid, a ``bins_per_octave`` that is
            not a positive multiple of 12, or a pair that yields fewer than two
            distinct anchors -- which is what an unalignable pair looks like, and
            is reported rather than answered with a map a caller cannot use.
        SonareError: ``NOT_SUPPORTED`` when the library was built without the
            arrangement subsystem.

    Example:
        >>> anchors, alignment = libsonare.align_take_to_reference(guide, take, 44100)
        >>> project.set_warp_map(1, anchors)
        >>> project.set_clip_warp_ref(take_clip, 1)
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_align_take_to_reference_ex"):
        raise _not_supported(
            "loaded libsonare does not export sonare_align_take_to_reference_ex; "
            "rebuild or upgrade the shared library before calling align_take_to_reference"
        )
    if take_sample_rate is None:
        take_sample_rate = sample_rate
    # Left at 0 when the caller supplied nothing, which is what the C entry reads
    # as "library value"; neither field has a meaning at 0, so there is no
    # default-filling call to make first. Assigned unconverted so the struct's
    # own narrowing sees the caller's value rather than a truncation of it.
    config = SonareTakeAlignConfig()
    if hop_length is not None:
        config.hop_length = hop_length
    if bins_per_octave is not None:
        config.bins_per_octave = bins_per_octave

    reference_array, reference_len = _to_c_float_array(
        reference, fn_name="align_take_to_reference", arg_name="reference"
    )
    take_array, take_len = _to_c_float_array(
        take, fn_name="align_take_to_reference", arg_name="take"
    )
    out_anchors = ctypes.POINTER(SonareProjectWarpAnchor)()
    out_count = ctypes.c_size_t()
    out_alignment = SonareTakeAlignment()
    rc = lib.sonare_align_take_to_reference_ex(
        reference_array,
        _to_c_size_t(reference_len, "reference_len"),
        _to_c_int(sample_rate, "sample_rate"),
        take_array,
        _to_c_size_t(take_len, "take_len"),
        _to_c_int(take_sample_rate, "take_sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out_anchors),
        ctypes.byref(out_count),
        ctypes.byref(out_alignment),
    )
    try:
        _check(rc)
        anchors = [
            (out_anchors[i].warp_sample, out_anchors[i].source_sample)
            for i in range(out_count.value)
        ]
    finally:
        # The array is heap-owned, so it is released whether the rows were read
        # or the read raised part-way. A refused call leaves the pointer NULL.
        if out_anchors:
            lib.sonare_free_warp_anchors(out_anchors)
    return anchors, TakeAlignment(
        mean_residual_frames=out_alignment.mean_residual_frames,
        reference_frames=int(out_alignment.reference_frames),
        take_frames=int(out_alignment.take_frames),
    )


# The annotation's own extension cap; an analysis chord carries at most 3.
_CHORD_EXTENSION_CAPACITY = 32
# Analysis modes are 0..6 (Mode.MAJOR..Mode.LOCRIAN).
_ANALYSIS_MODE_COUNT = 7


def _pitch_class_arg(value: object, name: str) -> int:
    """Return ``value`` as a pitch class 0..11, refusing anything else by name."""
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= int(value) <= 11:
        raise SonareValueError(f"{name} {value!r} is not a pitch class (0..11)")
    return int(value)


def chord_symbol_from_analysis(chord: Chord) -> dict[str, object]:
    """Convert an analysis :class:`Chord` into the fields of a chord annotation.

    The 25 analysis qualities are coarser in the annotation (8 families plus
    extension scale degrees), so an ordinal offset mislabels most of them; this is
    the library's own conversion. The result carries the keys
    :meth:`Project.annotate_chords` reads, so it spreads into an entry once the
    caller adds the span:
    ``{"start_ppq": a, "end_ppq": b, **chord_symbol_from_analysis(c)}``.
    Times are not converted.

    Returns:
        ``{"root_pc", "quality", "extensions", "slash_bass_pc"}``. The root is kept
        as given, including for an unknown chord; ``slash_bass_pc`` is 255 when
        ``chord.bass`` is ``None`` or equals the root. ``extensions`` is a list of
        scale degrees.

    Raises:
        TypeError: If ``chord`` is not a :class:`Chord`.
        SonareValueError: Naming the field, for a root or bass not in 0..11 or a
            quality that is not a chord quality name.
    """
    if not isinstance(chord, Chord):
        raise TypeError(f"chord must be a Chord, got {type(chord).__name__}")
    lib = _get_lib()
    if not hasattr(lib, "sonare_chord_symbol_from_analysis"):
        raise _not_supported(
            "loaded libsonare does not export sonare_chord_symbol_from_analysis; "
            "rebuild or upgrade the shared library before calling chord_symbol_from_analysis"
        )
    root = _pitch_class_arg(chord.root, "chord.root")
    ordinal = _CHORD_QUALITY_ORDINALS.get(chord.quality)
    if ordinal is None:
        raise SonareValueError(f"chord.quality {chord.quality!r} is not a chord quality")
    bass = root if chord.bass is None else _pitch_class_arg(chord.bass, "chord.bass")
    out_root = ctypes.c_uint32()
    out_quality = ctypes.c_uint32()
    out_slash = ctypes.c_uint32()
    out_count = ctypes.c_size_t()
    extensions = (ctypes.c_uint8 * _CHORD_EXTENSION_CAPACITY)()
    _check(
        lib.sonare_chord_symbol_from_analysis(
            _to_c_int32(root, "chord.root"),
            _to_c_int32(ordinal, "chord.quality"),
            _to_c_int32(bass, "chord.bass"),
            ctypes.byref(out_root),
            ctypes.byref(out_quality),
            extensions,
            _to_c_size_t(_CHORD_EXTENSION_CAPACITY, "extension_capacity"),
            ctypes.byref(out_count),
            ctypes.byref(out_slash),
        )
    )
    return {
        "root_pc": int(out_root.value),
        "quality": int(out_quality.value),
        "extensions": [int(extensions[i]) for i in range(out_count.value)],
        "slash_bass_pc": int(out_slash.value),
    }


def key_mode_from_analysis(mode: Mode | int) -> int:
    """Convert an analysis key :class:`Mode` into the ``mode`` of a key annotation.

    The annotation numbering reserves 0 for an unknown mode, so every analysis
    mode lands one ordinal higher. The result is the fourth element of a
    :meth:`Project.annotate_keys` tuple.

    Raises:
        TypeError: If ``mode`` is not a :class:`Mode` or an integer.
        SonareValueError: If ``mode`` is not in 0..6.
    """
    if isinstance(mode, bool) or not isinstance(mode, int):
        raise TypeError(f"mode must be a Mode, got {type(mode).__name__}")
    if not 0 <= int(mode) < _ANALYSIS_MODE_COUNT:
        raise SonareValueError(f"mode {mode!r} is not an analysis mode (0..6)")
    lib = _get_lib()
    if not hasattr(lib, "sonare_key_mode_from_analysis"):
        raise _not_supported(
            "loaded libsonare does not export sonare_key_mode_from_analysis; "
            "rebuild or upgrade the shared library before calling key_mode_from_analysis"
        )
    out_mode = ctypes.c_uint32()
    _check(lib.sonare_key_mode_from_analysis(_to_c_int32(mode, "mode"), ctypes.byref(out_mode)))
    return int(out_mode.value)
