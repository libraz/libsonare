"""Mastering wrappers for libsonare."""

from __future__ import annotations

import contextlib
import ctypes
import json
import math
from collections.abc import Sequence
from typing import Any

from ._errors import _invalid_state, _not_supported
from ._ffi import (
    SonareEqSnapshot,
    SonareStreamingLoudnessGain,
)
from ._mastering_offline import _chain_params
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    ErrorCode,
    SonareValueError,
    _check,
    _check_realtime,
    _float_array_result,
    _get_lib,
    _guard_buffer,
    _last_error,
    _narrow_float,
    _narrow_int,
    _to_c_double,
    _to_c_float,
    _to_c_float_array,
    _to_c_float_array_owned,
    _to_c_int,
    _to_c_size_t,
    _utf8_arg,
    _validate_samples,
)
from .types import (
    EqSpectrumSnapshot,
    StreamingLoudnessGain,
)


def _streaming_loudness_gain_result(out: SonareStreamingLoudnessGain) -> StreamingLoudnessGain:
    """Copy the C result into the public dataclass."""
    return StreamingLoudnessGain(
        loudness_static_gain_db=float(out.loudness_static_gain_db),
        true_peak_db=float(out.true_peak_db),
        integrated_lufs=float(out.integrated_lufs),
    )


