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


def test_hpss_cli_writes_harmonic_and_percussive_stems() -> None:
    """HPSS output paths produce the two documented stem WAVs (M-4)."""
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
    """Analysis commands must not silently accept an output path (M-4)."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        output_path = os.path.join(tmpdir, "ignored.wav")
        _write_test_wav(wav_path, _generate_sine(440, 22050, 0.25), 22050)

        result = _run_cli(["bpm", wav_path, "--output", output_path])

        assert result.returncode != 0
        assert "bpm does not produce an audio file" in result.stderr
        assert not os.path.exists(output_path)


def test_lufs_cli_emits_strict_json_for_silence() -> None:
    """Non-finite loudness measurements are JSON null, never NaN/-Infinity (M-1)."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "silence.wav")
        _write_test_wav(wav_path, [0.0] * 4096, 22050)

        result = _run_cli(["lufs", wav_path, "--json"])

        assert result.returncode == 0, result.stderr
        assert "Infinity" not in result.stdout
        assert "NaN" not in result.stdout
        payload = json.loads(result.stdout)
        assert payload["integrated"] is None


def test_project_cli_preserves_common_options_before_subcommand() -> None:
    """Project-level --json/-o survive parsing a project child command (M-3)."""
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
    ("command", "accepted"),
    [
        ("key", True),
        ("mel", True),
        ("chroma", True),
        ("spectral", True),
        ("hpss", False),
        ("onset-envelope", True),
        ("nnls-chroma", False),
        ("tempogram", True),
        ("plp", True),
        ("bpm", False),
        ("beats", False),
        ("analyze", False),
        ("pitch", False),
        ("mastering", False),
    ],
)
def test_fft_options_are_only_advertised_by_consuming_commands(command, accepted) -> None:
    """A visible FFT option always belongs to a handler that consumes it."""
    result = _run_cli([command, "--help"])
    assert result.returncode == 0
    assert ("--n-fft" in result.stdout) is accepted
    assert ("--hop-length" in result.stdout) is accepted
    expects_mels = accepted and command in {
        "mel",
        "onset-envelope",
        "nnls-chroma",
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

    output = tmp_path / "result.bin"
    output.write_bytes(b"old")

    def fail_replace(_source, _target):
        raise OSError("injected replace failure")

    monkeypatch.setattr(os, "replace", fail_replace)
    with pytest.raises(OSError, match="injected"):
        cli._atomic_write_bytes(str(output), b"new")

    assert output.read_bytes() == b"old"
    assert list(tmp_path.iterdir()) == [output]


def test_atomic_wav_writer_preserves_old_output_and_cleans_temp(monkeypatch, tmp_path) -> None:
    """WAV finalization failure cannot truncate an earlier render."""
    from libsonare import cli

    output = tmp_path / "result.wav"
    output.write_bytes(b"old")

    def fail_replace(_source, _target):
        raise OSError("injected replace failure")

    monkeypatch.setattr(os, "replace", fail_replace)
    with pytest.raises(OSError, match="injected"):
        cli._write_wav(str(output), [0.25] * 10, 48000)

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
        assert "invalid room geometry" in result.stderr


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


def test_voice_preset_validate_rejects_an_invalid_preset_file() -> None:
    """Validation failure is a CI-visible invalid-format exit, not success."""
    with tempfile.TemporaryDirectory() as tmpdir:
        preset_path = os.path.join(tmpdir, "invalid-preset.json")
        with open(preset_path, "w", encoding="utf-8") as fh:
            fh.write('{"not": "a voice changer preset"}')
        result = _run_cli(["voice-preset-validate", preset_path, "--json"])
        assert result.returncode == 5, result.stderr
        payload = json.loads(result.stdout)
        assert payload["ok"] is False
