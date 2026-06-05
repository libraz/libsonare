"""Round-trip / ABI validation for the newly wired inverse + StreamAnalyzer API."""

import ctypes
import math

import pytest

import libsonare as ls
from libsonare._runtime import SonareStreamConfig, _get_lib


def _is_finite_list(xs):
    return all(math.isfinite(float(x)) for x in xs)


def _sine(n, freq, sr):
    return [0.5 * math.sin(2.0 * math.pi * freq * i / sr) for i in range(n)]


def test_stream_config_defaults_match_native_config():
    raw = SonareStreamConfig()
    lib = _get_lib()
    assert lib.sonare_stream_analyzer_config_default(ctypes.byref(raw)) == 0

    cfg = ls.StreamConfig()
    assert cfg.sample_rate == raw.sample_rate
    assert cfg.n_fft == raw.n_fft
    assert cfg.hop_length == raw.hop_length
    assert cfg.n_mels == raw.n_mels
    assert cfg.fmin == raw.fmin
    assert cfg.fmax == raw.fmax
    assert cfg.tuning_ref_hz == raw.tuning_ref_hz
    assert int(cfg.compute_magnitude) == raw.compute_magnitude
    assert int(cfg.compute_mel) == raw.compute_mel
    assert int(cfg.compute_chroma) == raw.compute_chroma
    assert int(cfg.compute_onset) == raw.compute_onset
    assert int(cfg.compute_spectral) == raw.compute_spectral
    assert cfg.emit_every_n_frames == raw.emit_every_n_frames
    assert cfg.magnitude_downsample == raw.magnitude_downsample
    assert cfg.key_update_interval_sec == raw.key_update_interval_sec
    assert cfg.bpm_update_interval_sec == raw.bpm_update_interval_sec
    assert cfg.window == raw.window
    assert cfg.output_format == raw.output_format


