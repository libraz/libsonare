"""Audio-to-MIDI transcription: ``transcribe`` and ``Project.transcribe_to_clip``.

The positive assertions are about content -- which MIDI notes come back, in
which order, on which grid -- because a suite of rejections alone passes on a
facade that returns an empty result for everything.
"""

from __future__ import annotations

import json
import math
from collections.abc import Callable, Sequence

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

SR = 22050

# The played sequence and the MIDI notes it must come back as, at A440.
_FREQUENCIES = (261.6256, 329.6276, 391.9954, 523.2511)
_EXPECTED_NOTES = (60, 64, 67, 72)

_NOTE_ON = 0x9
_NOTE_OFF = 0x8


def _status(word0: int) -> int:
    return (word0 >> 20) & 0xF


def _note_number(word0: int) -> int:
    return (word0 >> 8) & 0x7F


def _velocity(word0: int) -> int:
    return word0 & 0x7F


def _group(word0: int) -> int:
    return (word0 >> 24) & 0xF


def _channel(word0: int) -> int:
    return (word0 >> 16) & 0xF


def _tone(freq: float, duration: float = 0.35, amp: float = 0.4) -> np.ndarray:
    """One faded sine note, so the segmenter sees an onset rather than a click."""
    count = int(SR * duration)
    samples = amp * np.sin(2.0 * np.pi * freq * np.arange(count) / SR)
    fade = int(0.005 * SR)
    envelope = np.ones(count)
    envelope[:fade] = np.linspace(0.0, 1.0, fade)
    envelope[-fade:] = np.linspace(1.0, 0.0, fade)
    return (samples * envelope).astype(np.float32)


@pytest.fixture(scope="module")
def melody() -> np.ndarray:
    """The four-note sequence every content assertion below reads."""
    silence = np.zeros(int(SR * 0.05), dtype=np.float32)
    return np.concatenate([part for freq in _FREQUENCIES for part in (_tone(freq), silence)])


@pytest.fixture(scope="module")
def transcription(melody: np.ndarray) -> libsonare.TranscribeResult:
    """One transcription at a known tempo, shared by the tests that only read it."""
    return libsonare.transcribe(melody, SR, tempo_bpm=120.0)


def test_transcribes_the_played_notes_in_order(
    transcription: libsonare.TranscribeResult,
) -> None:
    """The notes that were played come back, as note-on / note-off pairs."""
    starts = [_note_number(e.word0) for e in transcription.events if _status(e.word0) == _NOTE_ON]
    ends = [_note_number(e.word0) for e in transcription.events if _status(e.word0) == _NOTE_OFF]
    assert starts == list(_EXPECTED_NOTES)
    assert ends == list(_EXPECTED_NOTES)
    assert transcription.note_count == len(_EXPECTED_NOTES)
    assert len(transcription.events) == 2 * transcription.note_count
    assert transcription.tempo_bpm == pytest.approx(120.0)


@pytest.mark.parametrize("frequency,midi_note", [(55.0, 33), (61.7354, 35)])
def test_polyphonic_default_range_retains_low_notes(frequency: float, midi_note: int) -> None:
    audio = _tone(frequency, duration=1.0)
    omitted = libsonare.transcribe(audio, SR, polyphonic=True, tempo_bpm=120.0)
    explicit = libsonare.transcribe(
        audio, SR, polyphonic=True, tempo_bpm=120.0, f0_min_hz=55.0, f0_max_hz=1760.0
    )
    assert any(_note_number(e.word0) == midi_note for e in explicit.events)
    assert omitted.events == explicit.events


def test_partial_range_uses_the_selected_tracker_default() -> None:
    audio = _tone(440.0)
    libsonare.transcribe(audio, SR, tempo_bpm=120.0, f0_min_hz=1800.0)
    with pytest.raises(libsonare.SonareValueError, match="f0_max_hz") as failure:
        libsonare.transcribe(audio, SR, polyphonic=True, tempo_bpm=120.0, f0_min_hz=1800.0)
    assert failure.value.code == 4  # InvalidParameter from the core range resolver.


