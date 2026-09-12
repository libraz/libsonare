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

An entry point reached as a public method on an exported class counts the same
and is keyed by its qualified ``Class.method`` name. The walk follows the MRO
rather than one class's own ``vars``, because the two largest handle classes are
assembled from mixins and would otherwise contribute nothing; it stops at the
first base defined outside the package, so a class that merely subclasses
``dict`` or ``IntEnum`` does not drag the standard library's methods in. A
``Protocol`` is skipped: it declares what a *caller* implements and hands to the
library, so there is no facade there to guard.
"""

from __future__ import annotations

import ast
import inspect
from collections.abc import Callable
from pathlib import Path
from typing import Any, NamedTuple

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
# output length comes from a parameter rather than from the input. Any other
# function of this shape must preflight.
#
# Exempt is exempt from the *empty* half only. Non-finite input still has to be
# refused; what differs is where. Most of these leave it to the C ABI, which
# answers with its own generic code; `StreamingEqualizer.magnitude_response`
# scans for it in the facade and names the argument, which is strictly better
# and is pinned in `test_empty_nan_guards.py` rather than here — this module's
# two guard tests skip an exemption entirely, so an exemption silently drops the
# non-finite half of the rule along with the empty half.
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
        # Per frequency, so an empty frequency list asks nothing and an empty
        # curve is the answer.
        "StreamingEqualizer.magnitude_response",
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
    "median_hz": 220.0,
    # The note a split cuts and the run a merge joins. Both are rejected for
    # other reasons on this probe, which is fine: the guard fires first.
    "index": 0,
    "frame": 0,
    "first": 0,
    "last": 1,
    "win": 3,
    "pre_max": 1,
    "post_max": 1,
    "pre_avg": 1,
    "post_avg": 1,
    "delta": 0.1,
    "wait": 1,
    "sample_offset": 0,
}

# How to build an instance for a class whose buffer methods need one. Same
# polarity as `_SCALAR_ARGS`: a discovered class with no entry here fails the
# test loudly, so a new handle class cannot drop out of coverage quietly. The
# constructor arguments are the cheapest configuration that opens the handle —
# what is being probed is the preflight, which runs before any of them matter.
_CLASS_INSTANCES: dict[str, Callable[[], Any]] = {
    "Project": lambda: libsonare.Project(),
    "RealtimeVoiceChanger": lambda: libsonare.RealtimeVoiceChanger(22050),
    "SampleBank": lambda: libsonare.SampleBank(),
    "StreamAnalyzer": lambda: libsonare.StreamAnalyzer(),
    "StreamingEqualizer": lambda: libsonare.StreamingEqualizer(),
    "StreamingMasteringChain": lambda: libsonare.StreamingMasteringChain(),
    "StreamingRetune": lambda: libsonare.StreamingRetune(),
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
_SEQUENCE_ARGS = (
    "intervals",
    "boundaries",
    "notes",
    "ops",
    "voiced",
    "platforms",
    "factors",
    "events",
)


class _EntryPoint(NamedTuple):
    """One discovered buffer entry point and how to reach it."""

    signature: inspect.Signature
    owner: str | None
    """Exported class the entry point is a method of, or ``None`` for a function."""


def _is_buffer_parameter(parameter: inspect.Parameter) -> bool:
    alternatives = [part.strip() for part in str(parameter.annotation).split("|")]
    return bool(alternatives) and all(part in _BUFFER_TYPES for part in alternatives)


def _leading_parameters(signature: inspect.Signature) -> list[inspect.Parameter]:
    """The signature's parameters with an unbound method's receiver dropped."""
    return [
        parameter
        for parameter in signature.parameters.values()
        if parameter.name not in ("self", "cls")
    ]


def _public_method_names(cls: type) -> list[str]:
    """Public attribute names contributed by the package's own classes in the MRO."""
    names: set[str] = set()
    for base in cls.__mro__:
        if not getattr(base, "__module__", "").startswith("libsonare"):
            continue
        names.update(name for name in vars(base) if not name.startswith("_"))
    return sorted(names)


