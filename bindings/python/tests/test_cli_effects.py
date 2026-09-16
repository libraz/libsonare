"""CLI tests for offline effect subcommands and shared CLI helpers."""

from __future__ import annotations

import argparse
import json
from types import SimpleNamespace

# ruff: noqa: F403,F405
from ._analyzer_helpers import *


def _write_test_wav(path: str, samples: list[float], sample_rate: int) -> None:
    """Write mono 16-bit PCM WAV using only the standard library."""
    frames = bytearray()
    for s in samples:
        clamped = max(-1.0, min(1.0, s))
        frames += struct.pack("<h", int(round(clamped * 32767.0)))
    with wave.open(path, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate))
        wav.writeframes(bytes(frames))


def _run_cli(args: list[str]) -> subprocess.CompletedProcess:
    src_dir = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src_dir + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run(
        [sys.executable, "-m", "libsonare.cli", *args],
        capture_output=True,
        text=True,
        env=env,
    )


@pytest.mark.parametrize(
    ("command", "extra"),
    [
        ("pitch-shift", ["--semitones", "2"]),
        ("normalize", ["--target-db", "-3"]),
        ("time-stretch", ["--rate", "1.1"]),
        ("trim-silence", []),
        ("resample", ["--target-rate", "16000"]),
    ],
)
def test_effect_cli_commands_write_output(command: str, extra: list[str]) -> None:
    """Each new effect subcommand runs end-to-end and writes an output WAV."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        out_path = os.path.join(tmpdir, "out.wav")
        _write_test_wav(wav_path, _generate_sine(440, 22050, 0.5), 22050)

        result = _run_cli([command, wav_path, "--output", out_path, "--json", *extra])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["length"] > 0
        assert os.path.exists(out_path)


def test_pitch_shift_cli_reports_semitones_end_to_end() -> None:
    """pitch-shift emits the requested shift and a non-empty result."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        out_path = os.path.join(tmpdir, "shifted.wav")
        _write_test_wav(wav_path, _generate_sine(440, 22050, 0.5), 22050)
        result = _run_cli(["pitch-shift", wav_path, "-o", out_path, "--semitones", "3", "--json"])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["semitones"] == 3.0
        assert payload["sample_rate"] == 22050
        assert payload["length"] > 0


