"""CLI tests for the note editing commands: polyphonic-notes, polyphonic-render and tune-to-midi."""

from __future__ import annotations

import json

# ruff: noqa: F403,F405
from ._analyzer_helpers import *
from ._analyzer_helpers import _run_cli, _write_test_wav

# E4 and B4, a fifth apart: far outside the 50-cent default separation, and B4 is
# not a partial of E4, so the chain resolves one note per tone. A ridge has to last
# 140 ms to survive tracking, so half a second is close to the cheapest input the
# chain resolves at all.
_POLYPHONIC_SR = 44100


_POLYPHONIC_SECONDS = 0.5


_POLYPHONIC_HZ = (329.63, 493.88)


_POLYPHONIC_NOTE_COUNT = 2


# Each tone must land within this of a note's reported median. The two medians came
# back within 0.01 Hz of their tones, so the window is slack rather than a bound.
_POLYPHONIC_PITCH_TOLERANCE_CENTS = 50.0


def _write_polyphonic_chord(path: str) -> list[float]:
    """Write the two-tone fixture and return the samples as written to the file."""
    tones = [_generate_sine(hz, _POLYPHONIC_SR, _POLYPHONIC_SECONDS) for hz in _POLYPHONIC_HZ]
    samples = [0.3 * (low + high) for low, high in zip(*tones, strict=True)]
    _write_test_wav(path, samples, _POLYPHONIC_SR)
    return _read_wav_samples(path)


def _read_wav_samples(path: str) -> list[float]:
    with wave.open(path) as handle:
        frames = handle.readframes(handle.getnframes())
    return [value / 32768.0 for (value,) in struct.iter_unpack("<h", frames)]


def _rms(samples: list[float]) -> float:
    return math.sqrt(sum(value * value for value in samples) / len(samples)) if samples else 0.0


def test_polyphonic_notes_cli_reports_each_note_over_its_own_frame_span() -> None:
    """polyphonic-notes publishes the note table plus the three per-frame curves."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "chord.wav")
        _write_polyphonic_chord(wav_path)

        result = _run_cli(["polyphonic-notes", wav_path, "--json"])

        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert set(payload) == {
            "sample_rate",
            "frame_count",
            "note_count",
            "polyphony",
            "notes",
        }
        assert payload["sample_rate"] == _POLYPHONIC_SR
        assert len(payload["polyphony"]) == payload["frame_count"]
        # Non-vacuity for every per-note assertion below: an analysis that tracked
        # no ridge would satisfy all of them without looking at anything.
        assert payload["note_count"] == _POLYPHONIC_NOTE_COUNT
        assert len(payload["notes"]) == _POLYPHONIC_NOTE_COUNT

        for index, note in enumerate(payload["notes"]):
            assert set(note) == {
                "index",
                "onset_sample",
                "offset_sample",
                "frame_start",
                "frame_end",
                "median_hz",
                "median_cents",
                "f0_stability",
                "f0_hz",
                "amplitude",
                "salience",
            }
            assert note["index"] == index
            span = note["frame_end"] - note["frame_start"]
            assert span > 0
            for curve in ("f0_hz", "amplitude", "salience"):
                assert len(note[curve]) == span, curve

        medians = sorted(note["median_hz"] for note in payload["notes"])
        for reported, tone in zip(medians, sorted(_POLYPHONIC_HZ), strict=True):
            cents = abs(math.log2(reported / tone) * 1200.0)
            assert cents < _POLYPHONIC_PITCH_TOLERANCE_CENTS, (reported, tone)


def test_polyphonic_render_cli_without_an_edit_reproduces_its_input() -> None:
    """Every note starts on the identity edit, so the render is the round trip."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "chord.wav")
        out_path = os.path.join(tmpdir, "rendered.wav")
        source = _write_polyphonic_chord(wav_path)

        result = _run_cli(["polyphonic-render", wav_path, "-o", out_path, "--json"])

        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["edits"] == 0
        assert payload["note_count"] == _POLYPHONIC_NOTE_COUNT
        assert payload["sample_rate"] == _POLYPHONIC_SR
        assert payload["output"] == out_path

        rendered = _read_wav_samples(out_path)
        assert len(rendered) == len(source)
        error = _rms([a - b for a, b in zip(source, rendered, strict=True)])
        # 60 dB below the source, against a round trip measured at the 16-bit
        # floor. Muting both notes leaves the residual alone, roughly 18 dB down,
        # so the threshold separates a round trip from a render that lost a note.
        assert error < _rms(source) / 1000.0, error


