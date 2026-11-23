"""An argument that would size a result or work buffer past the limit is refused by name.

Each case passes a small input with an extreme argument, so the refusal fires before the
large allocation, and pins the whole message. The limit is 500000000 elements unless the
case says otherwise.
"""

from __future__ import annotations

import re

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

_SR = 22050
_CQT_KERNEL_LIMIT = 33554432
_ELEMENT_LIMIT = 500000000


def _refused(text: str):
    return pytest.raises(SonareValueError, match=re.escape(text))


def _zeros(count: int) -> np.ndarray:
    return np.zeros(count, dtype=np.float32)


def _sine(count: int) -> np.ndarray:
    return np.sin(np.arange(count, dtype=np.float32) * 0.05).astype(np.float32)


def _ones(rows: int, cols: int) -> np.ndarray:
    return np.ones((rows, cols), dtype=np.float32)


def _cqt_kernel_text(function: str) -> str:
    return f"{function}: n_bins must keep the filter kernel within {_CQT_KERNEL_LIMIT} elements"


def _cqt_output_text(function: str) -> str:
    return (
        f"{function}: hop_length must keep the constant-Q output within {_ELEMENT_LIMIT} elements"
    )


# One octave of 500 bins at one-sample hop: a million samples frame to more than 5e8 cells.
_CQT_OUTPUT_ARGS = {
    "sr": _SR,
    "hop_length": 1,
    "fmin": _SR / 4,
    "n_bins": 500,
    "bins_per_octave": 500,
}


