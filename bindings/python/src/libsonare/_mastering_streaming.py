"""Mastering wrappers for libsonare."""

from __future__ import annotations

import contextlib
import ctypes
import json
import math
from collections.abc import Mapping, Sequence

import numpy as np

from ._errors import _not_supported, _require_open
from ._ffi import (
    SonareEqSnapshot,
    SonareStreamingLoudnessGain,
)
from ._mastering_offline import _chain_params
from ._narrowing import _resolve_zero_default
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    SonareValueError,
    _check,
    _check_realtime,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _max_block_size,
    _measured_db,
    _narrow_float,
    _narrow_int,
    _to_c_double,
    _to_c_float,
    _to_c_float_array,
    _to_c_float_array_owned,
    _to_c_int,
    _to_c_size_t,
    _token_ordinal,
    _utf8_arg,
    _validate_samples,
)
from ._types_enums import _EQ_PHASE_MODES, _EQ_STEREO_PLACEMENTS, EqPhaseMode, EqStereoPlacement
from .types import (
    EqBandInput,
    EqSpectrumSnapshot,
    MasteringChainConfig,
    StereoBlock,
    StreamingLoudnessGain,
)


def _streaming_loudness_gain_result(out: SonareStreamingLoudnessGain) -> StreamingLoudnessGain:
    """Copy the C result into the public dataclass."""
    return StreamingLoudnessGain(
        loudness_static_gain_db=float(out.loudness_static_gain_db),
        true_peak_dbtp=_measured_db(out.true_peak_dbtp),
        integrated_lufs=_measured_db(out.integrated_lufs),
    )


