"""The binding's exception hierarchy and the C-ABI error codes it carries."""

from __future__ import annotations

from enum import IntEnum
from typing import Literal, TypeVar, cast

_Handle = TypeVar("_Handle")


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


def _unsupported_effect_symbol(symbol: str) -> SonareError:
    return _not_supported(f"libsonare does not export {symbol}; install a matching native library")


def _not_supported(message: str) -> SonareError:
    """Build the error for a capability the loaded native library lacks."""
    return SonareError(int(ErrorCode.NOT_SUPPORTED), message)


def _invalid_state(message: str) -> SonareError:
    """Build the error for a call the object's current state refuses (e.g. closed)."""
    return SonareError(int(ErrorCode.INVALID_STATE), message)


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