def test_voice_change_preset_rejects_simple_knob_conflict() -> None:
    """A realtime preset cannot be combined with offline pitch/formant controls."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        out_path = os.path.join(tmpdir, "changed.wav")
        _write_test_wav(wav_path, _generate_sine(440, 22050, 0.1), 22050)

        result = _run_cli(
            [
                "voice-change",
                wav_path,
                "-o",
                out_path,
                "--preset",
                "neutral-monitor",
                "--pitch-semitones",
                "5",
                "--formant-factor",
                "1.1",
            ]
        )

        assert result.returncode == 3
        assert "cannot be combined with a realtime preset" in result.stderr


def test_voice_change_preset_pack_applies_overrides_end_to_end() -> None:
    """A preset-pack entry and repeated override syntax render an output WAV."""
    pack_path = (
        Path(__file__).parents[3] / "schemas" / "realtime-voice-changer-presets.example.json"
    )
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        out_path = os.path.join(tmpdir, "changed.wav")
        _write_test_wav(wav_path, _generate_sine(440, 22050, 0.1), 22050)

        result = _run_cli(
            [
                "voice-change",
                wav_path,
                "-o",
                out_path,
                "--preset-pack",
                str(pack_path),
                "--preset",
                "neutral-monitor",
                "--set",
                "dsp.outputGainDb=-2",
                "--set",
                "dsp.reverb.mix=0.2",
                "--json",
            ]
        )

        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["length"] > 0
        assert payload["output"] == out_path
        assert os.path.exists(out_path)


def test_acoustic_cli_stays_blind_without_ir() -> None:
    """--ir is the only route into IR analysis, on an impulse-like file too.

    This is the anchor half of a two-CLI invariant: the Python CLI runs
    sonare_detect_acoustic, which takes no mode argument and is therefore blind
    by construction, and the native CLI case in tests/cli/cli_test.cpp pins the
    same output shape for the side that can pick a mode.
    """
    sample_rate = 48000
    decay = math.log(1000.0) / 0.6
    state = 0x1234567
    samples = []
    for index in range(int(0.75 * sample_rate)):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        noise = ((state >> 8) & 0xFFFF) / 32768.0 - 1.0
        samples.append(noise * math.exp(-decay * index / sample_rate))
    samples[0] = 1.0

    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "ir.wav")
        _write_test_wav(wav_path, samples, sample_rate)

        result = _run_cli(["acoustic", wav_path, "--json"])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["is_blind"] is True
        # Blind estimation cannot measure clarity and reports that as null
        # rather than as a value.
        assert payload["c50"] is None
        assert payload["c80"] is None
        assert payload["d50"] is None
        assert payload["c50_bands"] == []


def test_voice_preset_validate_set_delivers_a_json_value_containing_commas() -> None:
    """One --set occurrence is one assignment, commas inside the value included.

    Repeated occurrences used to be split on every comma, so an object, an
    array, or ordinary free text was torn into fragments with no escape
    available to the caller.
    """
    preset = {
        "schemaVersion": 1,
        "id": "set-fixture",
        "name": "Set Fixture",
        "category": "custom",
        "macros": {
            "pitch": 0,
            "formant": 1,
            "brightness": 0,
            "space": 0,
            "intensity": 0.5,
            "noiseControl": 0,
            "sibilance": 0,
        },
    }
    with tempfile.TemporaryDirectory() as tmpdir:
        preset_path = os.path.join(tmpdir, "macro-preset.json")
        with open(preset_path, "w", encoding="utf-8") as handle:
            json.dump(preset, handle)

        applied = _run_cli(
            [
                "voice-preset-validate",
                preset_path,
                "--set",
                "description=Adds warmth, presence, and air",
                "--set",
                'macros={"pitch":3,"brightness":0.75}',
                "--json",
            ]
        )
        assert applied.returncode == 0, applied.stderr
        normalized = json.loads(json.loads(applied.stdout)["normalized_json"])
        assert normalized["description"] == "Adds warmth, presence, and air"
        # Both members of the object have to arrive: pitch drives
        # retune.semitones and brightness 0.75 drives presenceDb +3, so either
        # one alone would leave the other at its fixture value.
        assert normalized["dsp"]["retune"]["semitones"] == pytest.approx(3.0)
        assert normalized["dsp"]["eq"]["presenceDb"] == pytest.approx(3.0)

        rejected = _run_cli(
            ["voice-preset-validate", preset_path, "--set", "macros.pitch=[1,2]", "--json"]
        )
        # The preset schema rejects the array on its own terms; the splitter
        # used to fail first, on the orphaned "2]" fragment.
        assert rejected.returncode == 3
        payload = json.loads(rejected.stdout)
        assert payload["ok"] is False
        assert payload["error"] == "field must be numeric: macros.pitch"


def test_normalize_cli_reports_target_db_end_to_end() -> None:
    """normalize emits the requested target level and a non-empty result."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        out_path = os.path.join(tmpdir, "normalized.wav")
        _write_test_wav(wav_path, [0.25 * s for s in _generate_sine(440, 22050, 0.5)], 22050)
        result = _run_cli(["normalize", wav_path, "-o", out_path, "--target-db", "-6", "--json"])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["target_db"] == -6.0
        assert payload["length"] > 0


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


def test_hpss_cli_writes_harmonic_and_percussive_stems() -> None:
    """HPSS output paths produce the two documented stem WAVs."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        output_path = os.path.join(tmpdir, "separated.wav")
        _write_test_wav(wav_path, _generate_sine(440, 22050, 0.25), 22050)

        result = _run_cli(["hpss", wav_path, "--output", output_path, "--json"])

        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["harmonic"] == os.path.join(tmpdir, "separated_harmonic.wav")
        assert payload["percussive"] == os.path.join(tmpdir, "separated_percussive.wav")
        assert os.path.exists(payload["harmonic"])
        assert os.path.exists(payload["percussive"])


def test_analysis_only_cli_rejects_output_path() -> None:
    """Analysis commands must not silently accept an output path."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        output_path = os.path.join(tmpdir, "ignored.wav")
        _write_test_wav(wav_path, _generate_sine(440, 22050, 0.25), 22050)

        result = _run_cli(["bpm", wav_path, "--output", output_path])

        assert result.returncode != 0
        assert "bpm does not produce an audio file" in result.stderr
        assert not os.path.exists(output_path)


