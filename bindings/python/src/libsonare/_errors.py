"""The binding's exception hierarchy and the C-ABI error codes it carries."""

from __future__ import annotations

from enum import IntEnum


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
    UNKNOWN = 99


class SonareError(RuntimeError):
    """Exception raised for non-OK Sonare C API return codes."""

    def __init__(self, code: int, message: str) -> None:
        self.code = int(code)
        super().__init__(f"[{self.code}] {message}")

    @property
    def code_name(self) -> str:
        """Canonical cross-binding name of :attr:`code`."""
        names = {
            ErrorCode.OK: "Ok",
            ErrorCode.FILE_NOT_FOUND: "FileNotFound",
            ErrorCode.INVALID_FORMAT: "InvalidFormat",
            ErrorCode.DECODE_FAILED: "DecodeFailed",
            ErrorCode.INVALID_PARAMETER: "InvalidParameter",
            ErrorCode.OUT_OF_MEMORY: "OutOfMemory",
            ErrorCode.NOT_SUPPORTED: "NotSupported",
            ErrorCode.INVALID_STATE: "InvalidState",
            ErrorCode.CANCELLED: "Cancelled",
            ErrorCode.ENCODE_FAILED: "EncodeFailed",
            ErrorCode.UNKNOWN: "Unknown",
        }
        try:
            return names[ErrorCode(self.code)]
        except ValueError:
            return names[ErrorCode.UNKNOWN]


class SonareValueError(SonareError, ValueError):
    """Exception raised when the binding rejects a caller-supplied argument.

    Deriving from both :class:`SonareError` and :class:`ValueError` is the
    compatibility contract, not an implementation detail: every
    argument-validation failure the binding raises must be caught by
    ``except ValueError`` (the plain type argument validation has always used)
    and by ``except SonareError`` (the binding's own error hierarchy), so
    neither style of caller needs to know which one a given entry point picks.

    Unlike :class:`SonareError`, the message is the plain validation text with
    no ``[code]`` prefix, since the failure never reached the C ABI.
    :attr:`code` defaults to :attr:`ErrorCode.INVALID_PARAMETER` so error-class
    and exit-code mapping treat it exactly like the C-ABI rejection it stands
    in for.
    """

    def __init__(self, message: str, code: int = int(ErrorCode.INVALID_PARAMETER)) -> None:
        self.code = int(code)
        ValueError.__init__(self, message)