@_guard_buffer("samples")
def streaming_loudness_gain(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    config: dict[str, Any] | None = None,
) -> StreamingLoudnessGain:
    """Measure the loudness numbers a :class:`StreamingMasteringChain` needs.

    Runs the offline chain described by ``config`` up to its loudness stage and
    measures there, so ``loudness_static_gain_db`` equals the gain
    :func:`mastering_chain` applies (ceiling clamp included) and ``true_peak_db``
    is the peak that clamp used. The gain is computed from the ``loudness``
    section whether or not the stage is enabled; a silent or below-gate stage
    input yields a gain of 0.

    Args:
        samples: Mono audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        config: Chain config as accepted by :func:`mastering_chain`.

    Returns:
        :class:`StreamingLoudnessGain`.

    Example::

        gain = streaming_loudness_gain(samples, 48000, config)
        chain = StreamingMasteringChain(
            config,
            loudness_static_gain_db=gain.loudness_static_gain_db,
            loudness_static_gain_peak_db=gain.true_peak_db,
        )
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_streaming_loudness_gain"):
        raise _not_supported("libsonare was built without streaming loudness gain support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _chain_params(config)
    out = SonareStreamingLoudnessGain()
    rc = lib.sonare_streaming_loudness_gain(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        param_array,
        _to_c_size_t(param_count, "param_count"),
        ctypes.byref(out),
    )
    _check(rc)
    return _streaming_loudness_gain_result(out)


@_guard_buffer("left", "right")
def streaming_loudness_gain_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    config: dict[str, Any] | None = None,
) -> StreamingLoudnessGain:
    """Stereo counterpart of :func:`streaming_loudness_gain`.

    The integrated loudness uses BS.1770 channel summing and ``true_peak_db`` is
    the larger of the two channel peaks.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_streaming_loudness_gain_stereo"):
        raise _not_supported("libsonare was built without streaming loudness gain support")
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    param_array, param_count = _chain_params(config)
    out = SonareStreamingLoudnessGain()
    rc = lib.sonare_streaming_loudness_gain_stereo(
        left_array,
        right_array,
        _to_c_size_t(left_length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        param_array,
        _to_c_size_t(param_count, "param_count"),
        ctypes.byref(out),
    )
    _check(rc)
    return _streaming_loudness_gain_result(out)


class StreamingMasteringChain:
    """Block-by-block streaming variant of :func:`mastering_chain`.

    Maintains processor state across :meth:`process_mono`/:meth:`process_stereo`
    calls. Only ProcessorBase-backed stages are supported: eq.tilt,
    dynamics.deesser, dynamics.transientShaper, dynamics.compressor,
    dynamics.multibandComp, saturation.tape, saturation.exciter,
    spectral.airBand, stereo.imager (stereo only), stereo.monoMaker (stereo
    only), maximizer.truePeakLimiter. Configurations that enable any of the five
    whole-signal repair stages (``repair.declick``, ``repair.declip``,
    ``repair.decrackle``, ``repair.dehum``, ``repair.dereverb``) or
    ``loudness`` raise :class:`SonareError` (code ``INVALID_PARAMETER``).

    ``repair.denoise`` runs here, but only with a noise estimator that is
    recursive in time. A stream defaults it to ``"spp"`` (``3``), so
    ``{"repair.denoise.enabled": True}`` prepares. The offline default,
    ``"quantile"`` (``0``), ranks every frame of the whole signal, which a
    stream never reaches the end of, so it is refused by name rather than
    substituted; ``"mcra"`` (``1``) and ``"imcra"`` (``2``) are accepted too.
    The two minimum-tracking estimators
    (``1`` and ``2``) seed their noise floor from the first frame they see and
    hold it for the half second their minimum window spans, so a stream opened
    in the middle of the programme is over-suppressed until it turns over;
    ``3`` tracks no minimum and is unaffected. Prefer ``3`` past that opening
    too: ``1`` and ``2`` over-report the floor for as long as the programme
    stays intermittent, and on a gated tone they leave the result below the
    untreated input.

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
                dB, measured at the loudness stage's input (see
                :func:`streaming_loudness_gain`). The streaming chain cannot
                measure whole-signal integrated LUFS, so a ``loudness``-enabled
                config raises unless a static gain is supplied here; when
                supplied it is applied per block before the loudness stage's
                true-peak limiter.
            loudness_static_gain_peak_db: True peak (dBFS) of the loudness
                stage's input the static gain was computed for. When given, the static
                gain is clamped to ``(ceiling_db - peak) +
                max(max_limiter_gain_reduction_db, 0)`` so the streaming preview
                does not overdrive the loudness limiter harder than the offline
                render. Ignored unless ``loudness_static_gain_db`` is given.
        """
        # Set first so a rejected config or a missing build leaves a valid
        # attribute for __del__/close() instead of raising AttributeError.
        self._handle = ctypes.c_void_p(0)
        lib = _get_lib()
        if not hasattr(lib, "sonare_streaming_mastering_chain_create"):
            raise _not_supported("libsonare was built without streaming mastering chain support")
        param_array, param_count = _chain_params(config)
        if loudness_static_gain_db is not None:
            if not hasattr(lib, "sonare_streaming_mastering_chain_create_ex"):
                raise _not_supported(
                    "libsonare was built without streaming loudness static-gain support"
                )
            # NaN is how the C ABI spells "no offline peak measured", so the
            # caller's own value is narrowed here rather than at the conversion:
            # a saturating one would arrive as that same reading.
            peak = (
                math.nan
                if loudness_static_gain_peak_db is None
                else _narrow_float(loudness_static_gain_peak_db, "loudness_static_gain_peak_db")
            )
            handle = lib.sonare_streaming_mastering_chain_create_ex(
                param_array,
                _to_c_size_t(param_count, "param_count"),
                _to_c_float(loudness_static_gain_db, "loudness_static_gain_db"),
                ctypes.c_float(peak),
            )
        else:
            handle = lib.sonare_streaming_mastering_chain_create(
                param_array, _to_c_size_t(param_count, "param_count")
            )
        if not handle:
            raise _last_error(
                int(ErrorCode.INVALID_PARAMETER), "failed to create StreamingMasteringChain"
            )
        self._lib = lib
        self._handle = ctypes.c_void_p(handle)
        self._max_block_size = 0

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
        # Narrowed rather than coerced: int() takes 512.7 as 512, and the block
        # layout remembered here would then differ from the one asked for.
        max_block_value = _narrow_int(max_block_size, "max_block_size", _C_INT_MIN, _C_INT_MAX)
        channels_value = _narrow_int(num_channels, "num_channels", _C_INT_MIN, _C_INT_MAX)
        rc = self._lib.sonare_streaming_mastering_chain_prepare(
            self._handle,
            _to_c_int(sample_rate, "sample_rate"),
            _to_c_int(max_block_value, "max_block_size"),
            _to_c_int(channels_value, "num_channels"),
        )
        _check(rc)
        self._max_block_size = max_block_value

    def process_mono(self, samples: Sequence[float] | list[float]) -> list[float]:
        """Process one mono block, returning the processed samples (length unchanged).

        The input is never modified: the C call processes in place over a private
        copy, so a caller passing a ``float32`` ndarray keeps their dry signal.
        """
        self._ensure_open()
        c_array, length = _to_c_float_array_owned(samples)
        rc = self._lib.sonare_streaming_mastering_chain_process_mono(
            self._handle, c_array, _to_c_size_t(length, "length")
        )
        _check(rc)
        return _float_array_result(c_array, length)

    def process_stereo(
        self,
        left: Sequence[float] | list[float],
        right: Sequence[float] | list[float],
    ) -> tuple[list[float], list[float]]:
        """Process one stereo block, returning the processed (left, right) channels.

        Neither input channel is modified; the C call processes in place over
        private copies.
        """
        self._ensure_open()
        left_array, left_length = _to_c_float_array_owned(left, arg_name="left")
        right_array, right_length = _to_c_float_array_owned(right, arg_name="right")
        if left_length != right_length:
            raise SonareValueError("left and right channel lengths must match")
        rc = self._lib.sonare_streaming_mastering_chain_process_stereo(
            self._handle, left_array, right_array, _to_c_size_t(left_length, "left_length")
        )
        _check(rc)
        return (
            _float_array_result(left_array, left_length),
            _float_array_result(right_array, right_length),
        )

    def flush_mono(self) -> list[float]:
        """Emit delayed audio and finite processor tails after the final mono block.

        Call until an empty list is returned. The initial ``latency_samples`` of
        the concatenated process/flush output are delayed; discard them when a
        time-aligned result is required.
        """
        self._ensure_flush_ready()
        block = (ctypes.c_float * self._max_block_size)()
        written = ctypes.c_size_t()
        rc = self._lib.sonare_streaming_mastering_chain_flush_mono(
            self._handle,
            block,
            _to_c_size_t(self._max_block_size, "max_block_size"),
            ctypes.byref(written),
        )
        _check(rc)
        return _float_array_result(block, written.value)

    def flush_stereo(self) -> tuple[list[float], list[float]]:
        """Stereo counterpart of :meth:`flush_mono`."""
        self._ensure_flush_ready()
        left = (ctypes.c_float * self._max_block_size)()
        right = (ctypes.c_float * self._max_block_size)()
        written = ctypes.c_size_t()
        rc = self._lib.sonare_streaming_mastering_chain_flush_stereo(
            self._handle,
            left,
            right,
            _to_c_size_t(self._max_block_size, "max_block_size"),
            ctypes.byref(written),
        )
        _check(rc)
        return (
            _float_array_result(left, written.value),
            _float_array_result(right, written.value),
        )

    def reset(self) -> None:
        """Reset all processor state without rebuilding."""
        self._ensure_open()
        rc = self._lib.sonare_streaming_mastering_chain_reset(self._handle)
        _check(rc)

    def set_parameter(self, key: str, value: float) -> None:
        """Update one realtime-safe full-chain parameter between processing calls.

        The chain must be prepared first; an argument error is reported before
        the unprepared state, as on the other bindings. ``key`` uses the same full dot
        notation as the constructor, for example ``"eq.tilt.tiltDb"``. The
        native chain rejects unknown, disabled, and structural parameters
        before mutation; a failed call therefore leaves the existing chain
        state and audio history intact.
        Serialize this control operation with processing, flush, reset and
        prepare. Parameter changes are not automatically smoothed.
        """
        self._ensure_open()
        if not isinstance(key, str):
            raise SonareValueError("key must be a string")
        if not key:
            raise SonareValueError("key must not be empty")
        if not hasattr(self._lib, "sonare_streaming_mastering_chain_set_parameter"):
            raise _not_supported("libsonare was built without streaming parameter support")
        key_bytes = _utf8_arg(key, "key")
        rc = self._lib.sonare_streaming_mastering_chain_set_parameter(
            self._handle,
            key_bytes,
            _to_c_double(value, "value"),
        )
        _check(rc)

    @property
    def latency_samples(self) -> int:
        """Total reported latency in samples across all active processors."""
        if self._handle is None or not self._handle:
            return 0
        return int(self._lib.sonare_streaming_mastering_chain_latency_samples(self._handle))

    def non_finite_substitution_count(self) -> int:
        """Return the non-finite samples this chain's stages replaced.

        Advisory telemetry, and the only thing that separates a degraded stream
        from a clean one. A substituting stage replaces a non-finite input
        sample with a finite in-domain one, so the output stays finite, in range
        and free of any error while carrying samples unrelated to the input. A
        non-zero count is what says the samples in between were not computed
        from what was supplied.

        Aggregated over every substituting stage, so it reports that a
        substitution happened without naming the stage. Cumulative over every
        block since the last :meth:`prepare`, which rebuilds the stages and so
        clears it; :meth:`reset` leaves it alone.
        """
        self._ensure_open()
        if not hasattr(self._lib, "sonare_streaming_mastering_chain_non_finite_substitution_count"):
            raise _not_supported(
                "libsonare was built without streaming mastering substitution telemetry"
            )
        out = ctypes.c_uint32()
        rc = self._lib.sonare_streaming_mastering_chain_non_finite_substitution_count(
            self._handle, ctypes.byref(out)
        )
        _check_realtime(rc)
        return int(out.value)

    def non_finite_discard_count(self) -> int:
        """Return the process/flush calls in which a stage discarded its state.

        The companion to :meth:`non_finite_substitution_count`, and not the
        same measurement: that one counts samples a stage replaced and so
        sums across stages, while a discard is a whole stage returning to its
        post-reset value and is counted once per call however many stages did
        it. A stage may run more than once per call, which is why this number
        is a delta over the call and never a sum.

        Non-finite input is rejected before any stage runs, so what a stage
        discards is always state it produced itself -- a finite sample large
        enough to overflow inside a filter, most often. Unlike the
        substitution count every stage can contribute, so a zero here means
        no stage discarded rather than that none could.

        Both process and flush calls count, since a flush drives the same
        stages. Cumulative over every call since the last :meth:`prepare`,
        which rebuilds the stages and so clears it, so the two counters on
        one handle share an epoch.
        """
        self._ensure_open()
        if not hasattr(self._lib, "sonare_streaming_mastering_chain_non_finite_discard_count"):
            raise _not_supported(
                "libsonare was built without streaming mastering discard telemetry"
            )
        out = ctypes.c_uint32()
        rc = self._lib.sonare_streaming_mastering_chain_non_finite_discard_count(
            self._handle, ctypes.byref(out)
        )
        _check_realtime(rc)
        return int(out.value)

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
            raise _invalid_state("StreamingMasteringChain is closed")

    def _ensure_flush_ready(self) -> None:
        self._ensure_open()
        if self._max_block_size <= 0:
            raise _invalid_state("StreamingMasteringChain must be prepared before flush")
        if not hasattr(self._lib, "sonare_streaming_mastering_chain_flush_mono"):
            raise _not_supported("libsonare was built without streaming mastering flush support")


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

    _PLACEMENTS = {
        "stereo": 0,
        "left": 1,
        "right": 2,
        "mid": 3,
        "side": 4,
    }

    def __init__(self, sample_rate: int = 48000, max_block_size: int = 512) -> None:
        # Set first so a failed create or a missing build leaves a valid
        # attribute for __del__/close() instead of raising AttributeError.
        self._handle = ctypes.c_void_p(0)
        lib = _get_lib()
        if not hasattr(lib, "sonare_eq_create"):
            raise _not_supported("libsonare was built without streaming equalizer support")
        # Narrowed rather than coerced: the rate reaches the C create as a float
        # but every later call as an int, so int() here would split the two.
        sample_rate_value = _narrow_int(sample_rate, "sample_rate", _C_INT_MIN, _C_INT_MAX)
        max_block_value = _narrow_int(max_block_size, "max_block_size", _C_INT_MIN, _C_INT_MAX)
        handle = lib.sonare_eq_create(
            float(sample_rate_value), _to_c_int(max_block_value, "max_block_size")
        )
        if not handle:
            raise _last_error(
                int(ErrorCode.INVALID_PARAMETER), "failed to create StreamingEqualizer"
            )
        self._lib = lib
        self._handle = ctypes.c_void_p(handle)
        self.sample_rate = sample_rate_value
        self.max_block_size = max_block_value
        self._sidechain_refs: object | None = None

    def set_band(self, index: int, band: dict[str, Any] | str) -> None:
        """Set one EQ band from a JSON string or a Python dict."""
        self._ensure_open()
        payload = band if isinstance(band, str) else json.dumps(band, separators=(",", ":"))
        rc = self._lib.sonare_eq_set_band(
            self._handle, _to_c_int(index, "index"), _utf8_arg(payload, "payload")
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
                raise SonareValueError(f"unknown EQ phase mode: {mode}")
            value = self._PHASES[key]
        else:
            value = _narrow_int(mode, "mode", _C_INT_MIN, _C_INT_MAX)
        _check(self._lib.sonare_eq_set_phase_mode(self._handle, _to_c_int(value, "value")))

    def set_auto_gain(self, enabled: bool) -> None:
        """Enable or disable auto-gain compensation."""
        self._ensure_open()
        self._lib.sonare_eq_set_auto_gain(self._handle, ctypes.c_int(1 if enabled else 0))

    def set_gain_scale(self, scale: float) -> None:
        """Set all-band EQ gain scale as a 0.0..2.0 multiplier."""
        self._ensure_open()
        _check(self._lib.sonare_eq_set_gain_scale(self._handle, _to_c_float(scale, "scale")))

    def set_output_gain_db(self, gain_db: float) -> None:
        """Set post-EQ output gain in dB."""
        self._ensure_open()
        _check(
            self._lib.sonare_eq_set_output_gain_db(self._handle, _to_c_float(gain_db, "gain_db"))
        )

    def set_output_pan(self, pan: float) -> None:
        """Set post-EQ stereo balance in -1.0..1.0; mono input ignores pan."""
        self._ensure_open()
        _check(self._lib.sonare_eq_set_output_pan(self._handle, _to_c_float(pan, "pan")))

    @_guard_buffer("samples")
    def set_sidechain_mono(self, samples: Sequence[float] | list[float]) -> None:
        """Set a mono external key for dynamic bands with ``externalSidechain`` enabled.

        Raises:
            SonareValueError: If ``samples`` is empty or holds a NaN or Inf sample.
        """
        self._ensure_open()
        c_array, length = _to_c_float_array(samples)
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 1
        channels = channel_array_type(ctypes.cast(c_array, ctypes.POINTER(ctypes.c_float)))
        _check(
            self._lib.sonare_eq_set_sidechain(
                self._handle, channels, ctypes.c_int(1), _to_c_int(length, "length")
            )
        )
        self._sidechain_refs = (c_array, channels)

    @_guard_buffer("left", "right")
    def set_sidechain_stereo(
        self,
        left: Sequence[float] | list[float],
        right: Sequence[float] | list[float],
    ) -> None:
        """Set a stereo external key for dynamic bands with ``externalSidechain`` enabled.

        Raises:
            SonareValueError: If either channel is empty or holds a NaN or Inf sample.
        """
        self._ensure_open()
        left_array, left_length = _to_c_float_array(left)
        right_array, right_length = _to_c_float_array(right)
        if left_length != right_length:
            raise SonareValueError("left and right sidechain lengths must match")
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 2
        channels = channel_array_type(
            ctypes.cast(left_array, ctypes.POINTER(ctypes.c_float)),
            ctypes.cast(right_array, ctypes.POINTER(ctypes.c_float)),
        )
        _check(
            self._lib.sonare_eq_set_sidechain(
                self._handle, channels, ctypes.c_int(2), _to_c_int(left_length, "left_length")
            )
        )
        self._sidechain_refs = (left_array, right_array, channels)

    def clear_sidechain(self) -> None:
        """Clear any pending external key buffer."""
        self._ensure_open()
        self._lib.sonare_eq_clear_sidechain(self._handle)
        self._sidechain_refs = None

    @_guard_buffer("source", "reference")
    def match(
        self,
        source: Sequence[float] | list[float],
        reference: Sequence[float] | list[float],
        max_bands: int = 8,
    ) -> None:
        """Configure live EQ bands by matching ``source`` to ``reference``.

        Raises:
            SonareValueError: If either buffer is empty or holds a NaN or Inf sample.
        """
        self._ensure_open()
        source_array, source_length = _to_c_float_array(source)
        reference_array, reference_length = _to_c_float_array(reference)
        if source_length != reference_length:
            raise SonareValueError("source and reference lengths must match")
        rc = self._lib.sonare_eq_match(
            self._handle,
            source_array,
            reference_array,
            _to_c_size_t(source_length, "source_length"),
            _to_c_int(self.sample_rate, "sample_rate"),
            _to_c_int(max_bands, "max_bands"),
        )
        _check(rc)

    def process_mono(self, samples: Sequence[float] | list[float]) -> list[float]:
        """Process one mono block, returning processed samples.

        The input is never modified; the C call processes in place over a
        private copy.
        """
        self._ensure_open()
        c_array, length = _to_c_float_array_owned(samples)
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 1
        channels = channel_array_type(ctypes.cast(c_array, ctypes.POINTER(ctypes.c_float)))
        _check(
            self._lib.sonare_eq_process(
                self._handle, channels, ctypes.c_int(1), _to_c_int(length, "length")
            )
        )
        self._sidechain_refs = None
        return _float_array_result(c_array, length)

    def process_stereo(
        self,
        left: Sequence[float] | list[float],
        right: Sequence[float] | list[float],
    ) -> tuple[list[float], list[float]]:
        """Process one stereo block, returning the processed (left, right) channels.

        Neither input channel is modified; the C call processes in place over
        private copies.
        """
        self._ensure_open()
        left_array, left_length = _to_c_float_array_owned(left, arg_name="left")
        right_array, right_length = _to_c_float_array_owned(right, arg_name="right")
        if left_length != right_length:
            raise SonareValueError("left and right channel lengths must match")
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 2
        channels = channel_array_type(
            ctypes.cast(left_array, ctypes.POINTER(ctypes.c_float)),
            ctypes.cast(right_array, ctypes.POINTER(ctypes.c_float)),
        )
        _check(
            self._lib.sonare_eq_process(
                self._handle, channels, ctypes.c_int(2), _to_c_int(left_length, "left_length")
            )
        )
        self._sidechain_refs = None
        return (
            _float_array_result(left_array, left_length),
            _float_array_result(right_array, right_length),
        )

    def spectrum(self) -> EqSpectrumSnapshot:
        """Return the latest pre/post sample stream and band-gain snapshot."""
        self._ensure_open()
        out = SonareEqSnapshot()
        _check(self._lib.sonare_eq_spectrum(self._handle, ctypes.byref(out)))
        pre_count = int(out.pre_count)
        post_count = int(out.post_count)
        return EqSpectrumSnapshot(
            pre_left=_float_array_result(out.pre_left, pre_count),
            pre_right=_float_array_result(out.pre_right, pre_count),
            post_left=_float_array_result(out.post_left, post_count),
            post_right=_float_array_result(out.post_right, post_count),
            # Sized from the ctypes fields themselves, the way the meter
            # telemetry conversion reads its fixed arrays: a literal repeating
            # SONARE_EQ_MAX_BANDS / SONARE_EQ_SPECTRUM_PROFILE_BANDS would keep
            # returning the old length after the C arrays grew.
            band_gain_db=_float_array_result(out.band_gain_db, len(out.band_gain_db)),
            profile_db=_float_array_result(out.profile_db, len(out.profile_db)),
            last_auto_gain_db=float(out.last_auto_gain_db),
            seq=int(out.seq),
        )

    def magnitude_response(
        self,
        frequencies_hz: Sequence[float] | list[float],
        *,
        placement: str = "stereo",
    ) -> list[float]:
        """Return the composite magnitude of the bands, in dB, at each frequency.

        The curve to draw over an analyzer. Built from the same coefficient
        design the audio path uses, so it states what the equalizer does rather
        than what its settings look like, and it carries the output gain, the
        gain scale, and whatever each dynamic band is applying at the moment of
        the call. Disabled, bypassed and -- when anything is soloed -- unsoloed
        bands drop out. Linear-phase bands are included: the phase mode changes
        the phase, not the magnitude.

        ``placement`` names the signal path the curve is for: ``"stereo"``,
        ``"left"``, ``"right"``, ``"mid"`` or ``"side"``. A band placed on
        stereo is on every path; one placed elsewhere appears only on its own.
        Each frequency is clamped to [0 Hz, Nyquist].

        An empty list is a defined request and comes back as an empty curve:
        the mapping is per frequency, so there is nothing to ask about.

        Raises:
            SonareValueError: If ``frequencies_hz`` holds a NaN or Inf value.
        """
        self._ensure_open()
        if not hasattr(self._lib, "sonare_eq_magnitude_response"):
            raise _not_supported("libsonare was built without EQ magnitude response support")
        key = placement.lower() if isinstance(placement, str) else ""
        ordinal = self._PLACEMENTS.get(key)
        if ordinal is None:
            raise SonareValueError(f"unknown EQ band placement: {placement}")
        # Non-finite only: an empty curve is a result, a NaN frequency is not.
        frequencies = _validate_samples(
            "magnitude_response", frequencies_hz, arg_name="frequencies_hz", allow_empty=True
        )
        c_frequencies, count = _to_c_float_array(frequencies)
        out = (ctypes.c_float * count)()
        _check(
            self._lib.sonare_eq_magnitude_response(
                self._handle,
                _to_c_int(ordinal, "ordinal"),
                c_frequencies,
                _to_c_size_t(count, "count"),
                out,
            )
        )
        return _float_array_result(out, count)

    @property
    def latency_samples(self) -> int:
        self._ensure_open()
        return int(self._lib.sonare_eq_latency_samples(self._handle))

    @property
    def last_auto_gain_db(self) -> float:
        self._ensure_open()
        return float(self._lib.sonare_eq_last_auto_gain_db(self._handle))

    def non_finite_discard_count(self) -> int:
        """Return the blocks in which the equalizer discarded recursive state.

        Advisory telemetry, and the only thing that separates a degraded EQ
        from a clean one. A discard returns the affected filter cells to
        their post-reset value, so the EQ recovers in silence and the output
        stays finite and in range while carrying samples unrelated to the
        input; nothing else reports that this happened.

        Covers every IIR plane the band layout uses -- stereo, per channel,
        and mid/side -- together with the automatic output gain and the
        detector state the dynamic bands drive. Linear-phase bands are not
        included and have nothing to include: an FIR keeps no recursive
        state, so a non-finite sample leaves its history on its own.

        The unit is one processed block, never a channel and never a plane,
        so a stereo block that discards on both adds one. Cumulative since
        the handle was created and never cleared, so two readings bracket a
        span of audio.
        """
        self._ensure_open()
        if not hasattr(self._lib, "sonare_eq_non_finite_discard_count"):
            raise _not_supported("libsonare was built without EQ discard-count support")
        out = ctypes.c_uint32()
        _check(self._lib.sonare_eq_non_finite_discard_count(self._handle, ctypes.byref(out)))
        return int(out.value)

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
            raise _invalid_state("StreamingEqualizer is closed")
