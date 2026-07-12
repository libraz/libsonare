"""CLI tests for offline effect subcommands and shared CLI helpers."""

from __future__ import annotations

import argparse
import json

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
        _write_test_wav(wav_path, _generate_sine(440, 22050, 0.5), 22050)
        result = _run_cli(["pitch-shift", wav_path, "--semitones", "3", "--json"])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["semitones"] == 3.0
        assert payload["sample_rate"] == 22050
        assert payload["length"] > 0


def test_normalize_cli_reports_target_db_end_to_end() -> None:
    """normalize emits the requested target level and a non-empty result."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        _write_test_wav(wav_path, [0.25 * s for s in _generate_sine(440, 22050, 0.5)], 22050)
        result = _run_cli(["normalize", wav_path, "--target-db", "-6", "--json"])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["target_db"] == -6.0
        assert payload["length"] > 0


def test_effect_cli_commands_appear_in_help() -> None:
    """The new offline effect subcommands are advertised in --help."""
    result = _run_cli(["--help"])
    assert result.returncode == 0
    for command in ("pitch-shift", "time-stretch", "normalize", "trim-silence", "resample"):
        assert command in result.stdout


def test_mix_cli_resamples_inputs_to_mixer_rate(monkeypatch) -> None:
    """cmd_mix resamples each input to the mixer rate before mixing (M-8)."""
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
            return list(left[0]), list(right[0])

        def close(self) -> None:
            pass

    monkeypatch.setattr(libsonare, "Mixer", FakeMixer)
    monkeypatch.setattr(libsonare, "mixing_scene_preset_json", lambda name: "{}")

    args = argparse.Namespace(
        scene="",
        preset="demo",
        input=["stem.wav"],
        output="",
        sample_rate=48000,
        block_size=512,
        json=True,
    )
    assert cli.cmd_mix(args) == 0
    # 441 samples at 44.1 kHz resample to 480 samples at 48 kHz.
    assert captured["sample_rate"] == 48000
    assert captured["left_len"] == 480


def test_resample_uses_native_antialiased_resampler(monkeypatch) -> None:
    """cmd_resample routes through the native resampler, not linear interpolation."""
    from libsonare import cli

    samples = _generate_sine(440, 44100, 0.02)
    monkeypatch.setattr(cli, "_load_audio", lambda path: (samples, 44100))

    args = argparse.Namespace(
        file="tone.wav",
        target_rate=48000,
        output="",
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
    """The missing-arg failure maps to EXIT_ERROR, consistent with other subcommands."""
    from libsonare.cli import EXIT_ERROR

    result = _run_cli(["synthesize-rir"])
    assert result.returncode == EXIT_ERROR
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
        assert "invalid room geometry" in result.stderr


def test_pcm16_clamps_and_stays_byte_identical() -> None:
    """The shared PCM helper preserves the clamp-and-scale contract (L-17)."""
    from libsonare import cli

    assert cli._pcm16(0.0) == struct.pack("<h", 0)
    assert cli._pcm16(1.0) == struct.pack("<h", 32767)
    assert cli._pcm16(-1.0) == struct.pack("<h", -32767)
    assert cli._pcm16(0.5) == struct.pack("<h", int(round(0.5 * 32767.0)))
    # Out-of-range values clamp to the full-scale endpoints.
    assert cli._pcm16(2.0) == struct.pack("<h", 32767)
    assert cli._pcm16(-2.0) == struct.pack("<h", -32767)


def test_voice_macro_override_table_maps_expected_dsp_paths() -> None:
    """Pin the CLI macro->dsp override table so drift from the C++ copy is caught (L-18)."""
    from libsonare import cli

    direct = {
        "macros.pitch": ("dsp", "retune", "semitones"),
        "macros.formant": ("dsp", "formant", "factor"),
        "macros.space": ("dsp", "reverb", "mix"),
        "macros.output": ("dsp", "outputGainDb"),
    }
    for macro, dsp_path in direct.items():
        root: dict = {}
        cli._apply_voice_macro_override(root, macro, 0.5)
        cursor: object = root
        for part in dsp_path:
            cursor = cursor[part]  # type: ignore[index]
        assert cursor == 0.5

    # macros.intensity applies the ratio = 1 + value * 4 transform.
    root = {}
    cli._apply_voice_macro_override(root, "macros.intensity", 0.5)
    assert root["dsp"]["compressor"]["ratio"] == 1.0 + 0.5 * 4.0

    # Exactly the five documented macros are recognised.
    recognised = []
    for name in ("pitch", "formant", "space", "intensity", "output"):
        probe: dict = {}
        cli._apply_voice_macro_override(probe, f"macros.{name}", 1.0)
        recognised.append(bool(probe))
    assert recognised == [True] * 5

    # Unknown macros and non-numeric values are no-ops.
    probe = {}
    cli._apply_voice_macro_override(probe, "macros.unknown", 1.0)
    assert probe == {}
    probe = {}
    cli._apply_voice_macro_override(probe, "macros.pitch", "not-a-number")
    assert probe == {}
