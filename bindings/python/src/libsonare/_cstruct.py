"""The C-struct base that refuses a numeric field a C type would silently change."""

from __future__ import annotations

import ctypes
import operator
from typing import Any, ClassVar, SupportsIndex

# ctypes type codes for the integer widths, taken from the field's own type so a
# platform alias (``c_int32 is c_int``, ``c_size_t is c_ulong``) needs no table.
# ``c_bool`` ('?') and the pointer types carry no range to leave.
_INTEGER_CODES = frozenset("bBhHiIlLqQ")

# The 32-bit float code, read off the field the same way. It is the only float
# code listed, because a Python float is an IEEE double: 'd' converts nothing,
# so a ``c_double`` field has no fold to refuse.
_FLOAT32_CODE = "f"


def _bounds_for(ctype: Any) -> tuple[int, int] | None:
    """The inclusive range ``ctype`` represents, or None if it is not an integer."""
    code = getattr(ctype, "_type_", None)
    if not isinstance(code, str) or code not in _INTEGER_CODES:
        return None
    bits = ctypes.sizeof(ctype) * 8
    if ctype(-1).value == -1:
        return -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    return 0, (1 << bits) - 1


def _is_float32(ctype: Any) -> bool:
    """Whether ``ctype`` is the C ``float`` a Python double narrows onto."""
    # An array type's ``_type_`` is its element class rather than a code string,
    # which is what keeps ``c_float * N`` out of the scalar set.
    return getattr(ctype, "_type_", None) == _FLOAT32_CODE


class CStruct(ctypes.Structure):
    """A ``ctypes.Structure`` whose numeric fields refuse a value the C type folds.

    ctypes applies the C conversion on assignment, so a field declared ``int32_t``
    takes ``2**32 + 2`` as 2 and one declared ``size_t`` takes ``-1`` as the
    largest representable size. A wrapped value is always inside the field's own
    type, so nothing downstream — not the C ABI's validators, not the core — can
    tell it from a setting the caller chose; and where 0 means "keep the default",
    which it does throughout the versioned configs here, a wrapped value is
    indistinguishable from an omitted one.

    A ``float`` field folds the same way, by saturating rather than wrapping:
    ``c_float`` halves the exponent range of the IEEE double a Python float is,
    so ``1e40`` arrives as ``inf``. That is the same defect — a quantity becoming
    a value the field reads as something other than a quantity — and it is worse
    on the fields documented to read a non-finite as "unspecified", where the
    overflowed request cannot be told from a deliberate one. Precision loss is
    not this defect: ``0.1`` rounded to the nearest ``float`` is still the
    quantity the caller asked for, and is accepted.

    Deriving the bound from the declared field type rather than from a per-field
    table is what makes this reach a field added later: a new ``_fields_`` entry
    is covered the moment it is declared. The interception covers direct
    assignment, an element of a ``Structure`` array, and both the keyword and the
    positional constructor.

    A ``bool`` is refused for the same reason it is in the argument readers: it
    is an ``int`` subclass and can never fail a range check, so a field that
    genuinely carries a flag spells the 0/1 out where it is marshalled.

    Left uncovered, each because there is no fold to refuse or nowhere to
    intercept it:

    - ``c_double`` fields: a Python float already is the field's own type.
    - ``c_bool`` and the pointer types: no range to leave.
    - An array-typed field (``c_float * N``): writing an element reaches the
      array object without passing through this ``__setattr__``, so elements
      are unguarded. Assigning a whole array is passed through unchanged.
    """

    __slots__ = ()

    # Read through ``cls.__dict__`` rather than attribute lookup, so a subclass
    # builds its own table instead of inheriting its parent's.
    _integer_bounds_cache: ClassVar[dict[str, tuple[int, int]]]
    _float_fields_cache: ClassVar[frozenset[str]]

    @classmethod
    def _integer_bounds(cls) -> dict[str, tuple[int, int]]:
        """Field name to inclusive range, built once per struct on first write."""
        bounds = cls.__dict__.get("_integer_bounds_cache")
        if bounds is None:
            bounds = {}
            for field in getattr(cls, "_fields_", ()):
                span = _bounds_for(field[1])
                if span is not None:
                    bounds[field[0]] = span
            cls._integer_bounds_cache = bounds
        return bounds

    @classmethod
    def _float_fields(cls) -> frozenset[str]:
        """Names of the scalar ``c_float`` fields, built once per struct as above."""
        names = cls.__dict__.get("_float_fields_cache")
        if names is None:
            names = frozenset(
                field[0] for field in getattr(cls, "_fields_", ()) if _is_float32(field[1])
            )
            cls._float_fields_cache = names
        return names

    def __setattr__(self, name: str, value: Any) -> None:
        # An already-built ctypes scalar went through the argument readers, which
        # apply the same range check; re-reading it here would only reject the
        # spelling.
        if not isinstance(value, ctypes._SimpleCData):
            cls = type(self)
            span = cls._integer_bounds().get(name)
            if span is not None:
                value = _narrow_field(value, name, *span)
            elif name in cls._float_fields():
                value = _narrow_float_field(value, name)
        ctypes.Structure.__setattr__(self, name, value)


def _narrow_field(value: object, name: str, low: int, high: int) -> int:
    """Return ``value`` as a plain ``int``, or refuse what the field would change."""
    if not isinstance(value, bool) and isinstance(value, SupportsIndex):
        try:
            integer = operator.index(value)
        except TypeError:
            pass
        else:
            if low <= integer <= high:
                return integer
    # Imported on the failure path only: _runtime reaches these structs through
    # _ffi, so a module-level import here would close the cycle.
    from ._runtime import SonareValueError, _narrowing_error

    raise SonareValueError(_narrowing_error(name, low, high))


def _narrow_float_field(value: object, name: str) -> float:
    """Return ``value`` as a plain ``float``, or refuse what the field would fold."""
    # Imported on call as _narrow_field is, but on every write: this half needs
    # the shared reader to accept as well as to refuse.
    from ._runtime import _narrow_float

    return _narrow_float(value, name)