def test_lufs_cli_emits_strict_json_for_silence() -> None:
    """Non-finite loudness measurements are JSON null, never NaN/-Infinity."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "silence.wav")
        _write_test_wav(wav_path, [0.0] * 4096, 22050)

        result = _run_cli(["lufs", wav_path, "--json"])

        assert result.returncode == 0, result.stderr
        assert "Infinity" not in result.stdout
        assert "NaN" not in result.stdout
        payload = json.loads(result.stdout)
        assert payload["integrated_lufs"] is None


def test_project_cli_preserves_common_options_before_subcommand() -> None:
    """Project-level --json/-o survive parsing a project child command."""
    with tempfile.TemporaryDirectory() as tmpdir:
        output_path = os.path.join(tmpdir, "empty-project.json")
        created = _run_cli(["project", "--json", "-o", output_path, "new"])

        assert created.returncode == 0, created.stderr
        assert json.loads(created.stdout)["output"] == output_path
        assert os.path.exists(output_path)

    abi = _run_cli(["project", "--json", "abi"])
    assert abi.returncode == 0, abi.stderr
    assert isinstance(json.loads(abi.stdout)["abi_version"], int)


def test_mastering_processor_cli_uses_the_runtime_stereo_catalog(monkeypatch) -> None:
    """Stereo routing follows the core catalog instead of a duplicated ID set (L-1)."""
    import libsonare
    from libsonare import cli

    monkeypatch.setattr(cli, "_load_audio", lambda _path: ([0.1, -0.1], 22050))
    monkeypatch.setattr(
        libsonare,
        "mastering_processor_catalog",
        lambda: [{"id": "custom.stereo", "stereoOnly": True}],
    )

    called: list[str] = []

    def stereo(name, left, right, **_kwargs):
        called.append(name)
        return argparse.Namespace(
            left=left,
            right=right,
            sample_rate=22050,
            input_lufs=-20.0,
            output_lufs=-19.0,
            applied_gain_db=1.0,
            latency_samples=0,
        )

    monkeypatch.setattr(libsonare, "mastering_process_stereo", stereo)
    args = argparse.Namespace(
        processor="custom.stereo", file="tone.wav", params="", output="", json=True
    )

    assert cli.cmd_mastering_processor(args) == 0
    assert called == ["custom.stereo"]


def test_effect_cli_commands_appear_in_help() -> None:
    """The new offline effect subcommands are advertised in --help."""
    result = _run_cli(["--help"])
    assert result.returncode == 0
    for command in ("pitch-shift", "time-stretch", "normalize", "trim-silence", "resample"):
        assert command in result.stdout


def test_mix_cli_resamples_inputs_to_mixer_rate(monkeypatch, tmp_path) -> None:
    """cmd_mix resamples each input to the mixer rate before mixing."""
    import libsonare
    from libsonare import cli

    captured: dict[str, object] = {}

    # A 44.1 kHz stem: mixing it untouched at the 48 kHz default would play fast.
    monkeypatch.setattr(cli, "_load_audio", lambda path: ([0.0] * 441, 44100))

    class FakeMixer:
        @classmethod
        def from_scene_json(cls, scene_json, *, sample_rate, block_size):
            captured["sample_rate"] = sample_rate
            return cls()

        def strip_count(self) -> int:
            return 1

        def compile(self) -> None:
            pass

        def process_stereo(self, left, right):
            captured["left_len"] = len(left[0])
            return SimpleNamespace(left=list(left[0]), right=list(right[0]))

        def tail_samples(self) -> int:
            return 0

        def close(self) -> None:
            pass

    monkeypatch.setattr(libsonare, "Mixer", FakeMixer)
    monkeypatch.setattr(libsonare, "mixing_scene_preset_json", lambda name: "{}")
    args = argparse.Namespace(
        scene="",
        preset="demo",
        input=["stem.wav"],
        output=str(tmp_path / "out.wav"),
        sample_rate=48000,
        block_size=512,
        json=True,
    )
    assert cli.cmd_mix(args) == 0
    # 441 samples at 44.1 kHz resample to 480 samples at 48 kHz.
    assert captured["sample_rate"] == 48000
    assert captured["left_len"] == 480


def test_mix_cli_processes_blocks_partial_block_and_tail(monkeypatch, tmp_path) -> None:
    """The CLI keeps one mixer alive across bounded blocks and drains its tail."""
    import libsonare
    from libsonare import cli

    block_lengths: list[int] = []
    drain_lengths: list[int] = []
    monkeypatch.setattr(cli, "_load_audio", lambda _path: ([0.25] * 513, 48000))

    class FakeMixer:
        @classmethod
        def from_scene_json(cls, _scene_json, *, sample_rate, block_size):
            assert sample_rate == 48000
            assert block_size == 512
            return cls()

        def strip_count(self) -> int:
            return 1

        def compile(self) -> None:
            pass

        def process_stereo(self, left, right):
            block_lengths.append(len(left[0]))
            return SimpleNamespace(left=list(left[0]), right=list(right[0]))

        def tail_samples(self) -> int:
            return 515

        def drain_tail_stereo(self, count):
            drain_lengths.append(count)
            return SimpleNamespace(left=[0.0] * count, right=[0.0] * count)

        def close(self) -> None:
            pass

    monkeypatch.setattr(libsonare, "Mixer", FakeMixer)
    monkeypatch.setattr(libsonare, "mixing_scene_preset_json", lambda _name: "{}")
    output = tmp_path / "mix.wav"
    args = argparse.Namespace(
        scene="",
        preset="demo",
        input=["stem.wav"],
        output=str(output),
        sample_rate=48000,
        block_size=512,
        json=True,
    )

    assert cli.cmd_mix(args) == 0
    assert block_lengths == [512, 1]
    assert drain_lengths == [512, 3]
    with wave.open(str(output), "rb") as wav:
        assert wav.getnchannels() == 2
        assert wav.getnframes() == 1028


def test_mix_cli_rejects_output_without_inputs(tmp_path) -> None:
    """An explicit output never succeeds without producing an artifact."""
    output = tmp_path / "missing.wav"
    result = _run_cli(["mix", "--preset", "vocalReverbSend", "-o", str(output)])

    assert result.returncode == 3
    assert "requires at least one --input" in result.stderr
    assert not output.exists()


def test_mix_cli_real_mixer_handles_multiple_blocks_and_stems(tmp_path) -> None:
    """The subprocess path renders >block-size input through the real mixer."""
    first = tmp_path / "first.wav"
    second = tmp_path / "second.wav"
    output = tmp_path / "mix.wav"
    _write_test_wav(str(first), [0.1] * 513, 48000)
    _write_test_wav(str(second), [0.05] * 513, 48000)

    result = _run_cli(
        [
            "mix",
            "--preset",
            "vocalReverbSend",
            "--input",
            str(first),
            "--input",
            str(second),
            "--block-size",
            "512",
            "--sample-rate",
            "48000",
            "--output",
            str(output),
            "--json",
        ]
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert payload["rendered_samples"] >= 513
    with wave.open(str(output), "rb") as wav:
        assert wav.getnchannels() == 2
        assert wav.getnframes() == payload["rendered_samples"]


@pytest.mark.parametrize(
    ("command", "expects_n_fft", "expects_hop"),
    [
        ("key", True, True),
        ("mel", True, True),
        ("chroma", True, True),
        ("spectral", True, True),
        ("hpss", True, True),
        ("onset-envelope", True, True),
        ("nnls-chroma", False, True),
        ("tempogram", True, True),
        ("plp", True, True),
        ("bpm", False, False),
        ("beats", False, False),
        ("analyze", False, False),
        ("pitch", False, True),
        ("pitch-correct", False, False),
        ("mastering", False, False),
    ],
)
def test_fft_options_are_only_advertised_by_consuming_commands(
    command, expects_n_fft, expects_hop
) -> None:
    """A visible FFT option always belongs to a handler that consumes it."""
    result = _run_cli([command, "--help"])
    assert result.returncode == 0
    assert ("--n-fft" in result.stdout) is expects_n_fft
    assert ("--hop-length" in result.stdout) is expects_hop
    expects_mels = command in {
        "mel",
        "onset-envelope",
        "tempogram",
        "plp",
    }
    assert ("--n-mels" in result.stdout) is expects_mels


def test_catalog_json_uses_native_cli_object_shapes(monkeypatch, capsys) -> None:
    """Catalog commands expose stable named top-level arrays."""
    import libsonare
    from libsonare import cli

    monkeypatch.setattr(libsonare, "mastering_processor_names", lambda: ["a"])
    monkeypatch.setattr(libsonare, "mastering_pair_processor_names", lambda: ["b"])
    monkeypatch.setattr(libsonare, "mastering_pair_analysis_names", lambda: ["c"])
    monkeypatch.setattr(libsonare, "mixing_scene_preset_names", lambda: ["d"])
    args = argparse.Namespace(json=True)

    cli.cmd_mastering_processors(args)
    cli.cmd_mastering_pair_processors(args)
    cli.cmd_mastering_pair_analyses(args)
    cli.cmd_mixing_presets(args)
    assert [json.loads(line) for line in capsys.readouterr().out.splitlines()] == [
        {"processors": ["a"]},
        {"processors": ["b"]},
        {"analyses": ["c"]},
        {"presets": ["d"]},
    ]


def test_atomic_byte_writer_preserves_old_output_and_cleans_temp(monkeypatch, tmp_path) -> None:
    """A failed final replace leaves the previous artifact untouched."""
    from libsonare import cli
    from libsonare._cli_common import EXIT_ENCODE_FAILED
    from libsonare._runtime import SonareError

    output = tmp_path / "result.bin"
    output.write_bytes(b"old")

    def fail_replace(_source, _target):
        raise OSError("injected replace failure")

    monkeypatch.setattr(os, "replace", fail_replace)
    with pytest.raises(SonareError, match="injected") as raised:
        cli._atomic_write_bytes(str(output), b"new")
    assert cli._exit_code_for(raised.value) == EXIT_ENCODE_FAILED

    assert output.read_bytes() == b"old"
    assert list(tmp_path.iterdir()) == [output]


def test_atomic_wav_writer_preserves_old_output_and_cleans_temp(monkeypatch, tmp_path) -> None:
    """WAV finalization failure cannot truncate an earlier render."""
    from libsonare import cli
    from libsonare._cli_common import EXIT_ENCODE_FAILED
    from libsonare._runtime import SonareError

    output = tmp_path / "result.wav"
    output.write_bytes(b"old")

    def fail_replace(_source, _target):
        raise OSError("injected replace failure")

    monkeypatch.setattr(os, "replace", fail_replace)
    # Creating, writing and finalizing the output are all stages of producing
    # the file, so a failure in any of them reports the encode class. The
    # commonest instance is a `-o` that resolves to a directory, which nothing
    # rejects until the atomic replace; escaping as a bare OSError it landed on
    # the generic error code while the native CLI reported exit 12 for the same
    # condition.
    with pytest.raises(SonareError, match="injected") as raised:
        cli._write_wav(str(output), [0.25] * 10, 48000)
    assert cli._exit_code_for(raised.value) == EXIT_ENCODE_FAILED

    assert output.read_bytes() == b"old"
    assert list(tmp_path.iterdir()) == [output]


@pytest.mark.parametrize(("size", "accepted"), [(1023, True), (1024, True), (1025, False)])
def test_bounded_reader_enforces_limit_before_copy(tmp_path, size, accepted) -> None:
    """Project/MIDI imports accept the limit and reject the first byte above it."""
    from libsonare import cli

    source = tmp_path / "large.mid"
    with source.open("wb") as fh:
        fh.truncate(size)

    if accepted:
        assert len(cli._read_bounded(str(source), 1024)) == size
    else:
        with pytest.raises(ValueError, match="1024 byte limit"):
            cli._read_bounded(str(source), 1024)


def test_memory_error_maps_to_out_of_memory_exit() -> None:
    from libsonare import cli

    assert cli._exit_code_for(MemoryError()) == cli.EXIT_OUT_OF_MEMORY


def test_cancelled_error_maps_to_cancelled_exit() -> None:
    from libsonare import SonareError, cli

    assert cli._exit_code_for(SonareError(8, "cancelled")) == cli.EXIT_CANCELLED


def test_resample_uses_native_antialiased_resampler(monkeypatch) -> None:
    """cmd_resample routes through the native resampler, not linear interpolation."""
    from libsonare import cli

    samples = _generate_sine(440, 44100, 0.02)
    monkeypatch.setattr(cli, "_load_audio", lambda path: (samples, 44100))
    monkeypatch.setattr(cli, "_write_wav", lambda *a, **k: None)

    # resample renders audio, so it requires an output destination (matching the
    # native CLI); the write itself is stubbed above.
    args = argparse.Namespace(
        file="tone.wav",
        target_rate=48000,
        output="resampled.wav",
        json=True,
    )

    native = cli._resample(samples, 44100, 48000)
    linear = cli._resample_linear(samples, 44100, 48000)
    # The C-ABI r8brain resampler is anti-aliased, so it diverges from the
    # linear-interpolation fallback that used to back this subcommand.
    assert native != pytest.approx(linear)
    assert len(native) == round(len(samples) * 48000 / 44100)
    assert cli.cmd_resample(args) == 0


def test_resample_falls_back_to_linear_without_native_lib(monkeypatch) -> None:
    """When the native library cannot load, cmd_resample degrades to linear."""
    import libsonare
    from libsonare import cli

    samples = _generate_sine(440, 44100, 0.02)

    def _raise_os_error(*_args, **_kwargs):
        raise OSError("libsonare shared library not found")

    monkeypatch.setattr(libsonare, "resample", _raise_os_error)

    result = cli._resample(samples, 44100, 48000)
    assert result == cli._resample_linear(samples, 44100, 48000)


def test_rir_and_morph_missing_output_raise_value_error() -> None:
    """Missing --output raises ValueError so the exit code is EXIT_ERROR (L-15)."""
    from libsonare import cli

    with pytest.raises(ValueError, match="requires --output"):
        cli.cmd_synthesize_rir(argparse.Namespace(output=""))
    with pytest.raises(ValueError, match="requires --output"):
        cli.cmd_room_morph(argparse.Namespace(output=""))


def test_synthesize_rir_missing_output_uses_error_exit_code() -> None:
    """The missing-arg failure maps to EXIT_INVALID_PARAMETER."""
    from libsonare.cli import EXIT_INVALID_PARAMETER

    result = _run_cli(["synthesize-rir"])
    assert result.returncode == EXIT_INVALID_PARAMETER
    assert "requires --output" in result.stderr


def test_chords_json_reports_c_bass_not_root(monkeypatch, capsys) -> None:
    """A slash chord with C in the bass emits bass=0/C, not the root."""
    import types as _types

    import libsonare
    from libsonare import cli
    from libsonare.types import PitchClass

    # F/C: root F (5), bass C (0). PitchClass.C == 0 is falsy, so a naive
    # ``chord.bass or chord.root`` would incorrectly report the root (5).
    fake_chord = _types.SimpleNamespace(
        name="F/C",
        root=PitchClass.F,
        quality="maj",
        bass=PitchClass.C,
        start=0.0,
        end=1.0,
        confidence=0.9,
    )
    fake_result = _types.SimpleNamespace(chords=[fake_chord])

    monkeypatch.setattr(cli, "_load_audio", lambda path: ([0.0] * 1024, 22050))
    monkeypatch.setattr(libsonare, "detect_chords", lambda *a, **k: fake_result)

    args = argparse.Namespace(
        file="unused.wav",
        min_duration=0.0,
        smoothing_window=0,
        threshold=0.0,
        triads_only=False,
        n_fft=2048,
        hop_length=512,
        no_beat_sync=True,
        use_hmm=False,
        hmm_beam_width=0,
        key_context=False,
        key_root="C",
        key_mode="major",
        detect_inversions=True,
        nnls=False,
        json=True,
    )
    assert cli.cmd_chords(args) == 0
    payload = json.loads(capsys.readouterr().out)
    chord = payload["chords"][0]
    assert chord["root"] == PitchClass.F.value
    assert chord["bass"] == PitchClass.C.value == 0


def test_synthesize_rir_invalid_geometry_maps_invalid_parameter(monkeypatch) -> None:
    """Invalid room geometry returns EXIT_INVALID_PARAMETER, not a bare 1."""
    import types as _types

    import libsonare
    from libsonare import cli
    from libsonare.cli import EXIT_INVALID_PARAMETER

    monkeypatch.setattr(
        libsonare,
        "synthesize_rir",
        lambda *a, **k: _types.SimpleNamespace(has_error=True, rir=[], sample_rate=48000),
    )
    args = argparse.Namespace(
        output="out.wav",
        length=7.0,
        width=5.0,
        height=3.0,
        source_x=1.0,
        source_y=1.0,
        source_z=1.2,
        listener_x=5.0,
        listener_y=4.0,
        listener_z=1.7,
        absorption=0.2,
        sample_rate=48000,
        ism_order=3,
        seed=1,
        max_seconds=0.0,
        sabine=False,
        json=False,
    )
    assert cli.cmd_synthesize_rir(args) == EXIT_INVALID_PARAMETER
    assert EXIT_INVALID_PARAMETER != 1


def test_synthesize_rir_invalid_geometry_exit_code_end_to_end() -> None:
    """The invalid-geometry failure maps through main() to EXIT_INVALID_PARAMETER."""
    from libsonare.cli import EXIT_INVALID_PARAMETER

    with tempfile.TemporaryDirectory() as tmpdir:
        out_path = os.path.join(tmpdir, "rir.wav")
        # A source far outside a 7x5x3 m room forces an invalid-geometry result.
        result = _run_cli(["synthesize-rir", "--output", out_path, "--source-x", "999"])
        assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
        assert "acoustic.source_outside_room" in result.stderr


def _run_cli_env(args: list[str], extra_env: dict[str, str]) -> subprocess.CompletedProcess:
    src_dir = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src_dir + os.pathsep + env.get("PYTHONPATH", "")
    env.update(extra_env)
    return subprocess.run(
        [sys.executable, "-m", "libsonare.cli", *args],
        capture_output=True,
        text=True,
        env=env,
    )


def test_synthesize_rir_invalid_geometry_honors_legacy_exit_code() -> None:
    """SONARE_LEGACY_EXIT=1 folds the granular invalid-geometry code down to 1."""
    with tempfile.TemporaryDirectory() as tmpdir:
        out_path = os.path.join(tmpdir, "rir.wav")
        result = _run_cli_env(
            ["synthesize-rir", "--output", out_path, "--source-x", "999"],
            {"SONARE_LEGACY_EXIT": "1"},
        )
        assert result.returncode == 1, result.stderr


def test_pcm16_clamps_and_stays_byte_identical() -> None:
    """The shared PCM helper preserves the clamp-and-scale contract."""
    from libsonare import cli

    assert cli._pcm16(0.0) == struct.pack("<h", 0)
    assert cli._pcm16(1.0) == struct.pack("<h", 32767)
    assert cli._pcm16(-1.0) == struct.pack("<h", -32767)
    assert cli._pcm16(0.5) == struct.pack("<h", 16384)
    # Out-of-range values clamp to the full-scale endpoints.
    assert cli._pcm16(2.0) == struct.pack("<h", 32767)
    assert cli._pcm16(-2.0) == struct.pack("<h", -32767)


# Codes the native writer produces for samples that separate the two rounding
# conventions: each `x` lands on or near a .5 boundary once scaled, where
# round-half-to-even and round-half-away-from-zero disagree. Recorded from
# `std::lroundf` on the 32-bit product, which is the contract `float_to_pcm16`
# and `float_to_pcm24` (src/core/audio_io.cpp) publish. The non-finite rows
# never reach the scale at all: they have no PCM image and are written as
# digital silence.
_PCM16_NATIVE_CODES = [
    (1.5259254723787308e-05, 1),
    (-1.5259254723787308e-05, -1),
    (4.577776417136192e-05, 2),
    (-4.577776417136192e-05, -2),
    (7.629627361893654e-05, 3),
    (-7.629627361893654e-05, -3),
    (0.00032044434919953346, 11),
    (-0.00032044434919953346, -11),
    (0.0030976287089288235, 102),
    (-0.0030976287089288235, -102),
    (0.500030517578125, 16385),
    (-0.500030517578125, -16385),
    (0.5625629425048828, 18434),
    (-0.8147373795509338, -26697),
    (-0.5259407162666321, -17234),
    (0.6181066036224365, 20254),
    (-0.7396008033483668, -24234),
    (0.30104981593725233, 9865),
    (1.0, 32767),
    (-1.0, -32767),
    (1.5, 32767),
    (-1.5, -32767),
    (0.0, 0),
    (float("nan"), 0),
    (float("inf"), 0),
    (float("-inf"), 0),
]

_PCM24_NATIVE_CODES = [
    (5.960465188081798e-08, 1),
    (-5.960465188081798e-08, -1),
    (2.9802325229866256e-07, 3),
    (-2.9802325229866256e-07, -3),
    (0.5, 4194304),
    (-0.5, -4194304),
    (0.5625629425048828, 4719120),
    (-0.7396008033483668, -6204221),
    (1.0, 8388607),
    (-1.0, -8388607),
    (1.5, 8388607),
    (-1.5, -8388607),
    (0.0, 0),
    (float("nan"), 0),
    (float("inf"), 0),
    (float("-inf"), 0),
]


def test_pcm_quantization_matches_the_native_writer_on_boundary_samples() -> None:
    """Python and native WAV writers agree sample for sample, halves included."""
    from libsonare._cli_common import _pcm16, _pcm24

    for sample, expected in _PCM16_NATIVE_CODES:
        assert _pcm16(sample) == struct.pack("<h", expected), sample
    for sample, expected in _PCM24_NATIVE_CODES:
        assert _pcm24(sample) == expected.to_bytes(3, "little", signed=True), sample


def test_pcm_boundary_vector_separates_the_two_rounding_conventions() -> None:
    """The vector above is not satisfied by round-half-to-even on float64.

    Round-half-away and round-half-to-even agree on every even-side sample, so a
    vector that never lands on a half would pass under either rule and assert
    nothing. This is the control that says the vector discriminates.
    """
    disagreeing = 0
    for sample, expected in _PCM16_NATIVE_CODES:
        if not math.isfinite(sample):  # substituted before the rounding step
            continue
        clamped = max(-1.0, min(1.0, sample))
        if int(round(clamped * 32767.0)) != expected:
            disagreeing += 1
    assert disagreeing >= 10


def test_voice_set_preserves_removed_macro_for_core_validation() -> None:
    """`macros.*` is no longer CLI sugar and reaches the core as an unknown field."""
    from libsonare import cli

    preset: dict[str, object] = {"dsp": {"retune": {"semitones": 0}}}
    resolved = cli._apply_voice_sets(preset, ["macros.pitch=12"])
    assert isinstance(resolved, dict)
    assert resolved["macros"] == {"pitch": 12}
    assert resolved["dsp"] == {"retune": {"semitones": 0}}


def test_voice_preset_validate_help_describes_a_preset_file() -> None:
    """The positional is a JSON preset, not audio, and audio-analysis flags
    (--n-fft/--hop-length/--n-mels) do not leak into this subcommand's help."""
    result = _run_cli(["voice-preset-validate", "--help"])
    assert result.returncode == 0, result.stderr
    assert "Voice preset JSON file" in result.stdout
    assert "Audio file path" not in result.stdout
    for leaked in ("--n-fft", "--hop-length", "--n-mels"):
        assert leaked not in result.stdout


