"""Metering, CLI, and detailed analyzer API tests."""

from __future__ import annotations

import argparse
import json
from types import SimpleNamespace

# ruff: noqa: F403,F405
from ._analyzer_helpers import *


def test_onset_envelope() -> None:
    """onset_envelope returns a finite per-frame envelope."""
    from libsonare import onset_envelope

    samples = _generate_sine(440, 22050, 2.0)
    env = onset_envelope(samples, sample_rate=22050)
    assert len(env) > 0
    assert _all_finite(env)


def test_fourier_tempogram() -> None:
    """fourier_tempogram returns an [n_bins x n_frames] magnitude matrix."""
    from libsonare import fourier_tempogram, onset_envelope

    samples = _generate_sine(440, 22050, 2.0)
    env = onset_envelope(samples, sample_rate=22050)
    win_length = 384
    n_frames, data = fourier_tempogram(env, sample_rate=22050, win_length=win_length)
    assert n_frames == len(env)
    n_bins = win_length // 2 + 1
    assert len(data) == n_bins * n_frames
    assert _all_finite(data)


def test_tempogram_ratio() -> None:
    """tempogram_ratio returns one finite value per factor."""
    from libsonare import onset_envelope, tempogram, tempogram_ratio

    samples = _generate_sine(440, 22050, 2.0)
    env = onset_envelope(samples, sample_rate=22050)
    win_length = 384
    _, tg = tempogram(env, sample_rate=22050)

    default_ratio = tempogram_ratio(tg, win_length=win_length, sample_rate=22050)
    assert len(default_ratio) == 5  # {0.5, 1, 2, 3, 4}
    assert _all_finite(default_ratio)

    explicit = tempogram_ratio(
        tg, win_length=win_length, sample_rate=22050, factors=[1.0, 2.0, 3.0]
    )
    assert len(explicit) == 3
    assert _all_finite(explicit)


def test_nnls_chroma() -> None:
    """nnls_chroma returns a row-major 12 x n_frames matrix."""
    from libsonare import nnls_chroma

    # 0.5 s keeps the NNLS solve (the dominant cost) fast without losing coverage.
    samples = _generate_sine(440, 22050, 0.5)
    n_frames, data = nnls_chroma(samples, sample_rate=22050)
    assert n_frames > 0
    assert len(data) == 12 * n_frames
    assert _all_finite(data)
    assert all(v >= 0.0 for v in data)  # NNLS output is non-negative


def test_lufs() -> None:
    """lufs returns finite loudness measures; louder signal reads higher."""
    from libsonare import lufs

    loud = _generate_sine(440, 48000, 3.0)
    quiet = [s * 0.1 for s in loud]

    loud_result = lufs(loud, sample_rate=48000)
    quiet_result = lufs(quiet, sample_rate=48000)

    for r in (loud_result, quiet_result):
        assert math.isfinite(r.integrated_lufs)
        assert math.isfinite(r.momentary_lufs)
        assert math.isfinite(r.short_term_lufs)
        assert math.isfinite(r.loudness_range)
        assert r.loudness_range >= 0.0

    assert loud_result.integrated_lufs > quiet_result.integrated_lufs


def test_momentary_and_short_term_lufs() -> None:
    """momentary_lufs and short_term_lufs return finite time series."""
    from libsonare import momentary_lufs, short_term_lufs

    samples = _generate_sine(440, 48000, 3.0)

    momentary = momentary_lufs(samples, sample_rate=48000)
    assert len(momentary) > 0
    assert _all_finite(momentary)

    short_term = short_term_lufs(samples, sample_rate=48000)
    assert len(short_term) > 0
    assert _all_finite(short_term)


def test_audio_onset_envelope() -> None:
    """Audio.onset_envelope returns a finite per-frame envelope."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 2.0), sample_rate=22050)
    env = audio.onset_envelope()
    assert len(env) > 0
    assert _all_finite(env)


def test_audio_nnls_chroma() -> None:
    """Audio.nnls_chroma returns a row-major 12 x n_frames matrix."""
    from libsonare import Audio

    # Wrapper-only check — the algorithm itself is covered by test_nnls_chroma.
    audio = Audio.from_buffer(_generate_sine(440, 22050, 0.5), sample_rate=22050)
    n_frames, data = audio.nnls_chroma()
    assert n_frames > 0
    assert len(data) == 12 * n_frames
    assert _all_finite(data)


def test_audio_lufs() -> None:
    """Audio.lufs returns finite loudness measures."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 48000, 3.0), sample_rate=48000)
    result = audio.lufs()
    assert math.isfinite(result.integrated_lufs)
    assert math.isfinite(result.momentary_lufs)
    assert math.isfinite(result.short_term_lufs)
    assert math.isfinite(result.loudness_range)


