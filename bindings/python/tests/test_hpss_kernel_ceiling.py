"""Kernel-size boundary tests for the HPSS entry points of the Python facade.

The two median filters each guard their kernel twice with the same error code:
once for odd-and-positive, which carries that code's generic message, and once
for the ceiling, which carries a message naming the filter and the ceiling's
value. Separating the two is the whole point -- an oversized kernel used to
reach the per-worker allocations, where the refusal depended on whether the host
overcommitted, so ``OutOfMemory`` / ``Unknown`` are exactly what must never come
back and a bare "it raises" would accept them.

Two of the three entry points here validate the kernel in the binding before the
call, and the third does not, so the same input produces a different refusal on
each. Each is asserted for what it actually produces rather than flattened.
"""

from __future__ import annotations

import math

import numpy as np
import pytest
from numpy.typing import NDArray

from libsonare import (
    ErrorCode,
    SonareError,
    SonareValueError,
    extract_percussive_events,
    hpss,
    hpss_with_residual,
)

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050

# The ceiling the refusal message names. Written out rather than derived, so a
# ceiling quietly lowered moves the message and fails here instead of following
# the change. It is a power of two and therefore even, so it is not itself a
# legal kernel: the largest a caller can pass is the odd value one below it, and
# the first value the ceiling guard sees is the odd value one above.
CEILING = 524288
LARGEST_LEGAL = 524287
FIRST_ABOVE_CEILING = 524289

INT_MAX = 2147483647
# Even, so it is refused for parity rather than for the ceiling.
INT_MAX_MINUS_ONE = 2147483646

# Values past the signed 32-bit range, each with what a ctypes c_int32 field
# truncates it to. The truncated value is what makes these the interesting
# inputs rather than ``INT_MAX``, which is exactly representable and arrives
# intact: two of them land on an ordinary legal kernel and two on 0, which the
# percussive-event config reads as "use the default". Every one therefore has a
# plausible SUCCESSFUL outcome waiting for it if the range check is dropped, and
# a refusal-shaped assertion cannot see that.
WRAPPING = [
    (2**32, 0),
    (2**32 + 1, 1),
    (2**32 + 3, 3),
    (3 * 2**32, 0),
    (2**31, -2147483648),
]

# A refusal reduced to the same domain as a result, so the two can be compared.
REFUSED = "refused"


@pytest.fixture(scope="module")
def tone() -> list[float]:
    """A plain sine, enough frames for the transform to have something to do."""
    return [math.sin(2.0 * math.pi * 440.0 * i / SR) for i in range(4096)]


@pytest.fixture(scope="module")
def hits() -> NDArray[np.float32]:
    """A click train over a quiet tone bed, so the detector returns events."""
    n = SR
    rng = np.random.default_rng(7)
    out = (0.02 * np.sin(2.0 * np.pi * 220.0 * np.arange(n) / SR)).astype(np.float32)
    decay = np.exp(-np.arange(200) / 25.0)
    for start in range(1000, n - 1000, 2205):
        out[start : start + 200] += (0.9 * decay * rng.uniform(-1.0, 1.0, 200)).astype(np.float32)
    return out


def _assert_parameter_refusal(error: SonareError) -> None:
    """Every refusal below is a parameter refusal, never an allocation failure."""
    assert error.code == int(ErrorCode.INVALID_PARAMETER)
    assert error.code_name == "InvalidParameter"
    # Stated separately from the equality above because these two are the
    # outcomes the ceiling guard exists to make unreachable, and a regression
    # that reintroduced either would be a different defect from a wrong code.
    assert error.code != int(ErrorCode.OUT_OF_MEMORY)
    assert error.code != int(ErrorCode.UNKNOWN)


def _assert_names_ceiling(error: SonareError, direction: str, kernel: int) -> None:
    """The refusal names the filter, the value passed, and the ceiling."""
    _assert_parameter_refusal(error)
    filter_name = (
        "median_filter_horizontal" if direction == "harmonic" else "median_filter_vertical"
    )
    assert f"{filter_name}: kernel_size {kernel} exceeds the maximum {CEILING}" in str(error)


def _kernels(direction: str, value: int) -> dict[str, int]:
    """Kernel keywords with ``direction`` set to ``value`` and the other default."""
    return {
        "kernel_harmonic": value if direction == "harmonic" else 31,
        "kernel_percussive": value if direction == "percussive" else 31,
    }


def _hpss_outcome(entry, tone, **kernels) -> str:
    """One comparable string per outcome, so two runs on different kernels differ."""
    try:
        result = entry(tone, SR, **kernels)
    except SonareError:
        return REFUSED
    fields = result if isinstance(result, dict) else {"harmonic": result.harmonic}
    return "|".join(
        f"{name}:{len(values)}:{float(np.asarray(values, dtype=np.float64).sum()):.9g}"
        for name, values in sorted(fields.items())
        if not isinstance(values, (int, float))
    )


