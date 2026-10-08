"""The numeric predicates that refuse a value a C type would silently change.

Shared by the ``_to_c_*`` argument readers and by :class:`CStruct`'s field
assignment, so both halves of the binding accept and refuse the same values with
the same wording.
"""

from __future__ import annotations

import math
import operator
from typing import SupportsFloat, SupportsIndex

from ._errors import SonareValueError


def _narrow_int(value: object, name: str, low: int, high: int) -> int:
    """Return ``value`` as a plain ``int``, or refuse what a C type would change.

    The one implementation behind every ``_to_c_*`` conversion and every integer
    struct field: the target range is the only thing that differs, and all refuse the
    same silent value change. A ctypes integer constructor applies the C
    conversion, so ``c_int(2**32)`` is 0 — which every versioned config field
    reads as "keep the default" — ``c_int(2**32 + 1)`` is 1, and
    ``c_size_t(-1)`` is the largest representable size. A wrapped value is always
    inside the target type, so no downstream range check can tell it from a
    request the caller meant.

    A ``bool`` is refused rather than read as 0/1: it is an ``int`` subclass, so
    it can never fail the range check, and the entry points that genuinely take a
    flag spell the conversion out at the call site.

    Args:
        value: Caller-supplied number.
        name: Field or argument name, named in the error message.
        low: Smallest value the target type represents.
        high: Largest value the target type represents.

    Returns:
        The value as a plain ``int``.

    Raises:
        SonareValueError: If ``value`` is not an integer, or does not fit.
    """
    if not isinstance(value, bool) and isinstance(value, SupportsIndex):
        try:
            integer = operator.index(value)
        except TypeError:
            pass
        else:
            if low <= integer <= high:
                return integer
    raise SonareValueError(_narrowing_error(name, low, high))


def _narrowing_error(name: str, low: int, high: int) -> str:
    """Word one refusal for both halves of the family, unsigned reading as such."""
    if low == 0:
        return f"{name} must be a non-negative integer within [0, {high}]"
    return f"{name} must be an integer within [{low}, {high}]"


# Largest finite value a 32-bit float represents. A double above it does not
# overflow on conversion, it saturates to an infinity ctypes hands on as a
# legal value.
_FLOAT32_MAX = 3.4028234663852886e38


def _narrow_float(value: object, name: str) -> float:
    """Return ``value`` as a plain ``float``, or refuse what a C type would change.

    The float half of the family, and the one implementation behind
    :func:`_to_c_float`. A Python float is an IEEE double, so ``c_float`` halves
    the exponent range without raising: ``c_float(1e40)`` is ``inf`` and
    ``c_float(-1e40)`` is ``-inf``. That is saturation rather than the integer
    family's wrap, and the consequence is the one :func:`_narrow_int` describes
    — the caller's quantity arrives as another legal value, and on the fields
    documented to read a non-finite input as "unspecified" it is
    indistinguishable from a deliberate request. A NaN or an infinity passed in
    directly folds onto the same reading, so it is refused here too.

    An ``int`` is accepted: a caller writes ``1`` as readily as ``1.0``. A
    ``bool`` is refused rather than read as 0/1, for the reason
    :func:`_narrow_int` gives.

    Args:
        value: Caller-supplied number.
        name: Field or argument name, named in the error message.

    Returns:
        The value as a plain ``float``.

    Raises:
        SonareValueError: If ``value`` is not a number, or does not fit.
    """
    if not isinstance(value, bool) and isinstance(value, SupportsFloat):
        try:
            number = float(value)
        except (TypeError, ValueError, OverflowError):
            pass
        else:
            if math.isfinite(number) and abs(number) <= _FLOAT32_MAX:
                return number
    raise SonareValueError(_float_narrowing_error(name))


def _float_narrowing_error(name: str) -> str:
    """Word one refusal for the float half, as :func:`_narrowing_error` does for ints."""
    return f"{name} must be a finite number within [-{_FLOAT32_MAX:g}, {_FLOAT32_MAX:g}]"


def _narrow_double(value: object, name: str) -> float:
    """Return ``value`` as a plain ``float``, or refuse what ``c_double`` would change.

    The double half of the family. A Python float IS an IEEE double, so unlike
    :func:`_narrow_float` there is no range to check — ``c_double`` cannot
    saturate a value that already came from one. What it does not refuse is a
    NaN or an infinity, which passes into the core unchanged and then reads as
    whatever that field's unspecified case happens to be. That is the single
    reason this exists, and it is why the error names finiteness rather than a
    range.

    An ``int`` too large for a double raises ``OverflowError`` inside ``float``
    and is refused with the same message; ``bool`` is refused rather than read
    as 0/1, for the reason :func:`_narrow_int` gives.

    Args:
        value: Caller-supplied number.
        name: Field or argument name, named in the error message.

    Returns:
        The value as a plain ``float``.

    Raises:
        SonareValueError: If ``value`` is not a number, or is not finite.
    """
    if not isinstance(value, bool) and isinstance(value, SupportsFloat):
        try:
            number = float(value)
        except (TypeError, ValueError, OverflowError):
            pass
        else:
            if math.isfinite(number):
                return number
    raise SonareValueError(f"{name} must be a finite number")


