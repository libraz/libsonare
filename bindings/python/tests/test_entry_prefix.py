"""Every refusal names the public entry the caller invoked, through one mechanism.

A public callable carries ``@_entry("<name>")``; a binding check raises its
bare text through ``_refuse`` / ``_refuse_type`` / ``_invalid_state``, and the
outermost entry prefixes its name on the way out. The source scan below holds
the package to that single route; the roster holds every public callable to
the decorator and to its public name; the behaviour rows pin the contract.
"""

from __future__ import annotations

import ast
import inspect
import re
import traceback
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

import numpy as np
import pytest

import libsonare
from libsonare import Audio, SonareError, SonareValueError
from libsonare._errors import _BARE_TEXT, _ENTRY_NAME
from libsonare._runtime import _coded_error

_PACKAGE = Path(libsonare.__file__).parent

# The command-line front-end owns its own messages.
_SKIPPED_FILES = re.compile(r"^(_cli|_ffi|_cstruct|__main__|cli\.py$)")

# Where the C ABI's own text becomes an exception; the core already names its entry.
_C_TRANSLATION = {("_runtime.py", "_coded_error"), ("_conversions.py", "_refused_argument")}

# The builders themselves.
_FORMATTERS = {"_refuse", "_refuse_type", "_invalid_state"}

# A string that joins the core's detail onto a fallback, not a name onto a refusal.
_PREFIX_EXEMPT = {("_runtime.py", "_last_error")}

# Public callables reached from more than one public class, which refuse nothing.
_ROSTER_EXEMPT = {"_HandleOwner.close", "SonareError.__init__", "SonareValueError.__init__"}

_PREFIX = re.compile(r"^(\{[^}]+\}|[A-Za-z_][\w.]*): ")


def _sources() -> Iterator[tuple[str, ast.Module]]:
    for path in sorted(_PACKAGE.glob("*.py")):
        if not _SKIPPED_FILES.match(path.name):
            yield path.name, ast.parse(path.read_text(encoding="utf-8"))


def _enclosing(tree: ast.Module) -> dict[int, str]:
    owner: dict[int, str] = {}

    def visit(node: ast.AST, name: str) -> None:
        for child in ast.iter_child_nodes(node):
            inner = child.name if isinstance(child, ast.FunctionDef) else name
            owner[id(child)] = inner
            visit(child, inner)

    visit(tree, "")
    return owner


def _string_head(node: ast.AST) -> str | None:
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.JoinedStr) and node.values:
        head = ""
        for part in node.values[:2]:
            if isinstance(part, ast.Constant):
                head += str(part.value)
            elif isinstance(part, ast.FormattedValue):
                head += "{" + ast.unparse(part.value) + "}"
        return head
    return None


def test_every_refusal_goes_through_the_formatter_and_carries_no_hand_prefix() -> None:
    direct: list[str] = []
    prefixed: list[str] = []
    formatter_sites = 0
    for file_name, tree in _sources():
        owner = _enclosing(tree)
        statements = {
            id(node.value)
            for node in ast.walk(tree)
            if isinstance(node, ast.Expr) and isinstance(node.value, (ast.Constant, ast.JoinedStr))
        }
        nested = {
            id(part)
            for node in ast.walk(tree)
            if isinstance(node, ast.JoinedStr)
            for part in node.values
        }
        for node in ast.walk(tree):
            where = f"{file_name}:{getattr(node, 'lineno', 0)}"
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
                if node.func.id in _FORMATTERS and file_name != "_errors.py":
                    formatter_sites += 1
                if node.func.id in {"SonareValueError", "TypeError"}:
                    if file_name == "_errors.py" and owner[id(node)] in {"_refuse", "_refuse_type"}:
                        continue
                    if (file_name, owner[id(node)]) not in _C_TRANSLATION:
                        direct.append(where)
            if id(node) in statements or id(node) in nested:
                continue
            head = _string_head(node)
            if head is None or not _PREFIX.match(head):
                continue
            if file_name == "_errors.py" or (file_name, owner[id(node)]) in _PREFIX_EXEMPT:
                continue
            prefixed.append(f"{where} {head[:40]!r}")
    print(f"formatter sites: {formatter_sites}; direct raises: {len(direct)}")
    print(f"hand-written prefixes: {len(prefixed)}")
    assert formatter_sites > 0
    assert direct == []
    assert prefixed == []


def _roster() -> Iterator[tuple[str, str, Any]]:
    """Yield (expected name, defining qualname, attribute) for each public callable."""
    public = {n for n in libsonare.__all__ if inspect.isclass(getattr(libsonare, n))}
    for name in libsonare.__all__:
        obj = getattr(libsonare, name)
        if inspect.isfunction(obj):
            yield name, inspect.unwrap(obj).__qualname__, obj
            continue
        if not inspect.isclass(obj) or not obj.__module__.startswith("libsonare"):
            continue
        for key in dir(obj):
            if key.startswith("_") and key not in {"__init__", "__post_init__"}:
                continue
            raw = inspect.getattr_static(obj, key)
            if isinstance(raw, property):
                continue
            function = getattr(raw, "__func__", raw)
            if not inspect.isfunction(function):
                continue
            target = inspect.unwrap(function)
            # Generated dataclass methods and stdlib Enum/Protocol machinery are not entries.
            if target.__code__.co_filename == "<string>" or not target.__module__.startswith(
                "libsonare"
            ):
                continue
            owner = target.__qualname__.split(".")[0]
            if owner != name and owner in public:
                continue
            expected = name if key in {"__init__", "__post_init__"} else f"{name}.{key}"
            yield expected, target.__qualname__, function