def _event_outcome(hits, **separation) -> str:
    """The event-list counterpart of :func:`_hpss_outcome`."""
    try:
        events = extract_percussive_events(hits, SR, **separation)
    except SonareError:
        return REFUSED
    return repr([(event.onset_sample, event.offset_sample) for event in events])


@pytest.mark.parametrize("entry", [hpss, hpss_with_residual])
@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
@pytest.mark.parametrize("kernel", [INT_MAX, FIRST_ABOVE_CEILING])
def test_oversized_odd_kernel_is_refused_by_name(entry, direction, kernel, tone) -> None:
    """An odd kernel above the ceiling is refused by the filter it would drive.

    Each filter carries its own copy of the guard, so one direction says nothing
    about the other, and each entry point reaches both filters independently.
    """
    with pytest.raises(SonareError) as raised:
        entry(tone, SR, **_kernels(direction, kernel))
    _assert_names_ceiling(raised.value, direction, kernel)


@pytest.mark.parametrize("entry", [hpss, hpss_with_residual])
@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
@pytest.mark.parametrize("kernel", [INT_MAX_MINUS_ONE, CEILING])
def test_even_kernel_is_refused_for_parity_not_for_the_ceiling(
    entry, direction, kernel, tone
) -> None:
    """An even kernel never reaches the ceiling guard, whatever its magnitude.

    ``INT_MAX - 1`` and the ceiling's own value are both even, so both die on
    odd-and-positive. The binding validates that itself, ahead of the call, so
    the refusal is the binding's and carries the parameter's name -- and it must
    not carry the ceiling, or a test could pass by tripping the wrong guard.
    """
    with pytest.raises(SonareValueError) as raised:
        entry(tone, SR, **_kernels(direction, kernel))
    _assert_parameter_refusal(raised.value)
    arg_name = f"kernel_{direction}"
    assert f"{arg_name} must be a positive odd signed 32-bit integer" in str(raised.value)
    assert str(CEILING) not in str(raised.value)


@pytest.mark.parametrize("entry", [hpss, hpss_with_residual])
@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
def test_a_kernel_past_the_signed_range_is_refused_rather_than_wrapped(
    entry, direction, tone
) -> None:
    """Python integers are unbounded, so the range check has to be the binding's.

    ``INT_MAX + 1`` is expressible here. Passed to ctypes unchecked it would wrap
    into a negative kernel and be refused for the wrong reason, so the binding
    range-checks it first and says so.
    """
    with pytest.raises(SonareValueError) as raised:
        entry(tone, SR, **_kernels(direction, INT_MAX + 1))
    _assert_parameter_refusal(raised.value)
    assert f"kernel_{direction} must be a positive odd signed 32-bit integer" in str(raised.value)


@pytest.mark.parametrize("entry", [hpss, hpss_with_residual])
@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
@pytest.mark.parametrize(("passed", "truncates_to"), WRAPPING)
def test_a_wrapping_kernel_is_refused_by_name(entry, direction, passed, truncates_to, tone) -> None:
    """Every value past the range is refused by the binding, naming the keyword."""
    del truncates_to
    with pytest.raises(SonareValueError) as raised:
        entry(tone, SR, **_kernels(direction, passed))
    _assert_parameter_refusal(raised.value)
    assert f"kernel_{direction} must be a positive odd signed 32-bit integer" in str(raised.value)
    assert "exceeds the maximum" not in str(raised.value)


@pytest.mark.parametrize("entry", [hpss, hpss_with_residual])
@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
@pytest.mark.parametrize(("passed", "truncates_to"), WRAPPING)
def test_a_wrapping_kernel_never_separates_as_the_truncated_one(
    entry, direction, passed, truncates_to, tone
) -> None:
    """The assertion the refusal above cannot make.

    A dropped range check does not raise -- it returns the default-kernel result
    or the truncated-kernel one, both of which look like an ordinary success.
    Both references are built here and compared against, so the silent path is
    what fails rather than merely the loud one.
    """
    outcome = _hpss_outcome(entry, tone, **_kernels(direction, passed))
    assert outcome == REFUSED
    assert outcome != _hpss_outcome(entry, tone)
    # 0 and the most negative int are not legal kernels, so there is no
    # truncated run to build for them; the refusal is the whole statement.
    if truncates_to > 0 and truncates_to % 2 == 1:
        assert outcome != _hpss_outcome(entry, tone, **_kernels(direction, truncates_to))


@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
def test_the_largest_legal_kernel_still_separates(direction, tone) -> None:
    """Without this, a guard that refused every kernel would satisfy the cases above."""
    result = hpss(tone, SR, **_kernels(direction, LARGEST_LEGAL))
    assert result.length == len(tone)
    assert len(result.harmonic) == len(tone)
    assert len(result.percussive) == len(tone)
    assert all(math.isfinite(value) for value in result.harmonic)
    assert all(math.isfinite(value) for value in result.percussive)


