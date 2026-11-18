"""Every sample-taking entry point scans its input; there is no switch to skip it.

The roster is every function that took a ``validate`` flag. For each one the
keyword is now refused as unexpected, and a NaN or an infinity in each buffer
argument is still refused naming the function, the argument and the index.
A last case walks the whole public namespace so no ``validate`` parameter can
come back unnoticed.
"""

from __future__ import annotations

import inspect
import math
import re
from collections.abc import Callable
from typing import Any

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

SR = 44100
N = 8192
BAD_INDEX = 1000

# Function -> the buffer arguments it scans, in signature order.
ROSTER: dict[str, tuple[str, ...]] = {
    "align_take_to_reference": ("reference", "take"),
    "decompose_stems": ("samples",),
    "decompose_stems_linked": ("channels",),
    "ebur128_loudness_range_lu": ("samples",),
    "harmonic": ("samples",),
    "hpss": ("samples",),
    "hpss_with_residual": ("samples",),
    "lufs": ("samples",),
    "lufs_interleaved": ("samples",),
    "lufs_series_interleaved": ("samples",),
    "mastering_dynamics_compressor": ("samples",),
    "mastering_dynamics_gate": ("samples",),
    "mastering_dynamics_transient_shaper": ("samples",),
    "metering_crest_factor_db": ("samples",),
    "metering_crest_factor_db_stereo": ("left", "right"),
    "metering_dc_offset": ("samples",),
    "metering_detect_clipping": ("samples",),
    "metering_dynamic_range": ("samples",),
    "metering_peak_db": ("samples",),
    "metering_phase_scope": ("left", "right"),
    "metering_phase_scope_decimated": ("left", "right"),
    "metering_rms_db": ("samples",),
    "metering_silence_ratio": ("samples",),
    "metering_spectrum": ("samples",),
    "metering_spectrum_frame": ("samples",),
    "metering_stereo_correlation": ("left", "right"),
    "metering_stereo_width": ("left", "right"),
    "metering_true_peak_dbtp": ("samples",),
    "metering_vectorscope": ("left", "right"),
    "metering_vectorscope_decimated": ("left", "right"),
    "momentary_lufs": ("samples",),
    "normalize": ("samples",),
    "normalize_stereo": ("left", "right"),
    "percussive": ("samples",),
    "phase_vocoder": ("samples",),
    "pitch_shift": ("samples",),
    "short_term_lufs": ("samples",),
    "time_stretch": ("samples",),
    "trim": ("samples",),
    "waveform_peak_pyramid": ("samples",),
    "waveform_peaks": ("samples",),
}

# Required non-buffer arguments, by name.
_SCALARS: dict[str, object] = {
    "sample_rate": SR,
    "num_channels": 1,
    "speed_ratio": 1.25,
    "pitch_semitones": 2.0,
}


def _tone() -> np.ndarray:
    t = np.arange(N, dtype=np.float32) / SR
    return (0.3 * np.sin(2.0 * np.pi * 440.0 * t)).astype(np.float32)


def _call_args(fn: Callable[..., Any], spoil: str | None, value: float) -> dict[str, Any]:
    kwargs: dict[str, Any] = {}
    for name, param in inspect.signature(fn).parameters.items():
        if param.default is not inspect.Parameter.empty:
            continue
        if name in _SCALARS:
            kwargs[name] = _SCALARS[name]
        elif name == "channels" and "int" in str(param.annotation):
            kwargs[name] = 1
        elif name == "channels":
            planes = np.stack([_tone(), _tone()])
            if spoil == name:
                planes[0, BAD_INDEX] = value
            kwargs[name] = planes
        else:
            buf = _tone()
            if spoil == name:
                buf[BAD_INDEX] = value
            kwargs[name] = buf
    return kwargs


def _cases() -> list[tuple[str, str, float]]:
    return [
        (fn, arg, value)
        for fn, args in ROSTER.items()
        for arg in args
        for value in (math.nan, math.inf)
    ]


@pytest.mark.parametrize("fn_name", sorted(ROSTER))
def test_validate_keyword_is_refused(fn_name: str) -> None:
    fn = getattr(libsonare, fn_name)
    with pytest.raises(TypeError, match="validate"):
        fn(**_call_args(fn, None, 0.0), validate=False)


@pytest.mark.parametrize(("fn_name", "arg", "value"), _cases())
def test_non_finite_input_is_refused_by_name(fn_name: str, arg: str, value: float) -> None:
    fn = getattr(libsonare, fn_name)
    label = f"{arg}[0]" if arg == "channels" else arg
    with pytest.raises(
        SonareValueError,
        match=rf"^{fn_name}: {re.escape(label)} contains NaN or Inf at index {BAD_INDEX}$",
    ):
        fn(**_call_args(fn, arg, value))


def test_no_public_callable_takes_validate() -> None:
    found: list[str] = []
    for name in libsonare.__all__:
        obj = getattr(libsonare, name)
        members = [(name, obj)]
        if inspect.isclass(obj):
            members = [(f"{name}.{k}", v) for k, v in vars(obj).items() if callable(v)]
        for qual, member in members:
            target = getattr(member, "__func__", member)
            if not callable(target):
                continue
            try:
                params = inspect.signature(target).parameters
            except (TypeError, ValueError):
                continue
            if "validate" in params:
                found.append(qual)
    assert found == []