def test_audio_momentary_and_short_term_lufs() -> None:
    """Audio.momentary_lufs and Audio.short_term_lufs return finite series."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 48000, 3.0), sample_rate=48000)
    momentary = audio.momentary_lufs()
    short_term = audio.short_term_lufs()
    assert len(momentary) > 0
    assert len(short_term) > 0
    assert _all_finite(momentary)
    assert _all_finite(short_term)


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


@pytest.mark.parametrize("command", ["lufs", "onset-envelope", "nnls-chroma", "tempogram"])
def test_cli_new_commands_smoke(command: str) -> None:
    """New CLI subcommands run end-to-end on a synthetic WAV and emit JSON."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        # Smoke only (exit code + JSON emitted); 0.5 s keeps nnls-chroma cheap.
        _write_test_wav(wav_path, _generate_sine(440, 22050, 0.5), 22050)

        result = _run_cli([command, wav_path, "--json"])
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip()


def test_project_cli_help_documents_sf2_limitations() -> None:
    """Project MIDI rendering help states the current SF2/synth-json CLI boundary."""

    project_help = _run_cli(["project", "bounce", "--help"])
    assert project_help.returncode == 0
    assert "--sf2" in project_help.stdout
    assert "--synth-json" in project_help.stdout
    assert "SoundFont-backed bounces" in project_help.stdout

    midi_render_help = _run_cli(["midi-render", "--help"])
    assert midi_render_help.returncode == 0
    assert "--sf2" in midi_render_help.stdout
    assert "--synth-json" in midi_render_help.stdout


def test_mastering_pair_analyze_cli_resamples_reference_rate(monkeypatch, capsys) -> None:
    """The pair-analysis CLI resamples reference audio to the master sample rate."""
    import argparse

    import libsonare
    from libsonare import cli

    calls: dict[str, object] = {}

    def fake_load_audio(path: str) -> tuple[list[float], int]:
        if path == "master.wav":
            return [0.0, 1.0, 0.0, -1.0], 4
        if path == "reference.wav":
            return [0.0, 1.0], 2
        raise AssertionError(path)

    def fake_mastering_pair_analyze(
        analysis: str,
        source: list[float],
        reference: list[float],
        *,
        sample_rate: int,
    ) -> str:
        calls["analysis"] = analysis
        calls["source"] = source
        calls["reference"] = reference
        calls["sample_rate"] = sample_rate
        return '{"ok":true}'

    monkeypatch.setattr(cli, "_load_audio", fake_load_audio)
    monkeypatch.setattr(libsonare, "mastering_pair_analyze", fake_mastering_pair_analyze)

    args = argparse.Namespace(
        analysis="match.referenceLoudness",
        file="master.wav",
        reference="reference.wav",
    )
    assert cli.cmd_mastering_pair_analyze(args) == 0

    assert calls["analysis"] == "match.referenceLoudness"
    assert calls["source"] == [0.0, 1.0, 0.0, -1.0]
    assert calls["reference"] == pytest.approx([0.0, 0.5, 1.0, 1.0])
    assert calls["sample_rate"] == 4
    assert capsys.readouterr().out.strip() == '{"ok":true}'