@_guard_buffer("samples")
def streaming_loudness_gain(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    config: MasteringChainConfig | None = None,
) -> StreamingLoudnessGain:
    """Measure the loudness numbers a :class:`StreamingMasteringChain` needs.

    Runs the offline chain described by ``config`` up to its loudness stage and
    measures there, so ``loudness_static_gain_db`` equals the gain
    :func:`master_audio` applies (ceiling clamp included) and ``true_peak_dbtp``
    is the peak that clamp used. The gain is computed from the ``loudness``
    section whether or not the stage is enabled; a silent or below-gate stage
    input yields a gain of 0.

    Args:
        samples: Mono audio samples.
        sample_rate: Sample rate in Hz.
        config: Chain config as accepted by :func:`master_audio`.

    Returns:
        :class:`StreamingLoudnessGain`.

    Example::

        gain = streaming_loudness_gain(samples, 48000, config)
        chain = StreamingMasteringChain(
            config,
            sample_rate=48000,
            num_channels=1,
            loudness_static_gain_db=gain.loudness_static_gain_db,
            loudness_static_gain_peak_db=gain.true_peak_dbtp,
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
    sample_rate: int,
    config: MasteringChainConfig | None = None,
) -> StreamingLoudnessGain:
    """Stereo counterpart of :func:`streaming_loudness_gain`.

    The integrated loudness uses BS.1770 channel summing and ``true_peak_dbtp`` is
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
    """Block-by-block streaming variant of :func:`master_audio`.

    Maintains processor state across :meth:`process_mono`/:meth:`process_stereo`
    calls. Only ProcessorBase-backed stages are supported: eq.tilt,
    dynamics.deesser, dynamics.transientShaper, dynamics.compressor,
    dynamics.multibandComp, saturation.tape, saturation.exciter,
    spectral.airBand, stereo.imager (stereo only), stereo.monoMaker (stereo
    only), maximizer.truePeakLimiter. Configurations that enable any of the five
    whole-signal repair stages (``repair.declick``, ``repair.declip``,
    ``repair.decrackle``, ``repair.dehum``, ``repair.dereverb``) or
    ``loudness`` raise :class:`SonareValueError`.

    ``repair.denoise`` runs here, but only with a noise estimator that is
    recursive in time. A stream defaults it to ``"spp"`` (``3``), so
    ``{"repair": {"denoise": {"enabled": True}}}`` prepares. The offline default,
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

        config = {"eq": {"tilt": {"tiltDb": 1.0}}}
        with StreamingMasteringChain(config, sample_rate=44100, num_channels=1) as chain:
            out = chain.process_mono(np.zeros(128, dtype=np.float32))
    """

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    def __init__(
        self,
        config: MasteringChainConfig | None = None,
        *,
        sample_rate: int,
        num_channels: int,
        max_block_size: int | None = None,
        loudness_static_gain_db: float | None = None,
        loudness_static_gain_peak_db: float | None = None,
    ) -> None:
        """Build and prepare a streaming mastering chain.

        Args:
            config: Nested chain config (see :func:`master_audio`).
            sample_rate: Sample rate in Hz.
            num_channels: 1 (mono) or 2 (stereo). Stereo-only stages
                (imager, monoMaker) are skipped for mono.
            max_block_size: Largest block, in samples, any one
                :meth:`process_mono` / :meth:`process_stereo` call passes;
                omitted, the library default applies.
            loudness_static_gain_db: Precomputed loudness normalization gain in
                dB, measured at the loudness stage's input (see
                :func:`streaming_loudness_gain`). The streaming chain cannot
                measure whole-signal integrated LUFS, so a ``loudness``-enabled
                config raises unless a static gain is supplied here; when
                supplied it is applied per block before the loudness stage's
                true-peak limiter.
            loudness_static_gain_peak_db: True peak (dBFS) of the loudness
                stage's input the static gain was computed for. When given, the static
                gain is clamped to ``(ceilingDbtp - peak) +
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
        # Narrowed rather than coerced: int() takes 512.7 as 512, and the block
        # layout remembered here would then differ from the one asked for.
        max_block_value = _resolve_zero_default(max_block_size, "max_block_size")
        channels_value = _narrow_int(num_channels, "num_channels", _C_INT_MIN, _C_INT_MAX)
        param_array, param_count = _chain_params(config)
        handle = ctypes.c_void_p()
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
            rc = lib.sonare_streaming_mastering_chain_create_ex(
                param_array,
                _to_c_size_t(param_count, "param_count"),
                _to_c_float(loudness_static_gain_db, "loudness_static_gain_db"),
                ctypes.c_float(peak),
                ctypes.byref(handle),
            )
        else:
            rc = lib.sonare_streaming_mastering_chain_create(
                param_array, _to_c_size_t(param_count, "param_count"), ctypes.byref(handle)
            )
        _check(rc)
        self._lib = lib
        self._handle = handle
        try:
            rc = lib.sonare_streaming_mastering_chain_prepare(
                self._handle,
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_int(max_block_value, "max_block_size"),
                _to_c_int(channels_value, "num_channels"),
            )
            _check(rc)
        except BaseException:
            self.close()
            raise

    def process_mono(self, samples: Sequence[float] | np.ndarray) -> np.ndarray:
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
        return _from_c_float_array(c_array, length)

    def process_stereo(
        self,
        left: Sequence[float] | np.ndarray,
        right: Sequence[float] | np.ndarray,
    ) -> StereoBlock:
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
        return StereoBlock(
            _from_c_float_array(left_array, left_length),
            _from_c_float_array(right_array, right_length),
        )

    def flush_mono(self) -> np.ndarray:
        """Emit delayed audio and finite processor tails after the final mono block.

        Call until an empty array is returned. The initial ``latency_samples`` of
        the concatenated process/flush output are delayed; discard them when a
        time-aligned result is required. When an enabled stage has an unbounded
        tail, pass a bounded number of silent blocks to :meth:`process_mono`
        instead.

        Raises:
            SonareError: ``NOT_SUPPORTED`` when an enabled stage has an unbounded
                tail; no output is produced.
        """
        self._ensure_flush_ready()
        capacity = self.max_block_size
        block = (ctypes.c_float * capacity)()
        written = ctypes.c_size_t()
        rc = self._lib.sonare_streaming_mastering_chain_flush_mono(
            self._handle,
            block,
            _to_c_size_t(capacity, "capacity"),
            ctypes.byref(written),
        )
        _check(rc)
        return _from_c_float_array(block, written.value)

    def flush_stereo(self) -> StereoBlock:
        """Stereo counterpart of :meth:`flush_mono`."""
        self._ensure_flush_ready()
        capacity = self.max_block_size
        left = (ctypes.c_float * capacity)()
        right = (ctypes.c_float * capacity)()
        written = ctypes.c_size_t()
        rc = self._lib.sonare_streaming_mastering_chain_flush_stereo(
            self._handle,
            left,
            right,
            _to_c_size_t(capacity, "capacity"),
            ctypes.byref(written),
        )
        _check(rc)
        return StereoBlock(
            _from_c_float_array(left, written.value),
            _from_c_float_array(right, written.value),
        )

    def reset(self) -> None:
        """Reset all processor state without rebuilding."""
        self._ensure_open()
        rc = self._lib.sonare_streaming_mastering_chain_reset(self._handle)
        _check(rc)

    def set_parameter(self, key: str, value: float) -> None:
        """Update one realtime-safe full-chain parameter between processing calls.

        ``key`` uses the same full dot
        notation as the constructor, for example ``"eq.tilt.tiltDb"``. The
        native chain rejects unknown, disabled, and structural parameters
        before mutation; a failed call therefore leaves the existing chain
        state and audio history intact.
        Serialize this control operation with processing, flush and reset.
        Parameter changes are not automatically smoothed.
        """
        self._ensure_open()
        if not isinstance(key, str):
            raise TypeError("key must be a string")
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
    def max_block_size(self) -> int:
        """The largest block :meth:`process_mono` / :meth:`process_stereo` accept."""
        self._ensure_open()
        return _max_block_size(
            self._lib.sonare_streaming_mastering_chain_max_block_size, self._handle
        )

    @property
    def latency_samples(self) -> int:
        """Total reported latency in samples across all active processors."""
        return int(
            self._lib.sonare_streaming_mastering_chain_latency_samples(
                _require_open(self._handle, "StreamingMasteringChain")
            )
        )

    @property
    def non_finite_substitution_count(self) -> int:
        """Non-finite samples this chain's stages replaced.

        Advisory telemetry, and the only thing that separates a degraded stream
        from a clean one. A substituting stage replaces a non-finite input
        sample with a finite in-domain one, so the output stays finite, in range
        and free of any error while carrying samples unrelated to the input. A
        non-zero count is what says the samples in between were not computed
        from what was supplied.

        Aggregated over every substituting stage, so it reports that a
        substitution happened without naming the stage. Cumulative over every
        block since construction; :meth:`reset` leaves it alone.
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

    @property
    def non_finite_discard_count(self) -> int:
        """Process/flush calls in which a stage discarded its state.

        The companion to :attr:`non_finite_substitution_count`, and not the
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
        stages. Cumulative over every call since construction, so the two
        counters on one handle share an epoch.
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
        ``dynamics.compressor``, ``maximizer.truePeakLimiter``) for the channel
        count given at construction; stereo-only stages are absent for mono.
        """
        self._ensure_open()
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
        """Return the object itself for ``with`` use."""
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        """Close the object on leaving the ``with`` block; never suppresses exceptions."""
        self.close()

    def __del__(self) -> None:
        # Defensive: __del__ must not raise
        with contextlib.suppress(Exception):
            self.close()

    def _ensure_open(self) -> None:
        _require_open(self._handle, "StreamingMasteringChain")

    def _ensure_flush_ready(self) -> None:
        self._ensure_open()
        if not hasattr(self._lib, "sonare_streaming_mastering_chain_flush_mono"):
            raise _not_supported("libsonare was built without streaming mastering flush support")