def test_voice_preset_validate_normalizes_a_preset_file() -> None:
    """The command still validates a preset JSON file end-to-end after the
    positional argument was reworked to take a preset file."""
    preset = _run_cli(["voice-preset", "--preset", "neutral-monitor", "--json"])
    assert preset.returncode == 0, preset.stderr
    with tempfile.TemporaryDirectory() as tmpdir:
        preset_path = os.path.join(tmpdir, "preset.json")
        with open(preset_path, "w", encoding="utf-8") as fh:
            fh.write(preset.stdout)
        result = _run_cli(["voice-preset-validate", preset_path, "--json"])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert isinstance(payload, dict)


def test_voice_preset_validate_accepts_native_preset_json_alias() -> None:
    """The native CLI's ``--preset-json`` spelling remains cross-CLI compatible."""
    preset = _run_cli(["voice-preset", "--preset", "neutral-monitor", "--json"])
    assert preset.returncode == 0, preset.stderr
    with tempfile.TemporaryDirectory() as tmpdir:
        preset_path = os.path.join(tmpdir, "preset.json")
        with open(preset_path, "w", encoding="utf-8") as fh:
            fh.write(preset.stdout)
        result = _run_cli(["voice-preset-validate", "--preset-json", preset_path, "--json"])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["ok"] is True


