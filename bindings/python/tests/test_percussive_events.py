"""Tests for the percussive-event Python facade.

``extract_percussive_events`` / ``render_percussive_events`` are a ctypes
pass-through over the C ABI of the same names. Unlike the note-object facade
they need nothing but the audio, so the fixtures here synthesise struck sounds
rather than a pitch contour: what is under test is the marshalling, the keyword
defaults and the edit semantics, not the detector (which has its own coverage).

Every synthesised hit carries its own noise seed and its own peak, because a flat
fixture lets a span that read a neighbour's samples match anyway. The layered
fixture exists for the same reason on the other axis: ``percussive_ratio``
describes a span's energy balance rather than classifying a hit, so it is only
tested once a hit under a loud sustain and the identical hit in silence are both
in play.
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare
from libsonare import SonareError, SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
# The library defaults, which the fixture positions below are reasoned in.
DEFAULT_MAX_EVENT_MS = 500.0
DEFAULT_MAX_EVENT_SAMPLES = 11025  # 500 ms at 22050 Hz, exactly.

# 60 ms at 22050 Hz, exactly, decaying over 12 ms.
HIT_SAMPLES = 1323
HIT_DECAY_MS = 12.0

# Three hits 400 ms apart at descending, distinct peaks: inside the 500 ms cap,
# so the cap binds on the last span only.
THREE_HIT_STARTS = (4410, 13230, 22050)
THREE_HIT_PEAKS = (0.50, 0.34, 0.22)
THREE_HIT_SECONDS = 2.0


def _noise(seed: int, n: int) -> NDArray[np.float64]:
    """Deterministic uniform noise in [-1, 1), seeded per hit."""
    return np.random.default_rng(seed).uniform(-1.0, 1.0, n)


def _hit(seed: int, peak: float) -> NDArray[np.float32]:
    """An exponentially decaying noise burst, normalised to exactly ``peak``.

    The seed is per hit so no two synthesised hits carry the same samples: a span
    that read a neighbour's audio cannot then match. The normalisation makes the
    synthesised level an analytic anchor for ``peak_amplitude``.
    """
    decay = HIT_DECAY_MS * 0.001 * SR
    burst = _noise(seed, HIT_SAMPLES) * np.exp(-np.arange(HIT_SAMPLES) / decay)
    return (peak * burst / np.max(np.abs(burst))).astype(np.float32)


def _with_hits(
    samples: NDArray[np.float32], specs: tuple[tuple[int, int, float], ...]
) -> NDArray[np.float32]:
    """Add ``(start, seed, peak)`` bursts into a copy of ``samples``."""
    out = samples.copy()
    for start, seed, peak in specs:
        burst = _hit(seed, peak)
        stop = min(start + len(burst), len(out))
        out[start:stop] += burst[: stop - start]
    return out


def _three_hits() -> NDArray[np.float32]:
    """Three isolated hits at descending, distinct levels, 400 ms apart."""
    return _with_hits(
        np.zeros(int(THREE_HIT_SECONDS * SR), dtype=np.float32),
        tuple(
            (start, seed, peak)
            for seed, (start, peak) in enumerate(
                zip(THREE_HIT_STARTS, THREE_HIT_PEAKS, strict=True), start=1
            )
        ),
    )


def _two_hits() -> NDArray[np.float32]:
    """Two isolated hits inside one second: the compact fixture the sweeps run on."""
    return _with_hits(np.zeros(SR, dtype=np.float32), ((3528, 31, 0.50), (12348, 32, 0.30)))


# Two hits 1.3 s apart, which is further than the 500 ms cap, so the first span
# is the cap exactly and there is room after it to move the hit into.
SPACED_HIT_STARTS = (6615, 35280)
SPACED_HIT_SECONDS = 2.2


def _spaced_hits() -> NDArray[np.float32]:
    """Two isolated hits far enough apart to move the first one clear of the second."""
    return _with_hits(
        np.zeros(int(SPACED_HIT_SECONDS * SR), dtype=np.float32),
        ((SPACED_HIT_STARTS[0], 41, 0.50), (SPACED_HIT_STARTS[1], 42, 0.32)),
    )


# 2 ms at 22050 Hz: short enough that the attack still trips the detector and
# long enough that the buffer carries no step discontinuity, so a sustained span
# reads as harmonic rather than as one long click.
NOTE_EDGE_SAMPLES = 44


def _note(n: int, start: int, stop: int, hz: float, amp: float) -> NDArray[np.float32]:
    """A sustained sine over ``[start, stop)`` of an ``n``-sample buffer.

    Each end is tapered by a raised-cosine edge, without which the square cut
    reads as a transient and the span stops being the harmonic reference the
    ``percussive_ratio`` cases compare against.
    """
    out = np.zeros(n, dtype=np.float32)
    position = np.arange(stop - start)
    envelope = np.ones(stop - start)
    taper = 0.5 - 0.5 * np.cos(np.pi * np.arange(NOTE_EDGE_SAMPLES) / NOTE_EDGE_SAMPLES)
    envelope[:NOTE_EDGE_SAMPLES] = taper
    envelope[-NOTE_EDGE_SAMPLES:] = taper[::-1]
    out[start:stop] = amp * envelope * np.sin(2.0 * np.pi * hz * position / SR)
    return out


# A quiet hit on top of a loud sustained note, plus the same hit with nothing
# under it. The only difference at the hit's position is what sustains through
# it, which is what percussive_ratio actually responds to.
LAYERED_HIT_START = SR  # 1.0 s in.
LAYERED_HIT_PEAK = 0.15
LAYERED_NOTE_AMPLITUDE = 0.8
LAYERED_NOTE_HZ = 440.0
LAYERED_SAMPLES = 2 * SR


def _layered() -> tuple[NDArray[np.float32], NDArray[np.float32], NDArray[np.float32]]:
    """``(mixed, note_only, hit_only)`` over the same two seconds."""
    note = _note(LAYERED_SAMPLES, 0, LAYERED_SAMPLES, LAYERED_NOTE_HZ, LAYERED_NOTE_AMPLITUDE)
    one_hit = ((LAYERED_HIT_START, 21, LAYERED_HIT_PEAK),)
    return (
        _with_hits(note, one_hit),
        note,
        _with_hits(np.zeros_like(note), one_hit),
    )


# A sustained note and, well after it has ended, one isolated hit. The two onsets
# are the same kind of event to the detector and opposite kinds to the
# separation, which is the spread a ratio threshold needs in order to select.
NOTE_THEN_HIT_NOTE = (8820, 44100)
NOTE_THEN_HIT_HIT_START = 52920
NOTE_THEN_HIT_SAMPLES = 3 * SR


def _note_then_hit() -> NDArray[np.float32]:
    """A 1.6 s note, then silence, then one isolated hit."""
    return _with_hits(
        _note(NOTE_THEN_HIT_SAMPLES, *NOTE_THEN_HIT_NOTE, 330.0, 0.8),
        ((NOTE_THEN_HIT_HIT_START, 11, 0.50),),
    )


def _rms(samples: NDArray[np.float32]) -> float:
    return float(np.sqrt(np.mean(np.asarray(samples, dtype=np.float64) ** 2)))


def _peak(samples: NDArray[np.float32]) -> float:
    return float(np.max(np.abs(samples)))


def _covering(events: list[libsonare.PercussiveEvent], sample: int) -> libsonare.PercussiveEvent:
    """The event whose span covers ``sample``; there is exactly one."""
    covering = [e for e in events if e.onset_sample <= sample < e.offset_sample]
    assert len(covering) == 1, f"{len(covering)} spans cover sample {sample}"
    return covering[0]


def _assert_edited(out: NDArray[np.float32], source: NDArray[np.float32]) -> None:
    """Assert the edit moved the output off ``source`` and left a signal behind.

    A bare difference check is a one-sided bound: silence differs from the source
    by the source's own peak and so passes one, and so does a blow-up. The
    amplitude band is what rules those two out.
    """
    assert _peak(out - source) > 0.01
    assert 0.5 * _peak(source) < _peak(out) < 2.0 * _peak(source)


def _extract_three() -> tuple[NDArray[np.float32], list[libsonare.PercussiveEvent]]:
    audio = _three_hits()
    events = libsonare.extract_percussive_events(audio, SR)
    # Three struck sounds in silence: the detector finds each, and the spans are
    # asserted rather than tolerated so every case below names one hit uniquely.
    assert len(events) == 3
    for event, start in zip(events, THREE_HIT_STARTS, strict=True):
        # The onset is backtracked, so it sits in front of the transient and the
        # span closes past it.
        assert event.onset_sample <= start
        assert event.offset_sample > start + HIT_SAMPLES
    return audio, events


# --- Extraction -------------------------------------------------------------


def test_extracted_events_are_well_formed_and_carry_no_edit() -> None:
    audio, events = _extract_three()
    previous_offset = 0
    for event in events:
        assert event.onset_sample >= previous_offset
        assert event.offset_sample > event.onset_sample
        assert event.offset_sample <= len(audio)
        previous_offset = event.offset_sample
        assert event.strength > 0.0
        assert event.peak_amplitude > 0.0
        assert 0.0 <= event.percussive_ratio <= 1.0
        assert event.edit == libsonare.PercussiveEventEdit()
        assert event.edit.time_offset_samples == 0
        assert event.edit.gain_db == 0.0
        assert event.edit.muted is False


def test_a_span_closes_on_the_next_onset_and_the_last_one_on_the_cap() -> None:
    audio, events = _extract_three()
    # Adjacent exactly: the interior spans are closed by the following onset and
    # nothing else, so there is no gap and no rounding to absorb.
    assert events[0].offset_sample == events[1].onset_sample
    assert events[1].offset_sample == events[2].onset_sample
    # The hits are 400 ms apart, inside the 500 ms cap, so it binds on the last.
    assert events[0].offset_sample - events[0].onset_sample < DEFAULT_MAX_EVENT_SAMPLES
    assert events[2].offset_sample - events[2].onset_sample == DEFAULT_MAX_EVENT_SAMPLES
    assert events[2].offset_sample < len(audio)


def test_each_event_measures_its_own_hit() -> None:
    # The three hits are synthesised at 0.50, 0.34 and 0.22 peak with different
    # noise seeds, so a span that measured a neighbour, or measured the whole
    # buffer, cannot reproduce this ordering.
    _, events = _extract_three()
    assert events[0].peak_amplitude > events[1].peak_amplitude
    assert events[1].peak_amplitude > events[2].peak_amplitude

    # An isolated hit is nearly all percussive, so the measured peak sits just
    # under the synthesised one rather than anywhere below it.
    assert 0.25 < events[0].peak_amplitude <= 0.55
    # And the ratios track the synthesised ones, which pins the figures to their
    # own hits rather than only ordering them.
    assert events[0].peak_amplitude / events[1].peak_amplitude == pytest.approx(
        THREE_HIT_PEAKS[0] / THREE_HIT_PEAKS[1], abs=0.45
    )

    # Isolated struck sounds, so the separation has to call them percussive. The
    # complementary half -- that the figure is not pinned at its ceiling for
    # everything -- is the layered case below.
    for event in events:
        assert event.percussive_ratio > 0.5


def test_percussive_ratio_describes_the_span_not_the_hit() -> None:
    # The identical hit, same seed and same level, read at opposite ends of the
    # figure: near 1 in silence and near 0 under a loud sustain, because the
    # sustain owns the span's energy. A low ratio is not "no hit here", which the
    # peak measured on the buried one says outright.
    mixed, _, hit_only = _layered()
    buried = _covering(libsonare.extract_percussive_events(mixed, SR), LAYERED_HIT_START)
    isolated = _covering(libsonare.extract_percussive_events(hit_only, SR), LAYERED_HIT_START)

    assert buried.percussive_ratio < 0.05
    assert isolated.percussive_ratio > 0.5
    assert buried.peak_amplitude > 0.3 * LAYERED_HIT_PEAK

    # peak_amplitude is measured on the percussive component, not on the source:
    # the source over that span is dominated by the 0.8 note, an order above the
    # hit's own peak, so the answer says which signal was measured.
    span = mixed[buried.onset_sample : buried.offset_sample]
    assert _peak(span) > 0.75
    assert buried.peak_amplitude < 0.35


def test_extract_percussive_events_reports_silence_as_an_empty_set() -> None:
    # Nothing detected is a measurement that came up empty, not a bad argument.
    assert libsonare.extract_percussive_events(np.zeros(SR, dtype=np.float32), SR) == []


# --- Every keyword argument reaches the C struct ----------------------------


def test_max_event_ms_reaches_the_config() -> None:
    audio, relaxed = _extract_three()
    # 40 ms is 882 samples at this rate, exactly, and shorter than the 400 ms
    # between hits, so it now closes every span including the interior ones.
    capped = libsonare.extract_percussive_events(audio, SR, max_event_ms=40.0)
    assert len(capped) == len(relaxed)
    for tight, loose in zip(capped, relaxed, strict=True):
        # The cap moves an offset and never an onset.
        assert tight.onset_sample == loose.onset_sample
        assert tight.offset_sample - tight.onset_sample == 882

    # None and the library default are the same request, so the value above is
    # the argument arriving rather than any non-default value perturbing things.
    explicit = libsonare.extract_percussive_events(audio, SR, max_event_ms=DEFAULT_MAX_EVENT_MS)
    assert explicit == relaxed


def test_min_percussive_ratio_selects_without_moving_a_span() -> None:
    # The defect this guards against is filtering the onsets and then closing the
    # spans, which silently lengthens every survivor that had a dropped
    # neighbour. Selection happens after measurement, so the survivors are the
    # very same events -- compared here field for field, not approximately.
    #
    # The note-then-hit fixture rather than the layered one: the figure describes
    # a span, so a spread only exists where the two spans differ in what sustains
    # through them. Under one continuous note every span reads low and there is no
    # threshold that selects anything.
    audio = _note_then_hit()
    all_events = libsonare.extract_percussive_events(audio, SR)
    ratios = [e.percussive_ratio for e in all_events]
    # Without a spread there is no threshold that selects, and the case is vacuous.
    assert max(ratios) - min(ratios) > 0.2

    threshold = 0.5 * (min(ratios) + max(ratios))
    kept = libsonare.extract_percussive_events(audio, SR, min_percussive_ratio=threshold)
    expected = [e for e in all_events if e.percussive_ratio >= threshold]
    assert 0 < len(expected) < len(all_events)
    assert kept == expected

    # 0 is both the default and "keep everything", so unlike every other keyword
    # here it is not distinguishable from None -- stated rather than left to be
    # discovered, because the C field is assigned as-is.
    assert libsonare.extract_percussive_events(audio, SR, min_percussive_ratio=0.0) == all_events
    # And 1.0 is inside the range rather than an error.
    at_ceiling = libsonare.extract_percussive_events(audio, SR, min_percussive_ratio=1.0)
    assert all(e.percussive_ratio >= 1.0 for e in at_ceiling)


def test_onset_delta_reaches_the_config() -> None:
    # Raising the detector's threshold offset finds fewer, stronger hits.
    #
    # The offset is in the units of the events' own `strength`, which is the
    # onset envelope's scale and is NOT normalised -- these three hits measure in
    # the tens. So the value is derived from the strengths the extraction just
    # reported rather than hard-coded: a literal picked by eye stops selecting
    # the moment the envelope's scale moves, and does so silently, because a
    # delta below the signal is indistinguishable from a delta nobody read.
    audio, relaxed = _extract_three()
    strengths = sorted(e.strength for e in relaxed)
    # Between the two strongest, so it keeps the top hit and drops the rest.
    fewer = libsonare.extract_percussive_events(
        audio, SR, onset_delta=0.5 * (strengths[-2] + strengths[-1])
    )
    assert 0 < len(fewer) < len(relaxed)

    # Both halves of the trap, stated so the scale is not rediscovered: the
    # library default reaches the same result as None, and a value chosen
    # assuming the scale is around 1 sits orders of magnitude below the signal
    # and selects nothing at all.
    assert libsonare.extract_percussive_events(audio, SR, onset_delta=0.06) == relaxed
    assert len(libsonare.extract_percussive_events(audio, SR, onset_delta=0.5)) == len(relaxed)

    # Exactly 0 is the one value not selectable, but a negative one is accepted
    # and puts the threshold below the default rather than being rejected.
    assert len(libsonare.extract_percussive_events(audio, SR, onset_delta=-10.0)) >= len(relaxed)


def test_onset_wait_reaches_the_config() -> None:
    # A minimum spacing wider than the gap between the hits merges them into one
    # detection; the 400 ms gap is 17 hops, so 64 frames comfortably covers it.
    audio, relaxed = _extract_three()
    assert len(libsonare.extract_percussive_events(audio, SR, onset_wait=64)) < len(relaxed)
    assert libsonare.extract_percussive_events(audio, SR, onset_wait=1) == relaxed


def test_a_fractional_onset_wait_is_refused_rather_than_truncated() -> None:
    audio, relaxed = _extract_three()

    # Positive control, so a guard that refused every value could not pass: a
    # whole-number wait is accepted and returns a different set from the default.
    spaced = libsonare.extract_percussive_events(audio, SR, onset_wait=64)
    assert spaced != relaxed

    # The outcome is recorded rather than asserted directly, because the claim is
    # not "it raises". int(-0.5) is the 0 this field reads as "keep the default",
    # so the defect's signature is a success returning exactly `relaxed`, which
    # an accepted row names.
    outcomes = []
    for onset_wait in (0.5, -0.5, 0.9, float("nan")):
        try:
            events = libsonare.extract_percussive_events(audio, SR, onset_wait=onset_wait)
        except SonareValueError as error:
            # The field, not merely "it raised": a rejection for an unrelated
            # reason reads identically without it, and the sibling surfaces name
            # the field in the same words.
            named = "onset_wait" in str(error)
            outcomes.append("refused" if named else f"refused, unnamed: {error}")
        else:
            outcomes.append(f"accepted, default set: {events == relaxed}")
    assert outcomes == ["refused"] * 4


def test_the_separation_fields_reach_both_configs() -> None:
    # One separation, two structs: extraction measures against it and rendering
    # repeats it, so each field is checked on both sides.
    audio = _two_hits()
    baseline = libsonare.extract_percussive_events(audio, SR)
    assert len(baseline) == 2

    # hpss_kernel_percussive is deliberately not in this loop -- it has its own
    # case below, on a fixture that can show it.
    for field, value in (
        ("n_fft", 1024),
        ("hop_length", 256),
        ("hpss_kernel_harmonic", 15),
    ):
        moved = libsonare.extract_percussive_events(audio, SR, **{field: value})
        # A different separation is a different measurement of the same audio.
        assert [e.peak_amplitude for e in moved] != [e.peak_amplitude for e in baseline], field

        # On the render side the same field decides which signal is lifted out of
        # the span, so a gain change renders differently under it.
        edited = [libsonare.PercussiveEvent(e.onset_sample, e.offset_sample) for e in baseline]
        edited[0].edit.gain_db = -12.0
        at_default = libsonare.render_percussive_events(audio, SR, edited)
        at_value = libsonare.render_percussive_events(audio, SR, edited, **{field: value})
        assert not np.array_equal(at_default, at_value), field
        # Both are edits rather than one of them being a no-op.
        _assert_edited(at_default, audio)
        _assert_edited(at_value, audio)

    # The library defaults spelled out reach the same result as None, so the
    # inequalities above are the arguments arriving and not noise.
    assert (
        libsonare.extract_percussive_events(
            audio,
            SR,
            n_fft=2048,
            hop_length=512,
            hpss_kernel_harmonic=31,
            hpss_kernel_percussive=31,
        )
        == baseline
    )


def test_hpss_kernel_percussive_reaches_both_configs() -> None:
    # Split out of the loop above because the compact fixture cannot show this
    # field at all: isolated bursts in silence drive the soft mask to saturation,
    # so the percussive component is the source whatever the vertical kernel is
    # and every reading sits at its ceiling -- peaks unchanged, ratios 1.0, for
    # kernels from 3 to 63. A field whose every input reads the same is untested,
    # which is the same reason the percussive_ratio case uses this fixture.
    #
    # Under a sustain the mask is not saturated and the kernel decides how much
    # of the span it calls percussive. 3 against the default 31: a nearby value
    # does not separate either, because 15 and 63 land within a hair of 31.
    mixed, _, _ = _layered()
    at_default = _covering(libsonare.extract_percussive_events(mixed, SR), LAYERED_HIT_START)
    narrow = _covering(
        libsonare.extract_percussive_events(mixed, SR, hpss_kernel_percussive=3),
        LAYERED_HIT_START,
    )
    # A shorter kernel calls more of the span percussive, so both measurements
    # rise together -- and from a floor rather than between two ceilings.
    assert at_default.percussive_ratio < 0.01 < narrow.percussive_ratio
    assert narrow.peak_amplitude > 2.0 * at_default.peak_amplitude

    # And on the render side, where the same field decides which signal is lifted
    # out of the span.
    edited = libsonare.PercussiveEvent(at_default.onset_sample, at_default.offset_sample)
    edited.edit.muted = True
    by_default = libsonare.render_percussive_events(mixed, SR, [edited])
    by_narrow = libsonare.render_percussive_events(mixed, SR, [edited], hpss_kernel_percussive=3)
    assert not np.array_equal(by_default, by_narrow)
    # Both lift something rather than one of them being a no-op.
    for rendered in (by_default, by_narrow):
        _assert_edited(rendered, mixed)


def test_fade_ms_reaches_the_render_config() -> None:
    audio, events = _extract_three()
    events[0].edit.muted = True

    default = libsonare.render_percussive_events(audio, SR, events)
    long_fade = libsonare.render_percussive_events(audio, SR, events, fade_ms=50.0)
    assert not np.array_equal(default, long_fade)
    np.testing.assert_array_equal(
        libsonare.render_percussive_events(audio, SR, events, fade_ms=5.0), default
    )
    # 0 is the C ABI's spelling of the default, so it reaches the default rather
    # than selecting a hard cut -- a zero-length fade is not selectable from here.
    np.testing.assert_array_equal(
        libsonare.render_percussive_events(audio, SR, events, fade_ms=0.0), default
    )
    # Both mute the hit rather than one of them being a no-op.
    for rendered in (default, long_fade):
        _assert_edited(rendered, audio)


# --- Rendering --------------------------------------------------------------


def test_an_identity_set_reproduces_the_input_bit_for_bit() -> None:
    audio, events = _extract_three()
    rendered = libsonare.render_percussive_events(audio, SR, events)
    assert rendered.shape == audio.shape
    assert rendered.dtype == np.float32
    np.testing.assert_array_equal(rendered, audio)


def test_an_empty_set_reproduces_the_input_bit_for_bit() -> None:
    audio = _two_hits()
    np.testing.assert_array_equal(libsonare.render_percussive_events(audio, SR, []), audio)


def test_a_hand_built_event_needs_only_a_span_and_an_edit() -> None:
    # render_percussive_events reads onset_sample, offset_sample and edit only,
    # so a host that found a span some other way can build the event itself.
    audio = _three_hits()
    # Up to the second hit, so muting takes the loudest hit and leaves the other
    # two -- the peak the amplitude band below is measured against.
    cut = THREE_HIT_STARTS[1]
    span = libsonare.PercussiveEvent(onset_sample=0, offset_sample=cut)
    assert span.edit == libsonare.PercussiveEventEdit()
    np.testing.assert_array_equal(libsonare.render_percussive_events(audio, SR, [span]), audio)

    span.edit.muted = True
    rendered = libsonare.render_percussive_events(audio, SR, [span])
    _assert_edited(rendered, audio)
    # The first hit is gone from its own window ...
    first = slice(THREE_HIT_STARTS[0], THREE_HIT_STARTS[0] + HIT_SAMPLES)
    assert _rms(rendered[first]) < 0.4 * _rms(audio[first])
    # ... and nothing outside the span moved at all.
    np.testing.assert_array_equal(rendered[cut:], audio[cut:])


def test_a_round_trip_changes_only_the_edit() -> None:
    # The shape a host actually holds: extract, touch nothing but `edit`, render.
    audio, events = _extract_three()
    before = [
        (e.onset_sample, e.offset_sample, e.strength, e.peak_amplitude, e.percussive_ratio)
        for e in events
    ]
    events[1].edit = libsonare.PercussiveEventEdit(gain_db=-12.0)
    rendered = libsonare.render_percussive_events(audio, SR, events)

    assert [
        (e.onset_sample, e.offset_sample, e.strength, e.peak_amplitude, e.percussive_ratio)
        for e in events
    ] == before
    _assert_edited(rendered, audio)

    # Only the edited event's span moved; the others are untouched bit for bit.
    for index in (0, 2):
        lo, hi = events[index].onset_sample, events[index].offset_sample
        np.testing.assert_array_equal(rendered[lo:hi], audio[lo:hi])


def test_muting_a_hit_leaves_the_sustain_under_it_sounding() -> None:
    # The property the whole separation exists for, so both halves are asserted:
    # the hit goes and the note stays, at its own level.
    mixed, note_only, _ = _layered()
    event = _covering(libsonare.extract_percussive_events(mixed, SR), LAYERED_HIT_START)
    event.edit.muted = True
    rendered = libsonare.render_percussive_events(mixed, SR, [event])
    assert rendered.shape == mixed.shape

    hit_window = slice(LAYERED_HIT_START, LAYERED_HIT_START + HIT_SAMPLES)
    # The "before" figure is the synthesised peak exactly, recovered by
    # subtracting the note-only reference.
    before = _peak(mixed[hit_window] - note_only[hit_window])
    assert before == pytest.approx(LAYERED_HIT_PEAK, abs=1e-5)
    assert _peak(rendered[hit_window] - note_only[hit_window]) < 0.5 * before

    # The note is still there at its own level, bounded from both sides rather
    # than merely "not silent". A 0.8 sine has RMS 0.8 / sqrt(2).
    span = slice(event.onset_sample, event.offset_sample)
    note_rms = LAYERED_NOTE_AMPLITUDE / np.sqrt(2.0)
    assert _rms(rendered[span]) == pytest.approx(note_rms, rel=0.10)

    # Nothing outside the span moved at all.
    np.testing.assert_array_equal(rendered[: event.onset_sample], mixed[: event.onset_sample])
    np.testing.assert_array_equal(rendered[event.offset_sample :], mixed[event.offset_sample :])


def test_a_time_offset_moves_the_hits_energy() -> None:
    # The spaced fixture rather than the three-hit one: shifting an event by its
    # own span has to land somewhere empty, or the arrival window reads the next
    # hit's energy and passes whether or not anything moved.
    audio = _spaced_hits()
    events = libsonare.extract_percussive_events(audio, SR)
    assert len(events) == 2
    event = events[0]
    span = event.offset_sample - event.onset_sample
    # The next onset is further off than the cap, so the span is the 500 ms cap
    # and the destination is adjacent, disjoint, and well clear of the second hit.
    assert span == DEFAULT_MAX_EVENT_SAMPLES
    destination = SPACED_HIT_STARTS[0] + span
    assert destination + HIT_SAMPLES < SPACED_HIT_STARTS[1]

    event.edit.time_offset_samples = span
    rendered = libsonare.render_percussive_events(audio, SR, [event])

    old = slice(SPACED_HIT_STARTS[0], SPACED_HIT_STARTS[0] + HIT_SAMPLES)
    new = slice(destination, destination + HIT_SAMPLES)
    source_level = _rms(audio[old])
    assert source_level > 0.0
    # Nothing was at the destination before the move, so its arrival level is the
    # moved signal and nothing else.
    assert _rms(audio[new]) < 0.01 * source_level
    # Energy leaves the old position ...
    assert _rms(rendered[old]) < 0.4 * source_level
    # ... and arrives at the new one, at the level it left with. Bounded above as
    # well: an arrival at ten times the level is not a move.
    assert 0.5 * source_level < _rms(rendered[new]) < 1.4 * source_level

    # Nothing before the span is written, which is where a wrap would land, and
    # the second hit is untouched bit for bit.
    np.testing.assert_array_equal(rendered[: event.onset_sample], audio[: event.onset_sample])
    np.testing.assert_array_equal(
        rendered[event.offset_sample + span :], audio[event.offset_sample + span :]
    )


def test_a_shift_past_the_end_is_truncated_rather_than_wrapped() -> None:
    audio, events = _extract_three()
    muted = libsonare.PercussiveEvent(events[0].onset_sample, events[0].offset_sample)
    muted.edit.muted = True
    silenced = libsonare.render_percussive_events(audio, SR, [muted])

    # Pushed clean past an end nothing arrives, so the result is the muted render
    # exactly. That is also the statement that nothing wrapped around.
    for offset in (len(audio), -len(audio)):
        shifted = libsonare.PercussiveEvent(events[0].onset_sample, events[0].offset_sample)
        shifted.edit.time_offset_samples = offset
        rendered = libsonare.render_percussive_events(audio, SR, [shifted])
        assert rendered.shape == audio.shape
        np.testing.assert_array_equal(rendered, silenced)

    # And the muted render is itself an edit, so the equalities above are not two
    # pass-throughs agreeing with each other.
    _assert_edited(silenced, audio)


def test_gain_scales_the_lifted_signal_by_a_known_factor() -> None:
    audio, events = _extract_three()

    def render_with(gain_db: float, muted: bool) -> NDArray[np.float32]:
        one = libsonare.PercussiveEvent(events[0].onset_sample, events[0].offset_sample)
        one.edit.gain_db = gain_db
        one.edit.muted = muted
        return libsonare.render_percussive_events(audio, SR, [one])

    # The render is source + (gain - 1) * lifted, so a difference against the
    # source is the lifted signal at a known scale: +1 at +6.02 dB, -1/2 at
    # -6.02 dB, -1 when muted. Those relations are exact whatever the lifted
    # signal turns out to be, so the tolerance is float rounding and nothing more.
    up = render_with(6.0206, False) - audio
    down = render_with(-6.0206, False) - audio
    gone = render_with(0.0, True) - audio
    assert _peak(up + 2.0 * down) < 1e-5
    assert _peak(up + gone) < 1e-5

    # The lifted signal is the hit and not a sliver of it, against the peak the
    # fixture synthesised.
    assert 0.3 * THREE_HIT_PEAKS[0] < _peak(up) < 1.2 * THREE_HIT_PEAKS[0]

    # Direction and rough size over the hit itself, bounded from both sides.
    window = slice(THREE_HIT_STARTS[0], THREE_HIT_STARTS[0] + HIT_SAMPLES)
    source_level = _rms(audio[window])
    assert 1.3 < _rms(render_with(6.0206, False)[window]) / source_level < 2.05
    assert 0.45 < _rms(render_with(-6.0206, False)[window]) / source_level < 0.85


def test_muted_discards_the_gain() -> None:
    # "The other fields then do not apply", so a muted event renders the same
    # whatever gain it carries.
    audio, events = _extract_three()

    def render_muted(gain_db: float) -> NDArray[np.float32]:
        one = libsonare.PercussiveEvent(events[0].onset_sample, events[0].offset_sample)
        one.edit.gain_db = gain_db
        one.edit.muted = True
        return libsonare.render_percussive_events(audio, SR, [one])

    silenced = render_muted(0.0)
    np.testing.assert_array_equal(render_muted(12.0), silenced)
    np.testing.assert_array_equal(render_muted(-12.0), silenced)
    _assert_edited(silenced, audio)


# --- Validation -------------------------------------------------------------


def test_extract_percussive_events_rejects_invalid_arguments() -> None:
    audio = _two_hits()

    # The buffer guard reports an empty or non-finite input by name, ahead of the
    # C ABI's generic invalid-parameter return.
    with pytest.raises(SonareValueError, match="extract_percussive_events"):
        libsonare.extract_percussive_events(np.zeros(0, dtype=np.float32), SR)
    with pytest.raises(SonareValueError, match="extract_percussive_events"):
        libsonare.extract_percussive_events(np.full(SR, np.nan, dtype=np.float32), SR)

    with pytest.raises(SonareError):
        libsonare.extract_percussive_events(audio, 0)

    # The separation inverts an STFT, so a framing that cannot be overlap-added
    # back is an error: n_fft even and at least 2, hop_length in (0, n_fft / 2].
    for kwargs in (
        {"n_fft": 2047},
        {"n_fft": -2048},
        {"hop_length": -512},
        {"n_fft": 2048, "hop_length": 2048},
        {"hpss_kernel_harmonic": -1},
        {"hpss_kernel_percussive": -1},
        {"onset_wait": -1},
        {"max_event_ms": -1.0},
        {"max_event_ms": np.nan},
        {"min_percussive_ratio": -0.01},
        {"min_percussive_ratio": 1.01},
        {"min_percussive_ratio": np.inf},
    ):
        with pytest.raises(SonareError):
            libsonare.extract_percussive_events(audio, SR, **kwargs)

    # Positive controls: a hop of exactly half the window is the inclusive end of
    # the overlap rule, and both ends of the ratio range are inside it, so the
    # rejections above are the rules biting rather than "any non-default value".
    for kwargs in (
        {"n_fft": 2048, "hop_length": 1024},
        {"n_fft": 1024, "hop_length": 512},
        {"min_percussive_ratio": 0.0},
        {"min_percussive_ratio": 1.0},
    ):
        assert isinstance(libsonare.extract_percussive_events(audio, SR, **kwargs), list)


def test_render_percussive_events_rejects_invalid_arguments() -> None:
    audio = _two_hits()
    length = len(audio)

    def edited(onset: int, offset: int) -> libsonare.PercussiveEvent:
        event = libsonare.PercussiveEvent(onset_sample=onset, offset_sample=offset)
        event.edit.gain_db = -3.0
        return event

    with pytest.raises(SonareValueError, match="render_percussive_events"):
        libsonare.render_percussive_events(np.zeros(0, dtype=np.float32), SR, [edited(0, 1600)])

    # An empty span has nothing to lift, a reversed one is not a span, and
    # neither end may sit outside the audio.
    for onset, offset in ((4410, 4410), (8820, 4410), (-512, 4410), (4410, length + 1)):
        with pytest.raises(SonareError):
            libsonare.render_percussive_events(audio, SR, [edited(onset, offset)])

    # Overlapping source spans are not a renderable set; touching ones are.
    with pytest.raises(SonareError):
        libsonare.render_percussive_events(audio, SR, [edited(0, 11025), edited(5512, 16537)])
    assert (
        libsonare.render_percussive_events(
            audio, SR, [edited(0, 11025), edited(11025, 22050)]
        ).shape
        == audio.shape
    )

    # A non-finite gain is not the identity edit, so it reaches validation rather
    # than being waved through as "changes nothing".
    for gain_db in (np.nan, np.inf, -np.inf):
        event = edited(4410, 15435)
        event.edit.gain_db = float(gain_db)
        with pytest.raises(SonareError):
            libsonare.render_percussive_events(audio, SR, [event])

    # The renderer repeats the extraction's separation, so it is under the same
    # overlap-add rule, and a negative or non-finite fade is not a fade.
    for kwargs in (
        {"fade_ms": -1.0},
        {"fade_ms": np.nan},
        {"fade_ms": np.inf},
        {"n_fft": 2047},
        {"hop_length": -512},
        {"n_fft": 2048, "hop_length": 1025},
    ):
        with pytest.raises(SonareError):
            libsonare.render_percussive_events(audio, SR, [edited(4410, 15435)], **kwargs)

    # Positive control: the same event with nothing poisoned renders, so none of
    # the above passes by rejecting every render.
    assert libsonare.render_percussive_events(audio, SR, [edited(4410, 15435)]).shape == audio.shape


def test_a_hand_built_event_has_to_state_its_span() -> None:
    # The span has no default, so an event built without one does not exist to be
    # rendered. A default of 0 made the two bounds equal, and a zero-length span
    # renders as nothing: the edit below would have been dropped in silence while
    # the call reported success, which is what the sibling surfaces reject.
    audio = _two_hits()
    with pytest.raises(TypeError):
        libsonare.PercussiveEvent()  # type: ignore[call-arg]

    stated = libsonare.PercussiveEvent(onset_sample=4410, offset_sample=15435)
    stated.edit.muted = True
    rendered = libsonare.render_percussive_events(audio, SR, [stated])
    # The control: the span that IS stated reaches the render and changes it.
    assert rendered.shape == audio.shape
    assert not np.array_equal(rendered[4410:15435], audio[4410:15435])
    np.testing.assert_array_equal(rendered[15435:], audio[15435:])


def test_a_broken_framing_is_rejected_even_for_an_identity_set() -> None:
    # Two promises pull against each other: an all-identity set is a bit-exact
    # pass-through that runs no separation, and the framing is validated anyway.
    # An implementation that checks the framing where it builds the STFT returns
    # the input instead of raising, and only this case sees it.
    audio, events = _extract_three()
    assert all(e.edit == libsonare.PercussiveEventEdit() for e in events)
    np.testing.assert_array_equal(libsonare.render_percussive_events(audio, SR, events), audio)

    for event_set in (events, []):
        with pytest.raises(SonareError):
            libsonare.render_percussive_events(audio, SR, event_set, n_fft=2047)


def test_an_unrenderable_span_is_rejected_even_with_an_identity_edit() -> None:
    # An unrenderable set is unrenderable whether or not this call would touch
    # it, so the all-identity fast path does not get to skip the checks.
    audio = _two_hits()
    identity = libsonare.PercussiveEvent(onset_sample=4410, offset_sample=4410)
    assert identity.edit == libsonare.PercussiveEventEdit()
    with pytest.raises(SonareError):
        libsonare.render_percussive_events(audio, SR, [identity])

    # One bad event poisons a set that is otherwise renderable and entirely
    # identity.
    good = libsonare.PercussiveEvent(onset_sample=0, offset_sample=4410)
    with pytest.raises(SonareError):
        libsonare.render_percussive_events(audio, SR, [good, identity])
    np.testing.assert_array_equal(libsonare.render_percussive_events(audio, SR, [good]), audio)
