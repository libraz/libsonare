"""Rule-derived coverage check for the sample-buffer preflight guards.

`_guard_buffer` (and the older in-body `_validate_samples` calls) exist so that
an empty or non-finite buffer is rejected by the facade, naming the function the
caller invoked and the offending argument, instead of surfacing the C ABI's
generic ``[4] Invalid parameter``. Applying that guard is a per-function edit,
which makes the set of guarded entry points a hand-maintained list — and a
hand-maintained list drifts the moment a new buffer-taking function is added.

These tests derive the set instead: every public callable whose first parameter
is annotated as a one-dimensional numeric buffer must reject empty and
non-finite input with `SonareValueError`. Selection is by annotation shape only
— the guard itself is spelling-independent (``@_guard_buffer("frames")``,
``@_guard_buffer("tempogram_data")``, ``arg_name="frequencies"``), so keying the
set off a list of accepted argument names would reintroduce the hand-maintained
list this module exists to remove. A new entry point of that shape fails here
until it is guarded or explicitly exempted.
"""

from __future__ import annotations

import inspect
from typing import Any

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

# Spellings of a one-dimensional numeric buffer. A parameter qualifies when
# EVERY alternative of its annotation is one of these, which is what separates a
# buffer from a container of buffers: `mix_stereo(strips: Sequence[tuple[
# Sequence[float], Sequence[float]]])` merely *contains* one of these fragments
# and takes a list of strips, not a buffer. Matching the whole alternative
# rather than a substring is also why `samples_to_frames(samples: int, ...)`
# stays out — it is the annotation, never the name, that decides.
_BUFFER_TYPES = frozenset(
    {
        "Sequence[float]",
        "list[float]",
        "Sequence[int]",
        "list[int]",
        "np.ndarray",
        "ndarray",
    }
)

# Entry points for which an empty buffer is a defined result rather than an
# error, so preflighting it would reject input that works today. Two shapes:
# element-wise conversions that return an empty buffer, and generators whose
# output length comes from a parameter rather than from the input. The C ABI
# still rejects their non-finite input. Any other function of this shape must
# preflight.
#
# Each entry was decided by calling the function with an empty buffer and
# keeping whatever it already did, not by assumption; `test_exemptions_accept_
# empty_input` re-checks that claim on every run.
#
# Kept as an exemption rather than an inclusion list on purpose — the polarity
# matters. A new unguarded function fails the test; a stale exemption fails
# `test_every_exemption_is_live` below.
_EMPTY_INPUT_IS_DEFINED = frozenset(
    {
        # Element-wise: empty in, empty out.
        "amplitude_to_db",
        "db_to_amplitude",
        "db_to_power",
        "deemphasis",
        "frame_signal",
        "onset_backtrack",
        "pcen",
        "peak_pick",
        "plp",
        "power_to_db",
        "preemphasis",
        "vector_normalize",
        # Output length comes from a parameter, so an empty input is a
        # legitimate request for silence / padding rather than a rejection.
        "clicks",
        "fix_length",
        "pad_center",
        # Tempogram family: an empty onset envelope yields an empty tempogram
        # plus its (input-independent) tempo axis.
        "cyclic_tempogram",
        "fourier_tempogram",
        "tempogram",
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
    "n_components": 1,
    "frame_rate": 100.0,
    "win": 3,
    "pre_max": 1,
    "post_max": 1,
    "pre_avg": 1,
    "post_avg": 1,
    "delta": 0.1,
    "wait": 1,
}

# A matrix entry point takes its shape alongside the flat row-major buffer, and
# rejects a buffer whose length is not rows * cols before it looks at the
# contents. Probing with one row and as many columns as the buffer has keeps the
# shape consistent for both the empty and the non-finite probe, so what the test
# observes is the guard rather than a shape complaint.
_MATRIX_ROW_ARGS = (
    "rows",
    "n",
    "n_bins",
    "n_chroma",
    "n_features",
    "n_mels",
    "n_mfcc",
    "n_rows",
    "x_rows",
    "y_rows",
)
_MATRIX_COL_ARGS = ("cols", "n_frames", "n_lags", "x_cols", "y_cols")

# Parameters that are themselves buffers, and take the same probe input.
_BUFFER_ARGS = (
    "right",
    "y",
    "source",
    "reference",
    "f0_hz",
    "beat_strengths",
    "voiced_prob",
    "energy",
)

# Parameters that accept an (empty) sequence of side inputs.
_SEQUENCE_ARGS = ("intervals", "boundaries", "ops", "voiced", "platforms", "factors")


def _is_buffer_parameter(parameter: inspect.Parameter) -> bool:
    alternatives = [part.strip() for part in str(parameter.annotation).split("|")]
    return bool(alternatives) and all(part in _BUFFER_TYPES for part in alternatives)


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
_GUARDED = sorted(set(_ENTRY_POINTS) - _EMPTY_INPUT_IS_DEFINED)
_EXEMPT = sorted(_EMPTY_INPUT_IS_DEFINED & set(_ENTRY_POINTS))


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
        elif parameter.name in _MATRIX_ROW_ARGS:
            args.append(1)
        elif parameter.name in _MATRIX_COL_ARGS:
            args.append(len(buffer))
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
    # empties the discovery cannot slip through as a green run, and that
    # narrowing the rule back to a list of accepted argument spellings (which
    # would drop roughly a quarter of the set) fails here.
    assert len(_ENTRY_POINTS) >= 150, sorted(_ENTRY_POINTS)
    assert len(_GUARDED) >= 135, _GUARDED


def test_every_exemption_is_live() -> None:
    """A renamed exemption must not silently hide a real coverage gap."""
    stale = sorted(_EMPTY_INPUT_IS_DEFINED - set(_ENTRY_POINTS))
    assert stale == [], f"exempted functions no longer exist as buffer entry points: {stale}"


@pytest.mark.parametrize("name", _EXEMPT)
def test_exemption_accepts_empty_input(name: str) -> None:
    """An exemption claims empty input is defined; make it prove that.

    Without this, exempting a function is indistinguishable from suppressing a
    real gap: the two guard tests skip it either way. Here a function that
    actually rejects empty input fails, so the only way onto the exemption list
    is to be a function that returns a result for it.
    """
    signature = _ENTRY_POINTS[name]
    args = _call_arguments(name, signature, np.zeros(0, dtype=np.float32))
    getattr(libsonare, name)(*args)


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