def test_cqt_refuses_past_the_work_size_limit():
    with _refused(_cqt_kernel_text("cqt")):
        libsonare.cqt(_zeros(4096), n_bins=_CQT_KERNEL_LIMIT // 2 + 1)
    with _refused(_cqt_output_text("cqt")):
        libsonare.cqt(_zeros(1_000_001), **_CQT_OUTPUT_ARGS)


def test_pseudo_cqt_refuses_past_the_work_size_limit():
    with _refused(_cqt_kernel_text("pseudo_cqt")):
        libsonare.pseudo_cqt(_zeros(4096), n_bins=_CQT_KERNEL_LIMIT // 2 + 1)


def test_hybrid_cqt_refuses_past_the_work_size_limit():
    with _refused(_cqt_kernel_text("hybrid_cqt")):
        libsonare.hybrid_cqt(_zeros(4096), n_bins=_CQT_KERNEL_LIMIT // 2 + 1)
    with _refused(_cqt_output_text("hybrid_cqt")):
        libsonare.hybrid_cqt(_zeros(1_000_001), **_CQT_OUTPUT_ARGS)


def test_vqt_refuses_past_the_work_size_limit():
    with _refused(_cqt_kernel_text("vqt")):
        libsonare.vqt(_zeros(4096), n_bins=_CQT_KERNEL_LIMIT // 2 + 1)
    with _refused(_cqt_output_text("vqt")):
        libsonare.vqt(_zeros(1_000_001), **_CQT_OUTPUT_ARGS)


def test_cqt_to_audio_refuses_past_the_work_size_limit():
    # 65536 * 7631 hops is the first reconstruction past 5e8 samples.
    with _refused(
        "cqt_to_audio: hop_length must keep the reconstructed signal within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.cqt_to_audio(_ones(1, 7631), _SR, hop_length=65536)
    # A 131072-point FFT carries 65537 bins, so 7631 frames of them pass 5e8 cells.
    with _refused(
        "cqt_to_audio: magnitude length must keep the STFT magnitude within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.cqt_to_audio(_ones(1, 7631), _SR, hop_length=1, fmin_hz=0.3, bins_per_octave=1)


def test_vqt_to_audio_refuses_past_the_work_size_limit():
    with _refused(
        "vqt_to_audio: hop_length must keep the reconstructed signal within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.vqt_to_audio(_ones(1, 7631), _SR, hop_length=65536)
    with _refused(
        "vqt_to_audio: magnitude length must keep the STFT magnitude within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.vqt_to_audio(
            _ones(1, 7631), _SR, hop_length=1, fmin_hz=0.3, bins_per_octave=1, gamma=1e-3
        )


def test_pitch_pyin_refuses_past_the_work_size_limit():
    with _refused(
        f"pitch_pyin: hop_length must keep the observation matrix within {_ELEMENT_LIMIT} elements"
    ):
        libsonare.pitch_pyin(
            _zeros(220000),
            _SR,
            frame_length=2048,
            hop_length=1,
            f0_min_hz=_SR / 2040,
            f0_max_hz=_SR / 2.1,
        )


def test_analyze_melody_refuses_past_the_work_size_limit():
    with _refused(
        "analyze_melody: hop_length must keep the observation matrix within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.analyze_melody(
            _zeros(220000),
            _SR,
            f0_min_hz=_SR / 2040,
            f0_max_hz=_SR / 2.1,
            frame_length=2048,
            hop_length=1,
            use_pyin=True,
        )


def test_polyphonic_analysis_refuses_past_the_work_size_limit():
    # A 1 Hz to 1e8 Hz axis at one cent per bin is about 31900 bins; 20000 hops of it pass 5e8.
    config = {
        "n_fft": 64,
        "hop_length": 1,
        "cents_per_bin": 1.0,
        "cent_ref_hz": 1.0,
        "cent_max_hz": 1e8,
    }
    with _refused(
        "PolyphonicAnalysis: cents_per_bin must keep the cent spectrum within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.PolyphonicAnalysis(_zeros(20000), 44100, **config)
    with _refused(
        "PolyphonicAnalysis.analyze: cents_per_bin must keep the cent spectrum within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.PolyphonicAnalysis.analyze(_zeros(20000), 44100, **config)


def test_decompose_refuses_past_the_work_size_limit():
    with _refused(
        f"decompose: n_components must keep the component weights within {_ELEMENT_LIMIT} elements"
    ):
        libsonare.decompose([1.0], 1, 1, _ELEMENT_LIMIT + 1)
    with _refused(
        "decompose: n_components must keep the component activations within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.decompose([1.0] * 5001, 1, 5001, 100000)


def test_decompose_stems_refuses_past_the_work_size_limit():
    with _refused(
        "decompose_stems: n_components must keep the component weights within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.decompose_stems(
            _sine(8192), _SR, n_components=_ELEMENT_LIMIT + 1, n_fft=4, hop_length=1
        )
    with _refused(
        "decompose_stems: n_components must keep the component activations within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.decompose_stems(_sine(8192), _SR, n_components=100000, n_fft=4, hop_length=1)


def test_decompose_stems_linked_refuses_past_the_work_size_limit():
    channels = np.stack([_sine(8192), _sine(8192)])
    with _refused(
        "decompose_stems_linked: n_components must keep the component weights within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.decompose_stems_linked(
            channels, _SR, n_components=_ELEMENT_LIMIT + 1, n_fft=4, hop_length=1
        )
    with _refused(
        "decompose_stems_linked: n_components must keep the component activations within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.decompose_stems_linked(channels, _SR, n_components=100000, n_fft=4, hop_length=1)


def test_phase_vocoder_refuses_past_the_work_size_limit():
    with _refused(
        "phase_vocoder: speed_ratio must keep the stretched spectrogram within "
        f"{_ELEMENT_LIMIT} elements"
    ):
        libsonare.phase_vocoder(_sine(8192), _SR, 1e-6)


def test_note_stretch_refuses_past_the_work_size_limit():
    with _refused(
        "note_stretch: stretch_ratio must keep the stretched note within 2147483647 elements"
    ):
        libsonare.note_stretch(_sine(8192), _SR, 1000, 6000, 1e6)


# 12288 is the widest tempogram window; 81400 onset frames of its 6145 bins pass 5e8 cells.
_TEMPOGRAM_WINDOW = 12288


def test_fourier_tempogram_refuses_past_the_work_size_limit():
    with _refused(
        f"fourier_tempogram: win_length must keep the tempogram within {_ELEMENT_LIMIT} elements"
    ):
        libsonare.fourier_tempogram(np.ones(81400, dtype=np.float32), _SR, 512, _TEMPOGRAM_WINDOW)


def test_cyclic_tempogram_refuses_past_the_work_size_limit():
    with _refused(
        f"cyclic_tempogram: win_length must keep the tempogram within {_ELEMENT_LIMIT} elements"
    ):
        libsonare.cyclic_tempogram(np.ones(81400, dtype=np.float32), _SR, 512, _TEMPOGRAM_WINDOW)
    with _refused(
        f"cyclic_tempogram: n_bins must keep the cyclic tempogram within {_ELEMENT_LIMIT} elements"
    ):
        libsonare.cyclic_tempogram(np.ones(1000, dtype=np.float32), _SR, 512, 8, 60.0, 500001)


def test_plp_refuses_past_the_work_size_limit():
    with _refused(f"plp: win_length must keep the tempogram within {_ELEMENT_LIMIT} elements"):
        libsonare.plp(np.ones(81400, dtype=np.float32), _SR, 512, win_length=_TEMPOGRAM_WINDOW)


def test_tempogram_refuses_past_the_work_size_limit():
    with _refused(
        f"tempogram: win_length must keep the tempogram within {_ELEMENT_LIMIT} elements"
    ):
        libsonare.tempogram(np.ones(40700, dtype=np.float32), _SR, 512, _TEMPOGRAM_WINDOW)


def test_segment_cross_similarity_refuses_past_the_work_size_limit():
    # 46341 * 46341 is the first square past a 32-bit index.
    with _refused(
        "segment_cross_similarity: x length must keep the similarity matrix within "
        "2147483647 elements"
    ):
        libsonare.segment_cross_similarity(_ones(1, 46341), _ones(1, 46341))


def test_frame_signal_refuses_past_the_work_size_limit():
    with _refused(
        f"frame_signal: hop_length must keep the framed output within {_ELEMENT_LIMIT} elements"
    ):
        libsonare.frame_signal(_zeros(70000), 35000, 1)


def test_remix_refuses_past_the_work_size_limit():
    intervals = np.tile(np.array([[0, 1_000_000]], dtype=np.int32), (501, 1))
    with _refused(
        f"remix: intervals must keep the remixed signal within {_ELEMENT_LIMIT} elements"
    ):
        libsonare.remix(_zeros(1_000_000), intervals, _SR, False)


def test_pad_center_refuses_past_the_work_size_limit():
    with _refused(
        f"pad_center: target_size must keep the padded output within {_ELEMENT_LIMIT} elements"
    ):
        libsonare.pad_center([1.0], _ELEMENT_LIMIT + 1)


def test_fix_length_refuses_past_the_work_size_limit():
    with _refused(
        f"fix_length: target_size must keep the padded output within {_ELEMENT_LIMIT} elements"
    ):
        libsonare.fix_length([1.0], _ELEMENT_LIMIT + 1)
