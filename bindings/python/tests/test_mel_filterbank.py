"""mel_filterbank and mel_frequencies against the librosa references and their refusals."""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

REFERENCE = Path(__file__).resolve().parents[3] / "tests" / "librosa" / "reference"


def _rows(name: str) -> list[dict]:
    return json.loads((REFERENCE / name).read_text(encoding="utf-8"))["data"]


def _row_id(row: dict) -> str:
    return "-".join(f"{k}={row[k]}" for k in ("sr", "n_fft", "n_mels", "htk", "fmax") if k in row)


@pytest.mark.parametrize("row", _rows("mel_filterbank.json"), ids=_row_id)
def test_mel_filterbank_matches_librosa(row: dict) -> None:
    # A row without "norm" is librosa's default Slaney normalization; null is none.
    norm = "none" if "norm" in row and row["norm"] is None else "slaney"
    fb = libsonare.mel_filterbank(
        row["sr"],
        row["n_fft"],
        n_mels=row["n_mels"],
        fmax_hz=row.get("fmax"),
        htk=row["htk"],
        norm=norm,
    )
    assert fb.dtype == np.float32
    assert list(fb.shape) == row["shape"]
    np.testing.assert_allclose(float(fb.sum(dtype=np.float64)), row["sum"], rtol=1e-3)
    np.testing.assert_allclose(float(fb.max()), row["max"], rtol=1e-3)
    np.testing.assert_allclose(fb.sum(axis=1, dtype=np.float64), row["row_sums"], rtol=1e-3)
    for m, expected in enumerate(row.get("first_rows", [])):
        want = np.asarray(expected, dtype=np.float64)
        got = fb[m, : want.size].astype(np.float64)
        zero = want == 0.0
        assert np.all(np.abs(got[zero]) <= 1e-7)
        np.testing.assert_allclose(got[~zero], want[~zero], rtol=1e-3)


@pytest.mark.parametrize(
    "row",
    _rows("mel_frequencies.json"),
    ids=lambda r: f"n={r['n_mels']}-fmin={r['fmin']}-fmax={r['fmax']}-htk={r['htk']}",
)
def test_mel_frequencies_matches_librosa(row: dict) -> None:
    points = libsonare.mel_frequencies(row["n_mels"], row["fmin"], row["fmax"], row["htk"])
    assert points.dtype == np.float32
    want = np.asarray(row["values"], dtype=np.float64)
    assert points.shape == want.shape
    zero = want == 0.0
    assert np.all(np.abs(points[zero]) <= 1e-3)
    np.testing.assert_allclose(points[~zero], want[~zero], rtol=1e-4)


def test_mel_filterbank_band_peaks_at_its_centre_point() -> None:
    sr, n_fft, n_mels = 22050, 2048, 40
    fb = libsonare.mel_filterbank(sr, n_fft, n_mels=n_mels, norm="none")
    points = libsonare.mel_frequencies(n_mels + 2, 0.0, sr / 2)
    bin_hz = sr / n_fft
    for m in range(n_mels):
        assert abs(int(np.argmax(fb[m])) * bin_hz - float(points[m + 1])) <= bin_hz


def test_mel_filterbank_omitted_fmax_is_the_nyquist_and_above_it_is_refused() -> None:
    omitted = libsonare.mel_filterbank(44100, 1024, n_mels=32)
    nyquist = libsonare.mel_filterbank(44100, 1024, n_mels=32, fmax_hz=22050.0)
    np.testing.assert_array_equal(omitted, nyquist)
    with pytest.raises(
        libsonare.SonareValueError,
        match=r"^mel_filterbank: fmax_hz must be finite and at most 22050$",
    ):
        libsonare.mel_filterbank(44100, 1024, n_mels=32, fmax_hz=22051.0)


def test_mel_filterbank_slaney_divides_each_band_by_its_width() -> None:
    slaney = libsonare.mel_filterbank(22050, 1024, n_mels=16)
    none = libsonare.mel_filterbank(22050, 1024, n_mels=16, norm="none")
    points = libsonare.mel_frequencies(18, 0.0, 11025.0).astype(np.float64)
    width = 2.0 / (points[2:] - points[:-2])
    assert np.all(none.max(axis=1) <= 1.0)
    np.testing.assert_allclose(
        slaney, none * width[:, None].astype(np.float32), rtol=1e-4, atol=1e-9
    )


def test_one_band_and_one_point_are_accepted() -> None:
    assert libsonare.mel_filterbank(22050, 1024, n_mels=1).shape == (1, 513)
    np.testing.assert_array_equal(libsonare.mel_frequencies(1, 100.0, 200.0), [100.0])


