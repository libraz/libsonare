"""Metering, CLI, and detailed analyzer API tests."""

from __future__ import annotations

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