def _buffer_entry_points() -> dict[str, _EntryPoint]:
    """Every exported callable whose leading parameter is a sample buffer."""
    found: dict[str, _EntryPoint] = {}
    # `__init__.pyi` does not re-declare `__all__`, so reach for it dynamically;
    # a missing one collapses the set and trips `test_buffer_entry_point_floor`.
    exported: tuple[str, ...] = tuple(getattr(libsonare, "__all__", ()))
    for name in exported:
        obj = getattr(libsonare, name, None)
        if obj is None:
            continue
        if inspect.isclass(obj):
            if getattr(obj, "_is_protocol", False):
                continue
            for method_name in _public_method_names(obj):
                method = getattr(obj, method_name, None)
                if method is None or not callable(method):
                    continue
                try:
                    signature = inspect.signature(method)
                except (TypeError, ValueError):  # pragma: no cover - C builtins
                    continue
                parameters = _leading_parameters(signature)
                if parameters and _is_buffer_parameter(parameters[0]):
                    found[f"{name}.{method_name}"] = _EntryPoint(signature, name)
            continue
        if not callable(obj):
            continue
        try:
            signature = inspect.signature(obj)
        except (TypeError, ValueError):  # pragma: no cover - C builtins
            continue
        parameters = list(signature.parameters.values())
        if parameters and _is_buffer_parameter(parameters[0]):
            found[name] = _EntryPoint(signature, None)
    return found


_ENTRY_POINTS = _buffer_entry_points()
_METHOD_ENTRY_POINTS = {
    name: entry for name, entry in _ENTRY_POINTS.items() if entry.owner is not None
}
_GUARDED = sorted(set(_ENTRY_POINTS) - _EMPTY_INPUT_IS_DEFINED)
_EXEMPT = sorted(_EMPTY_INPUT_IS_DEFINED & set(_ENTRY_POINTS))

# Block-processing methods on the audio-thread path. What they fail is this
# module's rule, not a promise of their own: the guard's per-block `np.isfinite`
# walk is the O(n) cost `_check_realtime` exists to keep off that thread, so
# these take pre-validated blocks by contract and the caller owns the check.
#
# Two consequences worth keeping apart, and each entry says which. `unnamed`
# means the non-finite block is already refused and only the error's class and
# wording fall short of the rule. `silent` means it is accepted, and what the
# value then does was measured per method rather than assumed — the three
# families answer differently, and the equalizer's answer is the one that does
# not end.
#
# Not an exemption list. These stay parametrized and stay running under a strict
# xfail, so guarding one turns it into an xpass and fails here until the entry
# is removed — an exemption would have let the same fix pass unnoticed.
_REALTIME_BLOCK_PATH = {
    "RealtimeVoiceChanger.process_interleaved": (
        "silent: the caller owns the finite check on an audio-thread block call; "
        "output stays finite but is perturbed from the latency boundary onwards"
    ),
    "RealtimeVoiceChanger.process_mono": (
        "silent: the caller owns the finite check on an audio-thread block call; "
        "output stays finite but is perturbed from the latency boundary onwards"
    ),
    "RealtimeVoiceChanger.process_planar_stereo": (
        "unnamed: the caller owns the finite check on an audio-thread block call; "
        "the non-finite block is refused as a bare [4] Invalid parameter"
    ),
    "StreamAnalyzer.process": (
        "silent: the caller owns the finite check on a streaming block call; the "
        "analysis frames overlapping the block carry garbage, later frames recover"
    ),
    "StreamAnalyzer.process_with_offset": (
        "silent: the caller owns the finite check on a streaming block call; the "
        "analysis frames overlapping the block carry garbage, later frames recover"
    ),
    "StreamingEqualizer.process_mono": (
        "silent: the caller owns the finite check on a streaming block call; one "
        "non-finite sample poisons the biquad state until clear(), not just the block"
    ),
    "StreamingEqualizer.process_stereo": (
        "silent: the caller owns the finite check on a streaming block call; one "
        "non-finite sample poisons the biquad state until clear(), not just the block"
    ),
    "StreamingMasteringChain.process_mono": (
        "unnamed: the caller owns the finite check on a streaming block call; "
        "the non-finite block is refused as a bare [7] Invalid state"
    ),
    "StreamingMasteringChain.process_stereo": (
        "unnamed: the caller owns the finite check on a streaming block call; "
        "the non-finite block is refused as a bare [7] Invalid state"
    ),
    "StreamingRetune.process_mono": (
        "unnamed: the caller owns the finite check on a streaming block call; "
        "the non-finite block is refused as a bare [7] Invalid state"
    ),
}


