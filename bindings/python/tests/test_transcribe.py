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


def _status(data0: int) -> int:
    return (data0 >> 20) & 0xF


def _note_number(data0: int) -> int:
    return (data0 >> 8) & 0x7F


def _velocity(data0: int) -> int:
    return data0 & 0x7F


def _group(data0: int) -> int:
    return (data0 >> 24) & 0xF


def _channel(data0: int) -> int:
    return (data0 >> 16) & 0xF


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
    starts = [
        _note_number(data0)
        for _ppq, data0, _d1 in transcription.events
        if _status(data0) == _NOTE_ON
    ]
    ends = [
        _note_number(data0)
        for _ppq, data0, _d1 in transcription.events
        if _status(data0) == _NOTE_OFF
    ]
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
        audio, SR, polyphonic=True, tempo_bpm=120.0, fmin=55.0, fmax=1760.0
    )
    assert any(_note_number(data0) == midi_note for _, data0, _ in explicit.events)
    assert omitted.events == explicit.events


def test_partial_range_uses_the_selected_tracker_default() -> None:
    audio = _tone(440.0)
    libsonare.transcribe(audio, SR, tempo_bpm=120.0, fmin=1800.0)
    with pytest.raises(libsonare.SonareError, match="fmax") as failure:
        libsonare.transcribe(audio, SR, polyphonic=True, tempo_bpm=120.0, fmin=1800.0)
    assert failure.value.code == 4  # InvalidParameter from the core range resolver.


def test_events_are_in_canonical_ppq_order(transcription: libsonare.TranscribeResult) -> None:
    """Non-decreasing in PPQ, and a note-off precedes a note-on sharing its tick.

    The tie-break is what keeps a consumer that plays the list in order from
    starting a note and immediately stopping it where two of the same pitch meet.
    """
    ppqs = [ppq for ppq, _d0, _d1 in transcription.events]
    assert ppqs == sorted(ppqs)
    assert all(math.isfinite(ppq) and ppq >= 0.0 for ppq in ppqs)
    for (ppq, data0, _d1), (next_ppq, next_data0, _next_d1) in zip(
        transcription.events, transcription.events[1:], strict=False
    ):
        if ppq == next_ppq and _status(data0) != _status(next_data0):
            assert (_status(data0), _status(next_data0)) == (_NOTE_OFF, _NOTE_ON)


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
    assert [(data0, data1) for _ppq, data0, data1 in fast.events] == [
        (data0, data1) for _ppq, data0, data1 in slow.events
    ]
    assert slow.events[-1][0] > 0.0

    for (slow_ppq, _d0, _d1), (fast_ppq, _f0, _f1) in zip(slow.events, fast.events, strict=True):
        assert fast_ppq == pytest.approx(2.0 * slow_ppq, abs=1e-9)


def test_fixed_velocity_sets_every_note_on(melody: np.ndarray) -> None:
    """Every note-on carries the requested velocity; a note-off stays at 0."""
    result = libsonare.transcribe(melody, SR, tempo_bpm=120.0, fixed_velocity=77)
    assert result.note_count == len(_EXPECTED_NOTES)
    for _ppq, data0, _d1 in result.events:
        expected = 77 if _status(data0) == _NOTE_ON else 0
        assert _velocity(data0) == expected


def test_measured_velocity_is_in_domain(transcription: libsonare.TranscribeResult) -> None:
    """Measured velocities land in 1..127, never the 0 that means note-off."""
    for _ppq, data0, _d1 in transcription.events:
        if _status(data0) == _NOTE_ON:
            assert 1 <= _velocity(data0) <= 127


def test_a_lower_reference_shifts_every_note_up_a_semitone(melody: np.ndarray) -> None:
    """``reference_hz`` is read, not accepted and ignored.

    A reference a semitone below A440 puts the same audio a semitone higher on
    the keyboard, so this fails on a field that never reaches the tracker.
    """
    shifted = libsonare.transcribe(
        melody, SR, tempo_bpm=120.0, reference_hz=440.0 / 2.0 ** (1.0 / 12.0)
    )
    assert [_note_number(data0) for _ppq, data0, _d1 in shifted.events] == [
        note + 1 for note in _EXPECTED_NOTES for _ in range(2)
    ]


def test_group_and_channel_reach_the_events(melody: np.ndarray) -> None:
    result = libsonare.transcribe(melody, SR, tempo_bpm=120.0, group=3, channel=5)
    assert result.events
    for _ppq, data0, _d1 in result.events:
        assert (_group(data0), _channel(data0)) == (3, 5)


def test_the_polyphonic_chain_transcribes_the_same_pitches(melody: np.ndarray) -> None:
    """The multi-F0 chain is reachable and answers in the same register.

    Its note set is its own -- it finds overlaps the monophonic tracker cannot --
    so what is asserted is that the flag selects a chain that works, not that the
    two agree note for note.
    """
    result = libsonare.transcribe(melody, SR, tempo_bpm=120.0, polyphonic=True)
    assert result.note_count > 0
    assert len(result.events) == 2 * result.note_count
    assert {_note_number(data0) for _ppq, data0, _d1 in result.events} <= set(_EXPECTED_NOTES)