def test_project_cli_exports_smf_and_midi_render_smoke() -> None:
    """The Python CLI exposes project validation, SMF export/import, and MIDI render."""
    from libsonare import Project, project_abi_version

    if project_abi_version() == 0:
        pytest.skip("arrangement/project ABI is not available")

    with tempfile.TemporaryDirectory() as tmpdir:
        root = Path(tmpdir)
        project_path = root / "song.json"
        smf_path = root / "song.mid"
        imported_path = root / "imported.json"
        wav_path = root / "render.wav"

        project = Project()
        try:
            project.set_sample_rate(22050)
            track_id, clip_id = project.add_midi_clip(0.0, 1.0)
            project.set_track_midi_destination(track_id, 0)
            project.set_midi_events(
                clip_id,
                [
                    Project.midi_note_on(0.0, 0, 0, 60, 100),
                    Project.midi_note_off(0.5, 0, 0, 60, 0),
                ],
            )
            project_path.write_text(project.to_json(), encoding="utf-8")
        finally:
            project.close()

        validate = _run_cli(["project", "validate", "--in", str(project_path), "--json"])
        assert validate.returncode == 0, validate.stderr
        assert json.loads(validate.stdout)["valid"] is True

        export = _run_cli(
            ["project", "export-smf", "--in", str(project_path), "-o", str(smf_path), "--json"]
        )
        assert export.returncode == 0, export.stderr
        assert smf_path.read_bytes().startswith(b"MThd")
        assert json.loads(export.stdout)["bytes"] == smf_path.stat().st_size

        imported = _run_cli(
            ["project", "import-smf", "--smf", str(smf_path), "-o", str(imported_path), "--json"]
        )
        assert imported.returncode == 0, imported.stderr
        assert imported_path.exists()
        assert json.loads(imported.stdout)["first_clip_id"] > 0

        rendered = _run_cli(
            [
                "midi-render",
                "--in",
                str(project_path),
                "-o",
                str(wav_path),
                "--frames",
                "2048",
                "--sample-rate",
                "22050",
                "--json",
            ]
        )
        assert rendered.returncode == 0, rendered.stderr
        payload = json.loads(rendered.stdout)
        assert payload["synth"] is True
        assert payload["frames"] == 2048
        with wave.open(str(wav_path), "rb") as wav:
            assert wav.getframerate() == 22050
            assert wav.getnframes() == 2048


def test_mastering_chain_cli_writes_output_and_merges_params(monkeypatch, tmp_path, capsys) -> None:
    """mastering-chain is exposed as a CLI wrapper over the Python mastering API."""
    import libsonare
    from libsonare import cli

    calls: dict[str, object] = {}

    def fake_load_audio(path: str) -> tuple[list[float], int]:
        assert path == "input.wav"
        return [0.25, -0.25, 0.0], 22050

    def fake_mastering_chain(
        samples: list[float],
        *,
        sample_rate: int,
        config: dict[str, object],
    ) -> SimpleNamespace:
        calls["samples"] = samples
        calls["sample_rate"] = sample_rate
        calls["config"] = config
        return SimpleNamespace(
            samples=[0.0, 0.1, -0.1],
            sample_rate=sample_rate,
            input_lufs=-20.0,
            output_lufs=-14.0,
            applied_gain_db=6.0,
            stages=["eq.tilt", "loudness"],
        )

    monkeypatch.setattr(cli, "_load_audio", fake_load_audio)
    monkeypatch.setattr(libsonare, "mastering_chain", fake_mastering_chain)

    output = tmp_path / "chain.wav"
    args = argparse.Namespace(
        file="input.wav",
        output=str(output),
        json=True,
        config='{"eq.tilt.tiltDb": 1.5}',
        config_file="",
        params="loudness.enabled=1",
    )

    assert cli.cmd_mastering_chain(args) == 0
    assert calls["samples"] == [0.25, -0.25, 0.0]
    assert calls["sample_rate"] == 22050
    assert calls["config"] == {"eq.tilt.tiltDb": 1.5, "loudness.enabled": 1.0}
    assert output.exists()
    payload = json.loads(capsys.readouterr().out)
    assert payload["stages"] == ["eq.tilt", "loudness"]
    assert payload["output"] == str(output)


def test_master_cli_applies_preset_and_override_params(monkeypatch, tmp_path, capsys) -> None:
    """master is a preset-oriented wrapper over master_audio."""
    import libsonare
    from libsonare import cli

    calls: dict[str, object] = {}

    monkeypatch.setattr(cli, "_load_audio", lambda path: ([0.1, -0.1], 44100))

    def fake_master_audio(
        samples: list[float],
        *,
        sample_rate: int,
        preset_name: str,
        overrides: dict[str, object],
    ) -> SimpleNamespace:
        calls["samples"] = samples
        calls["sample_rate"] = sample_rate
        calls["preset_name"] = preset_name
        calls["overrides"] = overrides
        return SimpleNamespace(
            samples=[0.0, 0.2],
            sample_rate=sample_rate,
            input_lufs=-18.0,
            output_lufs=-12.0,
            applied_gain_db=6.0,
            stages=["preset", "loudness"],
        )

    monkeypatch.setattr(libsonare, "master_audio", fake_master_audio)

    output = tmp_path / "master.wav"
    args = argparse.Namespace(
        file="input.wav",
        output=str(output),
        json=True,
        preset="streaming",
        config='{"eq.tilt.tiltDb": -0.5}',
        config_file="",
        params="loudness.enabled=1",
    )

    assert cli.cmd_master(args) == 0
    assert calls["samples"] == [0.1, -0.1]
    assert calls["sample_rate"] == 44100
    assert calls["preset_name"] == "streaming"
    assert calls["overrides"] == {"eq.tilt.tiltDb": -0.5, "loudness.enabled": 1.0}
    assert output.exists()
    payload = json.loads(capsys.readouterr().out)
    assert payload["preset"] == "streaming"
    assert payload["stages"] == ["preset", "loudness"]