def test_voice_preset_validate_rejects_an_invalid_preset_file() -> None:
    """Validation failure is a CI-visible invalid-format exit, not success."""
    with tempfile.TemporaryDirectory() as tmpdir:
        preset_path = os.path.join(tmpdir, "invalid-preset.json")
        with open(preset_path, "w", encoding="utf-8") as fh:
            fh.write('{"not": "a voice changer preset"}')
        result = _run_cli(["voice-preset-validate", preset_path, "--json"])
        assert result.returncode == 3, result.stderr
        payload = json.loads(result.stdout)
        assert payload["ok"] is False


def test_synthesize_rir_reports_every_warning_and_keeps_the_tail_headroom() -> None:
    """A max_seconds below the direct-sound arrival raises three warnings at once.

    None of them sets ``has_error``, so discarding them made a truncated RIR
    indistinguishable from a complete one under a green exit. All three have to
    reach the caller, which also means the C ABI cannot publish only the first.
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        out_path = os.path.join(tmpdir, "rir.wav")
        result = _run_cli(
            [
                "synthesize-rir",
                "--output",
                out_path,
                "--max-seconds",
                "0.005",
                "--sample-rate",
                "22050",
                "--json",
            ]
        )
        assert result.returncode == 0, result.stderr
        for code in (
            "acoustic.rir_length_clamped",
            "acoustic.rir_length_floored",
            "acoustic.no_late_tail",
        ):
            assert code in result.stderr

        # The diagnostics go to stderr, so the JSON document on stdout stays
        # exactly the payload both CLIs publish.
        payload = json.loads(result.stdout)
        assert set(payload) == {"output", "samples", "sample_rate"}

        # A RIR carries its physical 1/(4*pi*d) attenuation, so its peak sits far
        # below full scale; 16-bit PCM spends roughly 36 dB of the headroom the
        # tail needs, and half the reported samples came back exactly zero.
        with wave.open(out_path, "rb") as handle:
            assert handle.getsampwidth() == 3

        # A request the synthesizer can satisfy in full stays silent, so a
        # warning line is evidence about that run rather than boilerplate.
        complete_path = os.path.join(tmpdir, "complete.wav")
        complete = _run_cli(
            [
                "synthesize-rir",
                "--output",
                complete_path,
                "--max-seconds",
                "2",
                "--sample-rate",
                "22050",
                "--json",
            ]
        )
        assert complete.returncode == 0, complete.stderr
        assert "warning:" not in complete.stderr
        with wave.open(complete_path, "rb") as handle:
            assert handle.getsampwidth() == 3
