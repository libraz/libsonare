"""Mastering wrappers for libsonare."""

from __future__ import annotations

import contextlib
import ctypes
import json
import math
from collections.abc import Sequence
from typing import Any

from ._ffi import (
    SonareEqSnapshot,
)
from ._mastering_offline import _chain_params
from ._runtime import _check, _get_lib, _to_c_float_array
from .types import (
    EqSpectrumSnapshot,
)


class StreamingMasteringChain:
    """Block-by-block streaming variant of :func:`mastering_chain`.

    Maintains processor state across :meth:`process_mono`/:meth:`process_stereo`
    calls. Only ProcessorBase-backed stages (eq.tilt, dynamics.compressor,
    saturation.tape, saturation.exciter, spectral.airBand, stereo.imager,
    stereo.monoMaker, maximizer.truePeakLimiter) are supported. Configurations
    that enable ``repair.denoise`` or ``loudness`` raise :class:`RuntimeError`.

    Example::

        chain = StreamingMasteringChain({"eq.tilt.tiltDb": 1.0})
        chain.prepare(sample_rate=44100, max_block_size=512, num_channels=1)
        out = chain.process_mono([0.1] * 512)
        chain.reset()

    Can also be used as a context manager to ensure the underlying handle is
    released::

        with StreamingMasteringChain({"eq.tilt.tiltDb": 1.0}) as chain:
            chain.prepare(44100, 512, 1)
            ...
    """

    def __init__(
        self,
        config: dict[str, Any] | None = None,
        *,
        loudness_static_gain_db: float | None = None,
        loudness_static_gain_peak_db: float | None = None,
    ) -> None:
        """Create a streaming mastering chain.

        Args:
            config: Flat chain params (see :func:`mastering_chain`).
            loudness_static_gain_db: Precomputed loudness normalization gain in
                dB (e.g. ``target_lufs - measured_integrated_lufs``, measured
                offline). The streaming chain cannot measure whole-signal
                integrated LUFS, so a ``loudness``-enabled config raises unless a
                static gain is supplied here; when supplied it is applied per
                block before the loudness stage's true-peak limiter.
            loudness_static_gain_peak_db: Offline-measured true-peak (dBFS) of the
                source the static gain was computed for. When given, the static
                gain is clamped to ``ceilingDb - peak`` so the streaming preview
                does not overdrive the loudness limiter harder than the offline
                render. Ignored unless ``loudness_static_gain_db`` is given.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_streaming_mastering_chain_create"):
            raise RuntimeError("libsonare was built without streaming mastering chain support")
        param_array, param_count = _chain_params(config)
        if loudness_static_gain_db is not None:
            if not hasattr(lib, "sonare_streaming_mastering_chain_create_ex"):
                raise RuntimeError(
                    "libsonare was built without streaming loudness static-gain support"
                )
            peak = (
                math.nan
                if loudness_static_gain_peak_db is None
                else float(loudness_static_gain_peak_db)
            )
            handle = lib.sonare_streaming_mastering_chain_create_ex(
                param_array,
                ctypes.c_size_t(param_count),
                ctypes.c_float(float(loudness_static_gain_db)),
                ctypes.c_float(peak),
            )
        else:
            handle = lib.sonare_streaming_mastering_chain_create(
                param_array, ctypes.c_size_t(param_count)
            )
        if not handle:
            detail = ""
            if hasattr(lib, "sonare_last_error_message"):
                raw = lib.sonare_last_error_message()
                if raw:
                    detail = raw.decode("utf-8", errors="replace")
            message = "failed to create StreamingMasteringChain"
            if detail:
                message = f"{message}: {detail}"
            raise RuntimeError(message)
        self._lib = lib
        self._handle = ctypes.c_void_p(handle)
        self._prepared_channels = 0

    def prepare(self, sample_rate: int, max_block_size: int, num_channels: int) -> None:
        """Initialize processors for the given sample rate and block layout.

        Args:
            sample_rate: Sample rate in Hz.
            max_block_size: Maximum block size in samples per
                :meth:`process_mono` / :meth:`process_stereo` call.
            num_channels: 1 (mono) or 2 (stereo). Stereo-only stages
                (imager, monoMaker) are skipped when ``num_channels`` is 1.
        """
        self._ensure_open()
        rc = self._lib.sonare_streaming_mastering_chain_prepare(
            self._handle,
            ctypes.c_int(int(sample_rate)),
            ctypes.c_int(int(max_block_size)),
            ctypes.c_int(int(num_channels)),
        )
        _check(rc)
        self._prepared_channels = int(num_channels)

    def process_mono(self, samples: Sequence[float] | list[float]) -> list[float]:
        """Process one mono block, returning the processed samples (length unchanged)."""
        self._ensure_open()
        c_array, length = _to_c_float_array(samples)
        rc = self._lib.sonare_streaming_mastering_chain_process_mono(
            self._handle, c_array, ctypes.c_size_t(length)
        )
        _check(rc)
        return [float(c_array[i]) for i in range(length)]

    def process_stereo(
        self,
        left: Sequence[float] | list[float],
        right: Sequence[float] | list[float],
    ) -> tuple[list[float], list[float]]:
        """Process one stereo block, returning the processed (left, right) channels."""
        self._ensure_open()
        left_array, left_length = _to_c_float_array(left)
        right_array, right_length = _to_c_float_array(right)
        if left_length != right_length:
            raise ValueError("left and right channel lengths must match")
        rc = self._lib.sonare_streaming_mastering_chain_process_stereo(
            self._handle, left_array, right_array, ctypes.c_size_t(left_length)
        )
        _check(rc)
        return (
            [float(left_array[i]) for i in range(left_length)],
            [float(right_array[i]) for i in range(right_length)],
        )

    def reset(self) -> None:
        """Reset all processor state without rebuilding."""
        self._ensure_open()
        rc = self._lib.sonare_streaming_mastering_chain_reset(self._handle)
        _check(rc)

    @property
    def latency_samples(self) -> int:
        """Total reported latency in samples across all active processors."""
        if self._handle is None or not self._handle:
            return 0
        return int(self._lib.sonare_streaming_mastering_chain_latency_samples(self._handle))

    def stage_names(self) -> list[str]:
        """Return the realized stage names in processing order.

        Reflects which stages the config actually enables (e.g. ``eq.tilt``,
        ``dynamics.compressor``, ``maximizer.truePeakLimiter``). Stage selection
        happens in :meth:`prepare` — stereo-only stages depend on the channel
        count — so this returns an empty list until :meth:`prepare` is called.
        """
        if self._handle is None or not self._handle:
            return []
        if not hasattr(self._lib, "sonare_streaming_mastering_chain_stage_names"):
            return []
        raw = self._lib.sonare_streaming_mastering_chain_stage_names(self._handle)
        return raw.decode("utf-8").splitlines() if raw else []

    def close(self) -> None:
        """Release the underlying C handle. Safe to call multiple times."""
        if self._handle is not None and self._handle:
            self._lib.sonare_streaming_mastering_chain_destroy(self._handle)
            self._handle = ctypes.c_void_p(0)

    def __enter__(self) -> StreamingMasteringChain:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def __del__(self) -> None:
        # Defensive: __del__ must not raise
        with contextlib.suppress(Exception):
            self.close()

    def _ensure_open(self) -> None:
        if self._handle is None or not self._handle:
            raise RuntimeError("StreamingMasteringChain is closed")


class StreamingEqualizer:
    """Block-by-block unified EQ wrapper around the native ``SonareEq`` handle."""

    _PHASES = {
        "zero": 1,
        "zero-latency": 1,
        "zero_latency": 1,
        "natural": 2,
        "natural-phase": 2,
        "natural_phase": 2,
        "linear": 3,
        "linear-phase": 3,
        "linear_phase": 3,
    }

    def __init__(self, sample_rate: int = 48000, max_block_size: int = 512) -> None:
        lib = _get_lib()
        if not hasattr(lib, "sonare_eq_create"):
            raise RuntimeError("libsonare was built without streaming equalizer support")
        handle = lib.sonare_eq_create(float(sample_rate), int(max_block_size))
        if not handle:
            raise RuntimeError("failed to create StreamingEqualizer")
        self._lib = lib
        self._handle = ctypes.c_void_p(handle)
        self.sample_rate = int(sample_rate)
        self.max_block_size = int(max_block_size)
        self._sidechain_refs: object | None = None

    def set_band(self, index: int, band: dict[str, Any] | str) -> None:
        """Set one EQ band from a JSON string or a Python dict."""
        self._ensure_open()
        payload = band if isinstance(band, str) else json.dumps(band, separators=(",", ":"))
        rc = self._lib.sonare_eq_set_band(
            self._handle, ctypes.c_int(int(index)), payload.encode("utf-8")
        )
        _check(rc)

    def clear(self) -> None:
        """Clear all EQ bands."""
        self._ensure_open()
        self._lib.sonare_eq_clear(self._handle)

    def set_phase_mode(self, mode: int | str) -> None:
        """Set the global phase mode: zero/natural/linear or 1/2/3."""
        self._ensure_open()
        if isinstance(mode, str):
            key = mode.lower()
            if key not in self._PHASES:
                raise ValueError(f"unknown EQ phase mode: {mode}")
            value = self._PHASES[key]
        else:
            value = int(mode)
        _check(self._lib.sonare_eq_set_phase_mode(self._handle, ctypes.c_int(value)))

    def set_auto_gain(self, enabled: bool) -> None:
        """Enable or disable auto-gain compensation."""
        self._ensure_open()
        self._lib.sonare_eq_set_auto_gain(self._handle, ctypes.c_int(1 if enabled else 0))

    def set_gain_scale(self, scale: float) -> None:
        """Set all-band EQ gain scale as a 0.0..2.0 multiplier."""
        self._ensure_open()
        _check(self._lib.sonare_eq_set_gain_scale(self._handle, ctypes.c_float(float(scale))))

    def set_output_gain_db(self, gain_db: float) -> None:
        """Set post-EQ output gain in dB."""
        self._ensure_open()
        _check(self._lib.sonare_eq_set_output_gain_db(self._handle, ctypes.c_float(float(gain_db))))

    def set_output_pan(self, pan: float) -> None:
        """Set post-EQ stereo balance in -1.0..1.0; mono input ignores pan."""
        self._ensure_open()
        _check(self._lib.sonare_eq_set_output_pan(self._handle, ctypes.c_float(float(pan))))

    def set_sidechain_mono(self, samples: Sequence[float] | list[float]) -> None:
        """Set a mono external key for dynamic bands with ``externalSidechain`` enabled."""
        self._ensure_open()
        c_array, length = _to_c_float_array(samples)
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 1
        channels = channel_array_type(ctypes.cast(c_array, ctypes.POINTER(ctypes.c_float)))
        _check(self._lib.sonare_eq_set_sidechain(self._handle, channels, ctypes.c_int(1), length))
        self._sidechain_refs = (c_array, channels)

    def set_sidechain_stereo(
        self,
        left: Sequence[float] | list[float],
        right: Sequence[float] | list[float],
    ) -> None:
        """Set a stereo external key for dynamic bands with ``externalSidechain`` enabled."""
        self._ensure_open()
        left_array, left_length = _to_c_float_array(left)
        right_array, right_length = _to_c_float_array(right)
        if left_length != right_length:
            raise ValueError("left and right sidechain lengths must match")
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 2
        channels = channel_array_type(
            ctypes.cast(left_array, ctypes.POINTER(ctypes.c_float)),
            ctypes.cast(right_array, ctypes.POINTER(ctypes.c_float)),
        )
        _check(
            self._lib.sonare_eq_set_sidechain(
                self._handle, channels, ctypes.c_int(2), ctypes.c_int(left_length)
            )
        )
        self._sidechain_refs = (left_array, right_array, channels)

    def clear_sidechain(self) -> None:
        """Clear any pending external key buffer."""
        self._ensure_open()
        self._lib.sonare_eq_clear_sidechain(self._handle)
        self._sidechain_refs = None

    def match(
        self,
        source: Sequence[float] | list[float],
        reference: Sequence[float] | list[float],
        max_bands: int = 8,
    ) -> None:
        """Configure live EQ bands by matching ``source`` to ``reference``."""
        self._ensure_open()
        source_array, source_length = _to_c_float_array(source)
        reference_array, reference_length = _to_c_float_array(reference)
        if source_length != reference_length:
            raise ValueError("source and reference lengths must match")
        rc = self._lib.sonare_eq_match(
            self._handle,
            source_array,
            reference_array,
            ctypes.c_size_t(source_length),
            ctypes.c_int(self.sample_rate),
            ctypes.c_int(int(max_bands)),
        )
        _check(rc)

    def process_mono(self, samples: Sequence[float] | list[float]) -> list[float]:
        """Process one mono block, returning processed samples."""
        self._ensure_open()
        c_array, length = _to_c_float_array(samples)
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 1
        channels = channel_array_type(ctypes.cast(c_array, ctypes.POINTER(ctypes.c_float)))
        _check(self._lib.sonare_eq_process(self._handle, channels, ctypes.c_int(1), length))
        self._sidechain_refs = None
        return [float(c_array[i]) for i in range(length)]

    def process_stereo(
        self,
        left: Sequence[float] | list[float],
        right: Sequence[float] | list[float],
    ) -> tuple[list[float], list[float]]:
        """Process one stereo block, returning the processed (left, right) channels."""
        self._ensure_open()
        left_array, left_length = _to_c_float_array(left)
        right_array, right_length = _to_c_float_array(right)
        if left_length != right_length:
            raise ValueError("left and right channel lengths must match")
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 2
        channels = channel_array_type(
            ctypes.cast(left_array, ctypes.POINTER(ctypes.c_float)),
            ctypes.cast(right_array, ctypes.POINTER(ctypes.c_float)),
        )
        _check(
            self._lib.sonare_eq_process(
                self._handle, channels, ctypes.c_int(2), ctypes.c_int(left_length)
            )
        )
        self._sidechain_refs = None
        return (
            [float(left_array[i]) for i in range(left_length)],
            [float(right_array[i]) for i in range(right_length)],
        )

    def spectrum(self) -> EqSpectrumSnapshot:
        """Return the latest pre/post sample stream and band-gain snapshot."""
        self._ensure_open()
        out = SonareEqSnapshot()
        _check(self._lib.sonare_eq_spectrum(self._handle, ctypes.byref(out)))
        pre_count = int(out.pre_count)
        post_count = int(out.post_count)
        return EqSpectrumSnapshot(
            pre_left=[float(out.pre_left[i]) for i in range(pre_count)],
            pre_right=[float(out.pre_right[i]) for i in range(pre_count)],
            post_left=[float(out.post_left[i]) for i in range(post_count)],
            post_right=[float(out.post_right[i]) for i in range(post_count)],
            band_gain_db=[float(out.band_gain_db[i]) for i in range(24)],
            profile_db=[float(out.profile_db[i]) for i in range(16)],
            last_auto_gain_db=float(out.last_auto_gain_db),
            seq=int(out.seq),
        )

    @property
    def latency_samples(self) -> int:
        self._ensure_open()
        return int(self._lib.sonare_eq_latency_samples(self._handle))

    @property
    def last_auto_gain_db(self) -> float:
        self._ensure_open()
        return float(self._lib.sonare_eq_last_auto_gain_db(self._handle))

    def close(self) -> None:
        """Release the underlying C handle. Safe to call multiple times."""
        if self._handle is not None and self._handle:
            self._lib.sonare_eq_destroy(self._handle)
            self._handle = ctypes.c_void_p(0)

    def __enter__(self) -> StreamingEqualizer:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _ensure_open(self) -> None:
        if self._handle is None or not self._handle:
            raise RuntimeError("StreamingEqualizer is closed")
