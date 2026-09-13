"""The C-struct base that refuses an integer field a C type would silently change."""

from __future__ import annotations

import ctypes
import operator
from typing import Any

# ctypes type codes for the integer widths, taken from the field's own type so a
# platform alias (``c_int32 is c_int``, ``c_size_t is c_ulong``) needs no table.
# ``c_bool`` ('?'), the floats and the pointer types are deliberately absent:
# only an integer assignment can wrap.
_INTEGER_CODES = frozenset("bBhHiIlLqQ")


def _bounds_for(ctype: Any) -> tuple[int, int] | None:
    """The inclusive range ``ctype`` represents, or None if it is not an integer."""
    code = getattr(ctype, "_type_", None)
    if not isinstance(code, str) or code not in _INTEGER_CODES:
        return None
    bits = ctypes.sizeof(ctype) * 8
    if ctype(-1).value == -1:
        return -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    return 0, (1 << bits) - 1


class CStruct(ctypes.Structure):
    """A ``ctypes.Structure`` whose integer fields refuse a value that would wrap.

    ctypes applies the C conversion on assignment, so a field declared ``int32_t``
    takes ``2**32 + 2`` as 2 and one declared ``size_t`` takes ``-1`` as the
    largest representable size. A wrapped value is always inside the field's own
    type, so nothing downstream — not the C ABI's validators, not the core — can
    tell it from a setting the caller chose; and where 0 means "keep the default",
    which it does throughout the versioned configs here, a wrapped value is
    indistinguishable from an omitted one.

    Deriving the bound from the declared field type rather than from a per-field
    table is what makes this reach a field added later: a new ``_fields_`` entry
    is covered the moment it is declared. The interception covers direct
    assignment, an element of a ``Structure`` array, and both the keyword and the
    positional constructor.

    A ``bool`` is refused for the same reason it is in the argument readers: it
    is an ``int`` subclass and can never fail a range check, so a field that
    genuinely carries a flag spells the 0/1 out where it is marshalled.
    """

    __slots__ = ()

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

    def __setattr__(self, name: str, value: Any) -> None:
        span = type(self)._integer_bounds().get(name)
        # An already-built ctypes scalar went through the argument readers, which
        # apply the same range check; re-reading it here would only reject the
        # spelling.
        if span is not None and not isinstance(value, ctypes._SimpleCData):
            value = _narrow_field(value, name, *span)
        ctypes.Structure.__setattr__(self, name, value)


def _narrow_field(value: object, name: str, low: int, high: int) -> int:
    """Return ``value`` as a plain ``int``, or refuse what the field would change."""
    if not isinstance(value, bool):
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
