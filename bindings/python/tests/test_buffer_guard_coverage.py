"""Rule-derived coverage check for the sample-buffer preflight guards.

`_guard_buffer` (and the older in-body `_validate_samples` calls) exist so that
an empty or non-finite buffer is rejected by the facade, naming the function the
caller invoked and the offending argument, instead of surfacing the C ABI's
generic ``[4] Invalid parameter``. Applying that guard is a per-function edit,
which makes the set of guarded entry points a hand-maintained list — and a
hand-maintained list drifts the moment a new buffer-taking function is added.

These tests derive the set instead: every public callable whose first parameter
is a float-sequence named ``samples`` / ``left`` / ``data`` must reject empty and
non-finite input with `SonareValueError`. A new entry point of that shape fails
here until it is guarded or explicitly exempted.
"""

from __future__ import annotations

import inspect
from typing import Any

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

# The buffer-argument spellings this repo uses for a facade's leading audio /
# matrix input. `_runtime._validate_samples` reports whichever name applies.
_BUFFER_ARG_NAMES = ("samples", "left", "data")

# Annotation fragments that mark a parameter as a buffer rather than a scalar.
# `samples_to_frames(samples: int, ...)` reuses the name for a sample *count*,
# so the name alone cannot decide.
_BUFFER_ANNOTATIONS = ("Sequence[float]", "Sequence[int]", "np.ndarray", "ndarray")

# Entry points whose contract is deliberately empty-in / empty-out: they are
# element-wise conversions with nothing to reject, and the C ABI already rejects
# their non-finite input. Any other function of this shape must preflight.
#
# Kept as an exemption rather than an inclusion list on purpose — the polarity
# matters. A new unguarded function fails the test; a stale exemption fails
# `test_every_exemption_is_live` below.
_EMPTY_IN_EMPTY_OUT = frozenset(
    {
        "preemphasis",
        "deemphasis",
        "frame_signal",
    }
)

# Values for parameters a discovered function requires before it can be called
# at all. Anything not listed fails the test loudly rather than skipping the
# function, so a new required parameter cannot quietly drop it from coverage.
_SCALAR_ARGS: dict[str, Any] = {
    "sample_rate": 22050,
    "channels": 1,
    "src_sr": 22050,
    "target_sr": 44100,
    "frame_length": 2048,
    "hop_length": 512,
    "target_size": 4,
    "target_midi": 69.0,
    "length_m": 5.0,
    "width_m": 4.0,
    "height_m": 3.0,
    "k": 1,
    "n_chroma": 1,
    "n_frames": 1,
    "n_bins": 1,
}

# Parameters that are themselves buffers, and take the same probe input.
_BUFFER_ARGS = ("right", "y", "source", "reference", "f0_hz")

# Parameters that accept an (empty) sequence of side inputs.
_SEQUENCE_ARGS = ("intervals", "boundaries", "ops", "voiced", "platforms", "factors")


def _is_buffer_parameter(parameter: inspect.Parameter) -> bool:
    if parameter.name not in _BUFFER_ARG_NAMES:
        return False
    annotation = str(parameter.annotation)
    return any(fragment in annotation for fragment in _BUFFER_ANNOTATIONS)


def _buffer_entry_points() -> dict[str, inspect.Signature]:
    """Every exported callable whose leading parameter is a sample buffer."""
    found: dict[str, inspect.Signature] = {}
    # `__init__.pyi` does not re-declare `__all__`, so reach for it dynamically;
    # a missing one collapses the set and trips `test_buffer_entry_point_floor`.
    exported: tuple[str, ...] = tuple(getattr(libsonare, "__all__", ()))
    for name in exported:
        obj = getattr(libsonare, name, None)
        if obj is None or inspect.isclass(obj) or not callable(obj):
            continue
        try:
            signature = inspect.signature(obj)
        except (TypeError, ValueError):  # pragma: no cover - C builtins
            continue
        parameters = list(signature.parameters.values())
        if parameters and _is_buffer_parameter(parameters[0]):
            found[name] = signature
    return found


_ENTRY_POINTS = _buffer_entry_points()
_GUARDED = sorted(set(_ENTRY_POINTS) - _EMPTY_IN_EMPTY_OUT)


def _call_arguments(name: str, signature: inspect.Signature, buffer: np.ndarray) -> list[Any]:
    """Positional arguments that reach ``name``'s body with ``buffer`` as input."""
    args: list[Any] = []
    for index, parameter in enumerate(signature.parameters.values()):
        if parameter.kind in (
            parameter.KEYWORD_ONLY,
            parameter.VAR_POSITIONAL,
            parameter.VAR_KEYWORD,
        ):
            break
        if index == 0:
            args.append(buffer)
            continue
        if parameter.default is not parameter.empty:
            break
        if parameter.name in _BUFFER_ARGS:
            args.append(buffer)
        elif parameter.name in _SEQUENCE_ARGS:
            args.append([])
        elif parameter.name in _SCALAR_ARGS:
            args.append(_SCALAR_ARGS[parameter.name])
        elif parameter.name in ("rows", "cols"):
            # `_segment_input` needs positive dimensions before it inspects the
            # matrix, so keep the product reachable for a non-empty probe.
            args.append(1 if parameter.name == "rows" else max(len(buffer), 1))
        elif parameter.name == "key_root":
            args.append(libsonare.PitchClass.C)
        else:
            pytest.fail(
                f"{name}: no probe value for required parameter {parameter.name!r}; "
                "add one to this module so the guard stays covered"
            )
    return args


def test_buffer_entry_point_floor() -> None:
    """Guard the derived set itself: a rule that collapses to nothing passes."""
    # A floor, not an exact count — new buffer entry points are expected. It
    # only has to be high enough that an import or annotation regression which
    # empties the discovery cannot slip through as a green run.
    assert len(_ENTRY_POINTS) >= 110, sorted(_ENTRY_POINTS)
    assert len(_GUARDED) >= 110, _GUARDED


def test_every_exemption_is_live() -> None:
    """A renamed exemption must not silently hide a real coverage gap."""
    stale = sorted(_EMPTY_IN_EMPTY_OUT - set(_ENTRY_POINTS))
    assert stale == [], f"exempted functions no longer exist as buffer entry points: {stale}"


@pytest.mark.parametrize("name", _GUARDED)
def test_empty_buffer_is_rejected(name: str) -> None:
    signature = _ENTRY_POINTS[name]
    args = _call_arguments(name, signature, np.zeros(0, dtype=np.float32))
    with pytest.raises(SonareValueError) as excinfo:
        getattr(libsonare, name)(*args)
    assert name in str(excinfo.value), f"{name}: message does not name the function"


@pytest.mark.parametrize("name", _GUARDED)
def test_non_finite_buffer_is_rejected(name: str) -> None:
    signature = _ENTRY_POINTS[name]
    args = _call_arguments(name, signature, np.full(64, np.nan, dtype=np.float32))
    with pytest.raises(SonareValueError) as excinfo:
        getattr(libsonare, name)(*args)
    assert name in str(excinfo.value), f"{name}: message does not name the function"