def test_polyphonic_render_cli_applies_one_edit_per_occurrence() -> None:
    """--edit is repeatable and each occurrence reaches the render."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "chord.wav")
        plain_path = os.path.join(tmpdir, "plain.wav")
        edited_path = os.path.join(tmpdir, "edited.wav")
        _write_polyphonic_chord(wav_path)

        plain = _run_cli(["polyphonic-render", wav_path, "-o", plain_path, "--json"])
        assert plain.returncode == 0, plain.stderr
        edited = _run_cli(
            [
                "polyphonic-render",
                wav_path,
                "-o",
                edited_path,
                "--edit",
                "0.muted=on",
                "--edit",
                "1.gain_db=-12",
                "--json",
            ]
        )

        assert edited.returncode == 0, edited.stderr
        payload = json.loads(edited.stdout)
        assert payload["edits"] == 2
        assert _read_wav_samples(edited_path) != _read_wav_samples(plain_path)


@pytest.mark.parametrize(
    ("assignment", "message"),
    [
        ("0.no_such_field=1", "unknown --edit field: no_such_field"),
        ("9.gain_db=-3", "--edit note index out of range: 9"),
        ("0.gain_db=nan", "numeric value for --edit must be finite: nan"),
        # The native parsers read a value whole and an offset is a C int, so
        # neither an underscore separator nor a wider offset is a number here.
        ("0.gain_db=1_0", "invalid float value for --edit: 1_0"),
        (
            "0.time_offset_samples=99999999999",
            "invalid integer value for --edit: 99999999999",
        ),
    ],
)
def test_polyphonic_render_cli_refuses_a_bad_edit(assignment: str, message: str) -> None:
    """An unusable --edit fails as a parameter error instead of being ignored."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "chord.wav")
        out_path = os.path.join(tmpdir, "rendered.wav")
        _write_polyphonic_chord(wav_path)

        result = _run_cli(
            ["polyphonic-render", wav_path, "-o", out_path, "--edit", assignment, "--json"]
        )

        assert result.returncode == 3, result.stdout
        assert message in result.stderr
        assert not os.path.exists(out_path)