def test_events_are_in_canonical_ppq_order(transcription: libsonare.TranscribeResult) -> None:
    """Non-decreasing in PPQ, and a note-off precedes a note-on sharing its tick.

    The tie-break is what keeps a consumer that plays the list in order from
    starting a note and immediately stopping it where two of the same pitch meet.
    """
    ppqs = [e.ppq for e in transcription.events]
    assert ppqs == sorted(ppqs)
    assert all(math.isfinite(ppq) and ppq >= 0.0 for ppq in ppqs)
    for event, following in zip(transcription.events, transcription.events[1:], strict=False):
        if event.ppq == following.ppq and _status(event.word0) != _status(following.word0):
            assert (_status(event.word0), _status(following.word0)) == (_NOTE_OFF, _NOTE_ON)


def test_a_detected_tempo_is_reported(melody: np.ndarray) -> None:
    """``tempo_bpm=None`` detects one, and says which it used."""
    detected = libsonare.transcribe(melody, SR)
    assert math.isfinite(detected.tempo_bpm)
    assert detected.tempo_bpm > 0.0
    assert detected.note_count == len(_EXPECTED_NOTES)


def test_doubling_the_tempo_doubles_the_ppq_coordinates(melody: np.ndarray) -> None:
    """The same audio on twice the tempo is twice as many beats long.

    PPQ is seconds x bpm / 60, so a faster grid puts MORE beats under a fixed
    span of audio -- the coordinates grow rather than shrink.
    """
    slow = libsonare.transcribe(melody, SR, tempo_bpm=120.0)
    fast = libsonare.transcribe(melody, SR, tempo_bpm=240.0)

    # Only the time axis may move: identical payloads mean the same notes at the
    # same velocities, and a last coordinate above zero keeps the ratio from
    # being satisfied by 0 == 2 * 0 on a fixture that transcribed to nothing.
    assert [(e.word0, e.word1) for e in fast.events] == [(e.word0, e.word1) for e in slow.events]
    assert slow.events[-1].ppq > 0.0

    for slow_event, fast_event in zip(slow.events, fast.events, strict=True):
        assert fast_event.ppq == pytest.approx(2.0 * slow_event.ppq, abs=1e-9)


def test_fixed_velocity_sets_every_note_on(melody: np.ndarray) -> None:
    """Every note-on carries the requested velocity; a note-off stays at 0."""
    result = libsonare.transcribe(melody, SR, tempo_bpm=120.0, fixed_velocity=77)
    assert result.note_count == len(_EXPECTED_NOTES)
    for event in result.events:
        expected = 77 if _status(event.word0) == _NOTE_ON else 0
        assert _velocity(event.word0) == expected


def test_measured_velocity_is_in_domain(transcription: libsonare.TranscribeResult) -> None:
    """Measured velocities land in 1..127, never the 0 that means note-off."""
    for event in transcription.events:
        if _status(event.word0) == _NOTE_ON:
            assert 1 <= _velocity(event.word0) <= 127


def test_a_lower_reference_shifts_every_note_up_a_semitone(melody: np.ndarray) -> None:
    """``reference_hz`` is read, not accepted and ignored.

    A reference a semitone below A440 puts the same audio a semitone higher on
    the keyboard, so this fails on a field that never reaches the tracker.
    """
    shifted = libsonare.transcribe(
        melody, SR, tempo_bpm=120.0, reference_hz=440.0 / 2.0 ** (1.0 / 12.0)
    )
    assert [_note_number(e.word0) for e in shifted.events] == [
        note + 1 for note in _EXPECTED_NOTES for _ in range(2)
    ]


