"""Mixing wrappers for libsonare."""

from __future__ import annotations

import ctypes
import typing
from collections.abc import Sequence

from ._ffi import SonareMixGoniometerPoint, SonareSurroundPan
from ._runtime import (
    AutomationCurve,
    MeterTap,
    MixMeterSnapshot,
    MixResult,
    PanLaw,
    SendTiming,
    SonareMixMeterSnapshot,
    _check,
    _curve_value,
    _get_lib,
    _meter_tap_value,
    _mix_meter_from_c,
    _pan_law_value,
    _pan_mode_value,
    _send_timing_value,
    _to_c_float_array,
)
from .types import GoniometerPoint

# A strip is addressed either by its index in [0, strip_count()) or by its
# string id (as declared in the scene JSON).
StripRef = int | str


class MixerStereoResult(typing.NamedTuple):
    """Stereo master output of :meth:`Mixer.process_stereo`.

    Mirrors the Node/WASM ``{left, right, sampleRate}`` result shape.
    """

    left: list[float]
    right: list[float]
    sample_rate: int


def mixing_scene_preset_names() -> list[str]:
    """Return built-in mixer scene preset identifiers."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mixing_scene_preset_names"):
        raise RuntimeError("libsonare was built without mixing support")
    raw = lib.sonare_mixing_scene_preset_names()
    return raw.decode("utf-8").splitlines() if raw else []


def mixing_scene_preset_json(preset_name: str) -> str:
    """Return the JSON scene template for a built-in mixer preset."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mixing_scene_preset_json"):
        raise RuntimeError("libsonare was built without mixing support")
    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mixing_scene_preset_json(preset_name.encode("utf-8"), ctypes.byref(json_ptr))
    _check(rc)
    try:
        return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


