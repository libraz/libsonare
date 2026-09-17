"""Percussive-event extraction, editing and rendering."""

from __future__ import annotations

import ctypes
import dataclasses
from collections.abc import Sequence

import numpy as np

from ._ffi import (
    SonarePercussiveEvent,
    SonarePercussiveEventConfig,
    SonarePercussiveEventEdit,
    SonarePercussiveEventsResult,
    SonarePercussiveRenderConfig,
)
from ._runtime import (
    _INT64_MAX,
    _INT64_MIN,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _narrow_int,
    _out_float_array,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _unsupported_effect_symbol,
    _validate_c_int_field,
)

# The two percussive-event configs are versioned separately from the note-object
# ones, and are likewise at layout version 1.
_PERCUSSIVE_STRUCT_VERSION = 1


@dataclasses.dataclass
class PercussiveEventEdit:
    """A pending, non-destructive change to one :class:`PercussiveEvent`.

    The default instance is the identity edit, so an event carrying it is left
    untouched by :func:`render_percussive_events` (and a set whose edits are all
    identity reproduces the input bit for bit). Mutate the fields in place to
    schedule work.

    A struck sound has no steady pitch to edit, so the axes are time and
    amplitude and there is deliberately nothing else here -- this is not a
    :class:`NoteEdit` with fields removed, and the two models never refer to each
    other.

    Attributes:
        time_offset_samples: Moves the hit along the timeline; negative moves it
            earlier. A shift that pushes the signal past either end of the audio
            is truncated there rather than wrapped.
        gain_db: Level change applied to the hit. It scales the percussive
            component of the span, which is the signal ``peak_amplitude`` is
            measured on, not the source.
        muted: ``True`` silences the hit; the other fields then do not apply.
            What is silenced is the percussive component alone, so whatever was
            sustaining under the hit keeps sounding.

    Example:
        >>> events[0].edit = libsonare.PercussiveEventEdit(gain_db=-6.0)
        >>> events[1].edit.time_offset_samples = -441  # 20 ms earlier at 22050 Hz
    """

    time_offset_samples: int = 0
    gain_db: float = 0.0
    muted: bool = False


@dataclasses.dataclass
class PercussiveEvent:
    """One editable percussive event returned by :func:`extract_percussive_events`.

    Sample bounds are half-open into the source audio. The three measured figures
    are taken on the percussive component of the span rather than on the span
    itself, because that component is the signal an edit acts on.

    It carries no pitch and is never associated with a :class:`NoteObject`: the
    two models come from separate calls and do not refer to each other.

    :func:`render_percussive_events` reads ``onset_sample``, ``offset_sample``
    and ``edit`` only, so a host may hand back exactly what
    :func:`extract_percussive_events` produced, or build a bare
    ``PercussiveEvent(onset_sample=..., offset_sample=...)`` for a span it found
    some other way.

    The span has no default, so an event built by hand states it. The other
    surfaces declare the same two fields mandatory on their own event input and
    reject an omitted one; a default of 0 here would instead have been a
    zero-length span, rendering the event's edit as nothing.

    Attributes:
        onset_sample: First sample of the event, backtracked to the start of the
            transient rather than left where peak-picking landed.
        offset_sample: One past the last sample. The next onset closes a span;
            where none follows, ``max_event_ms`` does.
        strength: Detector strength at the onset, on the onset envelope's own
            scale. It orders events against each other and carries no absolute
            meaning.
        peak_amplitude: Peak absolute sample of the percussive component over the
            span, linear. This is the signal ``edit.gain_db`` scales.
        percussive_ratio: Share of the span's energy the separation assigned to
            percussion, in ``[0, 1]``; 0 when the span is silent. It describes the
            *span*, not the onset that opened it: an isolated hit sits near 1, but
            a real hit under a loud sustain sits near 0, because the sustain owns
            the span's energy. So it is not a test for whether a hit is there.
        edit: The pending :class:`PercussiveEventEdit`, identity on a freshly
            extracted event.
    """

    onset_sample: int
    offset_sample: int
    strength: float = 0.0
    peak_amplitude: float = 0.0
    percussive_ratio: float = 0.0
    edit: PercussiveEventEdit = dataclasses.field(default_factory=PercussiveEventEdit)