def test_group_and_channel_reach_the_events(melody: np.ndarray) -> None:
    result = libsonare.transcribe(melody, SR, tempo_bpm=120.0, group=3, channel=5)
    assert result.events
    for event in result.events:
        assert (_group(event.word0), _channel(event.word0)) == (3, 5)


def test_the_polyphonic_chain_transcribes_the_same_pitches(melody: np.ndarray) -> None:
    """The multi-F0 chain is reachable and answers in the same register.

    Its note set is its own -- it finds overlaps the monophonic tracker cannot --
    so what is asserted is that the flag selects a chain that works, not that the
    two agree note for note.
    """
    result = libsonare.transcribe(melody, SR, tempo_bpm=120.0, polyphonic=True)
    assert result.note_count > 0
    assert len(result.events) == 2 * result.note_count
    assert {_note_number(e.word0) for e in result.events} <= set(_EXPECTED_NOTES)


def test_transcribe_events_are_project_midi_event_records(
    transcription: libsonare.TranscribeResult,
) -> None:
    """Each event is the record :meth:`Project.set_midi_events` takes, not a tuple."""
    assert transcription.events
    assert all(isinstance(e, libsonare.ProjectMidiEvent) for e in transcription.events)
    project = libsonare.Project()
    try:
        clip_id = project.add_midi_clip(0.0, 16.0).clip_id
        project.set_midi_events(clip_id, transcription.events)
        assert len(_clip_events(project, clip_id)) == len(transcription.events)
    finally:
        project.close()