def test_silence_transcribes_to_nothing_and_still_reports_the_tempo() -> None:
    """Finding no notes is not an error."""
    result = libsonare.transcribe(np.zeros(SR // 2, dtype=np.float32), SR, tempo_bpm=96.0)
    assert result.events == []
    assert result.note_count == 0
    assert result.tempo_bpm == pytest.approx(96.0)


def _rejections(entry: str) -> list[tuple[str, dict[str, object], str]]:
    """(id, keyword options, message fragment) for the shared config arguments."""
    return [
        ("fixed_velocity", {"fixed_velocity": 128}, f"{entry}: fixed_velocity must be an integer"),
        ("fixed_velocity_zero", {"fixed_velocity": 0}, f"{entry}: fixed_velocity must be"),
        ("velocity_floor_db", {"velocity_floor_db": 0.0}, f"{entry}: velocity_floor_db must be"),
        ("velocity_floor_positive", {"velocity_floor_db": 6.0}, f"{entry}: velocity_floor_db"),
        ("fmin", {"fmin": 0.0}, f"{entry}: fmin must be positive"),
        ("fmin_above_fmax", {"fmin": 900.0, "fmax": 200.0}, f"{entry}: fmax must be above fmin"),
        ("reference_hz", {"reference_hz": float("nan")}, f"{entry}: reference_hz must be"),
        ("min_note_ms", {"min_note_ms": -1.0}, f"{entry}: min_note_ms must be positive"),
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
            f"{entry}: reattack_ratio must be 0 or above 1",
        ),
        (
            "reattack_negative",
            {"reattack_ratio": -2.0},
            f"{entry}: reattack_ratio must be 0 or above 1",
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
    with pytest.raises(SonareValueError) as excinfo:
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
            "transcribe: samples contains NaN or Inf at index 0",
            id="nan",
        ),
        pytest.param(
            lambda audio: libsonare.transcribe(audio, 0),
            "transcribe: sample_rate must be an integer",
            id="sample_rate",
        ),
        pytest.param(
            lambda audio: libsonare.transcribe(audio, SR, tempo_bpm=0.0),
            "transcribe: tempo_bpm must be positive",
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
    content = json.loads(project.to_json())["midi_content"]
    return [
        (float(event["ppq"]), int(event["data0"]), int(event["data1"]))
        for event in content.get(str(clip_id), [])
    ]


def test_transcribe_to_clip_writes_events_that_read_back(melody: np.ndarray) -> None:
    project = libsonare.Project()
    try:
        _track_id, clip_id = project.add_midi_clip(0.0, 16.0)
        note_count = project.transcribe_to_clip(clip_id, melody, SR)
        assert note_count == len(_EXPECTED_NOTES)

        events = _clip_events(project, clip_id)
        assert len(events) == 2 * note_count
        assert [
            _note_number(data0) for _ppq, data0, _d1 in events if _status(data0) == _NOTE_ON
        ] == list(_EXPECTED_NOTES)
    finally:
        project.close()


def test_transcribe_to_clip_replaces_rather_than_appends(melody: np.ndarray) -> None:
    """The clip's whole event list is replaced, as ``set_midi_events`` does."""
    project = libsonare.Project()
    try:
        _track_id, clip_id = project.add_midi_clip(0.0, 16.0)
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
        assert 40 not in {_note_number(data0) for _ppq, data0, _d1 in events}

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
            _track_id, clip_id = project.add_midi_clip(0.0, 64.0)
            project.transcribe_to_clip(clip_id, melody, SR)
            written.append(_clip_events(project, clip_id))
        finally:
            project.close()

    # As in the free function's test: identical payloads, and a last coordinate
    # above zero, so the ratio cannot be met by a clip that stayed empty.
    assert [(data0, data1) for _ppq, data0, data1 in written[1]] == [
        (data0, data1) for _ppq, data0, data1 in written[0]
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
        _track_id, clip_id = project.add_midi_clip(0.0, 16.0)
        with pytest.raises(SonareValueError) as excinfo:
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
            "transcribe_to_clip: samples contains NaN or Inf at index 0",
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
        _track_id, clip_id = project.add_midi_clip(0.0, 16.0)
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


def _note_ons(events: list[tuple[float, int, int]]) -> int:
    return sum(1 for _ppq, data0, _d1 in events if _status(data0) == _NOTE_ON)


def _max_simultaneous(events: list[tuple[float, int, int]]) -> int:
    """Largest number of notes sounding at once, a note-off ending before a note-on starts."""
    edges = sorted((ppq, 1 if _status(d0) == _NOTE_ON else -1) for ppq, d0, _d1 in events)
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
        return max((p for p, d0, _d1 in result.events if _status(d0) == _NOTE_OFF), default=-1.0)

    default = _poly(decaying_tone)
    zero = _poly(decaying_tone, min_ridge_peak_ratio=0.0)
    assert default.note_count > 0
    # With no fade threshold the ridge rides the decay to the end of the take.
    assert last_off(zero) > last_off(default)
    assert _note_ons(_poly(decaying_tone, min_ridge_peak_ratio=0.9).events) == 0


def test_reattack_ratio_splits_a_restruck_tone(restruck_tone: np.ndarray) -> None:
    default = _poly(restruck_tone)
    assert _note_ons(default.events) == 2
    # 0 is "no split", not the default 2.0; so is a ratio nothing climbs past.
    assert _note_ons(_poly(restruck_tone, reattack_ratio=0.0).events) == 1
    assert _note_ons(_poly(restruck_tone, reattack_ratio=100.0).events) == 1
    assert _note_ons(_poly(restruck_tone, reattack_ratio=2.0).events) == 2


def test_to_clip_forwards_the_polyphonic_limits(four_note_chord: np.ndarray) -> None:
    project = libsonare.Project()
    try:
        _track_id, clip_id = project.add_midi_clip(0.0, 8.0)
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