def test_an_ordinary_kernel_still_separates(tone) -> None:
    """The positive control for the refusals: the default path is unaffected."""
    result = hpss(tone, SR)
    assert result.length == len(tone)
    assert any(value != 0.0 for value in result.harmonic)

    residual = hpss_with_residual(tone, SR)
    assert set(residual) == {"harmonic", "percussive", "residual", "sampleRate"}
    assert len(residual["harmonic"]) == len(tone)


# -- The percussive-event facade, which does not pre-validate the kernel --


@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
@pytest.mark.parametrize("kernel", [INT_MAX, FIRST_ABOVE_CEILING])
def test_percussive_events_oversized_kernel_reaches_the_ceiling_guard(
    direction, kernel, hits
) -> None:
    """This facade passes the kernel straight through, so the core refuses it.

    The separation fields are a versioned config rather than keyword-validated
    arguments, so unlike ``hpss`` the refusal here comes back from the filter
    itself and names the ceiling.
    """
    with pytest.raises(SonareError) as raised:
        extract_percussive_events(hits, SR, **{f"hpss_kernel_{direction}": kernel})
    _assert_names_ceiling(raised.value, direction, kernel)


@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
@pytest.mark.parametrize(("passed", "truncates_to"), WRAPPING)
def test_percussive_events_refuse_a_wrapping_kernel_by_name(
    direction, passed, truncates_to, hits
) -> None:
    """The separation fields are range-checked before the C struct truncates them."""
    del truncates_to
    arg_name = f"hpss_kernel_{direction}"
    with pytest.raises(SonareValueError) as raised:
        extract_percussive_events(hits, SR, **{arg_name: passed})
    _assert_parameter_refusal(raised.value)
    assert f"{arg_name} must fit in a signed 32-bit integer" in str(raised.value)
    assert "exceeds the maximum" not in str(raised.value)


@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
@pytest.mark.parametrize(("passed", "truncates_to"), WRAPPING)
def test_percussive_events_never_separate_on_the_truncated_kernel(
    direction, passed, truncates_to, hits
) -> None:
    """The worst path on this facade is a success, not a wrong refusal.

    0 is this config's spelling of "use the default", so a kernel that truncated
    to it returned the default run's events unchanged -- byte for byte the right
    answer to a question the caller never asked.
    """
    arg_name = f"hpss_kernel_{direction}"
    outcome = _event_outcome(hits, **{arg_name: passed})
    assert outcome == REFUSED
    assert outcome != _event_outcome(hits)
    if truncates_to >= 0 and (truncates_to == 0 or truncates_to % 2 == 1):
        assert outcome != _event_outcome(hits, **{arg_name: truncates_to})


@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
def test_percussive_events_even_kernel_trips_the_core_parity_guard(direction, hits) -> None:
    """Nothing pre-validates parity here either, so the core's own guard answers.

    It shares the ceiling guard's error code and carries the generic message for
    it, which is what tells the two refusals apart from the caller's side.
    """
    with pytest.raises(SonareError) as raised:
        extract_percussive_events(hits, SR, **{f"hpss_kernel_{direction}": INT_MAX_MINUS_ONE})
    _assert_parameter_refusal(raised.value)
    assert "exceeds the maximum" not in str(raised.value)


@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
def test_percussive_events_treat_zero_as_the_default_rather_than_refusing(direction, hits) -> None:
    """Zero selects the default on this facade, where ``hpss`` would refuse it.

    Every field of the versioned separation config takes its default at 0, so an
    explicit 0 is indistinguishable from an omitted keyword -- it is not the
    odd-and-positive refusal the same value draws from ``hpss``. Asserted
    against the default run rather than just "it did not raise", so a 0 that
    quietly selected something else would still fail.
    """
    default = extract_percussive_events(hits, SR)
    zeroed = extract_percussive_events(hits, SR, **{f"hpss_kernel_{direction}": 0})
    assert len(default) > 0
    assert [(event.onset_sample, event.offset_sample) for event in zeroed] == [
        (event.onset_sample, event.offset_sample) for event in default
    ]


@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
def test_percussive_events_refuse_a_negative_kernel(direction, hits) -> None:
    """Negative is the one sign the config resolver rejects before the core sees it."""
    with pytest.raises(SonareError) as raised:
        extract_percussive_events(hits, SR, **{f"hpss_kernel_{direction}": -1})
    _assert_parameter_refusal(raised.value)


@pytest.mark.parametrize("direction", ["harmonic", "percussive"])
def test_percussive_events_accept_the_largest_legal_kernel(direction, hits) -> None:
    """The positive control on this facade: the ceiling lets its own bound through."""
    events = extract_percussive_events(hits, SR, **{f"hpss_kernel_{direction}": LARGEST_LEGAL})
    assert len(events) > 0