def _percussive_separation(
    fn_name: str,
    n_fft: int | None,
    hop_length: int | None,
    hpss_kernel_harmonic: int | None,
    hpss_kernel_percussive: int | None,
) -> dict[str, int]:
    """The separation fields both percussive configs carry; 0 keeps each default.

    Extraction measures events against this separation and rendering has to
    repeat it, so it is one set of fields both sides take rather than a framing
    each of them restates -- and one place the signed-32-bit narrowing is
    checked, since 0 is the default here and a value that wraps to it would
    separate on the default while reporting success.
    """
    fields = {
        "n_fft": n_fft,
        "hop_length": hop_length,
        "hpss_kernel_harmonic": hpss_kernel_harmonic,
        "hpss_kernel_percussive": hpss_kernel_percussive,
    }
    return {
        name: 0 if value is None else _validate_c_int_field(fn_name, value, name)
        for name, value in fields.items()
    }


def _percussive_event_from_c(row: SonarePercussiveEvent) -> PercussiveEvent:
    """Build one :class:`PercussiveEvent` from a C row, before it is released."""
    return PercussiveEvent(
        onset_sample=int(row.onset_sample),
        offset_sample=int(row.offset_sample),
        strength=float(row.strength),
        peak_amplitude=float(row.peak_amplitude),
        percussive_ratio=float(row.percussive_ratio),
        edit=PercussiveEventEdit(
            time_offset_samples=int(row.edit.time_offset_samples),
            gain_db=float(row.edit.gain_db),
            muted=bool(row.edit.muted),
        ),
    )


def _percussive_events_to_c(events: Sequence[PercussiveEvent]) -> tuple[object, int]:
    """Marshal an event list into the C event array; an empty set is NULL and 0."""
    count = len(events)
    if count == 0:
        return None, 0

    c_events = (SonarePercussiveEvent * count)()
    for i, event in enumerate(events):
        # The three measured figures are not marshalled: rendering reads the span
        # and the edit, so nothing on the far side reads them back.
        c_events[i].onset_sample = _narrow_int(
            event.onset_sample,
            f"render_percussive_events: events[{i}].onset_sample",
            _INT64_MIN,
            _INT64_MAX,
        )
        c_events[i].offset_sample = _narrow_int(
            event.offset_sample,
            f"render_percussive_events: events[{i}].offset_sample",
            _INT64_MIN,
            _INT64_MAX,
        )
        c_events[i].edit = SonarePercussiveEventEdit(
            # Narrowed rather than coerced: int(0.5) is 0, which is this field's
            # identity, so a sub-sample shift would render unmoved and report
            # success.
            time_offset_samples=_validate_c_int_field(
                "render_percussive_events",
                event.edit.time_offset_samples,
                f"events[{i}].edit.time_offset_samples",
            ),
            gain_db=float(event.edit.gain_db),
            muted=1 if event.edit.muted else 0,
        )
    return c_events, count