def _guard_parameters() -> list[Any]:
    """``_GUARDED`` as parametrize arguments, block-path methods strictly xfailed."""
    return [
        pytest.param(name, marks=pytest.mark.xfail(strict=True, reason=_REALTIME_BLOCK_PATH[name]))
        if name in _REALTIME_BLOCK_PATH
        else name
        for name in _GUARDED
    ]


_GUARD_PARAMETERS = _guard_parameters()


def _invoke(name: str, entry: _EntryPoint, args: list[Any]) -> Any:
    """Call ``name`` with ``args``, opening and closing a handle if it needs one."""
    if entry.owner is None:
        return getattr(libsonare, name)(*args)

    method_name = name.split(".", 1)[1]
    owner = getattr(libsonare, entry.owner)
    if "self" not in entry.signature.parameters:
        return getattr(owner, method_name)(*args)

    factory = _CLASS_INSTANCES.get(entry.owner)
    if factory is None:
        pytest.fail(
            f"{name}: no instance factory for {entry.owner!r}; add one to this "
            "module so its buffer methods stay covered"
        )
    instance = factory()
    try:
        return getattr(instance, method_name)(*args)
    finally:
        close = getattr(instance, "close", None)
        if callable(close):
            close()


def _call_arguments(name: str, signature: inspect.Signature, buffer: np.ndarray) -> list[Any]:
    """Positional arguments that reach ``name``'s body with ``buffer`` as input."""
    args: list[Any] = []
    for index, parameter in enumerate(_leading_parameters(signature)):
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
    # Separately, because the method walk is the part that can collapse on its
    # own: a mixin reshuffle or a Protocol check that catches too much would
    # empty it while the module-level count stays comfortably over its floor.
    assert len(_METHOD_ENTRY_POINTS) >= 15, sorted(_METHOD_ENTRY_POINTS)


def test_every_exemption_is_live() -> None:
    """A renamed exemption must not silently hide a real coverage gap."""
    stale = sorted(_EMPTY_INPUT_IS_DEFINED - set(_ENTRY_POINTS))
    assert stale == [], f"exempted functions no longer exist as buffer entry points: {stale}"


def test_every_block_path_entry_is_live() -> None:
    """A renamed block-path method must not carry its xfail to a dead name.

    The strict xfail catches one that gets guarded; this catches one that gets
    renamed or dropped, where the marker would otherwise sit on a parametrize
    id that no longer exists and quietly stop applying to anything.
    """
    stale = sorted(set(_REALTIME_BLOCK_PATH) - set(_ENTRY_POINTS))
    assert stale == [], f"block-path methods no longer exist as buffer entry points: {stale}"


@pytest.mark.parametrize("name", _EXEMPT)
def test_exemption_accepts_empty_input(name: str) -> None:
    """An exemption claims empty input is defined; make it prove that.

    Without this, exempting a function is indistinguishable from suppressing a
    real gap: the two guard tests skip it either way. Here a function that
    actually rejects empty input fails, so the only way onto the exemption list
    is to be a function that returns a result for it.
    """
    entry = _ENTRY_POINTS[name]
    args = _call_arguments(name, entry.signature, np.zeros(0, dtype=np.float32))
    _invoke(name, entry, args)


def _assert_names_the_entry_point(name: str, message: str) -> None:
    """The rejection must name what the caller invoked.

    For a method that is the method's own name, not the exported ``Class.method``
    key: the guard builds its prefix from ``fn.__name__``, and the two largest
    handle classes are assembled from mixins, so the only qualified name reachable
    there is the private mixin's. Demanding the exported one would mean writing
    the prefix out by hand at each call site — the hand-maintained list this
    module exists to remove — and would leak ``_ProjectInspectionMixin`` at users
    if taken from ``__qualname__`` instead.
    """
    invoked = name.rsplit(".", 1)[-1]
    assert invoked in message, f"{name}: message does not name the entry point"