def test_polyphonic_render_cli_requires_an_output_file() -> None:
    """A render with nowhere to go is a parameter error, as on the native CLI."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "chord.wav")
        _write_polyphonic_chord(wav_path)

        result = _run_cli(["polyphonic-render", wav_path, "--json"])

        assert result.returncode == 3, result.stdout


_TUNE_SR = 48000


_TUNE_SECONDS = 0.5


# 440 Hz is MIDI 69 and 493.8833 Hz is MIDI 71, both exactly, so every expected
# pitch below is a whole number rather than a measured one.
_TUNE_TAKE_HZ = (440.0, 493.8833)


# C4 then G4, one quarter note each at the project's default 120 BPM, which puts
# the two targets over the take's two spans exactly.
_TUNE_REFERENCE_MIDI = (60, 67)


# The take tracked within 0.01 of its own tones and the tuned output within 0.09
# of its targets, so this window is slack rather than a bound.
_TUNE_PITCH_TOLERANCE_SEMITONES = 0.3


def _tune_take_samples() -> list[float]:
    """Two half-second tones a whole tone apart, each faded in and out.

    The fade keeps the two spans from meeting at a step the tracker reads as a
    transient, so the segmenter cuts one note per tone.
    """
    fade = int(0.01 * _TUNE_SR)
    count = int(_TUNE_SR * _TUNE_SECONDS)
    samples: list[float] = []
    for hz in _TUNE_TAKE_HZ:
        for index in range(count):
            gain = 1.0
            if index < fade:
                gain = 0.5 - 0.5 * math.cos(math.pi * index / fade)
            elif index > count - fade:
                gain = 0.5 - 0.5 * math.cos(math.pi * (count - index) / fade)
            samples.append(0.5 * gain * math.sin(2 * math.pi * hz * index / _TUNE_SR))
    return samples


def _tune_reference_smf(*midi_numbers: int, beats: float = 1.0, offset_beats: float = 0.0) -> bytes:
    """One note per argument, written by the library's own project exporter.

    The clip runs far past the last event on purpose: a note-off at exactly the
    clip end is not written, and the reader drops a note-on it never closes.
    """
    from libsonare import Project

    project = Project()
    try:
        _track_id, clip_id = project.add_midi_clip(0.0, 64.0)
        events = []
        for index, midi in enumerate(midi_numbers):
            start = offset_beats + index * beats
            events.append(Project.midi_note_on(start, 0, 0, midi, 100))
            events.append(Project.midi_note_off(start + beats, 0, 0, midi, 0))
        project.set_midi_events(clip_id, events)
        return project.export_smf()
    finally:
        project.close()


def _tune_fixture(tmpdir: str, *midi_numbers: int, **melody: float) -> tuple[str, str]:
    """Write the take and one reference melody, returning their two paths."""
    wav_path = os.path.join(tmpdir, "take.wav")
    smf_path = os.path.join(tmpdir, "reference.mid")
    _write_test_wav(wav_path, _tune_take_samples(), _TUNE_SR)
    Path(smf_path).write_bytes(_tune_reference_smf(*midi_numbers, **melody))  # type: ignore[arg-type]
    return wav_path, smf_path


def _median_midi(samples: list[float], start_sec: float, end_sec: float) -> float:
    """Median pitch over one span as a MIDI number, NaN when nothing is voiced."""
    import libsonare

    window = samples[int(start_sec * _TUNE_SR) : int(end_sec * _TUNE_SR)]
    track = libsonare.pitch_pyin(window, sample_rate=_TUNE_SR)
    voiced = sorted(
        hz
        for hz, flag in zip(track.f0, track.voiced_flag, strict=True)
        if flag and math.isfinite(hz) and hz > 0.0
    )
    if not voiced:
        return math.nan
    return 69.0 + 12.0 * math.log2(voiced[len(voiced) // 2] / 440.0)


def _tune_spans(samples: list[float]) -> tuple[float, float]:
    """Both note spans as MIDI numbers, measured inside the fades."""
    return _median_midi(samples, 0.05, 0.45), _median_midi(samples, 0.55, 0.95)


def test_tune_to_midi_cli_moves_each_note_onto_the_reference() -> None:
    """The take's two tones end up on the melody the MIDI file carries."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path, smf_path = _tune_fixture(tmpdir, *_TUNE_REFERENCE_MIDI)
        out_path = os.path.join(tmpdir, "tuned.wav")
        # Non-vacuity for the assertion below: the take has to start off the
        # reference, or a command that copied its input through would pass too.
        before = _tune_spans(_read_wav_samples(wav_path))
        assert before == pytest.approx((69.0, 71.0), abs=_TUNE_PITCH_TOLERANCE_SEMITONES), before

        result = _run_cli(
            ["tune-to-midi", wav_path, "-o", out_path, "--reference-smf", smf_path, "--json"]
        )

        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["note_count"] == len(_TUNE_TAKE_HZ)
        assert payload["assigned_count"] == len(_TUNE_REFERENCE_MIDI)
        assert payload["sample_rate"] == _TUNE_SR
        assert payload["output"] == out_path

        after = _tune_spans(_read_wav_samples(out_path))
        expected = tuple(float(midi) for midi in _TUNE_REFERENCE_MIDI)
        assert after == pytest.approx(expected, abs=_TUNE_PITCH_TOLERANCE_SEMITONES), after


