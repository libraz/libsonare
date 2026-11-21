"""The streaming analyzer takes and returns the shapes the batch analysis uses.

``StreamAnalyzer`` takes its settings as keywords, names its band edges in Hz,
spells the window as a token and requires the sample rate. Frames are band-major
arrays with ``None`` for a feature that was not computed, and the progressive
estimate sits under ``StreamStats.estimate`` with ``None`` until the analyzer has one.
"""

from __future__ import annotations

import math
from typing import get_args

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError, StreamAnalyzer
from libsonare.types import WindowType

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


def _triad(seconds: float) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    tone = sum(np.sin(2 * math.pi * f * t) for f in (261.63, 329.63, 392.0))
    return (0.25 * tone).astype(np.float32)


def _mel(**config: object) -> np.ndarray:
    with StreamAnalyzer(sample_rate=SR, n_mels=32, **config) as analyzer:  # type: ignore[arg-type]
        analyzer.process(_triad(0.5))
        mel = analyzer.read_frames(4).mel
    assert mel is not None
    return mel


def test_the_sample_rate_is_required() -> None:
    assert not hasattr(libsonare, "StreamConfig")
    with pytest.raises(TypeError):
        StreamAnalyzer()  # type: ignore[call-arg]
    with pytest.raises(TypeError):
        StreamAnalyzer({"sample_rate": SR})  # type: ignore[arg-type]


def test_the_magnitude_downsample_factor_is_not_a_setting() -> None:
    # The C field it fed is unused while magnitude readout is unsupported.
    with pytest.raises(TypeError, match="magnitude_downsample"):
        StreamAnalyzer(sample_rate=SR, magnitude_downsample=2)  # type: ignore[call-arg]


def test_an_absent_upper_edge_is_nyquist() -> None:
    np.testing.assert_allclose(_mel(), _mel(fmax_hz=SR / 2), rtol=1e-6)
    assert not np.allclose(_mel(), _mel(fmax_hz=2000.0))


@pytest.mark.parametrize("value", [0.0, -1.0])
def test_an_upper_edge_that_is_not_positive_is_refused(value: float) -> None:
    with pytest.raises(SonareValueError, match="fmax_hz"):
        StreamAnalyzer(sample_rate=SR, fmax_hz=value)


def test_the_lower_edge_reaches_the_core() -> None:
    assert not np.allclose(_mel(), _mel(fmin_hz=2000.0))


def test_every_window_token_runs_and_the_window_reaches_the_core() -> None:
    assert get_args(WindowType) == ("hann", "hamming", "blackman", "rectangular")
    mels = {window: _mel(window=window) for window in get_args(WindowType)}
    assert not np.allclose(mels["hann"], mels["rectangular"])


@pytest.mark.parametrize(("value", "error"), [("Hann", SonareValueError), (1, TypeError)])
def test_a_window_outside_the_tokens_is_refused(value: object, error: type) -> None:
    with pytest.raises(error):
        StreamAnalyzer(sample_rate=SR, window=value)  # type: ignore[arg-type]


def test_frames_are_band_major_columns_per_frame() -> None:
    config = dict(sample_rate=SR, n_mels=16)
    with StreamAnalyzer(**config) as whole:
        whole.process(_triad(0.5))
        batch = whole.read_frames(4)
    with StreamAnalyzer(**config) as single:
        single.process(_triad(0.5))
        columns = [single.read_frames(1) for _ in range(batch.n_frames)]
    assert batch.mel is not None and batch.chroma is not None
    assert batch.mel.shape == (16, batch.n_frames)
    assert batch.chroma.shape == (12, batch.n_frames)
    for index, frame in enumerate(columns):
        assert frame.mel is not None and frame.chroma is not None
        np.testing.assert_array_equal(batch.mel[:, index], frame.mel[:, 0])
        np.testing.assert_array_equal(batch.chroma[:, index], frame.chroma[:, 0])


@pytest.mark.parametrize(
    "read", ["read_frames", "read_quantized_frames_u8", "read_quantized_frames_i16"]
)
def test_a_feature_that_is_not_computed_is_none(read: str) -> None:
    config = dict(
        sample_rate=SR, n_mels=8, compute_mel=False, compute_onset=False, compute_spectral=False
    )
    with StreamAnalyzer(**config) as analyzer:
        analyzer.process(_triad(0.25))
        frames = getattr(analyzer, read)(2)
    assert frames.mel is None
    assert frames.onset_strength is None
    assert frames.spectral_centroid is None and frames.spectral_flatness is None
    assert frames.chroma is not None and frames.chroma.shape == (12, frames.n_frames)
    assert frames.rms_energy.shape == (frames.n_frames,)
    for removed in ("n_mels", "n_chroma", "feature_flags"):
        assert not hasattr(frames, removed)


def test_the_estimate_is_absent_before_any_audio() -> None:
    with StreamAnalyzer(sample_rate=SR) as analyzer:
        stats = analyzer.stats()
    estimate = stats.estimate
    assert (estimate.bpm, estimate.key, estimate.chord, estimate.current_bar) == (
        None,
        None,
        None,
        None,
    )
    assert estimate.detected_pattern_name is None
    for flattened in ("bpm", "key", "key_minor", "chord_root", "chord_name"):
        assert not hasattr(stats, flattened)


def test_the_estimate_carries_a_key_and_a_named_chord() -> None:
    with StreamAnalyzer(sample_rate=SR) as analyzer:
        analyzer.process(_triad(6.0))
        estimate = analyzer.stats().estimate
    assert isinstance(estimate.key, libsonare.Key)
    assert estimate.chord is not None
    assert estimate.chord.root == libsonare.PitchClass.C
    assert estimate.chord.quality == "major"
    assert estimate.chord.quality in libsonare.CHORD_QUALITIES
    assert estimate.chord.name == "C"
    assert all(change.quality in libsonare.CHORD_QUALITIES for change in estimate.chord_progression)


def test_state_reads_are_properties_and_close_is_the_only_teardown() -> None:
    analyzer = StreamAnalyzer(sample_rate=SR)
    try:
        analyzer.process(_triad(0.25))
        assert analyzer.sample_rate == SR
        assert analyzer.frame_count > 0
        assert analyzer.available_frames == analyzer.frame_count
        assert analyzer.current_time_sec > 0.0
        assert not hasattr(analyzer, "destroy")
        assert not hasattr(analyzer, "delete")
    finally:
        analyzer.close()


def test_a_new_timeline_is_anchored_by_start_sample() -> None:
    with StreamAnalyzer(sample_rate=SR) as analyzer:
        analyzer.reset(start_sample=SR)
        analyzer.process(_triad(0.25), start_sample=SR)
        assert analyzer.read_frames(1).timestamps_sec[0] >= 1.0
        with pytest.raises(TypeError):
            analyzer.reset(base_sample_offset=0)  # type: ignore[call-arg]


def test_the_quantize_ranges_take_the_stream_name() -> None:
    assert not hasattr(libsonare, "QuantizeConfig")
    with StreamAnalyzer(sample_rate=SR) as analyzer:
        analyzer.process(_triad(0.25))
        frames = analyzer.read_quantized_frames_u8(
            2, libsonare.StreamQuantizeConfig(centroid_max=1.0)
        )
    assert frames.spectral_centroid is not None
    assert frames.spectral_centroid[0] == 255