def test_mel_inverse_roundtrip():
    sr = 22050
    n_fft = 2048
    hop = 512
    n_mels = 128
    audio = _sine(sr // 4, 440.0, sr)  # 0.25 s — Griffin-Lim cost scales with frames

    mel = ls.mel_spectrogram(audio, sample_rate=sr, n_fft=n_fft, hop_length=hop, n_mels=n_mels)
    n_frames = mel.n_frames
    assert n_frames > 0
    assert len(mel.power) == n_mels * n_frames

    stft = ls.mel_to_stft(mel.power, n_mels, n_frames, sample_rate=sr, n_fft=n_fft)
    assert stft.rows == n_fft // 2 + 1
    assert stft.n_frames == n_frames
    assert len(stft.data) == stft.rows * stft.n_frames
    assert _is_finite_list(stft.data)
    assert max(stft.data) > 0.0  # reconstructed magnitude is non-trivial

    out = ls.mel_to_audio(mel.power, n_mels, n_frames, sample_rate=sr, n_fft=n_fft, hop_length=hop)
    assert len(out) >= (n_frames - 1) * hop
    assert _is_finite_list(out)
    assert max(abs(x) for x in out) > 0.0


def test_mel_inverse_honours_htk():
    sr = 22050
    n_fft = 1024
    hop = 256
    n_mels = 40
    audio = _sine(sr // 4, 440.0, sr)

    mel = ls.mel_spectrogram(
        audio, sample_rate=sr, n_fft=n_fft, hop_length=hop, n_mels=n_mels, htk=True
    )
    slaney = ls.mel_to_stft(mel.power, n_mels, mel.n_frames, sample_rate=sr, n_fft=n_fft, htk=False)
    htk = ls.mel_to_stft(mel.power, n_mels, mel.n_frames, sample_rate=sr, n_fft=n_fft, htk=True)

    assert htk.rows == n_fft // 2 + 1
    assert htk.n_frames == mel.n_frames
    assert _is_finite_list(htk.data)
    assert sum((a - b) ** 2 for a, b in zip(slaney.data, htk.data, strict=True)) > 1e-6

    out = ls.mel_to_audio(
        mel.power,
        n_mels,
        mel.n_frames,
        sample_rate=sr,
        n_fft=n_fft,
        hop_length=hop,
        n_iter=2,
        htk=True,
    )
    assert len(out) >= (mel.n_frames - 1) * hop
    assert _is_finite_list(out)


def test_mfcc_inverse_roundtrip():
    sr = 22050
    n_fft = 2048
    hop = 512
    n_mels = 128
    audio = _sine(sr // 4, 220.0, sr)  # 0.25 s — see mel roundtrip above

    mf = ls.mfcc(audio, sample_rate=sr, n_fft=n_fft, hop_length=hop, n_mfcc=20)
    n_frames = mf.n_frames
    assert n_frames > 0
    assert len(mf.coefficients) == 20 * n_frames

    mel = ls.mfcc_to_mel(mf.coefficients, 20, n_frames, n_mels=n_mels)
    assert mel.rows == n_mels
    assert mel.n_frames == n_frames
    assert len(mel.data) == n_mels * n_frames
    assert _is_finite_list(mel.data)

    out = ls.mfcc_to_audio(
        mf.coefficients, 20, n_frames, sample_rate=sr, n_fft=n_fft, hop_length=hop, n_mels=n_mels
    )
    assert len(out) >= (n_frames - 1) * hop
    assert _is_finite_list(out)


def test_inverse_transforms_reject_dim_length_mismatch():
    # Regression: the inverse/decompose wrappers forwarded a flat matrix plus
    # declared dims to the C ABI without a length check, so a mismatched length
    # caused a C-side heap over-read. They must now raise ValueError up front.
    n_mels, n_frames = 8, 4  # expects 32 elements
    short = [0.1] * (n_mels * n_frames - 1)
    with pytest.raises(ValueError):
        ls.mel_to_stft(short, n_mels, n_frames)
    with pytest.raises(ValueError):
        ls.mel_to_audio(short, n_mels, n_frames)

    n_mfcc = 5  # expects 20 elements
    short_mfcc = [0.1] * (n_mfcc * n_frames - 1)
    with pytest.raises(ValueError):
        ls.mfcc_to_mel(short_mfcc, n_mfcc, n_frames)
    with pytest.raises(ValueError):
        ls.mfcc_to_audio(short_mfcc, n_mfcc, n_frames)

    n_features = 6  # expects 24 elements
    short_spec = [0.1] * (n_features * n_frames - 1)
    with pytest.raises(ValueError):
        ls.decompose(short_spec, n_features, n_frames, 2)
    with pytest.raises(ValueError):
        ls.nn_filter(short_spec, n_features, n_frames)


def test_stream_analyzer_abi_and_stats():
    sr = 22050
    n_samples = sr * 4  # 4 seconds
    audio = _sine(n_samples, 440.0, sr)

    cfg = ls.StreamConfig(sample_rate=sr, n_fft=2048, hop_length=512, n_mels=128)
    with ls.StreamAnalyzer(cfg) as sa:
        # Feed in two blocks to exercise cumulative offset tracking.
        half = n_samples // 2
        sa.process(audio[:half])
        sa.process(audio[half:])

        assert sa.sample_rate() == sr
        assert sa.frame_count() > 0

        stats = sa.stats()
        # --- Critical ABI / struct-padding checks ---
        # total_samples is a size_t immediately after an int32 field and before a
        # float field. The analyzer reports samples consumed into complete frames
        # (total_frames * hop_length), with a sub-frame remainder still buffered.
        hop = cfg.hop_length
        assert stats.total_samples == stats.total_frames * hop
        # Bounded by what we fed (within one analysis window).
        assert n_samples - cfg.n_fft <= stats.total_samples <= n_samples
        # ABI cross-check: the size_t field and the float field that straddle the
        # padding boundary must be mutually consistent. Misaligned padding would
        # make these disagree.
        assert abs(stats.duration_seconds - stats.total_samples / sr) < 1e-3, (
            f"ABI mismatch: duration_seconds {stats.duration_seconds} != "
            f"total_samples/sr {stats.total_samples / sr}"
        )
        assert stats.total_frames > 0
        # Field-by-field sanity: every numeric field must be finite & in range.
        assert math.isfinite(stats.bpm) and stats.bpm >= 0.0
        assert 0.0 <= stats.bpm_confidence <= 1.0
        assert stats.bpm_candidate_count >= 0
        assert -1 <= stats.key <= 11
        assert isinstance(stats.key_minor, bool)
        assert 0.0 <= stats.key_confidence <= 1.0
        assert -1 <= stats.chord_root <= 11
        assert math.isfinite(stats.chord_confidence)
        assert math.isfinite(stats.bar_duration) and stats.bar_duration >= 0.0
        assert stats.used_frames >= 0

        # current_time should reflect consumed audio (<= fed, consistent with stats).
        assert abs(sa.current_time() - stats.total_samples / sr) < 0.1

        # Drain frames; verify SOA array lengths are internally consistent.
        avail = sa.available_frames()
        if avail > 0:
            frames = sa.read_frames(avail)
            assert frames.n_frames == avail
            assert frames.n_mels == 128
            assert len(frames.timestamps) == avail
            assert len(frames.mel) == avail * frames.n_mels
            assert len(frames.chroma) == avail * 12
            assert len(frames.onset_strength) == avail
            assert len(frames.chord_root) == avail
            assert _is_finite_list(frames.timestamps)
            assert _is_finite_list(frames.mel)


def test_stream_analyzer_quantized_python_api():
    sr = 22050
    cfg = ls.StreamConfig(sample_rate=sr, n_fft=1024, hop_length=256, n_mels=32, window=1)
    with ls.StreamAnalyzer(cfg) as sa:
        sa.process(_sine(sr // 2, 440.0, sr))
        frames = sa.read_frames_u8(4)
        assert 0 < frames.n_frames <= 4
        assert frames.n_mels == 32
        assert len(frames.mel) == frames.n_frames * frames.n_mels
        assert len(frames.chroma) == frames.n_frames * 12
        assert all(0 <= value <= 255 for value in frames.mel)

        sa.process(_sine(sr // 2, 440.0, sr))
        frames16 = sa.read_frames_i16(4)
        assert 0 < frames16.n_frames <= 4
        assert frames16.n_mels == 32
        assert len(frames16.mel) == frames16.n_frames * frames16.n_mels
        assert all(-32768 <= value <= 32767 for value in frames16.chroma)


def test_stream_analyzer_quantize_config_override():
    sr = 22050
    cfg = ls.StreamConfig(sample_rate=sr, n_fft=1024, hop_length=256, n_mels=32, window=1)

    # A tiny centroid_max saturates the (positive) spectral centroid to the u8
    # maximum; a huge centroid_max collapses it toward zero. Reading identical
    # audio with the two configs must differ, proving the override reaches the
    # native quantizer.
    with ls.StreamAnalyzer(cfg) as tight:
        tight.process(_sine(sr // 2, 440.0, sr))
        narrow = tight.read_frames_u8(4, ls.QuantizeConfig(centroid_max=1.0))
    with ls.StreamAnalyzer(cfg) as wide:
        wide.process(_sine(sr // 2, 440.0, sr))
        broad = wide.read_frames_u8(4, ls.QuantizeConfig(centroid_max=1.0e9))

    assert narrow.n_frames == broad.n_frames
    assert narrow.spectral_centroid[0] == 255  # saturated by the narrow range
    assert narrow.spectral_centroid != broad.spectral_centroid


def test_stream_analyzer_reset():
    sr = 22050
    cfg = ls.StreamConfig(sample_rate=sr)
    sa = ls.StreamAnalyzer(cfg)
    try:
        sa.process(_sine(sr, 330.0, sr))
        before = sa.stats()
        assert before.total_samples > 0
        assert before.total_samples == before.total_frames * cfg.hop_length
        sa.reset()
        assert sa.stats().total_samples == 0
        assert sa.frame_count() == 0
    finally:
        sa.close()
