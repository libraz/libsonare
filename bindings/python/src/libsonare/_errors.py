"""The binding's exception hierarchy and the C-ABI error codes it carries."""

from __future__ import annotations

import functools
from collections.abc import Callable
from enum import IntEnum
from typing import Literal, ParamSpec, TypeVar, cast

_Handle = TypeVar("_Handle")
_P = ParamSpec("_P")
_R = TypeVar("_R")


class ErrorCode(IntEnum):
    """Public C-ABI error codes carried by :class:`SonareError`."""

    OK = 0
    FILE_NOT_FOUND = 1
    INVALID_FORMAT = 2
    DECODE_FAILED = 3
    INVALID_PARAMETER = 4
    OUT_OF_MEMORY = 5
    NOT_SUPPORTED = 6
    INVALID_STATE = 7
    CANCELLED = 8
    ENCODE_FAILED = 9
    ABI_MISMATCH = 10
    STALE_RESULT = 11
    UNKNOWN = 99


ErrorCodeName = Literal[
    "Ok",
    "FileNotFound",
    "InvalidFormat",
    "DecodeFailed",
    "InvalidParameter",
    "OutOfMemory",
    "NotSupported",
    "InvalidState",
    "Cancelled",
    "EncodeFailed",
    "AbiMismatch",
    "StaleResult",
    "Unknown",
]
"""The cross-binding spelling of each :class:`ErrorCode`, shared with the JS ``codeName``."""


def _error_code(code: int) -> ErrorCode:
    try:
        return ErrorCode(int(code))
    except ValueError:
        return ErrorCode.UNKNOWN


class SonareError(RuntimeError):
    """Exception raised for non-OK Sonare C API return codes.

    :attr:`code` is an :class:`ErrorCode` member; a code this binding does not
    know reads as :attr:`ErrorCode.UNKNOWN`.
    """

    code: ErrorCode
    """The failure's :class:`ErrorCode` member."""

    def __init__(self, code: int, message: str) -> None:
        self.code = _error_code(code)
        super().__init__(f"[{int(code)}] {message}")

    @property
    def code_name(self) -> ErrorCodeName:
        """The name of :attr:`code` shared by every binding, such as ``"InvalidParameter"``.

        It is the PascalCase form of the :class:`ErrorCode` member name and equals
        the JS ``codeName``; :attr:`code` is the enum member itself.
        """
        return cast(ErrorCodeName, "".join(w.capitalize() for w in self.code.name.split("_")))


class SonareValueError(SonareError, ValueError):
    """Exception raised when the binding rejects a caller-supplied argument.

    Deriving from both :class:`SonareError` and :class:`ValueError` is the
    compatibility contract, not an implementation detail: every
    argument-validation failure the binding raises must be caught by
    ``except ValueError`` (the plain type argument validation has always used)
    and by ``except SonareError`` (the binding's own error hierarchy), so
    neither style of caller needs to know which one a given entry point picks.

    Every refused caller argument is this class, whether the binding caught it
    or the C ABI answered ``INVALID_PARAMETER``; the refusal contract is by
    cause, not by layer. Unlike :class:`SonareError`, the message is the plain
    validation text with no ``[code]`` prefix. :attr:`code` defaults to
    :attr:`ErrorCode.INVALID_PARAMETER` so error-class and exit-code mapping
    treat both origins identically.
    """

    def __init__(self, message: str, code: int = ErrorCode.INVALID_PARAMETER) -> None:
        self.code = _error_code(code)
        ValueError.__init__(self, message)


# The refusal text a binding check raised, before any entry named itself on it.
_BARE_TEXT = "_sonare_bare_text"
# The public name an entry wrapper carries, read by the roster test.
_ENTRY_NAME = "__sonare_entry__"


def _refuse(message: str) -> SonareValueError:
    """Build the refusal of an out-of-domain argument; the public entry prefixes its name."""
    exc = SonareValueError(message)
    setattr(exc, _BARE_TEXT, message)
    return exc


def _refuse_type(message: str) -> TypeError:
    """Build the refusal of a wrong-typed argument; the public entry prefixes its name."""
    exc = TypeError(message)
    setattr(exc, _BARE_TEXT, message)
    return exc


def _entry(name: str) -> Callable[[Callable[_P, _R]], Callable[_P, _R]]:
    """Name a public callable on every refusal raised beneath it.

    Only exceptions built by :func:`_refuse`, :func:`_refuse_type` and
    :func:`_invalid_state` are touched; their text becomes ``"<name>: <bare text>"``
    (after the ``[code]`` of a coded error), rebuilt from the bare text so the
    outermost entry the caller invoked wins over any nested one. The same object
    is re-raised, and the success path does no extra work.
    """

    def decorate(fn: Callable[_P, _R]) -> Callable[_P, _R]:
        @functools.wraps(fn)
        def entry(*args: _P.args, **kwargs: _P.kwargs) -> _R:
            try:
                return fn(*args, **kwargs)
            except (SonareError, TypeError) as exc:
                bare = getattr(exc, _BARE_TEXT, None)
                if bare is not None:
                    # Native text that already opens with this entry keeps a single name.
                    text = bare if bare.startswith(f"{name}: ") else f"{name}: {bare}"
                    if isinstance(exc, SonareError) and not isinstance(exc, SonareValueError):
                        text = f"[{int(exc.code)}] {text}"
                    exc.args = (text,)
                raise

        setattr(entry, _ENTRY_NAME, name)
        return entry

    return decorate


def _unsupported_effect_symbol(symbol: str) -> SonareError:
    return _not_supported(f"libsonare does not export {symbol}; install a matching native library")


def _not_supported(message: str) -> SonareError:
    """Build the error for a capability the loaded native library lacks."""
    return SonareError(int(ErrorCode.NOT_SUPPORTED), message)


def _invalid_state(message: str) -> SonareError:
    """Build the error for a call the object's current state refuses (e.g. closed)."""
    exc = SonareError(int(ErrorCode.INVALID_STATE), message)
    setattr(exc, _BARE_TEXT, message)
    return exc


def _require_open(handle: _Handle | None, owner: str) -> _Handle:
    """Return ``handle`` if the object is open, else raise the coded InvalidState error.

    The one closed-handle guard: a closed object holds ``None``, a NULL ``c_void_p`` or ``0``.
    """
    if not handle:
        raise _invalid_state(f"{owner} is closed")
    return handle


def _unknown_error(message: str) -> SonareError:
    """Build the error for a native result that fails the binding's sanity checks."""
    return SonareError(int(ErrorCode.UNKNOWN), message)
