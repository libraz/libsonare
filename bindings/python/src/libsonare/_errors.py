"""The binding's exception hierarchy and the C-ABI error codes it carries."""

from __future__ import annotations

import contextlib
import functools
import re
from collections.abc import Callable, Iterator
from enum import IntEnum
from typing import Literal, ParamSpec, TypeVar, cast

from ._integer_domains import DOMAINS as _INTEGER_DOMAINS

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

ErrorReason = Literal[
    "malformedJson",
    "unsupportedSchemaVersion",
    "notCompiledIn",
    "notPersistable",
    "unresolvedSource",
    "unresolvedSoundFont",
    "deviceUnavailable",
    "deviceFormatRefused",
    "engineDeviceDriven",
    "releasedHandle",
]
"""Machine-readable cause of a coded error."""


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
    entry: str | None
    """The public entry the caller invoked."""
    reason: ErrorReason | None
    """Machine-readable cause of this error, when available."""
    _cli_error_message: str

    def __init__(self, code: int, message: str) -> None:
        self.code = _error_code(code)
        self.entry = None
        self.reason = None
        super().__init__(f"[{int(code)}] {message}")

    @property
    def code_name(self) -> ErrorCodeName:
        """The name of :attr:`code` shared by every binding, such as ``"InvalidParameter"``.

        It is the PascalCase form of the :class:`ErrorCode` member name and equals
        the JS ``codeName``; :attr:`code` is the enum member itself.
        """
        return cast(ErrorCodeName, "".join(w.capitalize() for w in self.code.name.split("_")))


class SonareValueError(SonareError, ValueError):
    """Exception for an out-of-domain value or an unknown name.

    This exception derives from :class:`SonareError` and :class:`ValueError`.
    A wrong type or shape caught by the binding raises :class:`TypeError`
    through :func:`_refuse_type`. The binding translates a C-ABI
    ``INVALID_PARAMETER`` into this class.

    The message contains the validation text with the public entry name
    when available. :attr:`code` defaults to :attr:`ErrorCode.INVALID_PARAMETER`.
    """

    def __init__(self, message: str, code: int = ErrorCode.INVALID_PARAMETER) -> None:
        self.code = _error_code(code)
        self.entry = None
        self.reason = None
        ValueError.__init__(self, message)


# The refusal text a binding check raised, before any entry named itself on it.
_BARE_TEXT = "_sonare_bare_text"
# The public name an entry wrapper carries, read by the roster test.
_ENTRY_NAME = "__sonare_entry__"
# The keyword an integer refusal names, so the entry can state that keyword's core domain.
_INTEGER_SUBJECT = "_sonare_integer_subject"


def _refuse(message: str) -> SonareValueError:
    """Build the refusal of an out-of-domain argument; the public entry prefixes its name."""
    exc = SonareValueError(message)
    setattr(exc, _BARE_TEXT, message)
    return exc


def _refuse_integer(name: str, message: str) -> SonareValueError:
    """Build the refusal of a number that is not an integer of ``name``'s domain.

    ``message`` is the reader's own text; an entry whose keyword ``name`` has a core
    domain replaces it with that domain's text.
    """
    exc = _refuse(message)
    setattr(exc, _INTEGER_SUBJECT, name)
    return exc


# The core's wording of each kind of integer domain, by the row's ``stricter`` tag.
_KINDS = {
    None: "an integer",
    "power_of_two": "a power of two",
    "even": "an even integer",
    "odd": "an odd integer",
}


def _integer_domain_text(entry: str, exc: BaseException) -> None:
    """Restate an integer refusal raised beneath ``entry`` in the keyword's core domain."""
    name = getattr(exc, _INTEGER_SUBJECT, None)
    if name is None:
        return
    domain = _INTEGER_DOMAINS.get((entry, re.sub(r"\[\d+\]", "[]", name)))
    if domain is None:
        return
    lo, hi, stricter = domain
    bare = (
        f"{name} must be an integer of at least {lo}"
        if hi is None
        else f"{name} must be {_KINDS[stricter]} in [{lo}, {hi}]"
    )
    setattr(exc, _BARE_TEXT, bare)
    delattr(exc, _INTEGER_SUBJECT)


def _refuse_type(message: str) -> TypeError:
    """Build the refusal of a wrong-typed argument; the public entry prefixes its name."""
    exc = TypeError(message)
    setattr(exc, _BARE_TEXT, message)
    return exc


@contextlib.contextmanager
def _refusals_under(root: str) -> Iterator[None]:
    """Name a refusal a nested value's own reader raised at ``root``, the caller's path to it.

    Only refusals built by :func:`_refuse`, :func:`_refuse_type` and :func:`_refuse_integer` are
    touched. A reader's ``synth patch`` label, its name for a value read with no path, gives way to
    the path.
    """
    try:
        yield
    except (SonareError, TypeError) as exc:
        bare = getattr(exc, _BARE_TEXT, None)
        if bare is not None:
            rooted = f"{root}.{bare.removeprefix('synth patch ')}"
            setattr(exc, _BARE_TEXT, rooted)
            exc.args = (rooted,)
            name = getattr(exc, _INTEGER_SUBJECT, None)
            if name is not None:
                setattr(exc, _INTEGER_SUBJECT, f"{root}.{name}")
        raise


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
                if isinstance(exc, SonareError):
                    exc.entry = name
                _integer_domain_text(name, exc)
                bare = getattr(exc, _BARE_TEXT, None)
                if bare is not None:
                    text = f"{name}: {bare}"
                    if isinstance(exc, SonareError) and not isinstance(exc, SonareValueError):
                        text = f"[{int(exc.code)}] {text}"
                    exc.args = (text,)
                raise

        setattr(entry, _ENTRY_NAME, name)
        return entry

    return decorate


def _not_supported(message: str) -> SonareError:
    """Build the error for a capability the loaded native library lacks."""
    return SonareError(int(ErrorCode.NOT_SUPPORTED), message)


def _invalid_state(message: str, *, reason: ErrorReason | None = None) -> SonareError:
    """Build the error for a call the object's current state refuses (e.g. closed)."""
    exc = SonareError(int(ErrorCode.INVALID_STATE), message)
    exc.reason = reason
    setattr(exc, _BARE_TEXT, message)
    return exc


def _require_open(handle: _Handle | None, owner: str) -> _Handle:
    """Return ``handle`` if the object is open, else raise the coded InvalidState error.

    The one closed-handle guard: a closed object holds ``None``, a NULL ``c_void_p`` or ``0``.
    """
    if not handle:
        raise _invalid_state(f"{owner} is closed", reason="releasedHandle")
    return handle


def _malformed_output(message: str) -> SonareError:
    """Build the error for a value in the library's own output that this binding cannot read.

    The caller passed nothing wrong, so this is the coded ``INVALID_FORMAT`` and never an
    argument refusal: an ordinal or token outside the binding's mirror, or a null that names
    no infinite end, means the mirror has drifted from the loaded library. A drifted value
    reads ``unknown <noun> ordinal: <n>`` or ``unknown <noun> token: <t>``.
    """
    return SonareError(int(ErrorCode.INVALID_FORMAT), message)


def _unknown_error(message: str) -> SonareError:
    """Build the error for a failure whose cause has no code.

    A value of the library's own output that fails the reader's check is not one: that is
    :func:`_malformed_output`.
    """
    return SonareError(int(ErrorCode.UNKNOWN), message)