def test_every_public_callable_names_itself_and_keeps_its_signature() -> None:
    missing: list[str] = []
    misnamed: list[str] = []
    resigned: list[str] = []
    checked = 0
    for expected, qualname, function in _roster():
        if qualname in _ROSTER_EXEMPT:
            continue
        checked += 1
        given = getattr(function, _ENTRY_NAME, None)
        if given is None:
            missing.append(expected)
            continue
        if given != expected:
            misnamed.append(f"{expected} carries {given!r}")
        if inspect.signature(function) != inspect.signature(inspect.unwrap(function)):
            resigned.append(expected)
    print(f"decorated public callables: {checked}")
    assert checked > 500
    assert missing == []
    assert misnamed == []
    assert resigned == []


def _tone() -> np.ndarray:
    return np.zeros(4096, dtype=np.float32)


def test_the_outermost_entry_names_a_nested_refusal() -> None:
    with pytest.raises(SonareValueError) as inner:
        libsonare.analyze_impulse_response(_tone(), 22050, n_octave_bands=-1)
    assert str(inner.value).startswith("analyze_impulse_response: n_octave_bands ")
    with (
        Audio.from_buffer(_tone(), sample_rate=22050) as audio,
        pytest.raises(SonareValueError) as outer,
    ):
        audio.analyze_impulse_response(n_octave_bands=-1)
    text = str(outer.value)
    assert text.startswith("Audio.analyze_impulse_response: n_octave_bands ")
    assert text.count(": n_octave_bands") == 1


def test_python_own_type_error_passes_through_untouched() -> None:
    with pytest.raises(TypeError) as caught:
        libsonare.hpss(_tone(), 22050, bogus=1)  # type: ignore[call-arg]
    assert str(caught.value) == "hpss() got an unexpected keyword argument 'bogus'"
    assert getattr(caught.value, _BARE_TEXT, None) is None


def test_a_refusal_is_reraised_unchained_from_a_library_frame() -> None:
    with pytest.raises(SonareValueError) as caught:
        libsonare.hpss(_tone(), 22050, hop_length=0)
    error = caught.value
    assert str(error) == "hpss: hop_length must be an integer in [1, 2147483647]"
    assert getattr(error, _BARE_TEXT) == "hop_length must be an integer in [1, 2147483647]"
    assert error.__cause__ is None
    frames = traceback.extract_tb(error.__traceback__)
    assert Path(frames[-1].filename).parent == _PACKAGE
    assert any(frame.name == "hpss" for frame in frames)


def test_a_state_refusal_keeps_its_code_ahead_of_the_name() -> None:
    project = libsonare.Project()
    project.close()
    with pytest.raises(SonareError) as caught:
        project.add_track("audio", "a")
    assert str(caught.value) == "[7] Project.add_track: Project is closed"
    assert caught.value.code_name == "InvalidState"


def test_a_method_a_classmethod_and_a_constructor_name_their_entry() -> None:
    entries: dict[str, Callable[[], object]] = {
        "SampleBank.add_sample": lambda: libsonare.SampleBank().add_sample(
            _tone(), source_rate=0.0
        ),
        "Audio.from_buffer": lambda: Audio.from_buffer(_tone(), sample_rate=0),
        "Mixer": lambda: libsonare.Mixer(sample_rate=48000, scene=5),  # type: ignore[arg-type]
    }
    for name, call in entries.items():
        with pytest.raises((SonareValueError, TypeError)) as caught:
            call()
        assert str(caught.value).startswith(f"{name}: "), str(caught.value)


@pytest.mark.parametrize(
    ("native", "bare"),
    [
        ("project_add_clip: clip id is unknown", "clip id is unknown"),
        ("mel_spectrogram_ex: sample_rate is out of range", "sample_rate is out of range"),
        ("sonare_eq_set_band: band index is out of range", "band index is out of range"),
        ("mixer: unknown strip id", "mixer: unknown strip id"),
        ("StftConfig: n_fft must be even", "StftConfig: n_fft must be even"),
        ("not_a_c_entry_at_all: text", "not_a_c_entry_at_all: text"),
    ],
)
def test_a_c_entry_name_in_front_of_the_native_text_is_dropped(native: str, bare: str) -> None:
    # The public entry names itself, so the C entry it reached is not repeated after it.
    exc = _coded_error(int(libsonare.ErrorCode.INVALID_PARAMETER), native)
    assert getattr(exc, _BARE_TEXT) == bare
