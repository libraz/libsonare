"""Metering, CLI, and detailed analyzer API tests."""

from __future__ import annotations

import argparse
import json
import math
from types import SimpleNamespace
from typing import get_args

import numpy as np

# ruff: noqa: F403,F405
import libsonare as ls

from ._analyzer_helpers import *
from ._analyzer_helpers import _run_cli, _write_test_wav
from ._helpers import patch_mono_decode


def test_onset_strength() -> None:
    """onset_strength returns a finite per-frame envelope."""
    from libsonare import onset_strength

    samples = _generate_sine(440, 22050, 2.0)
    env = onset_strength(samples, sr=22050)
    assert len(env) > 0
    assert _all_finite(env)


def test_fourier_tempogram() -> None:
    """fourier_tempogram returns an [n_bins x n_frames] magnitude matrix."""
    from libsonare import fourier_tempogram, onset_strength

    samples = _generate_sine(440, 22050, 2.0)
    env = onset_strength(samples, sr=22050)
    win_length = 384
    data = fourier_tempogram(env, sr=22050, win_length=win_length)
    assert data.shape == (win_length // 2 + 1, len(env))
    assert np.isfinite(data).all()


def test_tempogram_ratio() -> None:
    """tempogram_ratio returns one finite value per factor."""
    from libsonare import onset_strength, tempogram, tempogram_ratio

    samples = _generate_sine(440, 22050, 2.0)
    env = onset_strength(samples, sr=22050)
    win_length = 384
    tg = tempogram(env, sr=22050, win_length=win_length)

    default_ratio = tempogram_ratio(tg, sr=22050)
    assert len(default_ratio) == 5  # {0.5, 1, 2, 3, 4}
    assert _all_finite(default_ratio)

    explicit = tempogram_ratio(tg, sr=22050, factors=[1.0, 2.0, 3.0])
    assert len(explicit) == 3
    assert _all_finite(explicit)


def test_nnls_chroma() -> None:
    """nnls_chroma returns a row-major 12 x n_frames matrix."""
    from libsonare import nnls_chroma

    # 0.5 s keeps the NNLS solve (the dominant cost) fast without losing coverage.
    samples = _generate_sine(440, 22050, 0.5)
    data = nnls_chroma(
        samples,
        sample_rate=22050,
        enable_stft_blend=False,
        stft_blend_weight=0.0,
        stft_blend_n_fft=2048,
    )
    assert data.shape[0] == 12
    assert data.shape[1] > 0
    assert np.isfinite(data).all()
    assert (data >= 0.0).all()  # NNLS output is non-negative


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
        assert math.isfinite(r.loudness_range_lu)
        assert r.loudness_range_lu >= 0.0

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


def test_audio_onset_strength() -> None:
    """Audio.onset_strength returns a finite per-frame envelope."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 2.0), sample_rate=22050)
    env = audio.onset_strength()
    assert len(env) > 0
    assert _all_finite(env)


def test_audio_nnls_chroma() -> None:
    """Audio.nnls_chroma returns a row-major 12 x n_frames matrix."""
    from libsonare import Audio

    # Wrapper-only check — the algorithm itself is covered by test_nnls_chroma.
    audio = Audio.from_buffer(_generate_sine(440, 22050, 0.5), sample_rate=22050)
    data = audio.nnls_chroma()
    assert data.shape[0] == 12
    assert data.shape[1] > 0
    assert np.isfinite(data).all()


def test_audio_lufs() -> None:
    """Audio.lufs returns finite loudness measures."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 48000, 3.0), sample_rate=48000)
    result = audio.lufs()
    assert math.isfinite(result.integrated_lufs)
    assert math.isfinite(result.momentary_lufs)
    assert math.isfinite(result.short_term_lufs)
    assert math.isfinite(result.loudness_range_lu)


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


@pytest.mark.parametrize("command", ["lufs", "onset-strength", "nnls-chroma", "tempogram"])
def test_cli_new_commands_smoke(command: str) -> None:
    """New CLI subcommands run end-to-end on a synthetic WAV and emit JSON."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        # Smoke only (exit code + JSON emitted); 0.5 s keeps nnls-chroma cheap.
        _write_test_wav(wav_path, _generate_sine(440, 22050, 0.5), 22050)

        result = _run_cli([command, wav_path, "--json"])
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip()


def test_cli_lufs_silence_series_emits_strict_json() -> None:
    """Silence loudness sentinels become null, including both optional series."""

    def reject_nonfinite(value: str) -> None:
        pytest.fail(f"non-standard JSON constant emitted: {value}")

    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "silence.wav")
        _write_test_wav(wav_path, [0.0] * (48000 * 4), 48000)

        result = _run_cli(["lufs", wav_path, "--json", "--series"])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout, parse_constant=reject_nonfinite)

    assert payload["integrated_lufs"] is None
    assert payload["momentary_lufs"] is None
    assert payload["short_term_lufs"] is None
    assert payload["momentary_series"]
    assert payload["short_term_series"]
    assert all(value is None for value in payload["momentary_series"])
    assert all(value is None for value in payload["short_term_series"])


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


def test_cli_exit_codes_distinguish_error_classes() -> None:
    """Failures map to distinct, C-ABI-aligned exit codes (not a blanket 1)."""
    from libsonare.cli import (
        EXIT_DECODE_FAILED,
        EXIT_FILE_NOT_FOUND,
        EXIT_INVALID_FORMAT,
        EXIT_USAGE,
    )

    # Usage errors keep argparse's native exit 2.
    assert _run_cli(["definitely-not-a-command"]).returncode == EXIT_USAGE
    assert _run_cli(["bpm"]).returncode == EXIT_USAGE  # missing required file arg.

    # A missing input file surfaces SONARE_ERROR_FILE_NOT_FOUND -> exit 4.
    missing = _run_cli(["bpm", "/no/such/sonare-test-file.wav"])
    assert missing.returncode == EXIT_FILE_NOT_FOUND
    assert "Error:" in missing.stderr

    # Undecodable bytes surface as a non-file, non-usage error class distinct
    # from the codes above. The exact class depends on the decode backend the
    # library was built with: an FFmpeg-enabled build routes the bytes through
    # avformat_open_input, which rejects them as DECODE_FAILED (exit 6); a build
    # without FFmpeg falls back to the native loader, which reports the unknown
    # container as INVALID_FORMAT (exit 5). Accept either so the test is
    # independent of SONARE_WITH_FFMPEG.
    with tempfile.TemporaryDirectory() as tmpdir:
        garbage = os.path.join(tmpdir, "garbage.wav")
        with open(garbage, "wb") as handle:
            handle.write(b"not a wav at all")
        assert _run_cli(["bpm", garbage]).returncode in (
            EXIT_INVALID_FORMAT,
            EXIT_DECODE_FAILED,
        )


def test_mastering_pair_analyze_cli_takes_reference_at_its_own_rate(monkeypatch, capsys) -> None:
    """Pair analysis hands the reference's own rate to the library, which resamples it."""
    import argparse

    from libsonare import _cli_mastering, audio, cli, mastering_analyze_pair

    # 2 kHz sits on the K-weighting shelf, so the tone read at the wrong rate (2.18 kHz)
    # is not the control's only difference; 1 s clears the loudness gate.
    source_samples = _generate_sine(2000, 48000, 1.0)
    reference_samples = _generate_sine(2000, 44100, 1.0)

    def fake_read_audio(path: str) -> bytes:
        if path in ("master.wav", "reference.wav"):
            return path.encode()
        raise AssertionError(path)

    def fake_decode_channels(data: bytes):
        if data == b"master.wav":
            return SimpleNamespace(
                sample_rate=48000,
                channels=np.asarray([source_samples], dtype=np.float32),
            )
        if data == b"reference.wav":
            return SimpleNamespace(
                sample_rate=44100,
                channels=np.asarray([reference_samples], dtype=np.float32),
            )
        raise AssertionError(data)

    monkeypatch.setattr(_cli_mastering, "_read_audio_bounded", fake_read_audio)
    monkeypatch.setattr(audio, "decode_channels", fake_decode_channels)

    args = argparse.Namespace(
        analysis="match.referenceLoudness",
        file="master.wav",
        reference="reference.wav",
        params="",
    )
    assert cli.cmd_mastering_pair_analyze(args) == 0
    payload = json.loads(capsys.readouterr().out)

    expected = mastering_analyze_pair(
        "match.referenceLoudness",
        source_samples,
        reference_samples,
        sample_rate=48000,
        reference_sample_rate=44100,
    )
    assert payload["reference_lufs"] == expected.reference_lufs
    # Control: the same reference declared at the source rate is a different signal.
    declared_at_source = mastering_analyze_pair(
        "match.referenceLoudness", source_samples, reference_samples, sample_rate=48000
    )
    assert payload["reference_lufs"] != declared_at_source.reference_lufs


def test_project_cli_exports_smf_and_midi_render_smoke() -> None:
    """The Python CLI exposes project validation, SMF export/import, and MIDI render."""
    from libsonare import Project, project_abi_version

    if project_abi_version() is None:
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
            added = project.add_midi_clip(0.0, 1.0)
            track_id, clip_id = added.track_id, added.clip_id
            project.set_track_midi_destination(track_id, 0)
            project.set_midi_events(
                clip_id,
                [
                    ls.midi_note_on(0.0, 0, 0, 60, 100),
                    ls.midi_note_off(0.5, 0, 0, 60, 0),
                ],
            )
            project_path.write_text(project.to_json(), encoding="utf-8")
        finally:
            project.close()

        validate = _run_cli(["project", "validate", str(project_path), "--json"])
        assert validate.returncode == 0, validate.stderr
        assert json.loads(validate.stdout)["valid"] is True

        export = _run_cli(
            ["project", "export-smf", str(project_path), "-o", str(smf_path), "--json"]
        )
        assert export.returncode == 0, export.stderr
        assert smf_path.read_bytes().startswith(b"MThd")
        assert json.loads(export.stdout)["bytes"] == smf_path.stat().st_size

        imported = _run_cli(
            ["project", "import-smf", str(smf_path), "-o", str(imported_path), "--json"]
        )
        assert imported.returncode == 0, imported.stderr
        assert imported_path.exists()
        assert json.loads(imported.stdout)["first_clip_id"] > 0

        rendered = _run_cli(
            [
                "midi-render",
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


def test_project_bounce_wav_writer_preserves_more_than_two_channels(tmp_path) -> None:
    from libsonare import cli
    from libsonare._cli_output import _OutputFit

    output = tmp_path / "surround.wav"
    frames, channels = cli._write_project_bounce_wav(
        str(output),
        [[0.1, 0.2, 0.3, 0.4], [0.0, -0.1, -0.2, -0.3]],
        48000,
        fit=_OutputFit(),
    )

    assert frames == 2
    assert channels == 4
    # >2 channels writes WAVE_FORMAT_EXTENSIBLE (the channel-position mask a
    # 5.1/7.1 player needs), which stdlib `wave` refuses to open.
    written_channels, written_frames, written_rate = _read_wav_extensible_header(str(output))
    assert written_channels == 4
    assert written_frames == 2
    assert written_rate == 48000


def test_mastering_processor_feeds_a_mono_source_to_both_channels(monkeypatch, capsys) -> None:
    """A mono source reaches a stereo-only processor as both channels, unannounced.

    Duplicating is right here and is the only way these processors are reachable
    from a mono file at all. It used to carry a warning calling the result a
    preview and pointing at the Python API, which was honest about a defect that
    is now fixed: the command folded the processed pair back to mono, so the
    widening it had just performed never reached the file. It writes the pair, so
    there is nothing left to caveat -- asserted negatively below, because a
    warning that outlives its reason reads as a live caution.
    """
    import libsonare
    from libsonare import _cli_common, cli

    monkeypatch.setattr(_cli_common, "_source_channel_count", lambda path: 1)
    patch_mono_decode(monkeypatch, lambda path: ([0.1, -0.1], 44100))

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
            declip_unreconstructed_runs=None,
        )

    monkeypatch.setattr(
        libsonare, "mastering_apply_processor_stereo", fake_mastering_process_stereo
    )

    args = argparse.Namespace(
        file="input.wav",
        output="",
        json=True,
        processor="stereo.imager",
        params="",
    )

    assert cli.cmd_mastering_processor(args) == 0
    captured = capsys.readouterr()
    assert "duplicates the mono input" not in captured.err
    assert "preview" not in captured.err
    payload = json.loads(captured.out)
    assert payload["processor"] == "stereo.imager"
    # The stereo path ran without anyone spelling a flag: the processor's own
    # stereo-only status is what routed it.
    assert payload["stereo"] is True


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
    ) -> libsonare.MasteringStreamingPreviewResult:
        calls["samples"] = samples
        calls["sample_rate"] = sample_rate
        calls["platforms"] = platforms
        return libsonare.MasteringStreamingPreviewResult(
            platforms=[
                libsonare.MasteringStreamingPreviewPlatform(
                    name="Service",
                    integrated_lufs=None,
                    true_peak_dbtp=-3.0,
                    normalization_gain_db=0.0,
                    ceiling_risk=False,
                )
            ]
        )

    monkeypatch.setattr(libsonare, "mastering_streaming_preview", fake_streaming_preview)

    args = argparse.Namespace(
        file="input.wav",
        platforms='[{"name":"Service","target_lufs":-14,"ceiling_dbtp":-1}]',
        platforms_file="",
    )

    assert cli.cmd_mastering_streaming(args) == 0
    assert calls["samples"] == [0.2, -0.2, 0.0]
    assert calls["sample_rate"] == 48000
    assert calls["platforms"] == [{"name": "Service", "target_lufs": -14, "ceiling_dbtp": -1}]
    assert json.loads(capsys.readouterr().out)["platforms"] == [
        {
            "name": "Service",
            "true_peak_dbtp": -3.0,
            "normalization_gain_db": 0.0,
            "ceiling_risk": False,
        }
    ]


def test_mastering_cli_help_lists_the_mastering_and_repair_commands() -> None:
    """The practical mastering CLI surface is advertised in --help."""
    result = _run_cli(["--help"])

    assert result.returncode == 0
    for command in ("mastering ", "mastering-normalize-loudness", "mastering-streaming", "repair"):
        assert command in result.stdout


def test_mixing_preset_cli_commands_smoke() -> None:
    presets = _run_cli(["mixing-presets", "--json"])
    assert presets.returncode == 0, presets.stderr
    names = json.loads(presets.stdout)["presets"]
    assert names

    preset = _run_cli(["mixing-preset", "--preset", names[0]])
    assert preset.returncode == 0, preset.stderr
    scene = json.loads(preset.stdout)
    assert "strips" in scene

    # Bare invocation must resolve to a valid default preset.
    default_preset = _run_cli(["mixing-preset"])
    assert default_preset.returncode == 0, default_preset.stderr
    assert "strips" in json.loads(default_preset.stdout)


def test_analyze_sections_returns_a_section_list() -> None:
    from libsonare import Section, SectionAnalysis, SectionType, analyze_sections

    samples = _generate_sine(220, 22050, 6.0) + _generate_sine(440, 22050, 6.0)
    result = analyze_sections(samples, sample_rate=22050, min_section_sec=2.0)
    assert isinstance(result, SectionAnalysis)
    assert isinstance(result.sections, list)
    for section in result.sections:
        assert isinstance(section, Section)
        assert section.type in get_args(SectionType)
        assert section.end_sec >= section.start_sec
        assert isinstance(section.name, str)


def test_analyze_melody_returns_melody_result() -> None:
    from libsonare import MelodyPoint, MelodyResult, analyze_melody

    samples = _generate_sine(220, 22050, 2.0)
    result = analyze_melody(samples, sample_rate=22050)
    assert isinstance(result, MelodyResult)
    assert isinstance(result.points, list)
    assert math.isfinite(result.mean_frequency_hz)
    for point in result.points[:8]:
        assert isinstance(point, MelodyPoint)
        assert math.isfinite(point.time_sec)


def test_cqt_and_vqt_return_cqt_result() -> None:
    from libsonare import CqtResult, cqt, vqt

    samples = _generate_sine(220, 22050, 1.0)
    for result in (
        cqt(samples, sr=22050, n_bins=24, bins_per_octave=12),
        vqt(samples, sr=22050, n_bins=24, bins_per_octave=12, gamma=10.0),
    ):
        assert isinstance(result, CqtResult)
        assert result.magnitude.shape[0] == 24
        assert result.magnitude.shape[1] > 0
        assert result.frequencies.shape == (24,)


def test_pseudo_and_hybrid_cqt_return_cqt_result() -> None:
    from libsonare import CqtResult, hybrid_cqt, pseudo_cqt

    samples = _generate_sine(220, 22050, 1.0)
    for result in (
        pseudo_cqt(samples, sr=22050, n_bins=24, bins_per_octave=12),
        hybrid_cqt(samples, sr=22050, n_bins=24, bins_per_octave=12),
    ):
        assert isinstance(result, CqtResult)
        assert result.magnitude.shape[0] == 24
        assert result.magnitude.shape[1] > 0
        assert result.frequencies.shape == (24,)


def test_onset_strength_multi_returns_band_matrix() -> None:
    from libsonare import onset_strength_multi

    samples = _generate_sine(440, 22050, 1.0)
    data = onset_strength_multi(samples, sample_rate=22050, n_bands=4)
    assert data.shape[0] == 4
    assert data.shape[1] > 0
    assert np.isfinite(data).all()