def test_mastering_processor_warns_for_mono_preview_of_stereo_processors(
    monkeypatch, capsys
) -> None:
    """stereo-only processor previews duplicate mono input and warn on stderr."""
    import libsonare
    from libsonare import cli

    monkeypatch.setattr(cli, "_load_audio", lambda path: ([0.1, -0.1], 44100))

    def fake_mastering_process_stereo(
        processor: str,
        left: list[float],
        right: list[float],
        *,
        sample_rate: int,
        params: dict[str, object],
    ) -> SimpleNamespace:
        assert processor == "stereo.imager"
        assert left == [0.1, -0.1]
        assert right == [0.1, -0.1]
        assert sample_rate == 44100
        assert params == {}
        return SimpleNamespace(
            left=[0.2, -0.2],
            right=[0.0, 0.0],
            sample_rate=sample_rate,
            input_lufs=-20.0,
            output_lufs=-19.0,
            applied_gain_db=0.0,
            latency_samples=0,
        )

    monkeypatch.setattr(libsonare, "mastering_process_stereo", fake_mastering_process_stereo)

    args = argparse.Namespace(
        file="input.wav",
        output="",
        json=True,
        processor="stereo.imager",
        params="",
    )

    assert cli.cmd_mastering_processor(args) == 0
    captured = capsys.readouterr()
    assert "duplicates the mono input" in captured.err
    assert json.loads(captured.out)["processor"] == "stereo.imager"


def test_mastering_streaming_cli_passes_platform_targets(monkeypatch, capsys) -> None:
    """mastering-streaming exposes the streaming preview JSON API."""
    import libsonare
    from libsonare import cli

    calls: dict[str, object] = {}

    monkeypatch.setattr(cli, "_load_audio", lambda path: ([0.2, -0.2, 0.0], 48000))

    def fake_streaming_preview(
        samples: list[float],
        *,
        sample_rate: int,
        platforms: list[dict[str, object]] | None,
    ) -> str:
        calls["samples"] = samples
        calls["sample_rate"] = sample_rate
        calls["platforms"] = platforms
        return '{"platforms":[{"name":"Service","gainDb":-1.0}]}'

    monkeypatch.setattr(libsonare, "mastering_streaming_preview", fake_streaming_preview)

    args = argparse.Namespace(
        file="input.wav",
        platforms='[{"name":"Service","targetLufs":-14,"ceilingDb":-1}]',
        platforms_file="",
    )

    assert cli.cmd_mastering_streaming(args) == 0
    assert calls["samples"] == [0.2, -0.2, 0.0]
    assert calls["sample_rate"] == 48000
    assert calls["platforms"] == [{"name": "Service", "targetLufs": -14, "ceilingDb": -1}]
    assert json.loads(capsys.readouterr().out)["platforms"][0]["name"] == "Service"


