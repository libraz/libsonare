"""The facade validators accept exactly what the ``_to_c_*`` readers accept.

Every reader narrows through ``_narrow_int``, which takes any non-``bool``
``SupportsIndex``. The facade validators used to run their own
``numbers.Integral`` test instead, and that is a strict subset: the ``Integral``
ABC supplies a concrete ``__index__``, so every ``Integral`` passes the reader's
test while a value the reader accepts could still fail theirs. An object whose
only integer-ness is ``__index__`` was therefore refused by the validator in
front of a reader that would have taken it -- the drift that routing both
through one predicate removes, and the one behaviour change that routing made.

Each case is driven through the public facade rather than the private validator,
so it fails if an entry point stops routing through the shared predicate. Each
asserts the value was USED rather than merely not refused: an argument a facade
quietly ignores is indistinguishable from one it accepted.

The second half is the more interesting one. The same object past the C range is
refused for its RANGE, not for its type, which says it reached the range check
rather than being turned away at the door.
"""

from __future__ import annotations

import hashlib
import math
import numbers
import operator
from typing import SupportsIndex

import numpy as np
import pytest

import libsonare as ls
from libsonare import SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


class Index:
    """Integer-like through ``__index__`` alone, outside ``numbers.Integral``."""

    def __init__(self, value: int) -> None:
        self._value = value

    def __index__(self) -> int:
        return self._value

    def __repr__(self) -> str:
        return f"Index({self._value})"


@pytest.fixture(scope="module")
def tone() -> np.ndarray:
    """A plain sine, enough frames for the transforms to have something to do."""
    n = 4096
    return (0.3 * np.sin(2.0 * math.pi * 440.0 * np.arange(n) / SR)).astype(np.float32)


@pytest.fixture(scope="module")
def padded() -> np.ndarray:
    """A burst between two silences, so a trim window length changes the result."""
    out = np.zeros(8192, dtype=np.float32)
    out[2000:6000] = (0.5 * np.sin(2.0 * math.pi * 220.0 * np.arange(4000) / SR)).astype(np.float32)
    return out


def _digest(values: object) -> str:
    array = np.ascontiguousarray(np.asarray(values, dtype=np.float64))
    return hashlib.sha256(array.tobytes()).hexdigest()


def test_the_probe_sits_in_the_gap_between_the_two_predicates() -> None:
    """Without this the cases below could all be passing on an ordinary int."""
    probe = Index(31)
    assert not isinstance(probe, numbers.Integral)
    assert isinstance(probe, SupportsIndex)
    assert operator.index(probe) == 31


# -- accepted, and used --------------------------------------------------------


@pytest.mark.parametrize(
    ("keyword", "value"),
    [("kernel_harmonic", 15), ("kernel_percussive", 15), ("n_fft", 1024), ("hop_length", 256)],
)
def test_an_index_only_hpss_argument_separates_as_the_value_it_names(keyword, value, tone) -> None:
    """The kernel, the STFT size and the hop each reach a different validator."""
    default = _digest(ls.hpss(tone, SR).harmonic)
    named = _digest(ls.hpss(tone, SR, **{keyword: value}).harmonic)
    assert named != default  # positive control: the plain value is not the default
    assert _digest(ls.hpss(tone, SR, **{keyword: Index(value)}).harmonic) == named


@pytest.mark.parametrize(("keyword", "value"), [("n_components", 2), ("n_iter", 20)])
def test_an_index_only_decompose_count_factors_as_the_count_it_names(keyword, value, tone) -> None:
    """The C-int config field, on the entry point whose 0 means "keep the default"."""

    def factored(**kwargs) -> str:
        # nndsvd rather than the default random init, so the result is a
        # deterministic function of the counts under test.
        return _digest(ls.decompose_stems(tone, SR, init="nndsvd", **kwargs)["components"])

    default = factored()
    named = factored(**{keyword: value})
    assert named != default  # positive control
    assert factored(**{keyword: Index(value)}) == named


@pytest.mark.parametrize(("keyword", "value"), [("frame_length", 512), ("hop_length", 128)])
def test_an_index_only_trim_window_trims_as_the_window_it_names(keyword, value, padded) -> None:
    """The ``trim`` pair, which carries its own copy of the same validator."""
    default = _digest(ls.trim(padded, SR))
    named = _digest(ls.trim(padded, SR, **{keyword: value}))
    assert named != default  # positive control
    assert _digest(ls.trim(padded, SR, **{keyword: Index(value)})) == named


# -- refused for the range it missed, not for its type -------------------------


def _refusal(entry, **kwargs) -> str:
    with pytest.raises(SonareValueError) as raised:
        entry(**kwargs)
    return str(raised.value)


@pytest.mark.parametrize(
    ("keyword", "domain"),
    [
        ("kernel_harmonic", "hpss: kernel_harmonic must be a positive odd signed 32-bit integer"),
        ("n_fft", "hpss: n_fft must be an even signed 32-bit integer >= 2"),
        ("hop_length", "hpss: hop_length must fit in a positive signed 32-bit integer"),
    ],
)
def test_an_index_only_hpss_argument_past_the_range_is_refused_for_its_range(
    keyword, domain, tone
) -> None:
    """Reaching the range check is what separates this from the old refusal.

    The value is index-able, so the type half passes and the range half answers.
    Asserted against the type wording as well, because that is the message the
    same input drew before the validators shared a predicate -- a regression
    would land back on it rather than on a different string.
    """
    message = _refusal(ls.hpss, samples=tone, sample_rate=SR, **{keyword: Index(2**32)})
    assert message == domain
    assert "must be an integer" not in message


def test_an_index_only_decompose_count_past_the_range_is_refused_for_its_range(tone) -> None:
    """The C-int config field words its range half differently from the domains above."""
    message = _refusal(ls.decompose_stems, samples=tone, sample_rate=SR, n_components=Index(2**32))
    assert message == "decompose_stems: n_components must fit in a signed 32-bit integer"
    assert "must be an integer" not in message


@pytest.mark.parametrize("keyword", ["frame_length", "hop_length"])
def test_an_index_only_trim_window_past_the_range_is_refused_for_its_range(keyword, padded) -> None:
    """The ``trim`` copy reaches its range check the same way."""
    message = _refusal(ls.trim, samples=padded, sample_rate=SR, **{keyword: Index(2**32)})
    assert message == f"trim: {keyword} must fit in a positive signed 32-bit integer"
    assert "must be an integer" not in message


# -- what did NOT widen --------------------------------------------------------


@pytest.mark.parametrize("value", [True, False])
def test_a_bool_is_still_refused_by_name_on_every_validator(value, tone, padded) -> None:
    """``bool`` is an ``int`` subclass, so only an explicit test keeps it out.

    It can never fail a range check, and the entry points that genuinely take a
    flag spell the conversion out at the call site. Both predicates refused it
    before and the shared one refuses it now; without this case the widening
    above could have carried it in.
    """
    assert "kernel_harmonic must be an integer" in _refusal(
        ls.hpss, samples=tone, sample_rate=SR, kernel_harmonic=value
    )
    assert "n_fft must be an integer" in _refusal(
        ls.hpss, samples=tone, sample_rate=SR, n_fft=value
    )
    assert "n_components must be an integer" in _refusal(
        ls.decompose_stems, samples=tone, sample_rate=SR, n_components=value
    )
    assert "frame_length must be an integer" in _refusal(
        ls.trim, samples=padded, sample_rate=SR, frame_length=value
    )