@pytest.mark.parametrize(
    ("call", "message"),
    [
        (
            lambda: libsonare.mel_filterbank(22050, 1024, fmax_hz=0.0),
            "mel_filterbank: fmax_hz must be greater than 0; omit it for the default",
        ),
        (
            lambda: libsonare.mel_filterbank(22050, 1024, fmin_hz=12000.0),
            "mel_filterbank: fmax_hz must be greater than fmin_hz",
        ),
        (
            lambda: libsonare.mel_filterbank(22050, 1024, fmin_hz=500.0, fmax_hz=400.0),
            "mel_filterbank: fmax_hz must be greater than fmin_hz",
        ),
        (
            lambda: libsonare.mel_filterbank(22050, 1024, n_mels=0),
            "mel_filterbank: n_mels must be an integer in [1, 4096]",
        ),
        (
            lambda: libsonare.mel_filterbank(22050, 1024, norm="area"),  # type: ignore[arg-type]
            "mel_filterbank: norm must be one of none, slaney",
        ),
        (
            lambda: libsonare.mel_frequencies(4, 100.0, 50.0),
            "mel_frequencies: fmax must be finite and greater than fmin",
        ),
        (
            lambda: libsonare.mel_frequencies(4, -1.0, 50.0),
            "mel_frequencies: fmin must be finite and at least 0",
        ),
        (
            lambda: libsonare.mel_frequencies(0),
            "mel_frequencies: n_mels must be an integer in [1, ",
        ),
    ],
)
def test_refusals_name_the_python_argument(call, message: str) -> None:
    with pytest.raises(SonareValueError) as caught:
        call()
    assert str(caught.value).startswith(message)


@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf])
@pytest.mark.parametrize("name", ["fmin_hz", "fmax_hz"])
def test_mel_filterbank_refuses_non_finite_bounds(name: str, bad: float) -> None:
    with pytest.raises(SonareValueError, match=rf"^mel_filterbank: {name} must be finite"):
        libsonare.mel_filterbank(22050, 1024, **{name: bad})


@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf])
@pytest.mark.parametrize("name", ["fmin", "fmax"])
def test_mel_frequencies_refuses_non_finite_bounds(name: str, bad: float) -> None:
    kwargs = {"fmin": 0.0, "fmax": 8000.0, name: bad}
    with pytest.raises(SonareValueError, match=rf"^mel_frequencies: {name} must be finite"):
        libsonare.mel_frequencies(8, **kwargs)


@pytest.mark.parametrize(
    ("call", "message"),
    [
        (
            lambda: libsonare.mel_spectrogram(np.zeros(4096, np.float32), 22050, fmin_hz=12000.0),
            "mel_spectrogram: fmax_hz must be greater than fmin_hz",
        ),
        (
            lambda: libsonare.mfcc(np.zeros(4096, np.float32), 22050, fmin=12000.0),
            "mfcc: fmax must be greater than fmin",
        ),
        (
            lambda: libsonare.mel_to_stft(np.ones((16, 4), np.float32), sr=22050, fmin=12000.0),
            "mel_to_stft: fmax must be greater than fmin",
        ),
        (
            lambda: libsonare.mel_to_audio(
                np.ones((16, 4), np.float32), sr=22050, fmin=12000.0, n_iter=1
            ),
            "mel_to_audio: fmax must be greater than fmin",
        ),
    ],
)
def test_mel_family_names_both_bounds_of_an_empty_band_range(call, message: str) -> None:
    with pytest.raises(SonareValueError) as caught:
        call()
    assert str(caught.value) == message


_SR = 22050
_TONE = (0.5 * np.sin(2 * np.pi * 440 * np.arange(_SR // 4) / _SR)).astype(np.float32)
_MEL = np.ones((16, 4), dtype=np.float32)
_MFCC = np.zeros((13, 4), dtype=np.float32)
_MEL_FAMILY = {
    "mel_spectrogram": lambda fmax: libsonare.mel_spectrogram(_TONE, _SR, fmax_hz=fmax),
    "mfcc": lambda fmax: libsonare.mfcc(_TONE, sr=_SR, fmax=fmax),
    "mel_to_stft": lambda fmax: libsonare.mel_to_stft(_MEL, sr=_SR, n_fft=512, fmax=fmax),
    "mel_to_audio": lambda fmax: libsonare.mel_to_audio(
        _MEL, sr=_SR, n_fft=512, hop_length=128, fmax=fmax, n_iter=1
    ),
    "mfcc_to_audio": lambda fmax: libsonare.mfcc_to_audio(
        _MFCC, n_mels=16, sr=_SR, n_fft=512, hop_length=128, fmax=fmax, n_iter=1
    ),
}


@pytest.mark.parametrize("name", sorted(_MEL_FAMILY))
def test_mel_family_refuses_an_fmax_of_zero_or_above_the_nyquist(name) -> None:
    """The Nyquist is the upper Mel edge; 0 and a larger finite value are refused, never read."""
    call = _MEL_FAMILY[name]
    call(_SR / 2)
    with pytest.raises(libsonare.SonareValueError):
        call(0.0)
    with pytest.raises(libsonare.SonareValueError, match=r"must be finite and at most 11025$"):
        call(_SR / 2 + 1)