def test_declip_cli_writes_repaired_audio(monkeypatch, tmp_path, capsys) -> None:
    """declip exposes the offline mastering repair API."""
    import libsonare
    from libsonare import cli

    calls: dict[str, object] = {}

    monkeypatch.setattr(cli, "_load_audio", lambda path: ([1.0, -1.0, 0.25], 22050))

    def fake_declip(
        samples: list[float],
        sample_rate: int,
        *,
        clip_threshold: float,
        lpc_order: int,
        iterations: int,
        lpc_blend: float,
    ) -> list[float]:
        calls["samples"] = samples
        calls["sample_rate"] = sample_rate
        calls["clip_threshold"] = clip_threshold
        calls["lpc_order"] = lpc_order
        calls["iterations"] = iterations
        calls["lpc_blend"] = lpc_blend
        return [0.8, -0.8, 0.25]

    monkeypatch.setattr(libsonare, "mastering_repair_declip", fake_declip)

    output = tmp_path / "declip.wav"
    args = argparse.Namespace(
        file="input.wav",
        output=str(output),
        json=True,
        clip_threshold=0.9,
        lpc_order=24,
        iterations=3,
        lpc_blend=0.5,
    )

    assert cli.cmd_declip(args) == 0
    assert calls == {
        "samples": [1.0, -1.0, 0.25],
        "sample_rate": 22050,
        "clip_threshold": 0.9,
        "lpc_order": 24,
        "iterations": 3,
        "lpc_blend": 0.5,
    }
    assert output.exists()
    payload = json.loads(capsys.readouterr().out)
    assert payload["samples"] == 3
    assert payload["output"] == str(output)


def test_mastering_cli_help_lists_preset_streaming_and_declip_commands() -> None:
    """The practical mastering CLI surface is advertised in --help."""
    result = _run_cli(["--help"])

    assert result.returncode == 0
    assert "master " in result.stdout
    assert "mastering-streaming" in result.stdout
    assert "declip" in result.stdout


def test_mixing_preset_cli_commands_smoke() -> None:
    presets = _run_cli(["mixing-presets", "--json"])
    assert presets.returncode == 0, presets.stderr
    names = json.loads(presets.stdout)["presets"]
    assert names

    preset = _run_cli(["mixing-preset", "--preset", names[0]])
    assert preset.returncode == 0, preset.stderr
    scene = json.loads(preset.stdout)
    assert "strips" in scene


def test_analyze_sections_returns_section_result() -> None:
    from libsonare import Section, SectionResult, SectionType, analyze_sections

    samples = _generate_sine(220, 22050, 6.0) + _generate_sine(440, 22050, 6.0)
    result = analyze_sections(samples, sample_rate=22050, min_section_sec=2.0)
    assert isinstance(result, SectionResult)
    assert isinstance(result.sections, list)
    for section in result.sections:
        assert isinstance(section, Section)
        assert isinstance(section.type, SectionType)
        assert section.end >= section.start
        assert isinstance(section.name, str)


def test_analyze_melody_returns_melody_result() -> None:
    from libsonare import MelodyPoint, MelodyResult, analyze_melody

    samples = _generate_sine(220, 22050, 2.0)
    result = analyze_melody(samples, sample_rate=22050)
    assert isinstance(result, MelodyResult)
    assert isinstance(result.points, list)
    assert math.isfinite(result.mean_frequency)
    for point in result.points[:8]:
        assert isinstance(point, MelodyPoint)
        assert math.isfinite(point.time)


def test_cqt_and_vqt_return_cqt_result() -> None:
    from libsonare import CqtResult, cqt, vqt

    samples = _generate_sine(220, 22050, 1.0)
    for result in (
        cqt(samples, sample_rate=22050, n_bins=24, bins_per_octave=12),
        vqt(samples, sample_rate=22050, n_bins=24, bins_per_octave=12, gamma=10.0),
    ):
        assert isinstance(result, CqtResult)
        assert result.n_bins == 24
        assert result.n_frames > 0
        assert len(result.magnitude) == result.n_bins * result.n_frames
        assert len(result.frequencies) == result.n_bins


def test_pseudo_and_hybrid_cqt_return_cqt_result() -> None:
    from libsonare import CqtResult, hybrid_cqt, pseudo_cqt

    samples = _generate_sine(220, 22050, 1.0)
    for result in (
        pseudo_cqt(samples, sample_rate=22050, n_bins=24, bins_per_octave=12),
        hybrid_cqt(samples, sample_rate=22050, n_bins=24, bins_per_octave=12),
    ):
        assert isinstance(result, CqtResult)
        assert result.n_bins == 24
        assert result.n_frames > 0
        assert len(result.magnitude) == result.n_bins * result.n_frames
        assert len(result.frequencies) == result.n_bins


def test_onset_strength_multi_returns_band_matrix() -> None:
    from libsonare import onset_strength_multi

    samples = _generate_sine(440, 22050, 1.0)
    n_frames, data = onset_strength_multi(samples, sample_rate=22050, n_bands=4)
    assert n_frames > 0
    assert len(data) == 4 * n_frames
    assert all(math.isfinite(value) for value in data)