@pytest.mark.parametrize("name", _GUARD_PARAMETERS)
def test_empty_buffer_is_rejected(name: str) -> None:
    entry = _ENTRY_POINTS[name]
    args = _call_arguments(name, entry.signature, np.zeros(0, dtype=np.float32))
    with pytest.raises(SonareValueError) as excinfo:
        _invoke(name, entry, args)
    _assert_names_the_entry_point(name, str(excinfo.value))


@pytest.mark.parametrize("name", _GUARD_PARAMETERS)
def test_non_finite_buffer_is_rejected(name: str) -> None:
    entry = _ENTRY_POINTS[name]
    args = _call_arguments(name, entry.signature, np.full(64, np.nan, dtype=np.float32))
    with pytest.raises(SonareValueError) as excinfo:
        _invoke(name, entry, args)
    _assert_names_the_entry_point(name, str(excinfo.value))


# The rank rejection is raised inside `_as_float32_buffer`, which names the
# argument from `arg_name` and defaults it to "samples". A call site coercing a
# differently-named argument therefore has to forward the real name, and
# forwarding is a per-call-site edit — the same drift this module exists to
# catch for the guards. Derived from the source rather than listed: a new call
# site fails here until it either forwards a name or is coercing something
# actually called `samples`.
_BINDING_SOURCE_ROOT = Path(__file__).parents[1] / "src" / "libsonare"


def _buffer_coercion_call_sites() -> list[tuple[str, int, ast.Call]]:
    """Every `_as_float32_buffer(...)` call in the binding, with its location."""
    sites: list[tuple[str, int, ast.Call]] = []
    for path in sorted(_BINDING_SOURCE_ROOT.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Name)
                and node.func.id == "_as_float32_buffer"
            ):
                sites.append((path.name, node.lineno, node))
    return sites


def test_every_buffer_coercion_site_names_the_argument_it_coerces() -> None:
    """A rank rejection must not report the helper's default for another name."""
    sites = _buffer_coercion_call_sites()
    # Non-vacuity: the walk has to be finding the call sites at all.
    assert len(sites) >= 10, sites

    misnaming = []
    for filename, lineno, call in sites:
        if any(keyword.arg == "arg_name" for keyword in call.keywords):
            continue
        coerced = call.args[0] if call.args else None
        if isinstance(coerced, ast.Name) and coerced.id == "samples":
            continue  # the helper's default already names it correctly
        misnaming.append(f"{filename}:{lineno}")
    assert misnaming == [], (
        "these coercions would report the default argument name 'samples' for a "
        f"differently-named argument: {misnaming}"
    )


@pytest.mark.parametrize(
    ("subject", "expected"),
    [(None, "channels"), ("clip channels", "clip channels")],
)
def test_planar_rank_rejection_names_the_caller_subject(subject, expected) -> None:
    """A 2-D channel reports the subject the caller passed, not 'samples'."""
    from libsonare._runtime import _planar_channel_arrays

    channels = [np.zeros((2, 2), dtype=np.float32)]
    kwargs = {} if subject is None else {"subject": subject}
    with pytest.raises(SonareValueError) as excinfo:
        _planar_channel_arrays(channels, **kwargs)
    message = str(excinfo.value)
    assert message.startswith(f"{expected} must be a 1-D buffer"), message
    assert "samples must be" not in message


@pytest.mark.parametrize("bad_side", ["left", "right"])
def test_planar_stereo_rank_rejection_names_the_side(bad_side: str) -> None:
    """Each planar-stereo channel reports its own parameter name."""
    good = np.zeros(128, dtype=np.float32)
    bad = np.zeros((2, 64), dtype=np.float32)
    arguments = (bad, good) if bad_side == "left" else (good, bad)

    with (
        libsonare.RealtimeVoiceChanger(
            48000, "bright-idol", max_block_size=128, channels=2
        ) as changer,
        pytest.raises(SonareValueError) as excinfo,
    ):
        changer.process_planar_stereo(*arguments)
    message = str(excinfo.value)
    assert message.startswith(f"{bad_side} must be a 1-D buffer"), message