class StreamingEqualizer:
    """Block-by-block unified EQ wrapper around the native ``SonareEq`` handle.

    Attributes:
        sample_rate: Sample rate in Hz the equalizer was created with.
    """

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    def __init__(self, *, sample_rate: int, max_block_size: int | None = None) -> None:
        """Create an equalizer; an omitted ``max_block_size`` takes the library default."""
        # Set first so a failed create or a missing build leaves a valid
        # attribute for __del__/close() instead of raising AttributeError.
        self._handle = ctypes.c_void_p(0)
        lib = _get_lib()
        if not hasattr(lib, "sonare_eq_create"):
            raise _not_supported("libsonare was built without streaming equalizer support")
        # Narrowed rather than coerced: the rate reaches the C create as a float
        # but every later call as an int, so int() here would split the two.
        sample_rate_value = _narrow_int(sample_rate, "sample_rate", _C_INT_MIN, _C_INT_MAX)
        max_block_value = _resolve_zero_default(max_block_size, "max_block_size")
        handle = ctypes.c_void_p()
        rc = lib.sonare_eq_create(
            float(sample_rate_value),
            _to_c_int(max_block_value, "max_block_size"),
            ctypes.byref(handle),
        )
        _check(rc)
        self._lib = lib
        self._handle = handle
        self.sample_rate = sample_rate_value
        self._sidechain_refs: object | None = None

    def set_band(self, index: int, band: EqBandInput) -> None:
        """Set one EQ band from an :data:`EqBandInput` mapping."""
        self._ensure_open()
        if not isinstance(band, Mapping):
            raise TypeError("band must be a mapping (parse JSON text with json.loads first)")
        payload = json.dumps(dict(band), separators=(",", ":"))
        rc = self._lib.sonare_eq_set_band(
            self._handle, _to_c_int(index, "index"), _utf8_arg(payload, "payload")
        )
        _check(rc)

    def clear(self) -> None:
        """Clear all EQ bands."""
        self._ensure_open()
        self._lib.sonare_eq_clear(self._handle)

    def set_phase_mode(self, mode: EqPhaseMode) -> None:
        """Set the global phase mode: ``"zero"``, ``"natural"`` or ``"linear"``."""
        self._ensure_open()
        value = _token_ordinal(mode, _EQ_PHASE_MODES, "EQ phase mode") + 1
        _check(self._lib.sonare_eq_set_phase_mode(self._handle, _to_c_int(value, "mode")))

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
    def set_sidechain_mono(self, samples: Sequence[float] | np.ndarray) -> None:
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
        left: Sequence[float] | np.ndarray,
        right: Sequence[float] | np.ndarray,
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
        source: Sequence[float] | np.ndarray,
        reference: Sequence[float] | np.ndarray,
        *,
        max_bands: int = 8,
        sample_rate: int | None = None,
        reference_sample_rate: int | None = None,
    ) -> None:
        """Configure live EQ bands by matching ``source`` to ``reference``.

        The buffers may differ in length and rate. ``sample_rate`` is the rate of
        ``source`` (omitted: the equalizer's own rate) and ``reference_sample_rate``
        the rate of ``reference`` (omitted: ``sample_rate``); a reference at
        another rate is resampled onto the source's before its spectrum is taken.

        Raises:
            SonareValueError: If either buffer is empty or holds a NaN or Inf sample.
        """
        self._ensure_open()
        source_array, source_length = _to_c_float_array(source)
        reference_array, reference_length = _to_c_float_array(reference)
        source_rate = self.sample_rate if sample_rate is None else sample_rate
        rc = self._lib.sonare_eq_match_ex(
            self._handle,
            source_array,
            _to_c_size_t(source_length, "source_length"),
            _to_c_int(source_rate, "sample_rate"),
            reference_array,
            _to_c_size_t(reference_length, "reference_length"),
            _to_c_int(
                source_rate if reference_sample_rate is None else reference_sample_rate,
                "reference_sample_rate",
            ),
            _to_c_int(max_bands, "max_bands"),
        )
        _check(rc)

    def process_mono(self, samples: Sequence[float] | np.ndarray) -> np.ndarray:
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
        return _from_c_float_array(c_array, length)

    def process_stereo(
        self,
        left: Sequence[float] | np.ndarray,
        right: Sequence[float] | np.ndarray,
    ) -> StereoBlock:
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
        return StereoBlock(
            _from_c_float_array(left_array, left_length),
            _from_c_float_array(right_array, right_length),
        )

    def spectrum(self) -> EqSpectrumSnapshot:
        """Return the latest pre/post sample stream and band-gain snapshot."""
        self._ensure_open()
        out = SonareEqSnapshot()
        _check(self._lib.sonare_eq_spectrum(self._handle, ctypes.byref(out)))
        pre_count = int(out.pre_count)
        post_count = int(out.post_count)
        return EqSpectrumSnapshot(
            pre_left=_from_c_float_array(out.pre_left, pre_count),
            pre_right=_from_c_float_array(out.pre_right, pre_count),
            post_left=_from_c_float_array(out.post_left, post_count),
            post_right=_from_c_float_array(out.post_right, post_count),
            # Sized from the ctypes fields themselves, the way the meter
            # telemetry conversion reads its fixed arrays: a literal repeating
            # SONARE_EQ_MAX_BANDS / SONARE_EQ_SPECTRUM_PROFILE_BANDS would keep
            # returning the old length after the C arrays grew.
            band_gain_db=_from_c_float_array(out.band_gain_db, len(out.band_gain_db)),
            profile_db=_from_c_float_array(out.profile_db, len(out.profile_db)),
            last_auto_gain_db=float(out.last_auto_gain_db),
            seq=int(out.seq),
        )

    def magnitude_response(
        self,
        frequencies_hz: Sequence[float] | np.ndarray,
        *,
        placement: EqStereoPlacement = "stereo",
    ) -> np.ndarray:
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

        An empty array is a defined request and comes back as an empty curve:
        the mapping is per frequency, so there is nothing to ask about.

        Raises:
            SonareValueError: If ``frequencies_hz`` holds a NaN or Inf value.
        """
        self._ensure_open()
        if not hasattr(self._lib, "sonare_eq_magnitude_response"):
            raise _not_supported("libsonare was built without EQ magnitude response support")
        ordinal = _token_ordinal(placement, _EQ_STEREO_PLACEMENTS, "EQ band placement")
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
        return _from_c_float_array(out, count)

    @property
    def max_block_size(self) -> int:
        """The largest block one ``process_*`` call accepts."""
        self._ensure_open()
        return _max_block_size(self._lib.sonare_eq_max_block_size, self._handle)

    @property
    def latency_samples(self) -> int:
        """Processing latency in samples. Raises SonareError (InvalidState) once closed."""
        self._ensure_open()
        return int(self._lib.sonare_eq_latency_samples(self._handle))

    @property
    def last_auto_gain_db(self) -> float:
        """Automatic output gain (dB) applied by the latest process call.

        Raises SonareError (InvalidState) once closed.
        """
        self._ensure_open()
        return float(self._lib.sonare_eq_last_auto_gain_db(self._handle))

    @property
    def non_finite_discard_count(self) -> int:
        """Blocks in which the equalizer discarded recursive state.

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
        _check_realtime(
            self._lib.sonare_eq_non_finite_discard_count(self._handle, ctypes.byref(out))
        )
        return int(out.value)

    def close(self) -> None:
        """Release the underlying C handle. Safe to call multiple times."""
        if self._handle is not None and self._handle:
            self._lib.sonare_eq_destroy(self._handle)
            self._handle = ctypes.c_void_p(0)

    def __enter__(self) -> StreamingEqualizer:
        """Return the object itself for ``with`` use."""
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        """Close the object on leaving the ``with`` block; never suppresses exceptions."""
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _ensure_open(self) -> None:
        _require_open(self._handle, "StreamingEqualizer")