def test_tune_to_midi_cli_gives_each_unmatched_policy_its_own_outcome() -> None:
    """A reference covering only the first span separates the three policies."""
    with tempfile.TemporaryDirectory() as tmpdir:
        # One target, so the take's second note has a measured pitch and no
        # target -- which is the only case the policy governs.
        wav_path, smf_path = _tune_fixture(tmpdir, _TUNE_REFERENCE_MIDI[0])
        outcomes: dict[str, tuple[int, float]] = {}
        for policy in ("leave", "mute", "nearest"):
            out_path = os.path.join(tmpdir, f"{policy}.wav")
            result = _run_cli(
                [
                    "tune-to-midi",
                    wav_path,
                    "-o",
                    out_path,
                    "--reference-smf",
                    smf_path,
                    "--unmatched-policy",
                    policy,
                    "--json",
                ]
            )
            assert result.returncode == 0, result.stderr
            payload = json.loads(result.stdout)
            second = _median_midi(_read_wav_samples(out_path), 0.55, 0.95)
            outcomes[policy] = (payload["assigned_count"], second)

        # Only "nearest" counts an untargeted note as an assignment.
        assert [outcomes[policy][0] for policy in ("leave", "mute", "nearest")] == [1, 1, 2]
        # "leave" renders the note as recorded, "mute" leaves nothing to track,
        # and "nearest" corrects it to the one target there is.
        assert outcomes["leave"][1] == pytest.approx(71.0, abs=_TUNE_PITCH_TOLERANCE_SEMITONES)
        assert math.isnan(outcomes["mute"][1])
        assert outcomes["nearest"][1] == pytest.approx(
            float(_TUNE_REFERENCE_MIDI[0]), abs=_TUNE_PITCH_TOLERANCE_SEMITONES
        )


def test_tune_to_midi_cli_reports_zero_assigned_for_a_reference_that_misses() -> None:
    """A melody lining up with nothing is an answer, not a failure."""
    with tempfile.TemporaryDirectory() as tmpdir:
        # 16 beats at 120 BPM is 8 s in, well past the end of a one-second take.
        wav_path, smf_path = _tune_fixture(tmpdir, *_TUNE_REFERENCE_MIDI, offset_beats=16.0)
        out_path = os.path.join(tmpdir, "tuned.wav")

        result = _run_cli(
            ["tune-to-midi", wav_path, "-o", out_path, "--reference-smf", smf_path, "--json"]
        )

        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["assigned_count"] == 0
        assert payload["note_count"] == len(_TUNE_TAKE_HZ)
        # The default policy is "leave", so nothing moved either.
        after = _tune_spans(_read_wav_samples(out_path))
        assert after == pytest.approx((69.0, 71.0), abs=_TUNE_PITCH_TOLERANCE_SEMITONES), after


def test_tune_to_midi_cli_forwards_an_overlap_ratio_of_zero() -> None:
    """0 is a value -- any overlap counts -- and not a request for the default."""
    with tempfile.TemporaryDirectory() as tmpdir:
        # A half-beat target covers a quarter second of a half-second note, so its
        # overlap ratio is about 0.5: inside a bound of 0 and outside the default.
        wav_path, smf_path = _tune_fixture(tmpdir, _TUNE_REFERENCE_MIDI[0], beats=0.5)
        assigned = []
        for extra in ([], ["--min-overlap-ratio", "0"]):
            out_path = os.path.join(tmpdir, f"ratio{len(extra)}.wav")
            result = _run_cli(
                [
                    "tune-to-midi",
                    wav_path,
                    "-o",
                    out_path,
                    "--reference-smf",
                    smf_path,
                    *extra,
                    "--json",
                ]
            )
            assert result.returncode == 0, result.stderr
            assigned.append(json.loads(result.stdout)["assigned_count"])

        assert assigned == [0, 1]


