import numpy as np
import pytest

import libsonare

_ROUTES = ("mfcc", "mfcc_to_mel", "mfcc_to_audio")


def _run(route: str, lifter: float, *, invalid_coefficients: bool = False) -> None:
    if route == "mfcc":
        samples = np.zeros(512, dtype=np.float32)
        libsonare.mfcc(samples, n_fft=512, hop_length=128, n_mels=16, n_mfcc=8, lifter=lifter)
        return
    coefficients = np.zeros((8, 4), dtype=np.float32)
    if invalid_coefficients:
        coefficients[0, 1] = np.nan
    if route == "mfcc_to_mel":
        libsonare.mfcc_to_mel(coefficients, n_mels=16, lifter=lifter)
    else:
        libsonare.mfcc_to_audio(
            coefficients, n_mels=16, n_fft=512, hop_length=128, n_iter=1, lifter=lifter
        )


@pytest.mark.parametrize("route", _ROUTES)
@pytest.mark.parametrize("lifter", [float("nan"), float("inf"), -float("inf"), -1.0, -1e-50])
def test_mfcc_routes_name_the_rejected_lifter(route: str, lifter: float) -> None:
    with pytest.raises(
        libsonare.SonareValueError, match=rf"^{route}: lifter must be finite and at least 0$"
    ):
        _run(route, lifter)


@pytest.mark.parametrize("route", _ROUTES)
def test_mfcc_routes_accept_an_explicit_negative_zero(route: str) -> None:
    _run(route, -0.0)


@pytest.mark.parametrize("route", ["mfcc_to_mel", "mfcc_to_audio"])
def test_inverse_mfcc_admits_lifter_before_coefficients(route: str) -> None:
    with pytest.raises(
        libsonare.SonareValueError, match=rf"^{route}: lifter must be finite and at least 0$"
    ):
        _run(route, -1e-50, invalid_coefficients=True)


def test_forward_mfcc_admits_lifter_before_samples() -> None:
    samples = np.zeros(512, dtype=np.float32)
    samples[1] = np.nan
    with pytest.raises(
        libsonare.SonareValueError, match=r"^mfcc: lifter must be finite and at least 0$"
    ):
        libsonare.mfcc(samples, n_fft=512, hop_length=128, n_mels=16, n_mfcc=8, lifter=-1e-50)
