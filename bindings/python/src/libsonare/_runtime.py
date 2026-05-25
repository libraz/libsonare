"""Shared runtime helpers for the libsonare Python binding."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

# _runtime is the shared re-export hub: feature submodules do
# `from ._runtime import *`, so forward the full C-struct and public type
# surfaces here instead of maintaining a partial hand-written list (an
# incomplete list silently breaks submodules at runtime with NameError).
from ._ffi import *  # noqa: F403
from .types import *  # noqa: F403

PAN_MODE_BALANCE = 0
PAN_MODE_STEREO_PAN = 1
PAN_MODE_DUAL_PAN = 2

_lib: ctypes.CDLL | None = None


def _get_lib() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        _lib = load_library()
    return _lib


def _check(rc: int) -> None:
    """Check a SonareError return code and raise on failure.

    When the C layer recorded a detailed thread-local message
    (``sonare_last_error_message``), it is preferred over the generic
    ``sonare_error_message(rc)`` fallback so users see the underlying cause.
    """
    if rc != SONARE_OK:
        lib = _get_lib()
        detail = lib.sonare_last_error_message()
        detail_str = detail.decode("utf-8") if detail else ""
        if detail_str:
            raise RuntimeError(detail_str)
        msg = lib.sonare_error_message(rc)
        raise RuntimeError(msg.decode("utf-8") if msg else f"sonare error {rc}")


def _to_c_float_array(
    samples: Sequence[float] | list[float],
) -> tuple[ctypes.Array[ctypes.c_float], int]:
    """Convert a sample sequence to a ctypes float array."""
    length = len(samples)
    c_array = (ctypes.c_float * length)(*samples)
    return c_array, length


def _to_c_int_array(values: Sequence[int] | list[int]) -> tuple[ctypes.Array[ctypes.c_int32], int]:
    length = len(values)
    c_array = (ctypes.c_int32 * length)(*values)
    return c_array, length


def _pan_mode_value(value: str | int) -> int:
    if isinstance(value, int):
        return value
    key = value.replace("_", "-").lower()
    if key == "balance":
        return PAN_MODE_BALANCE
    if key in ("stereo-pan", "stereopan", "pan"):
        return PAN_MODE_STEREO_PAN
    if key in ("dual-pan", "dualpan"):
        return PAN_MODE_DUAL_PAN
    raise ValueError(f"unknown pan mode: {value}")


def _curve_value(value: AutomationCurve | str | int) -> int:
    """Resolve an automation curve to its C enum value (0 linear, 1 exponential)."""
    if isinstance(value, AutomationCurve):
        return int(value)
    if isinstance(value, int):
        return value
    key = value.lower()
    if key in ("linear", "lin"):
        return int(AutomationCurve.LINEAR)
    if key in ("exponential", "exp"):
        return int(AutomationCurve.EXPONENTIAL)
    raise ValueError(f"unknown automation curve: {value}")


def _pan_law_value(value: PanLaw | str | int) -> int:
    """Resolve a pan law to its C enum value (0=-3dB, 1=-4.5dB, 2=-6dB, 3=linear)."""
    if isinstance(value, PanLaw):
        return int(value)
    if isinstance(value, int):
        return value
    key = value.replace("_", "-").lower()
    mapping = {
        "const-3db": PanLaw.CONST_3DB,
        "-3db": PanLaw.CONST_3DB,
        "const-4.5db": PanLaw.CONST_4_5DB,
        "-4.5db": PanLaw.CONST_4_5DB,
        "const-6db": PanLaw.CONST_6DB,
        "-6db": PanLaw.CONST_6DB,
        "linear": PanLaw.LINEAR_0DB,
        "linear-0db": PanLaw.LINEAR_0DB,
        "0db": PanLaw.LINEAR_0DB,
    }
    if key not in mapping:
        raise ValueError(f"unknown pan law: {value}")
    return int(mapping[key])


def _meter_tap_value(value: MeterTap | str | int) -> int:
    """Resolve a meter tap point to its C enum value (0 pre-fader, 1 post-fader)."""
    if isinstance(value, MeterTap):
        return int(value)
    if isinstance(value, int):
        return value
    key = value.replace("_", "-").lower()
    if key in ("pre-fader", "pre", "prefader"):
        return int(MeterTap.PRE_FADER)
    if key in ("post-fader", "post", "postfader"):
        return int(MeterTap.POST_FADER)
    raise ValueError(f"unknown meter tap: {value}")


def _mix_meter_from_c(snapshot: SonareMixMeterSnapshot) -> MixMeterSnapshot:
    return MixMeterSnapshot(
        peak_db_l=float(snapshot.peak_db_l),
        peak_db_r=float(snapshot.peak_db_r),
        rms_db_l=float(snapshot.rms_db_l),
        rms_db_r=float(snapshot.rms_db_r),
        correlation=float(snapshot.correlation),
        mono_compat_width=float(snapshot.mono_compat_width),
        mono_compat_peak=float(snapshot.mono_compat_peak),
        mono_compat_side_rms=float(snapshot.mono_compat_side_rms),
        likely_mono_compatible=bool(snapshot.likely_mono_compatible),
        momentary_lufs=float(snapshot.momentary_lufs),
        short_term_lufs=float(snapshot.short_term_lufs),
        integrated_lufs=float(snapshot.integrated_lufs),
        gain_reduction_db=float(snapshot.gain_reduction_db),
        true_peak_db_l=float(snapshot.true_peak_db_l),
        true_peak_db_r=float(snapshot.true_peak_db_r),
        max_true_peak_db=float(snapshot.max_true_peak_db),
        seq=int(snapshot.seq),
    )


def _mode_values(modes: Sequence[Mode | str] | str | None) -> list[int]:
    if modes is None:
        return []
    if isinstance(modes, str):
        key = modes.lower()
        if key in ("major-minor", "majmin", "diatonic"):
            return [int(Mode.MAJOR), int(Mode.MINOR)]
        if key in ("all", "modal"):
            return [
                int(Mode.MAJOR),
                int(Mode.MINOR),
                int(Mode.DORIAN),
                int(Mode.PHRYGIAN),
                int(Mode.LYDIAN),
                int(Mode.MIXOLYDIAN),
                int(Mode.LOCRIAN),
            ]
        modes = [modes]
    names = {
        "major": Mode.MAJOR,
        "maj": Mode.MAJOR,
        "minor": Mode.MINOR,
        "min": Mode.MINOR,
        "m": Mode.MINOR,
        "dorian": Mode.DORIAN,
        "phrygian": Mode.PHRYGIAN,
        "lydian": Mode.LYDIAN,
        "mixolydian": Mode.MIXOLYDIAN,
        "locrian": Mode.LOCRIAN,
    }
    out: list[int] = []
    for mode in modes:
        if isinstance(mode, str):
            key = mode.lower()
            if key not in names:
                raise ValueError(f"invalid mode: {mode}")
            out.append(int(names[key]))
        else:
            out.append(int(Mode(mode)))
    return out


def _profile_value(profile: KeyProfile | str | None) -> int:
    if profile is None:
        return int(KeyProfile.KRUMHANSL_SCHMUCKLER)
    if isinstance(profile, str):
        names = {
            "ks": KeyProfile.KRUMHANSL_SCHMUCKLER,
            "krumhansl": KeyProfile.KRUMHANSL_SCHMUCKLER,
            "krumhansl-schmuckler": KeyProfile.KRUMHANSL_SCHMUCKLER,
            "temperley": KeyProfile.TEMPERLEY,
            "shaath": KeyProfile.SHAATH,
            "keyfinder": KeyProfile.SHAATH,
            "faraldo-edmt": KeyProfile.FARALDO_EDMT,
            "edmt": KeyProfile.FARALDO_EDMT,
            "faraldo-edma": KeyProfile.FARALDO_EDMA,
            "edma": KeyProfile.FARALDO_EDMA,
            "faraldo-edmm": KeyProfile.FARALDO_EDMM,
            "edmm": KeyProfile.FARALDO_EDMM,
            "bellman-budge": KeyProfile.BELLMAN_BUDGE,
            "bellman": KeyProfile.BELLMAN_BUDGE,
        }
        key = profile.lower()
        if key not in names:
            raise ValueError(f"invalid key profile: {profile}")
        return int(names[key])
    return int(KeyProfile(profile))


def _float_array_result(out: ctypes.POINTER(ctypes.c_float), count: int) -> list[float]:
    return [float(out[i]) for i in range(count)]


def _optional_float_array_result(out: ctypes.POINTER(ctypes.c_float), count: int) -> list[float]:
    # A null pointer means the array was not computed (e.g. clarity bands in
    # blind mode); represent that as an empty list rather than crashing.
    if not out:
        return []
    return [float(out[i]) for i in range(count)]


def _int_array_result(out: ctypes.POINTER(ctypes.c_int), count: int) -> list[int]:
    return [int(out[i]) for i in range(count)]


def _call_float_transform(
    fn_name: str, values: Sequence[float] | list[float], *args: object
) -> list[float]:
    lib = _get_lib()
    c_array, length = _to_c_float_array(values)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = getattr(lib, fn_name)(
        c_array,
        ctypes.c_size_t(length),
        *args,
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


__all__ = [name for name in globals() if not name.startswith("__")]