def test_tune_to_midi_cli_forwards_a_correction_bound_of_zero() -> None:
    """0 saturates every correction to nothing: targets assigned, no note moved."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path, smf_path = _tune_fixture(tmpdir, *_TUNE_REFERENCE_MIDI)
        out_path = os.path.join(tmpdir, "tuned.wav")

        result = _run_cli(
            [
                "tune-to-midi",
                wav_path,
                "-o",
                out_path,
                "--reference-smf",
                smf_path,
                "--max-correction-semitones",
                "0",
                "--json",
            ]
        )

        assert result.returncode == 0, result.stderr
        assert json.loads(result.stdout)["assigned_count"] == len(_TUNE_REFERENCE_MIDI)
        after = _tune_spans(_read_wav_samples(out_path))
        assert after == pytest.approx((69.0, 71.0), abs=_TUNE_PITCH_TOLERANCE_SEMITONES), after


@pytest.mark.parametrize(
    ("extra", "message"),
    [
        (["--track", "-1"], "--track must be a non-negative track index: -1"),
        (["--min-overlap-ratio", "1.5"], "--min-overlap-ratio must be between 0 and 1: 1.5"),
        (["--min-overlap-ratio", "-0.1"], "--min-overlap-ratio must be between 0 and 1: -0.1"),
        (
            ["--max-correction-semitones", "-1"],
            "--max-correction-semitones must be non-negative: -1",
        ),
        (
            ["--unmatched-policy", "hum"],
            "--unmatched-policy must be one of leave, mute, nearest: hum",
        ),
    ],
)
def test_tune_to_midi_cli_refuses_a_value_outside_its_domain(
    extra: list[str], message: str
) -> None:
    """Each refusal is a parameter error, not argparse's usage error."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path, smf_path = _tune_fixture(tmpdir, *_TUNE_REFERENCE_MIDI)
        out_path = os.path.join(tmpdir, "tuned.wav")

        result = _run_cli(
            [
                "tune-to-midi",
                wav_path,
                "-o",
                out_path,
                "--reference-smf",
                smf_path,
                *extra,
                "--json",
            ]
        )

        assert result.returncode == 3, result.stdout
        assert message in result.stderr
        assert not os.path.exists(out_path)


def test_tune_to_midi_cli_requires_an_output_file() -> None:
    """A render with nowhere to go is a parameter error, as on the native CLI."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path, smf_path = _tune_fixture(tmpdir, *_TUNE_REFERENCE_MIDI)

        result = _run_cli(["tune-to-midi", wav_path, "--reference-smf", smf_path, "--json"])

        assert result.returncode == 3, result.stdout
        assert "tune-to-midi requires an output file (-o/--output)" in result.stderr


def test_tune_to_midi_cli_requires_a_reference_melody() -> None:
    """--reference-smf is argparse-required, so its absence is a usage error."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path, _smf_path = _tune_fixture(tmpdir, *_TUNE_REFERENCE_MIDI)
        out_path = os.path.join(tmpdir, "tuned.wav")

        result = _run_cli(["tune-to-midi", wav_path, "-o", out_path, "--json"])

        assert result.returncode == 2, result.stdout
        assert "--reference-smf" in result.stderr


def test_tune_to_midi_cli_track_index_reaches_the_smf_reader() -> None:
    """--track selects a MIDI-bearing track, so a file with one refuses index 1."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path, smf_path = _tune_fixture(tmpdir, *_TUNE_REFERENCE_MIDI)
        out_path = os.path.join(tmpdir, "tuned.wav")

        result = _run_cli(
            [
                "tune-to-midi",
                wav_path,
                "-o",
                out_path,
                "--reference-smf",
                smf_path,
                "--track",
                "1",
                "--json",
            ]
        )

        assert result.returncode == 3, result.stdout
        assert "no MIDI-bearing track at index 1" in result.stderr