@_guard_buffer("samples")
def extract_percussive_events(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    *,
    n_fft: int | None = None,
    hop_length: int | None = None,
    hpss_kernel_harmonic: int | None = None,
    hpss_kernel_percussive: int | None = None,
    onset_wait: int | None = None,
    onset_delta: float | None = None,
    max_event_ms: float | None = None,
    min_percussive_ratio: float | None = None,
) -> list[PercussiveEvent]:
    """Extract editable percussive events from audio.

    Unlike :func:`extract_notes` this needs nothing but the audio: onsets are
    detected on the percussive component rather than on the source, so a harmonic
    attack is attenuated before the detector sees it instead of being filtered
    out afterwards. Each onset opens a span that the next one closes, and every
    returned event carries the identity :class:`PercussiveEventEdit`. Feed the
    result back to :func:`render_percussive_events` to hear the edits.

    Each onset is backtracked to the transient's start, which is not optional and
    is why there is no knob for it: peak-picking lands after the attack, and a
    span that opened there would report the next hit's peak and leave its own
    attack behind when muted.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        n_fft: FFT size the separation and the onset detector share; ``None``
            keeps the library default (2048). The pair must overlap-add --
            ``n_fft`` even and at least 2, ``hop_length`` no more than half of it
            -- because the separation inverts an STFT.
        hop_length: Hop the same two share; ``None`` keeps the default (512).
            They cannot be set apart: an event measured on one framing and lifted
            out on another is not the same signal.
        hpss_kernel_harmonic: Median filter length the separation runs along
            time; ``None`` keeps the default (31). A longer harmonic kernel calls
            more of a sustained sound harmonic.
        hpss_kernel_percussive: The same, along frequency; ``None`` keeps 31.
        onset_wait: Minimum frames between consecutive onsets; ``None`` keeps the
            default (1). Must be a whole number: 0 is the default's own spelling,
            so a fractional wait is refused rather than resolved onto it.
        onset_delta: Offset added to the detector's adaptive threshold; raising
            it finds fewer, stronger hits and lowering it finds more. ``None``
            keeps the default (0.06), so exactly 0 is the one value not
            selectable here -- a negative one is accepted and puts the threshold
            below the default, which is the direction a caller reaching for 0
            wanted anyway.

            It is in the units of :attr:`PercussiveEvent.strength`, the onset
            envelope's own scale, which is **not** normalised: three isolated
            hits measured 37 to 51 on one fixture, where the 0.06 default selects
            nothing at all. Read a useful value off the strengths a default
            extraction returns rather than guessing one -- a number chosen
            assuming the scale is around 1 looks like a knob that does nothing.
        max_event_ms: Caps a span that no onset follows; ``None`` keeps the
            default (500). It binds at the end of a phrase and at the end of the
            track, and nowhere else. A span never runs past the end of the audio
            whatever this says.
        min_percussive_ratio: Drops an event whose ``percussive_ratio`` falls
            below this; must be in ``[0, 1]``. ``None`` keeps the default, which
            is 0 -- and 0 is also the meaningful "keep everything", so unlike
            every other argument here passing it explicitly is not distinct from
            passing ``None``. Raising it is useful on material that is mostly
            drums and wrong on a dense mix, where it also drops real hits sitting
            over a loud sustain. Spans are fixed before it drops anything, so
            raising it selects events without lengthening the survivors.

    Returns:
        List of :class:`PercussiveEvent` in time order; empty when nothing was
        detected, which is reported rather than raised.

    Raises:
        SonareValueError: If ``samples`` is empty or non-finite, or a framing,
            kernel size or ``onset_wait`` is not a whole number fitting in a
            signed 32-bit integer.
        SonareError: If the C call rejects the request (a framing that breaks
            overlap-add, a negative ``onset_wait``, a negative or non-finite
            ``max_event_ms``, or a ``min_percussive_ratio`` outside ``[0, 1]``).
            0 is not rejected for any of these: it is the C ABI's spelling of the
            default.

    Example:
        >>> events = libsonare.extract_percussive_events(audio, sr)
        >>> events[0].edit.muted = True
        >>> without_the_first_hit = libsonare.render_percussive_events(audio, sr, events)
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_extract_percussive_events"):
        raise _unsupported_effect_symbol("sonare_extract_percussive_events")

    c_array, length = _to_c_float_array(samples)
    config = SonarePercussiveEventConfig(
        struct_version=_PERCUSSIVE_STRUCT_VERSION,
        **_percussive_separation(
            "extract_percussive_events",
            n_fft,
            hop_length,
            hpss_kernel_harmonic,
            hpss_kernel_percussive,
        ),
        # Narrowed rather than coerced: int(0.5) is the 0 this field reads as
        # "keep the default", so a fractional wait would run at the default and
        # report success.
        onset_wait=(
            0
            if onset_wait is None
            else _validate_c_int_field("extract_percussive_events", onset_wait, "onset_wait")
        ),
        onset_delta=0.0 if onset_delta is None else float(onset_delta),
        max_event_ms=0.0 if max_event_ms is None else float(max_event_ms),
        # 0 is this field's own meaning as well as its default, so it is passed
        # through rather than read as "leave the default".
        min_percussive_ratio=(0.0 if min_percussive_ratio is None else float(min_percussive_ratio)),
    )
    out = SonarePercussiveEventsResult()
    rc = lib.sonare_extract_percussive_events(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return [_percussive_event_from_c(out.events[i]) for i in range(out.count)]
    finally:
        lib.sonare_free_percussive_events(ctypes.byref(out))


@_guard_buffer("samples")
def render_percussive_events(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    events: Sequence[PercussiveEvent],
    *,
    n_fft: int | None = None,
    hop_length: int | None = None,
    hpss_kernel_harmonic: int | None = None,
    hpss_kernel_percussive: int | None = None,
    fade_ms: float | None = None,
) -> np.ndarray:
    """Render edited percussive events over their source audio.

    Per event the lifted signal is the percussive component over
    ``[onset_sample, offset_sample)`` under the tail fade. It is subtracted where
    it sits and, unless the event is muted, added back at the shifted position
    scaled by the gain. Only that signal moves, so muting a hit leaves the
    harmonic content under it sounding and moving one does not drag its
    neighbours' sustain along.

    Each event's span and ``edit`` are read; ``strength``, ``peak_amplitude`` and
    ``percussive_ratio`` are ignored, so the list
    :func:`extract_percussive_events` returned can be passed straight back. A set
    whose edits are all identity reproduces the input bit for bit and runs no
    separation at all.

    Overlap is checked on the *source* spans only. Where an edit's
    ``time_offset_samples`` lands an event is not, so two moved events may be
    written over each other. A shift that pushes the signal past either end is
    truncated there rather than wrapped.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        events: Events to render; an empty sequence returns the input unchanged.
            Every span must be non-empty and inside ``samples``, and no two may
            overlap -- which holds for an extracted set as produced.
        n_fft: The separation the events were measured against. Pass back what
            :func:`extract_percussive_events` was called with, or a different
            separation lifts a different signal out of the span than the one the
            events describe. ``None`` keeps the library default (2048).
        hop_length: The same; ``None`` keeps the default (512).
        hpss_kernel_harmonic: The same; ``None`` keeps the default (31).
        hpss_kernel_percussive: The same; ``None`` keeps the default (31).
        fade_ms: Fade-out at the tail of each lifted span; ``None`` keeps the
            library default (5 ms). There is deliberately no matching fade-in: a
            span opens in front of its transient, where the percussive component
            is near-silent, so cutting square there costs nothing and keeps a
            muted hit's attack from surviving inside a fade. A zero-length fade
            is unreachable here rather than rejected -- 0 selects the default --
            and a hard cut is not a thing to want anyway, because what the fade
            shapes is the signal being subtracted, so squaring it off leaves a
            step.

    Returns:
        ``numpy.ndarray`` of ``float32`` with the same length as the input.

    Raises:
        SonareValueError: If ``samples`` is empty or non-finite, or a framing or
            kernel size does not fit in a signed 32-bit integer.
        SonareError: If the C call rejects the request (an empty, reversed or
            out-of-range span, overlapping source spans, a non-finite gain, a
            framing that breaks overlap-add, or a negative or non-finite
            ``fade_ms`` -- 0 is the default rather than a rejected value). The
            framing and every span are validated even when every edit is the
            identity and no separation runs, so an unusable request is an error
            on every set rather than on the ones that reach the separation.

    Example:
        >>> events[1].edit.time_offset_samples = 441  # 20 ms later at 22050 Hz
        >>> events[2].edit.gain_db = -3.0
        >>> rendered = libsonare.render_percussive_events(audio, sr, events)
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_render_percussive_events"):
        raise _unsupported_effect_symbol("sonare_render_percussive_events")

    c_array, length = _to_c_float_array(samples)
    c_events, count = _percussive_events_to_c(events)
    config = SonarePercussiveRenderConfig(
        struct_version=_PERCUSSIVE_STRUCT_VERSION,
        **_percussive_separation(
            "render_percussive_events",
            n_fft,
            hop_length,
            hpss_kernel_harmonic,
            hpss_kernel_percussive,
        ),
        fade_ms=0.0 if fade_ms is None else float(fade_ms),
    )
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_render_percussive_events(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                c_events,
                _to_c_size_t(count, "count"),
                ctypes.byref(config),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)