_INT64_MIN = -(2**63)
_INT64_MAX = 2**63 - 1

# The C ABI spells "not given" as -1 for the optional numeric parameters below.
_C_ABSENT = -1


def _seconds_to_samples(seconds: object, sample_rate: float, name: str) -> int:
    """Convert a time in seconds to the nearest whole sample at ``sample_rate``.

    Halves round up, as ``Math.round`` does in the JS facades, so one request
    names the same sample on every surface.

    Raises:
        SonareValueError: If ``seconds`` is not a finite number, or the result
            does not fit an ``int64``.
    """
    samples = math.floor(_narrow_double(seconds, name) * sample_rate + 0.5)
    if not _INT64_MIN <= samples <= _INT64_MAX:
        raise SonareValueError(f"{name} is out of range at {sample_rate} Hz")
    return samples


def _resolve_sample_bound(
    sample_value: object,
    sec_value: object,
    sample_rate: float,
    sample_name: str,
    sec_name: str,
    *,
    signed: bool = False,
) -> int | None:
    """Resolve one time bound spelled either in samples or in seconds.

    ``None`` on both spellings is absence and returns ``None`` so the caller
    applies its own default (an end bound's default is the end of the signal).
    Giving both is refused by name. A position is non-negative; a displacement
    (``signed=True``, such as a note's time offset) may be negative. A negative
    value is refused, never read as "absent".

    Raises:
        SonareValueError: If both spellings are given, the value is not a valid
            number, or a position is negative.
    """
    if sample_value is not None and sec_value is not None:
        raise SonareValueError(f"give {sample_name} or {sec_name}, not both")
    if sample_value is None and sec_value is None:
        return None
    if sec_value is not None:
        value = _seconds_to_samples(sec_value, sample_rate, sec_name)
        name = sec_name
    else:
        value = _narrow_int(sample_value, sample_name, _INT64_MIN, _INT64_MAX)
        name = sample_name
    if not signed and value < 0:
        raise SonareValueError(f"{name} must be non-negative")
    return value


def _resolve_time_offset(
    sample_value: object,
    sec_value: object,
    sample_rate: float,
    sample_name: str,
    sec_name: str,
) -> int:
    """Resolve a pending time offset from samples or seconds; unset is 0.

    ``time_offset_samples`` is 0 when unset, so only a non-zero sample value
    counts as giving the sample spelling; both spellings given is refused. The
    offset is signed, since a negative one moves the note or hit earlier.
    """
    resolved = _resolve_sample_bound(
        sample_value if sample_value != 0 else None,
        sec_value,
        sample_rate,
        sample_name,
        sec_name,
        signed=True,
    )
    if resolved is not None:
        return resolved
    # Zero is the identity; a zero-valued non-integer still fails here.
    return _narrow_int(sample_value, sample_name, _INT64_MIN, _INT64_MAX)


def _resolve_render_frame(render_frame: object) -> int:
    """Map an optional engine ``render_frame`` onto the C value.

    ``None`` is "apply at the next block head" (the C ABI's ``-1``); a negative
    frame is refused rather than read as the same thing.

    Raises:
        SonareValueError: If ``render_frame`` is not a non-negative integer.
    """
    if render_frame is None:
        return _C_ABSENT
    return _narrow_int(render_frame, "render_frame", 0, _INT64_MAX)


def _resolve_optional_non_negative(value: object, name: str) -> int | float:
    """Map an optional numeric argument onto the C value for "not given".

    ``None`` becomes the C ABI's ``-1``; an explicit negative or non-finite value
    is refused rather than read as the same thing.

    Raises:
        SonareValueError: If ``value`` is not a finite number, or is negative.
    """
    if value is None:
        return _C_ABSENT
    number = _narrow_double(value, name)
    if number < 0:
        raise SonareValueError(f"{name} must be non-negative (use None for the default)")
    return value  # type: ignore[return-value]


def _utf8_arg(value: str, name: str) -> bytes:
    """Encode ``value`` for a NUL-terminated ``char *``, refusing an embedded NUL.

    The C side reads up to the first NUL, so a string containing one would arrive
    silently shortened; every string argument crosses the boundary through here.
    """
    if "\x00" in value:
        raise SonareValueError(f"{name} must not contain NUL")
    return value.encode("utf-8")