def test_silence_transcribes_to_nothing_and_still_reports_the_tempo() -> None:
    """Finding no notes is not an error."""
    result = libsonare.transcribe(np.zeros(SR // 2, dtype=np.float32), SR, tempo_bpm=96.0)
    assert result.events == []
    assert result.note_count == 0
    assert result.tempo_bpm == pytest.approx(96.0)


def _refusal_type(options: dict[str, object]) -> type[Exception]:
    """A wrong-typed option is a TypeError; an out-of-domain value is a SonareValueError."""
    return TypeError if any(isinstance(v, bool) for v in options.values()) else SonareValueError


def _rejections(entry: str) -> list[tuple[str, dict[str, object], str]]:
    """(id, keyword options, message fragment) for the shared config arguments."""
    return [
        ("fixed_velocity", {"fixed_velocity": 128}, f"{entry}: fixed_velocity must be an integer"),
        ("fixed_velocity_zero", {"fixed_velocity": 0}, f"{entry}: fixed_velocity must be"),
        ("velocity_floor_db", {"velocity_floor_db": 0.0}, f"{entry}: velocity_floor_db must be"),
        ("velocity_floor_positive", {"velocity_floor_db": 6.0}, f"{entry}: velocity_floor_db"),
        ("f0_min_hz", {"f0_min_hz": 0.0}, f"{entry}: f0_min_hz must be greater than 0"),
        (
            "f0_min_above_f0_max",
            {"f0_min_hz": 900.0, "f0_max_hz": 200.0},
            f"{entry}: f0_max_hz must be above f0_min_hz",
        ),
        ("reference_hz", {"reference_hz": float("nan")}, f"{entry}: reference_hz must be"),
        ("min_note_ms", {"min_note_ms": -1.0}, f"{entry}: min_note_ms must be greater than 0"),
        (
            "min_note_division_zero",
            {"min_note_division": 0},
            f"{entry}: min_note_division must be an integer in [1, 128]",
        ),
        (
            "min_note_division_high",
            {"min_note_division": 129},
            f"{entry}: min_note_division must be an integer in [1, 128]",
        ),
        (
            "min_note_division_float",
            {"min_note_division": 32.0},
            f"{entry}: min_note_division must be an integer",
        ),
        (
            "min_note_division_bool",
            {"min_note_division": True},
            f"{entry}: min_note_division must be an integer",
        ),
        ("group", {"group": 16}, f"{entry}: group must be an integer in [0, 15]"),
        ("channel", {"channel": -1}, f"{entry}: channel must be an integer in [0, 15]"),
        ("max_polyphony_zero", {"max_polyphony": 0}, f"{entry}: max_polyphony must be an integer"),
        ("max_polyphony_high", {"max_polyphony": 65}, f"{entry}: max_polyphony must be an integer"),
        (
            "frame_ratio_negative",
            {"min_frame_peak_ratio": -0.1},
            f"{entry}: min_frame_peak_ratio must not be negative",
        ),
        (
            "frame_ratio_above_one",
            {"min_frame_peak_ratio": 1.5},
            f"{entry}: min_frame_peak_ratio must be at most 1",
        ),
        (
            "ridge_ratio_negative",
            {"min_ridge_peak_ratio": -1.0},
            f"{entry}: min_ridge_peak_ratio must not be negative",
        ),
        (
            "ridge_ratio_above_one",
            {"min_ridge_peak_ratio": 1.01},
            f"{entry}: min_ridge_peak_ratio must be at most 1",
        ),
        (
            "reattack_within_one",
            {"reattack_ratio": 1.0},
            f"{entry}: reattack_ratio must be above 1",
        ),
        (
            "reattack_zero",
            {"reattack_ratio": 0.0},
            f"{entry}: reattack_ratio must be above 1",
        ),
        (
            "reattack_negative",
            {"reattack_ratio": -2.0},
            f"{entry}: reattack_ratio must be above 1",
        ),
        (
            "reattack_nan",
            {"reattack_ratio": float("nan")},
            f"{entry}: reattack_ratio must be",
        ),
    ]


@pytest.mark.parametrize(
    ("options", "fragment"),
    [pytest.param(o, f, id=i) for i, o, f in _rejections("transcribe")],
)
def test_transcribe_rejects_out_of_domain_options(
    melody: np.ndarray, options: dict[str, object], fragment: str
) -> None:
    with pytest.raises(_refusal_type(options)) as excinfo:
        libsonare.transcribe(melody, SR, **options)
    assert fragment in str(excinfo.value)


@pytest.mark.parametrize(
    ("build", "fragment"),
    [
        pytest.param(
            lambda audio: libsonare.transcribe(np.zeros(0, dtype=np.float32), SR),
            "transcribe: samples must not be empty",
            id="empty",
        ),
        pytest.param(
            lambda audio: libsonare.transcribe(np.full(64, np.nan, dtype=np.float32), SR),
            "transcribe: samples contains a non-finite sample at index 0",
            id="nan",
        ),
        pytest.param(
            lambda audio: libsonare.transcribe(audio, 0),
            "transcribe: sample_rate must be an integer",
            id="sample_rate",
        ),
        pytest.param(
            lambda audio: libsonare.transcribe(audio, SR, tempo_bpm=0.0),
            "transcribe: tempo_bpm must be greater than 0",
            id="tempo_bpm",
        ),
    ],
)
def test_transcribe_rejects_bad_input(
    melody: np.ndarray, build: Callable[[np.ndarray], object], fragment: str
) -> None:
    """Each rejection names this function and the offending argument."""
    with pytest.raises(SonareValueError) as excinfo:
        build(melody)
    assert fragment in str(excinfo.value)


def _clip_events(project: libsonare.Project, clip_id: int) -> list[tuple[float, int, int]]:
    """The clip's stored events, read back through the project's own serializer."""
    content = json.loads(project.to_json())["midiContent"]
    return [
        (float(event["ppq"]), int(event["word0"]), int(event["word1"]))
        for event in content.get(str(clip_id), [])
    ]


def test_transcribe_to_clip_writes_events_that_read_back(melody: np.ndarray) -> None:
    project = libsonare.Project()
    try:
        clip_id = project.add_midi_clip(0.0, 16.0).clip_id
        note_count = project.transcribe_to_clip(clip_id, melody, SR)
        assert note_count == len(_EXPECTED_NOTES)

        events = _clip_events(project, clip_id)
        assert len(events) == 2 * note_count
        assert [
            _note_number(word0) for _ppq, word0, _d1 in events if _status(word0) == _NOTE_ON
        ] == list(_EXPECTED_NOTES)
    finally:
        project.close()


def test_transcribe_to_clip_replaces_rather_than_appends(melody: np.ndarray) -> None:
    """The clip's whole event list is replaced, as ``set_midi_events`` does."""
    project = libsonare.Project()
    try:
        clip_id = project.add_midi_clip(0.0, 16.0).clip_id
        project.set_midi_events(
            clip_id,
            [
                libsonare.Project.midi_note_on(0.0, 0, 0, 40, 100),
                libsonare.Project.midi_note_off(1.0, 0, 0, 40, 0),
            ],
        )
        assert len(_clip_events(project, clip_id)) == 2

        project.transcribe_to_clip(clip_id, melody, SR)
        events = _clip_events(project, clip_id)
        assert len(events) == 2 * len(_EXPECTED_NOTES)
        assert 40 not in {_note_number(word0) for _ppq, word0, _d1 in events}

        # And again over its own output, so a second pass cannot accumulate.
        project.transcribe_to_clip(clip_id, melody, SR)
        assert len(_clip_events(project, clip_id)) == 2 * len(_EXPECTED_NOTES)
    finally:
        project.close()


def test_transcribe_to_clip_reads_the_project_tempo_map(melody: np.ndarray) -> None:
    """The grid is the project's own, which is why this takes no tempo argument."""
    written: list[list[tuple[float, int, int]]] = []
    for bpm in (120.0, 240.0):
        project = libsonare.Project()
        try:
            project.set_tempo_segments([{"start_ppq": 0.0, "bpm": bpm}])
            clip_id = project.add_midi_clip(0.0, 64.0).clip_id
            project.transcribe_to_clip(clip_id, melody, SR)
            written.append(_clip_events(project, clip_id))
        finally:
            project.close()

    # As in the free function's test: identical payloads, and a last coordinate
    # above zero, so the ratio cannot be met by a clip that stayed empty.
    assert [(word0, word1) for _ppq, word0, word1 in written[1]] == [
        (word0, word1) for _ppq, word0, word1 in written[0]
    ]
    slow = [ppq for ppq, _d0, _d1 in written[0]]
    fast = [ppq for ppq, _d0, _d1 in written[1]]
    assert len(slow) == len(fast) == 2 * len(_EXPECTED_NOTES)
    assert slow[-1] > 0.0
    for slow_ppq, fast_ppq in zip(slow, fast, strict=True):
        assert fast_ppq == pytest.approx(2.0 * slow_ppq, abs=1e-6)


@pytest.mark.parametrize(
    ("options", "fragment"),
    [pytest.param(o, f, id=i) for i, o, f in _rejections("transcribe_to_clip")],
)
def test_transcribe_to_clip_rejects_out_of_domain_options(
    melody: np.ndarray, options: dict[str, object], fragment: str
) -> None:
    project = libsonare.Project()
    try:
        clip_id = project.add_midi_clip(0.0, 16.0).clip_id
        with pytest.raises(_refusal_type(options)) as excinfo:
            project.transcribe_to_clip(clip_id, melody, SR, **options)
        assert fragment in str(excinfo.value)
    finally:
        project.close()


@pytest.mark.parametrize(
    ("samples", "sample_rate", "fragment"),
    [
        pytest.param(
            np.zeros(0, dtype=np.float32),
            SR,
            "transcribe_to_clip: samples must not be empty",
            id="empty",
        ),
        pytest.param(
            np.full(64, np.nan, dtype=np.float32),
            SR,
            "transcribe_to_clip: samples contains a non-finite sample at index 0",
            id="nan",
        ),
        pytest.param(
            np.full(64, 0.1, dtype=np.float32),
            0,
            "transcribe_to_clip: sample_rate must be an integer",
            id="sample_rate",
        ),
    ],
)
def test_transcribe_to_clip_rejects_bad_input(
    samples: Sequence[float], sample_rate: int, fragment: str
) -> None:
    project = libsonare.Project()
    try:
        clip_id = project.add_midi_clip(0.0, 16.0).clip_id
        with pytest.raises(SonareValueError) as excinfo:
            project.transcribe_to_clip(clip_id, samples, sample_rate)
        assert fragment in str(excinfo.value)
    finally:
        project.close()


def test_transcribe_to_clip_rejects_an_unknown_clip(melody: np.ndarray) -> None:
    """A clip the project does not hold is refused by the C ABI, not written."""
    project = libsonare.Project()
    try:
        with pytest.raises(libsonare.SonareError):
            project.transcribe_to_clip(4242, melody, SR)
    finally:
        project.close()


# ---------------------------------------------------------------------------
# Polyphonic limits: max_polyphony, the two peak ratios and reattack_ratio
# ---------------------------------------------------------------------------

_POLY_SR = 44100


def _poly_sine(freq: float, seconds: float, amp: float) -> np.ndarray:
    count = int(_POLY_SR * seconds)
    return amp * np.sin(2.0 * np.pi * freq * np.arange(count) / _POLY_SR)


def _note_ons(events: list[libsonare.ProjectMidiEvent]) -> int:
    return sum(1 for e in events if _status(e.word0) == _NOTE_ON)


def _max_simultaneous(events: list[libsonare.ProjectMidiEvent]) -> int:
    """Largest number of notes sounding at once, a note-off ending before a note-on starts."""
    edges = sorted((e.ppq, 1 if _status(e.word0) == _NOTE_ON else -1) for e in events)
    open_notes = peak = 0
    for _ppq, step in edges:
        open_notes += step
        peak = max(peak, open_notes)
    return peak


def _poly(samples: np.ndarray, **options: object) -> libsonare.TranscribeResult:
    return libsonare.transcribe(
        samples.astype(np.float32), _POLY_SR, tempo_bpm=120.0, polyphonic=True, **options
    )


@pytest.fixture(scope="module")
def four_note_chord() -> np.ndarray:
    return np.sum([_poly_sine(hz, 1.0, 0.15) for hz in (261.63, 329.63, 392.0, 493.88)], axis=0)


@pytest.fixture(scope="module")
def weak_voice_chord() -> np.ndarray:
    """A strong tone with a second one 30 dB under it: below the default frame ratio."""
    return _poly_sine(261.63, 1.0, 0.4) + _poly_sine(392.0, 1.0, 0.012)


@pytest.fixture(scope="module")
def decaying_tone() -> np.ndarray:
    t = np.arange(int(_POLY_SR * 1.2)) / _POLY_SR
    return 0.4 * np.exp(-5.0 * t) * np.sin(2.0 * np.pi * 261.63 * t)


@pytest.fixture(scope="module")
def restruck_tone() -> np.ndarray:
    """One pitch struck again at 0.5 s: a decay to 0.12, then a 20 ms rise to 0.5.

    The level never falls low enough to end the ridge, so only a re-attack split
    can make it two notes.
    """
    half = int(0.5 * _POLY_SR)
    fall = 0.4 * np.exp(np.log(0.12 / 0.4) * np.arange(half) / half)
    rise = 0.12 + 0.38 * np.minimum(1.0, np.arange(half) / (0.02 * _POLY_SR))
    t = np.arange(2 * half) / _POLY_SR
    return np.concatenate([fall, rise]) * np.sin(2.0 * np.pi * 261.63 * t)


def test_max_polyphony_caps_the_simultaneous_notes(four_note_chord: np.ndarray) -> None:
    default = _poly(four_note_chord)
    capped = _poly(four_note_chord, max_polyphony=1)
    assert _max_simultaneous(default.events) == 4
    assert _max_simultaneous(capped.events) == 1


def test_min_frame_peak_ratio_changes_the_result(weak_voice_chord: np.ndarray) -> None:
    default = _poly(weak_voice_chord)
    stricter = _poly(weak_voice_chord, min_frame_peak_ratio=1.0)
    zero = _poly(weak_voice_chord, min_frame_peak_ratio=0.0)
    assert _note_ons(stricter.events) == _note_ons(default.events)
    # A written 0 is a real 0 (the search runs on), not the 0.20 default.
    assert _note_ons(zero.events) > _note_ons(default.events)


def test_min_ridge_peak_ratio_changes_the_note_length(decaying_tone: np.ndarray) -> None:
    def last_off(result: libsonare.TranscribeResult) -> float:
        return max((e.ppq for e in result.events if _status(e.word0) == _NOTE_OFF), default=-1.0)

    default = _poly(decaying_tone)
    zero = _poly(decaying_tone, min_ridge_peak_ratio=0.0)
    assert default.note_count > 0
    # With no fade threshold the ridge rides the decay to the end of the take.
    assert last_off(zero) > last_off(default)
    # A ratio near 1 ends the ridge almost as soon as the decay begins.
    assert last_off(_poly(decaying_tone, min_ridge_peak_ratio=0.9)) < last_off(default)


def test_reattack_ratio_splits_a_restruck_tone(restruck_tone: np.ndarray) -> None:
    # None is "no split"; so is a ratio nothing climbs past.
    assert _note_ons(_poly(restruck_tone).events) == 1
    assert _note_ons(_poly(restruck_tone, reattack_ratio=None).events) == 1
    assert _note_ons(_poly(restruck_tone, reattack_ratio=100.0).events) == 1
    assert _note_ons(_poly(restruck_tone, reattack_ratio=2.0).events) == 2


def test_to_clip_forwards_the_polyphonic_limits(four_note_chord: np.ndarray) -> None:
    project = libsonare.Project()
    try:
        clip_id = project.add_midi_clip(0.0, 8.0).clip_id
        count = project.transcribe_to_clip(
            clip_id,
            four_note_chord.astype(np.float32),
            _POLY_SR,
            polyphonic=True,
            max_polyphony=1,
        )
        assert count == 1
    finally:
        project.close()


@pytest.mark.parametrize(
    ("option", "value"),
    [
        pytest.param("max_polyphony", 3, id="max_polyphony"),
        pytest.param("min_frame_peak_ratio", 0.0, id="min_frame_peak_ratio"),
        pytest.param("min_ridge_peak_ratio", 0.5, id="min_ridge_peak_ratio"),
        pytest.param("reattack_ratio", 3.0, id="reattack_ratio"),
    ],
)
def test_a_polyphonic_limit_on_the_monophonic_path_is_refused_by_the_core(
    melody: np.ndarray, option: str, value: float
) -> None:
    """Not pre-checked here: the C ABI refuses it and names the field."""
    with pytest.raises(libsonare.SonareError) as excinfo:
        libsonare.transcribe(melody, SR, **{option: value})
    assert option in str(excinfo.value)


# ---------------------------------------------------------------------------
# min_note_division: the shortest note as a note value, on both paths
# ---------------------------------------------------------------------------


def test_min_note_division_drops_notes_shorter_than_the_note_value(melody: np.ndarray) -> None:
    default = libsonare.transcribe(melody, SR, tempo_bpm=120.0)
    # At 120 BPM a whole note is 2 s: no tone of the melody reaches it.
    whole = libsonare.transcribe(melody, SR, tempo_bpm=120.0, min_note_division=1)
    fine = libsonare.transcribe(melody, SR, tempo_bpm=120.0, min_note_division=32)
    assert default.note_count == len(_EXPECTED_NOTES)
    assert whole.note_count == 0
    assert fine.note_count == default.note_count


def test_min_note_division_applies_to_the_polyphonic_path(four_note_chord: np.ndarray) -> None:
    assert _note_ons(_poly(four_note_chord, min_note_division=32).events) == 4
    assert _note_ons(_poly(four_note_chord, min_note_division=1).events) == 0


def test_min_note_division_with_min_note_ms_is_refused_by_the_core(melody: np.ndarray) -> None:
    with pytest.raises(libsonare.SonareError) as excinfo:
        libsonare.transcribe(melody, SR, min_note_ms=30.0, min_note_division=32)
    assert "min_note_division" in str(excinfo.value)


def test_to_clip_forwards_min_note_division(melody: np.ndarray) -> None:
    project = libsonare.Project()
    try:
        clip_id = project.add_midi_clip(0.0, 8.0).clip_id
        assert project.transcribe_to_clip(clip_id, melody, SR, min_note_division=1) == 0
        assert project.transcribe_to_clip(clip_id, melody, SR, min_note_division=32) == len(
            _EXPECTED_NOTES
        )
    finally:
        project.close()


# ---------------------------------------------------------------------------
# monophonic_model: segments (default) or singing
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def vibrato_tone() -> np.ndarray:
    """A 2 s G4 harmonic tone with a 6 Hz, 75-cent vibrato."""
    t = np.arange(2 * SR) / SR
    inst_freq = 391.9954 * 2.0 ** (0.75 * np.sin(2.0 * np.pi * 6.0 * t) / 12.0)
    phase = 2.0 * np.pi * np.cumsum(inst_freq) / SR
    tone = sum(np.sin(h * phase) / h for h in (1, 2, 3))
    return (0.2 * tone).astype(np.float32)


def test_singing_model_holds_vibrato_in_one_note(vibrato_tone: np.ndarray) -> None:
    singing = libsonare.transcribe(vibrato_tone, SR, tempo_bpm=120.0, monophonic_model="singing")
    default = libsonare.transcribe(vibrato_tone, SR, tempo_bpm=120.0)
    assert singing.note_count == 1
    assert [_note_number(e.word0) for e in singing.events if _status(e.word0) == _NOTE_ON] == [67]
    assert singing.events != default.events


def test_explicit_segments_equals_the_default(vibrato_tone: np.ndarray) -> None:
    explicit = libsonare.transcribe(vibrato_tone, SR, tempo_bpm=120.0, monophonic_model="segments")
    default = libsonare.transcribe(vibrato_tone, SR, tempo_bpm=120.0, monophonic_model=None)
    assert explicit.events == default.events
    assert explicit.note_count == default.note_count


def test_unknown_monophonic_model_is_refused_by_name(melody: np.ndarray) -> None:
    with pytest.raises(SonareValueError, match="monophonic_model"):
        libsonare.transcribe(melody, SR, monophonic_model="choir")  # type: ignore[arg-type]


@pytest.mark.parametrize("value", [1, b"singing", True])
def test_non_string_monophonic_model_is_a_type_error(melody: np.ndarray, value: object) -> None:
    with pytest.raises(TypeError, match="monophonic_model"):
        libsonare.transcribe(melody, SR, monophonic_model=value)  # type: ignore[arg-type]


def test_singing_model_with_polyphonic_is_refused(melody: np.ndarray) -> None:
    with pytest.raises(SonareValueError, match="monophonic_model"):
        libsonare.transcribe(melody, SR, polyphonic=True, monophonic_model="singing")


def test_to_clip_forwards_monophonic_model(vibrato_tone: np.ndarray) -> None:
    project = libsonare.Project()
    try:
        clip_id = project.add_midi_clip(0.0, 8.0).clip_id
        assert (
            project.transcribe_to_clip(clip_id, vibrato_tone, SR, monophonic_model="singing") == 1
        )
    finally:
        project.close()
