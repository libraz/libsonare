"""Round-trip / ABI validation for the newly wired inverse + StreamAnalyzer API."""

import ctypes
import math

import numpy as np
import pytest

import libsonare as ls


def _sine(n, freq, sr):
    return [0.5 * math.sin(2.0 * math.pi * freq * i / sr) for i in range(n)]


def test_mel_inverse_roundtrip():
    sr = 22050
    n_fft = 2048
    hop = 512
    n_mels = 128
    audio = _sine(sr // 4, 440.0, sr)  # 0.25 s — Griffin-Lim cost scales with frames

    mel = ls.mel_spectrogram(audio, sample_rate=sr, n_fft=n_fft, hop_length=hop, n_mels=n_mels)
    assert mel.power.shape[0] == n_mels
    n_frames = mel.power.shape[1]
    assert n_frames > 0

    stft = ls.mel_to_stft(mel.power, sr=sr, n_fft=n_fft)
    assert isinstance(stft, np.ndarray)
    assert stft.shape == (n_fft // 2 + 1, n_frames)
    assert np.isfinite(stft).all()
    assert stft.max() > 0.0  # reconstructed magnitude is non-trivial

    out = ls.mel_to_audio(mel.power, sr=sr, n_fft=n_fft, hop_length=hop)
    assert len(out) >= (n_frames - 1) * hop
    assert np.isfinite(out).all()
    assert np.abs(out).max() > 0.0


def test_mel_inverse_honours_htk():
    sr = 22050
    n_fft = 1024
    hop = 256
    n_mels = 40
    audio = _sine(sr // 4, 440.0, sr)

    mel = ls.mel_spectrogram(
        audio, sample_rate=sr, n_fft=n_fft, hop_length=hop, n_mels=n_mels, htk=True
    )
    n_frames = mel.power.shape[1]
    slaney = ls.mel_to_stft(mel.power, sr=sr, n_fft=n_fft, htk=False)
    htk = ls.mel_to_stft(mel.power, sr=sr, n_fft=n_fft, htk=True)

    assert htk.shape == (n_fft // 2 + 1, n_frames)
    assert np.isfinite(htk).all()
    assert np.sum((slaney - htk) ** 2) > 1e-6

    out = ls.mel_to_audio(
        mel.power,
        sr=sr,
        n_fft=n_fft,
        hop_length=hop,
        n_iter=2,
        htk=True,
    )
    assert len(out) >= (n_frames - 1) * hop
    assert np.isfinite(out).all()


def test_mfcc_inverse_roundtrip():
    sr = 22050
    n_fft = 2048
    hop = 512
    n_mels = 128
    audio = _sine(sr // 4, 220.0, sr)  # 0.25 s — see mel roundtrip above

    mf = ls.mfcc(audio, sr=sr, n_fft=n_fft, hop_length=hop, n_mfcc=20)
    assert mf.coefficients.shape[0] == 20
    n_frames = mf.coefficients.shape[1]
    assert n_frames > 0

    mel = ls.mfcc_to_mel(mf.coefficients, n_mels=n_mels)
    assert isinstance(mel, np.ndarray)
    assert mel.shape == (n_mels, n_frames)
    assert np.isfinite(mel).all()

    lifted_mel = ls.mfcc_to_mel(mf.coefficients, n_mels=n_mels, lifter=22.0)
    assert np.isfinite(lifted_mel).all()
    assert np.any(np.abs(lifted_mel - mel) > 1.0e-5)

    out = ls.mfcc_to_audio(mf.coefficients, sr=sr, n_fft=n_fft, hop_length=hop, n_mels=n_mels)
    assert len(out) >= (n_frames - 1) * hop
    assert np.isfinite(out).all()


def test_inverse_transforms_refuse_a_flat_buffer_and_dim_length_mismatch():
    # A matrix entry point takes one 2-D array, so a dimension count can no longer
    # disagree with the buffer; a flat buffer is refused by name instead. The
    # decompose wrappers still take a flat matrix plus declared dims and must
    # raise ValueError up front rather than over-read on the C side.
    n_mels, n_frames = 8, 4
    flat = np.full(n_mels * n_frames, 0.1, dtype=np.float32)
    with pytest.raises(ls.SonareValueError, match="mel must be a 2-D array"):
        ls.mel_to_stft(flat)
    with pytest.raises(ls.SonareValueError, match="mel must be a 2-D array"):
        ls.mel_to_audio(flat)

    n_mfcc = 5
    flat_mfcc = np.full(n_mfcc * n_frames, 0.1, dtype=np.float32)
    with pytest.raises(ls.SonareValueError, match="coefficients must be a 2-D array"):
        ls.mfcc_to_mel(flat_mfcc)
    with pytest.raises(ls.SonareValueError, match="coefficients must be a 2-D array"):
        ls.mfcc_to_audio(flat_mfcc)

    n_features = 6  # expects 24 elements
    short_spec = [0.1] * (n_features * n_frames - 1)
    with pytest.raises(ValueError):
        ls.decompose(short_spec, n_features, n_frames, 2)
    with pytest.raises(ValueError):
        ls.nn_filter(short_spec, n_features, n_frames)


def test_inverse_transforms_reject_non_finite_cells():
    n_mels, n_frames = 8, 4
    mel = np.full((n_mels, n_frames), 0.1, dtype=np.float32)
    mel[0, 3] = math.nan
    with pytest.raises(ls.SonareError):
        ls.mel_to_stft(mel)
    with pytest.raises(ls.SonareError):
        ls.mel_to_audio(mel, n_fft=256, hop_length=64, n_iter=2)

    n_mfcc = 5
    mfcc = np.full((n_mfcc, n_frames), 0.1, dtype=np.float32)
    mfcc[1, 3] = math.inf
    with pytest.raises(ls.SonareError):
        ls.mfcc_to_mel(mfcc, n_mels=n_mels)
    with pytest.raises(ls.SonareError):
        ls.mfcc_to_audio(
            mfcc,
            n_mels=n_mels,
            n_fft=256,
            hop_length=64,
            n_iter=2,
        )


def test_cqt_vqt_inverse_reconstruction_and_shape_validation():
    sr = 8000
    source = _sine(2048, 261.6256, sr)
    cq = ls.cqt(source, sr=sr, hop_length=128, fmin=130.8128, n_bins=12)
    reconstructed = ls.cqt_to_audio(cq.magnitude, sr, 128, 130.8128, 12, 2)
    assert len(reconstructed) == len(source)
    assert np.isfinite(reconstructed).all()
    assert np.abs(reconstructed).max() > 1e-5

    vq = ls.vqt(source, sr=sr, hop_length=128, fmin=130.8128, n_bins=12)
    vqt_reconstructed = ls.vqt_to_audio(vq.magnitude, sr, 128, 130.8128, 12, 0, 2)
    assert len(vqt_reconstructed) == len(source)
    assert np.isfinite(vqt_reconstructed).all()
    with pytest.raises(ls.SonareValueError, match="magnitude must be a 2-D array"):
        ls.cqt_to_audio(cq.magnitude.reshape(-1), sample_rate=22050)
    with pytest.raises(ls.SonareError):
        ls.vqt_to_audio(vq.magnitude, n_iter=257, sample_rate=22050)


def test_stream_analyzer_abi_and_stats():
    sr = 22050
    n_samples = sr * 4  # 4 seconds
    audio = _sine(n_samples, 440.0, sr)

    cfg = dict(sample_rate=sr, n_fft=2048, hop_length=512, n_mels=128)
    with ls.StreamAnalyzer(**cfg) as sa:
        # Feed in two blocks to exercise cumulative offset tracking.
        half = n_samples // 2
        sa.process(audio[:half])
        sa.process(audio[half:])

        assert sa.sample_rate == sr
        assert sa.frame_count > 0

        stats = sa.stats()
        # --- Critical ABI / struct-padding checks ---
        # total_samples is a size_t immediately after an int32 field and before a
        # float field. The analyzer reports samples consumed into complete frames
        # (total_frames * hop_length), with a sub-frame remainder still buffered.
        hop = cfg["hop_length"]
        assert stats.total_samples == stats.total_frames * hop
        # Bounded by what we fed (within one analysis window).
        assert n_samples - cfg["n_fft"] <= stats.total_samples <= n_samples
        # ABI cross-check: the size_t field and the float field that straddle the
        # padding boundary must be mutually consistent. Misaligned padding would
        # make these disagree.
        assert abs(stats.duration_sec - stats.total_samples / sr) < 1e-3, (
            f"ABI mismatch: duration_sec {stats.duration_sec} != "
            f"total_samples/sr {stats.total_samples / sr}"
        )
        assert stats.total_frames > 0
        # Field-by-field sanity: every estimate present must be finite & in range.
        estimate = stats.estimate
        if estimate.bpm is not None:
            assert math.isfinite(estimate.bpm) and estimate.bpm > 0.0
            assert estimate.bpm_confidence is not None
            assert 0.0 <= estimate.bpm_confidence <= 1.0
        assert estimate.bpm_candidate_count >= 0
        if estimate.key is not None:
            assert estimate.key.mode in ("major", "minor")
            assert 0.0 <= estimate.key.confidence <= 1.0
        if estimate.chord is not None:
            assert math.isfinite(estimate.chord.confidence)
        if estimate.bar_duration_sec is not None:
            assert math.isfinite(estimate.bar_duration_sec) and estimate.bar_duration_sec >= 0.0
        assert estimate.used_frames >= 0

        # current_time should reflect consumed audio (<= fed, consistent with stats).
        assert abs(sa.current_time_sec - stats.total_samples / sr) < 0.1

        # Drain frames; verify SOA array lengths are internally consistent.
        avail = sa.available_frames
        if avail > 0:
            frames = sa.read_frames(avail)
            assert frames.n_frames == avail
            assert frames.timestamps_sec.shape == (avail,)
            assert frames.mel is not None and frames.mel.shape == (128, avail)
            assert frames.chroma is not None and frames.chroma.shape == (12, avail)
            assert frames.onset_strength is not None and frames.onset_strength.shape == (avail,)
            assert frames.chord_root is not None and frames.chord_root.shape == (avail,)
            assert np.all(np.isfinite(frames.timestamps_sec))
            assert np.all(np.isfinite(frames.mel))


def test_stream_analyzer_quantized_python_api():
    sr = 22050
    cfg = dict(sample_rate=sr, n_fft=1024, hop_length=256, n_mels=32, window="hamming")
    with ls.StreamAnalyzer(**cfg) as sa:
        sa.process(_sine(sr // 2, 440.0, sr))
        frames = sa.read_frames_u8(4)
        assert 0 < frames.n_frames <= 4
        assert frames.mel is not None and frames.mel.dtype == np.uint8
        assert frames.mel.shape == (32, frames.n_frames)
        assert frames.chroma is not None and frames.chroma.shape == (12, frames.n_frames)

        sa.process(_sine(sr // 2, 440.0, sr))
        frames16 = sa.read_frames_i16(4)
        assert 0 < frames16.n_frames <= 4
        assert frames16.mel is not None and frames16.mel.dtype == np.int16
        assert frames16.mel.shape == (32, frames16.n_frames)


def test_stream_analyzer_feature_shape_contract_all_combinations_and_read_types():
    input_samples = [0.0] * 32
    input_samples[8] = 0.5
    for mask in range(16):
        cfg = dict(
            sample_rate=8000,
            n_fft=32,
            hop_length=32,
            n_mels=8,
            compute_mel=bool(mask & 1),
            compute_chroma=bool(mask & 2),
            compute_onset=bool(mask & 4),
            compute_spectral=bool(mask & 8),
        )
        for method_name in ("read_frames", "read_frames_u8", "read_frames_i16"):
            with ls.StreamAnalyzer(**cfg) as analyzer:
                analyzer.process(input_samples)
                frames = getattr(analyzer, method_name)(1)
                assert frames.n_frames == 1

                def shape(array):
                    return None if array is None else array.shape

                assert shape(frames.mel) == ((8, 1) if mask & 1 else None)
                assert shape(frames.chroma) == ((12, 1) if mask & 2 else None)
                assert shape(frames.onset_strength) == ((1,) if mask & 4 else None)
                assert shape(frames.rms_energy) == (1,)
                assert shape(frames.spectral_centroid) == ((1,) if mask & 8 else None)
                assert shape(frames.spectral_flatness) == ((1,) if mask & 8 else None)
                if method_name == "read_frames":
                    assert shape(frames.chord_root) == ((1,) if mask & 2 else None)


@pytest.mark.parametrize(
    "field", ["output_format", "compute_magnitude", "fmin", "fmax", "tuning_ref_hz", "config"]
)
def test_stream_analyzer_has_no_dead_or_unitless_keywords(field):
    with pytest.raises(TypeError):
        ls.StreamAnalyzer(sample_rate=22050, **{field: 0})


def test_stream_analyzer_quantize_config_override():
    sr = 22050
    cfg = dict(sample_rate=sr, n_fft=1024, hop_length=256, n_mels=32, window="hamming")

    # A tiny centroid_max saturates the (positive) spectral centroid to the u8
    # maximum; a huge centroid_max collapses it toward zero. Reading identical
    # audio with the two configs must differ, proving the override reaches the
    # native quantizer.
    with ls.StreamAnalyzer(**cfg) as tight:
        tight.process(_sine(sr // 2, 440.0, sr))
        narrow = tight.read_frames_u8(4, ls.StreamQuantizeConfig(centroid_max=1.0))
    with ls.StreamAnalyzer(**cfg) as wide:
        wide.process(_sine(sr // 2, 440.0, sr))
        broad = wide.read_frames_u8(4, ls.StreamQuantizeConfig(centroid_max=1.0e9))

    assert narrow.n_frames == broad.n_frames
    assert narrow.spectral_centroid is not None and broad.spectral_centroid is not None
    assert narrow.spectral_centroid[0] == 255  # saturated by the narrow range
    assert not np.array_equal(narrow.spectral_centroid, broad.spectral_centroid)


def test_stream_analyzer_reset():
    sr = 22050
    cfg = dict(sample_rate=sr, hop_length=512)
    sa = ls.StreamAnalyzer(**cfg)
    try:
        sa.process(_sine(sr, 330.0, sr))
        before = sa.stats()
        assert before.total_samples > 0
        assert before.total_samples == before.total_frames * cfg["hop_length"]
        sa.reset()
        assert sa.stats().total_samples == 0
        assert sa.frame_count == 0
    finally:
        sa.close()


def test_stream_analyzer_external_offsets_preserve_buffered_timestamps_and_reject_gaps():
    cfg = dict(sample_rate=22050, n_fft=2048, hop_length=512)
    audio = [0.0] * 4096
    with ls.StreamAnalyzer(**cfg) as reference:
        reference.process(audio)
        expected = reference.read_frames(64).timestamps_sec
    with ls.StreamAnalyzer(**cfg) as analyzer:
        for offset in range(0, len(audio), 128):
            analyzer.process_with_offset(audio[offset : offset + 128], offset)
        assert analyzer.read_frames(64).timestamps_sec == pytest.approx(expected, abs=1e-7)
        analyzer.reset()
        analyzer.process_with_offset(audio[:128], 1000)
        with pytest.raises(ls.SonareError):
            analyzer.process_with_offset(audio[128:256], 1129)
        analyzer.reset(5000)
        analyzer.process_with_offset(audio[:128], 5000)


def test_stream_analyzer_rejects_invalid_size_arguments_without_wrapping():
    size_t_max = (1 << (ctypes.sizeof(ctypes.c_size_t) * 8)) - 1
    invalid_values = (-1, 1.5, float("nan"), float("inf"), size_t_max + 1, True)

    with ls.StreamAnalyzer(sample_rate=8000, n_fft=32, hop_length=32, n_mels=8) as analyzer:
        operations = (
            lambda value: analyzer.process_with_offset([0.0], value),
            analyzer.read_frames,
            analyzer.read_frames_u8,
            analyzer.read_frames_i16,
            analyzer.reset,
        )
        for value in invalid_values:
            for operation in operations:
                expected = TypeError if isinstance(value, bool) else ValueError
                with pytest.raises(expected, match=r"must be an integer in \[0, "):
                    operation(value)

        analyzer.reset(size_t_max)
        assert analyzer.read_frames(size_t_max).n_frames == 0
        assert analyzer.read_frames_u8(size_t_max).n_frames == 0
        assert analyzer.read_frames_i16(size_t_max).n_frames == 0

        analyzer.reset()
        analyzer.process_with_offset([0.0], 0)