class Mixer:
    """Scene-based persistent stereo mixer.

    Built from a scene JSON string (see :func:`mixing_scene_preset_json`), it
    routes per-strip stereo blocks through a compiled routing graph (sends,
    buses, inserts) into a stereo master.

    Strips are addressed by index in ``[0, strip_count())`` or by their string
    id. Strip handles are *borrowed*: they are owned by the native mixer and
    must not outlive it. This wrapper never exposes a raw handle and always
    resolves a fresh handle from the live mixer for each operation, so a strip
    method can only run while the mixer is open (:meth:`close` invalidates
    every strip). Do not retain results across a :meth:`close` call.
    """

    def __init__(
        self,
        handle: int,
        sample_rate: int,
        block_size: int,
        scene_warnings: list[str] | None = None,
    ) -> None:
        self._handle: int | None = handle
        self._sample_rate = sample_rate
        self._block_size = block_size
        self._scene_warnings: list[str] = scene_warnings or []

    @classmethod
    def from_scene_json(cls, json: str, sample_rate: int = 48000, block_size: int = 512) -> Mixer:
        """Build a mixer from a scene JSON string."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_from_scene_json"):
            raise RuntimeError("libsonare was built without mixing support")
        handle = lib.sonare_mixer_from_scene_json(
            json.encode("utf-8"), ctypes.c_int(sample_rate), ctypes.c_int(block_size)
        )
        if not handle:
            raise RuntimeError("failed to build mixer from scene JSON")
        # Capture any non-fatal load warning (e.g. insert params no processor
        # read) immediately, before any later C-ABI call overwrites it.
        warnings: list[str] = []
        if hasattr(lib, "sonare_last_warning_message"):
            raw = lib.sonare_last_warning_message()
            if raw:
                warnings = raw.decode("utf-8").splitlines()
        return cls(int(handle), sample_rate, block_size, warnings)

    def scene_warnings(self) -> list[str]:
        """Non-fatal warnings captured when this mixer was built from scene JSON.

        One entry per channel-strip insert that was handed param keys it does not
        read (a likely typo, or a key meant for a different processor). The scene
        still loaded; those keys simply took no effect. Empty when every key was
        consumed. Use :func:`mastering_insert_param_names` to discover the keys an
        insert accepts.
        """
        return list(self._scene_warnings)

    def compile(self) -> None:
        """Rebuild and compile the routing graph from the current scene."""
        self._require()
        _check(_get_lib().sonare_mixer_compile(self._handle))

    def strip_count(self) -> int:
        """Return the number of strips in the mixer."""
        self._require()
        lib = _get_lib()
        if hasattr(lib, "sonare_mixer_get_strip_count"):
            out = ctypes.c_size_t()
            _check(lib.sonare_mixer_get_strip_count(self._handle, ctypes.byref(out)))
            return int(out.value)
        if not hasattr(lib, "sonare_mixer_strip_count"):
            raise RuntimeError("libsonare was built without insert-automation support")
        return int(lib.sonare_mixer_strip_count(self._handle))

    def add_bus(self, bus_id: str, role: str = "aux") -> None:
        """Add a bus to the mixer topology.

        ``role`` is one of ``"master"``, ``"aux"``, or ``"submix"`` and defaults
        to ``"aux"`` (the C-API canonical default applied for a null role in
        ``sonare_mixer_add_bus``). The routing graph is marked dirty; call
        :meth:`compile` (or :meth:`process_stereo`) to rebuild.
        """
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_add_bus"):
            raise RuntimeError("libsonare was built without mixer bus support")
        _check(
            lib.sonare_mixer_add_bus(
                self._handle,
                bus_id.encode("utf-8"),
                role.encode("utf-8") if role is not None else None,
            )
        )

    def remove_bus(self, bus_id: str) -> None:
        """Remove a bus by id from the mixer topology."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_remove_bus"):
            raise RuntimeError("libsonare was built without mixer bus support")
        _check(lib.sonare_mixer_remove_bus(self._handle, bus_id.encode("utf-8")))

    def bus_count(self) -> int:
        """Return the number of buses in the mixer topology."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_bus_count"):
            raise RuntimeError("libsonare was built without mixer bus support")
        out = ctypes.c_size_t()
        _check(lib.sonare_mixer_bus_count(self._handle, ctypes.byref(out)))
        return int(out.value)

    def add_vca_group(
        self, group_id: str, gain_db: float = 0.0, members: Sequence[str] | None = None
    ) -> None:
        """Add a VCA group with the given id, gain offset, and strip members."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_add_vca_group"):
            raise RuntimeError("libsonare was built without mixer VCA support")
        member_list = list(members or [])
        if member_list:
            member_array = (ctypes.c_char_p * len(member_list))(
                *[m.encode("utf-8") for m in member_list]
            )
            member_ptr = ctypes.cast(member_array, ctypes.POINTER(ctypes.c_char_p))
        else:
            member_ptr = None
        _check(
            lib.sonare_mixer_add_vca_group(
                self._handle,
                group_id.encode("utf-8"),
                ctypes.c_float(gain_db),
                member_ptr,
                ctypes.c_size_t(len(member_list)),
            )
        )

    def remove_vca_group(self, group_id: str) -> None:
        """Remove a VCA group by id from the mixer topology."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_remove_vca_group"):
            raise RuntimeError("libsonare was built without mixer VCA support")
        _check(lib.sonare_mixer_remove_vca_group(self._handle, group_id.encode("utf-8")))

    def set_vca_group_gain_db(self, group_id: str, gain_db: float) -> None:
        """Set an existing VCA group's gain in dB."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_set_vca_group_gain_db"):
            raise RuntimeError("libsonare was built without mixer VCA support")
        _check(
            lib.sonare_mixer_set_vca_group_gain_db(
                self._handle,
                group_id.encode("utf-8"),
                ctypes.c_float(gain_db),
            )
        )

    def vca_group_count(self) -> int:
        """Return the number of VCA groups in the mixer topology."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_vca_group_count"):
            raise RuntimeError("libsonare was built without mixer VCA support")
        out = ctypes.c_size_t()
        _check(lib.sonare_mixer_vca_group_count(self._handle, ctypes.byref(out)))
        return int(out.value)

    def _strip_handle(self, strip: StripRef) -> ctypes.c_void_p:
        """Resolve a strip reference to a borrowed native handle.

        Accepts an integer index in ``[0, strip_count())`` or a string strip id.
        The returned handle is owned by the mixer and is only valid while the
        mixer is open; callers must not store it.
        """
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_strip_at"):
            raise RuntimeError("libsonare was built without strip-handle support")
        if isinstance(strip, str):
            handle = lib.sonare_mixer_strip_by_id(self._handle, strip.encode("utf-8"))
            if not handle:
                raise KeyError(f"mixer strip id not found: {strip}")
        else:
            handle = lib.sonare_mixer_strip_at(self._handle, ctypes.c_size_t(strip))
            if not handle:
                raise IndexError("mixer strip index out of range")
        return ctypes.c_void_p(int(handle))

    def strip_by_id(self, strip_id: str) -> int:
        """Return the index of the strip with ``strip_id``.

        Raises ``KeyError`` if no strip with that id exists. The returned index
        is stable for the lifetime of the current topology.
        """
        self._require()
        lib = _get_lib()
        target = lib.sonare_mixer_strip_by_id(self._handle, strip_id.encode("utf-8"))
        if not target:
            raise KeyError(f"mixer strip id not found: {strip_id}")
        for index in range(self.strip_count()):
            handle = lib.sonare_mixer_strip_at(self._handle, ctypes.c_size_t(index))
            if handle and int(handle) == int(target):
                return index
        raise KeyError(f"mixer strip id not found: {strip_id}")

    def set_soloed(self, strip: StripRef, soloed: bool) -> None:
        """Set a strip's solo state (takes effect without a recompile)."""
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_soloed(handle, ctypes.c_int(1 if soloed else 0)))

    def set_solo_safe(self, strip: StripRef, solo_safe: bool) -> None:
        """Mark a strip solo-safe so other strips' solo never implied-mutes it."""
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_solo_safe(handle, ctypes.c_int(1 if solo_safe else 0)))

    def set_polarity_invert(self, strip: StripRef, invert_left: bool, invert_right: bool) -> None:
        """Invert the polarity of the left and/or right channel of a strip."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_set_polarity_invert(
                handle,
                ctypes.c_int(1 if invert_left else 0),
                ctypes.c_int(1 if invert_right else 0),
            )
        )

    def set_pan_law(self, strip: StripRef, pan_law: PanLaw | str | int) -> None:
        """Set a strip's pan law (``PanLaw`` enum, name, or int 0..3)."""
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_pan_law(handle, ctypes.c_int(_pan_law_value(pan_law))))

    def set_channel_delay_samples(self, strip: StripRef, delay_samples: int) -> None:
        """Set a per-strip channel delay in samples (recompiled on next compile)."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_set_channel_delay_samples(handle, ctypes.c_int(delay_samples))
        )

    def set_vca_offset_db(self, strip: StripRef, offset_db: float) -> None:
        """Set a strip's live VCA gain offset in dB (not persisted to the scene)."""
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_vca_offset_db(handle, ctypes.c_float(offset_db)))

    def set_dual_pan(self, strip: StripRef, left: float, right: float) -> None:
        """Set independent left/right pan positions for a strip (dual-pan mode)."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_set_dual_pan(
                handle, ctypes.c_float(left), ctypes.c_float(right)
            )
        )

    def set_surround_pan(
        self,
        strip: StripRef,
        *,
        azimuth: float = 0.0,
        elevation: float = 0.0,
        divergence: float = 0.0,
        lfe: float = 0.0,
        distance: float = 1.0,
    ) -> None:
        """Set a strip's surround pan position (used when feeding a >2-channel bus).

        Phase 1 honors ``azimuth`` (-180..180 deg, 0 = front-center),
        ``divergence`` (0 = point source, 1 = spread across the front) and
        ``lfe`` (0..1 send into the LFE plane); ``elevation``/``distance`` are
        reserved. Stored on the scene and inert until the surround DSP path
        applies it.
        """
        handle = self._strip_handle(strip)
        pan = SonareSurroundPan(
            azimuth=azimuth,
            elevation=elevation,
            divergence=divergence,
            lfe=lfe,
            distance=distance,
        )
        _check(_get_lib().sonare_strip_set_surround_pan(handle, ctypes.byref(pan)))

    def set_fader_db(self, strip: StripRef, db: float) -> None:
        """Set a strip's fader gain in dB (takes effect without a recompile)."""
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_fader_db(handle, ctypes.c_float(db)))

    def set_input_trim_db(self, strip: StripRef, db: float) -> None:
        """Set a strip's input trim in dB."""
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_input_trim_db(handle, ctypes.c_float(db)))

    def set_pan(self, strip: StripRef, pan: float, pan_mode: int | str | None = None) -> None:
        """Set a strip's pan position.

        ``pan_mode`` accepts a :class:`PanLaw`-independent pan mode as an int
        (``0`` balance, ``1`` stereo-pan, ``2`` dual-pan), a name
        (``"balance"``/``"stereoPan"``/``"stereo-pan"``/``"dualPan"``/
        ``"dual-pan"``), or ``None`` to keep the strip's current pan mode. The
        ``None``/keep sentinel is passed to the C ABI as ``-1``.
        """
        handle = self._strip_handle(strip)
        mode = -1 if pan_mode is None else _pan_mode_value(pan_mode)
        _check(_get_lib().sonare_strip_set_pan(handle, ctypes.c_float(pan), ctypes.c_int(mode)))

    def set_width(self, strip: StripRef, width: float) -> None:
        """Set a strip's stereo width."""
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_width(handle, ctypes.c_float(width)))

    def set_muted(self, strip: StripRef, muted: bool) -> None:
        """Set a strip's mute state (takes effect without a recompile)."""
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_muted(handle, ctypes.c_int(1 if muted else 0)))

    def add_send(
        self,
        strip: StripRef,
        send_id: str,
        destination_bus_id: str,
        send_db: float = 0.0,
        timing: SendTiming | str | int = SendTiming.POST_FADER,
    ) -> int:
        """Add a send from a strip to a bus and return its send index.

        ``timing`` accepts a :class:`SendTiming` enum, a name
        (``"pre_fader"``/``"post_fader"`` or ``"pre"``/``"post"``), or an int
        (``0`` post-fader, ``1`` pre-fader). Call :meth:`compile` after adding
        sends before processing.
        """
        handle = self._strip_handle(strip)
        index_out = ctypes.c_size_t()
        _check(
            _get_lib().sonare_strip_add_send(
                handle,
                send_id.encode("utf-8"),
                destination_bus_id.encode("utf-8"),
                ctypes.c_float(send_db),
                ctypes.c_int(_send_timing_value(timing)),
                ctypes.byref(index_out),
            )
        )
        return int(index_out.value)

    def set_send_db(self, strip: StripRef, index: int, db: float) -> None:
        """Set the level in dB of an existing send on a strip (by send index)."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_set_send_db(handle, ctypes.c_size_t(index), ctypes.c_float(db))
        )

    def remove_send(self, strip: StripRef, index: int) -> None:
        """Remove a strip's send by add-order index.

        Higher sends shift down by one index. The send is dropped from both the
        live strip and the scene mirror; call :meth:`compile` (or process) before
        the next render to rebuild the routing graph.
        """
        handle = self._strip_handle(strip)
        lib = _get_lib()
        if not hasattr(lib, "sonare_strip_remove_send"):
            raise RuntimeError("libsonare was built without strip remove_send support")
        _check(lib.sonare_strip_remove_send(handle, ctypes.c_uint32(index)))

    def strip_meter(
        self, strip: StripRef, tap: MeterTap | str | int = MeterTap.POST_FADER
    ) -> MixMeterSnapshot:
        """Read a per-strip meter snapshot at the given tap point.

        Args:
            strip: Strip index or id.
            tap: ``MeterTap`` enum, name (``"pre_fader"``/``"post_fader"``), or
                int (``0`` pre-fader, ``1`` post-fader).

        Returns:
            A :class:`MixMeterSnapshot` with all peak/RMS/correlation/LUFS/true
            -peak fields populated.
        """
        handle = self._strip_handle(strip)
        lib = _get_lib()
        snapshot = SonareMixMeterSnapshot()
        if hasattr(lib, "sonare_strip_meter_tap"):
            _check(
                lib.sonare_strip_meter_tap(
                    handle, ctypes.c_int(_meter_tap_value(tap)), ctypes.byref(snapshot)
                )
            )
        else:
            _check(lib.sonare_strip_meter(handle, ctypes.byref(snapshot)))
        return _mix_meter_from_c(snapshot)

    def meter_tap(
        self, strip: StripRef, tap: MeterTap | str | int = MeterTap.POST_FADER
    ) -> MixMeterSnapshot:
        """Alias of :meth:`strip_meter` matching the C ``meter_tap`` naming."""
        return self.strip_meter(strip, tap)

    def read_goniometer_latest(self, strip: StripRef, max_points: int) -> list[GoniometerPoint]:
        """Read up to ``max_points`` of the latest goniometer (vectorscope) data."""
        if max_points <= 0:
            return []
        handle = self._strip_handle(strip)
        buffer = (SonareMixGoniometerPoint * max_points)()
        count = _get_lib().sonare_strip_read_goniometer_latest(
            handle,
            ctypes.cast(buffer, ctypes.POINTER(SonareMixGoniometerPoint)),
            ctypes.c_size_t(max_points),
        )
        return [
            GoniometerPoint(left=float(buffer[i].left), right=float(buffer[i].right))
            for i in range(int(count))
        ]

    def schedule_fader_automation(
        self,
        strip: StripRef,
        sample_pos: int,
        fader_db: float,
        curve: AutomationCurve | str | int = AutomationCurve.LINEAR,
    ) -> None:
        """Schedule a sample-accurate fader (dB) automation event on a strip."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_schedule_fader_automation(
                handle,
                ctypes.c_int64(sample_pos),
                ctypes.c_float(fader_db),
                ctypes.c_int(_curve_value(curve)),
            )
        )

    def schedule_pan_automation(
        self,
        strip: StripRef,
        sample_pos: int,
        pan: float,
        curve: AutomationCurve | str | int = AutomationCurve.LINEAR,
    ) -> None:
        """Schedule a sample-accurate pan automation event on a strip."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_schedule_pan_automation(
                handle,
                ctypes.c_int64(sample_pos),
                ctypes.c_float(pan),
                ctypes.c_int(_curve_value(curve)),
            )
        )

    def schedule_width_automation(
        self,
        strip: StripRef,
        sample_pos: int,
        width: float,
        curve: AutomationCurve | str | int = AutomationCurve.LINEAR,
    ) -> None:
        """Schedule a sample-accurate stereo-width automation event on a strip."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_schedule_width_automation(
                handle,
                ctypes.c_int64(sample_pos),
                ctypes.c_float(width),
                ctypes.c_int(_curve_value(curve)),
            )
        )

    def schedule_send_automation(
        self,
        strip: StripRef,
        send_index: int,
        sample_pos: int,
        db: float,
        curve: AutomationCurve | str | int = AutomationCurve.LINEAR,
    ) -> None:
        """Schedule a sample-accurate send-level (dB) automation event on a strip."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_schedule_send_automation(
                handle,
                ctypes.c_size_t(send_index),
                ctypes.c_int64(sample_pos),
                ctypes.c_float(db),
                ctypes.c_int(_curve_value(curve)),
            )
        )

    def schedule_insert_automation(
        self,
        strip_index: StripRef,
        insert_index: int,
        param_id: int,
        sample_pos: int,
        value: float,
        curve: AutomationCurve | str | int = AutomationCurve.LINEAR,
    ) -> None:
        """Schedule a sample-accurate insert-parameter automation event.

        Args:
            strip_index: Strip index in ``[0, strip_count())`` or strip id.
            insert_index: Index into the strip's combined pre/post insert chain.
            param_id: Processor-specific parameter id.
            sample_pos: Absolute sample position from the start of processing.
            value: Target parameter value.
            curve: ``AutomationCurve`` enum, name, or int (``0`` linear,
                ``1`` exponential, ``2`` hold, ``3`` s-curve). Accepts a raw
                int for backward compatibility.
        """
        handle = self._strip_handle(strip_index)
        lib = _get_lib()
        if not hasattr(lib, "sonare_strip_schedule_insert_automation"):
            raise RuntimeError("libsonare was built without insert-automation support")
        _check(
            lib.sonare_strip_schedule_insert_automation(
                handle,
                ctypes.c_uint(insert_index),
                ctypes.c_uint(param_id),
                ctypes.c_int64(sample_pos),
                ctypes.c_float(value),
                ctypes.c_int(_curve_value(curve)),
            )
        )

    def process_stereo(
        self,
        left_channels: Sequence[Sequence[float]],
        right_channels: Sequence[Sequence[float]],
    ) -> MixerStereoResult:
        """Mix one block of per-strip stereo audio into the stereo master.

        ``left_channels[i]`` / ``right_channels[i]`` are strip ``i``'s channels.
        Returns a :class:`MixerStereoResult` (``left``, ``right``,
        ``sample_rate``) matching the Node/WASM ``{left, right, sampleRate}``
        shape. With no input strips the master is silent (an empty block), to
        match the C ABI which accepts ``input_count=0``.
        """
        self._require()
        if len(left_channels) != len(right_channels):
            raise ValueError("left_channels and right_channels must have the same length")

        left_arrays: list[ctypes.Array[ctypes.c_float]] = []
        right_arrays: list[ctypes.Array[ctypes.c_float]] = []
        length: int | None = None
        for left, right in zip(left_channels, right_channels, strict=True):
            left_array, left_length = _to_c_float_array(left)
            right_array, right_length = _to_c_float_array(right)
            if left_length != right_length:
                raise ValueError("left and right channel lengths must match")
            if length is None:
                length = left_length
            elif left_length != length:
                raise ValueError("all strips must have the same length")
            left_arrays.append(left_array)
            right_arrays.append(right_array)

        # No input strips -> silent (empty) master, matching the C ABI which
        # accepts input_count=0 and returns OK.
        if length is None:
            length = 0

        left_ptrs = (ctypes.POINTER(ctypes.c_float) * len(left_arrays))(
            *[ctypes.cast(arr, ctypes.POINTER(ctypes.c_float)) for arr in left_arrays]
        )
        right_ptrs = (ctypes.POINTER(ctypes.c_float) * len(right_arrays))(
            *[ctypes.cast(arr, ctypes.POINTER(ctypes.c_float)) for arr in right_arrays]
        )
        out_left = (ctypes.c_float * length)()
        out_right = (ctypes.c_float * length)()
        _check(
            _get_lib().sonare_mixer_process_stereo(
                self._handle,
                left_ptrs,
                right_ptrs,
                ctypes.c_size_t(len(left_arrays)),
                out_left,
                out_right,
                ctypes.c_size_t(length),
            )
        )
        return MixerStereoResult(
            left=[float(out_left[i]) for i in range(length)],
            right=[float(out_right[i]) for i in range(length)],
            sample_rate=int(self._sample_rate),
        )

    def tail_samples(self) -> int:
        """Return the mixer's reverb/delay tail length in samples.

        This is how many additional samples should be drained with
        :meth:`drain_tail_stereo` after the last input block to capture the
        decaying effect tails (reverb, delay) left in the routing graph.
        """
        self._require()
        out = ctypes.c_int()
        _check(_get_lib().sonare_mixer_tail_samples(self._handle, ctypes.byref(out)))
        return int(out.value)

    def drain_tail_stereo(self, num_samples: int) -> MixerStereoResult:
        """Drain ``num_samples`` of trailing effect-tail audio with no new input.

        Pushes silence through the compiled graph so the stereo master keeps
        emitting the decaying reverb / delay tails. ``num_samples`` must be
        ``> 0`` and not exceed the mixer's block size. Returns a
        :class:`MixerStereoResult` matching :meth:`process_stereo`.
        """
        self._require()
        count = int(num_samples)
        if count <= 0:
            raise ValueError("num_samples must be > 0")
        if count > self._block_size:
            raise ValueError(
                f"num_samples ({count}) must not exceed the mixer block size ({self._block_size})"
            )
        out_left = (ctypes.c_float * count)()
        out_right = (ctypes.c_float * count)()
        _check(
            _get_lib().sonare_mixer_drain_tail_stereo(
                self._handle,
                out_left,
                out_right,
                ctypes.c_size_t(count),
            )
        )
        return MixerStereoResult(
            left=[float(out_left[i]) for i in range(count)],
            right=[float(out_right[i]) for i in range(count)],
            sample_rate=int(self._sample_rate),
        )

    def to_scene_json(self) -> str:
        """Serialize the current scene (strips, buses, sends, connections)."""
        self._require()
        lib = _get_lib()
        json_ptr = ctypes.c_char_p()
        _check(lib.sonare_mixer_to_scene_json(self._handle, ctypes.byref(json_ptr)))
        try:
            return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
        finally:
            if json_ptr.value:
                lib.sonare_free_string(json_ptr)

    def close(self) -> None:
        """Release the underlying native mixer. Safe to call more than once."""
        if self._handle is not None:
            _get_lib().sonare_mixer_destroy(self._handle)
            self._handle = None

    def __del__(self) -> None:
        self.close()

    def _require(self) -> None:
        if self._handle is None:
            raise RuntimeError("Mixer has been closed")


def mix_stereo(
    strips: Sequence[tuple[Sequence[float], Sequence[float]]],
    sample_rate: int = 48000,
    fader_db: Sequence[float] | None = None,
    pan: Sequence[float] | None = None,
    pan_mode: Sequence[str | int] | str | int = "balance",
    width: Sequence[float] | None = None,
    muted: Sequence[bool] | None = None,
    input_trim_db: Sequence[float] | None = None,
) -> MixResult:
    """Render a small stereo mixer scene from per-strip left/right buffers.

    Args:
        strips: Sequence of ``(left, right)`` sample buffers. All buffers must
            have the same length.
        sample_rate: Sample rate in Hz.
        fader_db: Optional per-strip fader values in dB.
        pan: Optional per-strip pan values in ``[-1, 1]``.
        pan_mode: Either one mode for all strips or per-strip modes:
            ``"balance"``, ``"stereoPan"``, or ``"dualPan"``.
        width: Optional per-strip stereo width values.
        muted: Optional per-strip mute flags.
        input_trim_db: Optional per-strip input trim values in dB.

    Note:
        The per-strip meters in the result expose integrating loudness fields
        (``momentary_lufs``, ``short_term_lufs``, ``integrated_lufs``) and
        ``true_peak_db``. These require sustained streaming to be meaningful:
        on a short one-shot mix they have not accumulated enough history and
        read the ``-120`` dB floor sentinel. For accurate loudness/true-peak
        metering, drive a strip over a streaming session instead.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mixer_create"):
        raise RuntimeError("libsonare was built without mixing support")
    if not strips:
        raise ValueError("at least one strip is required")

    # Each per-strip option must have exactly one entry per strip; otherwise the
    # per-index access below would raise a cryptic IndexError (or silently use
    # the wrong strip's value). Validate up front with a clear message.
    n_strips = len(strips)
    for name, opt in (
        ("input_trim_db", input_trim_db),
        ("fader_db", fader_db),
        ("pan", pan),
        ("width", width),
        ("muted", muted),
    ):
        if opt is not None and len(opt) != n_strips:
            raise ValueError(f"mix_stereo: '{name}' must have one entry per strip ({n_strips})")
    if (
        isinstance(pan_mode, Sequence)
        and not isinstance(pan_mode, str)
        and len(pan_mode) != n_strips
    ):
        raise ValueError(f"mix_stereo: 'pan_mode' must have one entry per strip ({n_strips})")

    left_arrays: list[ctypes.Array[ctypes.c_float]] = []
    right_arrays: list[ctypes.Array[ctypes.c_float]] = []
    length: int | None = None
    for left, right in strips:
        left_array, left_length = _to_c_float_array(left)
        right_array, right_length = _to_c_float_array(right)
        if left_length != right_length:
            raise ValueError("left and right channel lengths must match")
        if length is None:
            length = left_length
        elif left_length != length:
            raise ValueError("all strips must have the same length")
        left_arrays.append(left_array)
        right_arrays.append(right_array)

    assert length is not None
    mixer = lib.sonare_mixer_create(ctypes.c_int(sample_rate), ctypes.c_int(max(1, length)))
    if not mixer:
        raise RuntimeError("failed to create mixer")

    try:
        strip_handles: list[ctypes.c_void_p] = []
        for index in range(len(strips)):
            handle = lib.sonare_mixer_add_strip(mixer, f"strip{index}".encode())
            if not handle:
                raise RuntimeError("failed to add mixer strip")
            strip_handles.append(ctypes.c_void_p(handle))

            if input_trim_db is not None:
                _check(
                    lib.sonare_strip_set_input_trim_db(
                        strip_handles[-1], ctypes.c_float(input_trim_db[index])
                    )
                )
            if fader_db is not None:
                _check(
                    lib.sonare_strip_set_fader_db(
                        strip_handles[-1], ctypes.c_float(fader_db[index])
                    )
                )
            if pan is not None:
                mode = (
                    pan_mode[index]
                    if isinstance(pan_mode, Sequence) and not isinstance(pan_mode, str)
                    else pan_mode
                )
                _check(
                    lib.sonare_strip_set_pan(
                        strip_handles[-1],
                        ctypes.c_float(pan[index]),
                        ctypes.c_int(_pan_mode_value(mode)),
                    )
                )
            if width is not None:
                _check(lib.sonare_strip_set_width(strip_handles[-1], ctypes.c_float(width[index])))
            if muted is not None:
                _check(
                    lib.sonare_strip_set_muted(
                        strip_handles[-1], ctypes.c_int(1 if muted[index] else 0)
                    )
                )

        left_ptrs = (ctypes.POINTER(ctypes.c_float) * len(left_arrays))(
            *[ctypes.cast(arr, ctypes.POINTER(ctypes.c_float)) for arr in left_arrays]
        )
        right_ptrs = (ctypes.POINTER(ctypes.c_float) * len(right_arrays))(
            *[ctypes.cast(arr, ctypes.POINTER(ctypes.c_float)) for arr in right_arrays]
        )
        out_left = (ctypes.c_float * length)()
        out_right = (ctypes.c_float * length)()
        rc = lib.sonare_mixer_process_stereo(
            mixer,
            left_ptrs,
            right_ptrs,
            ctypes.c_size_t(len(strips)),
            out_left,
            out_right,
            ctypes.c_size_t(length),
        )
        _check(rc)

        meters: list[MixMeterSnapshot] = []
        for handle in strip_handles:
            snapshot = SonareMixMeterSnapshot()
            _check(lib.sonare_strip_meter(handle, ctypes.byref(snapshot)))
            meters.append(_mix_meter_from_c(snapshot))

        return MixResult(
            left=[float(out_left[i]) for i in range(length)],
            right=[float(out_right[i]) for i in range(length)],
            sample_rate=int(sample_rate),
            meters=meters,
        )
    finally:
        lib.sonare_mixer_destroy(mixer)
