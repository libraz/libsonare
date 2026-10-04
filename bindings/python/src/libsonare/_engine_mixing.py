"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING, cast

from ._engine_conversions import _band_json_arg
from ._runtime import (
    _UINT32_MAX,
    PanLawInput,
    SendTiming,
    SidechainSourceKind,
    SonareEngineBus,
    SonareEngineTrackLane,
    SonareEngineTrackSend,
    _check,
    _get_lib,
    _narrow_int,
    _pan_law_value,
    _pan_mode_value,
    _send_timing_value,
    _sidechain_source_kind_value,
    _to_c_float,
    _to_c_int,
    _to_c_uint,
    _to_c_uint32,
)


class _EngineMixingMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

    def set_track_lanes(self, lanes: Sequence[int | Mapping[str, object]]) -> None:
        raw = (SonareEngineTrackLane * len(lanes))()
        send_arrays: list[object] = []
        for i, lane in enumerate(lanes):
            # ctypes zero-inits source_channel_layout to 0 (mono); default to
            # stereo (ChannelLayout.STEREO) so callers that omit it keep the
            # prior stereo behavior.
            raw[i].source_channel_layout = 1
            if isinstance(lane, Mapping):
                # Assigned unconverted so the struct's own narrowing sees the
                # caller value; int() would truncate a fraction past it.
                raw[i].track_id = cast(
                    int, lane["track_id"] if "track_id" in lane else lane["trackId"]
                )
                sends = cast(Sequence[Mapping[str, object]], lane.get("sends", []))
                if sends:
                    send_array = (SonareEngineTrackSend * len(sends))()
                    for send_index, send in enumerate(sends):
                        if not isinstance(send, Mapping):
                            raise TypeError("track lane send must be a mapping")
                        send_array[send_index].bus_id = cast(
                            int, send["bus_id"] if "bus_id" in send else send["busId"]
                        )
                        send_array[send_index].level_db = float(
                            cast(
                                float,
                                send["level_db"]
                                if "level_db" in send
                                else send.get("levelDb", 0.0),
                            )
                        )
                        send_array[send_index].enabled = 1 if bool(send.get("enabled", True)) else 0
                        # Default to post-fader so callers that omit the timing
                        # key keep the prior behavior.
                        timing = send.get("timing", send.get("send_timing", send.get("sendTiming")))
                        send_array[send_index].send_timing = (
                            _send_timing_value(cast(SendTiming | str | int, timing))
                            if timing is not None
                            else int(SendTiming.POST_FADER)
                        )
                    raw[i].sends = send_array
                    raw[i].send_count = len(sends)
                    send_arrays.append(send_array)
                # Narrowed rather than coerced: int(0.5) is the 0 this field
                # reads as "stay on the master mix", so a fractional bus id
                # would leave the lane unrouted and report success.
                raw[i].output_bus_id = _narrow_int(
                    lane["output_bus_id"]
                    if "output_bus_id" in lane
                    else lane.get("outputBusId", 0),
                    f"set_track_lanes: lanes[{i}].output_bus_id",
                    0,
                    _UINT32_MAX,
                )
                if "source_channel_layout" in lane or "sourceChannelLayout" in lane:
                    raw[i].source_channel_layout = (
                        cast(int, lane["source_channel_layout"])
                        if "source_channel_layout" in lane
                        else cast(int, lane["sourceChannelLayout"])
                    )
            else:
                raw[i].track_id = lane
        _check(_get_lib().sonare_engine_set_track_lanes(self._require_handle(), raw, len(lanes)))

    def set_lane_sidechain(self, track_id: int, insert_index: int, source_track_id: int) -> None:
        """Key one insert of a lane strip from another lane's post-strip audio.

        Sidechain for ducking/sidechainRouter inserts; ``source_track_id`` 0
        removes the binding.
        """
        _check(
            _get_lib().sonare_engine_set_lane_sidechain(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint(insert_index, "insert_index"),
                _to_c_uint32(source_track_id, "source_track_id"),
            )
        )

    def set_bus_sidechain(
        self,
        bus_id: int,
        insert_index: int,
        source_kind: SidechainSourceKind | str | int,
        source_id: int,
    ) -> None:
        """Key one insert of a bus strip from a track's or another bus's signal.

        Bus-strip counterpart of :meth:`set_lane_sidechain`. ``source_kind`` is
        ``"track"``/``"bus"`` (or the matching 0/1 ordinal); ``source_id`` 0
        removes the binding. Control-thread-only: do not call concurrently
        with :meth:`process`.
        """
        _check(
            _get_lib().sonare_engine_set_bus_sidechain(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                _to_c_int(_sidechain_source_kind_value(source_kind), "source_kind"),
                _to_c_uint32(source_id, "source_id"),
            )
        )

    def set_master_sidechain(
        self, insert_index: int, source_kind: SidechainSourceKind | str | int, source_id: int
    ) -> None:
        """Key one insert of the master strip from a track's or a bus's signal.

        Master-strip counterpart of :meth:`set_bus_sidechain`.
        """
        _check(
            _get_lib().sonare_engine_set_master_sidechain(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                _to_c_int(_sidechain_source_kind_value(source_kind), "source_kind"),
                _to_c_uint32(source_id, "source_id"),
            )
        )

    def set_track_buses(self, buses: Sequence[Mapping[str, object]]) -> None:
        raw = (SonareEngineBus * len(buses))()
        send_arrays: list[object] = []
        for i, bus in enumerate(buses):
            # Assigned unconverted so the struct's own narrowing sees the caller
            # value; int() would truncate a fraction past it.
            raw[i].bus_id = cast(int, bus["bus_id"] if "bus_id" in bus else bus["busId"])
            raw[i].gain_db = float(
                cast(float, bus["gain_db"] if "gain_db" in bus else bus.get("gainDb", 0.0))
            )
            # ctypes zero-inits channel_layout to 0 (mono); default to stereo
            # (ChannelLayout.STEREO) unless the caller specifies it.
            if "channel_layout" in bus or "channelLayout" in bus:
                raw[i].channel_layout = cast(
                    int,
                    bus["channel_layout"] if "channel_layout" in bus else bus["channelLayout"],
                )
            else:
                raw[i].channel_layout = 1
            sends = cast(Sequence[Mapping[str, object]], bus.get("sends", []))
            if sends:
                send_array = (SonareEngineTrackSend * len(sends))()
                for send_index, send in enumerate(sends):
                    if not isinstance(send, Mapping):
                        raise TypeError("bus send must be a mapping")
                    send_array[send_index].bus_id = cast(
                        int, send["bus_id"] if "bus_id" in send else send["busId"]
                    )
                    send_array[send_index].level_db = float(
                        cast(
                            float,
                            send["level_db"] if "level_db" in send else send.get("levelDb", 0.0),
                        )
                    )
                    send_array[send_index].enabled = 1 if bool(send.get("enabled", True)) else 0
                    # Default to post-fader so callers that omit the timing key
                    # keep the prior behavior.
                    timing = send.get("timing", send.get("send_timing", send.get("sendTiming")))
                    send_array[send_index].send_timing = (
                        _send_timing_value(cast(SendTiming | str | int, timing))
                        if timing is not None
                        else int(SendTiming.POST_FADER)
                    )
                raw[i].sends = send_array
                raw[i].send_count = len(sends)
                send_arrays.append(send_array)
            # Narrowed rather than coerced: int(0.5) is the 0 this field reads
            # as "stay on the master mix", so a fractional bus id would leave
            # the bus unrouted and report success.
            raw[i].output_bus_id = _narrow_int(
                bus["output_bus_id"] if "output_bus_id" in bus else bus.get("outputBusId", 0),
                f"set_track_buses: buses[{i}].output_bus_id",
                0,
                _UINT32_MAX,
            )
        _check(_get_lib().sonare_engine_set_track_buses(self._require_handle(), raw, len(buses)))

    def set_bus_strip_json(self, bus_id: int, scene_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_bus_strip_json(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                scene_json.encode("utf-8"),
            )
        )

    def set_bus_strip_eq_band(
        self, bus_id: int, band_index: int, band: Mapping[str, object] | str
    ) -> None:
        """Realtime change of one bus-strip EQ band.

        Bus-strip counterpart of :meth:`set_track_strip_eq_band`.
        """
        _check(
            _get_lib().sonare_engine_set_bus_strip_eq_band_json(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_int(band_index, "band_index"),
                _band_json_arg(band),
            )
        )

    def set_bus_strip_eq_band_json(self, bus_id: int, band_index: int, band_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_bus_strip_eq_band_json(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_int(band_index, "band_index"),
                band_json.encode("utf-8"),
            )
        )

    def set_track_strip_json(self, track_id: int, scene_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_json(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                scene_json.encode("utf-8"),
            )
        )

    def set_track_strip_eq_band(
        self, track_id: int, band_index: int, band: Mapping[str, object] | str
    ) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_eq_band_json(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_int(band_index, "band_index"),
                _band_json_arg(band),
            )
        )

    def set_track_strip_eq_band_json(self, track_id: int, band_index: int, band_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_eq_band_json(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_int(band_index, "band_index"),
                band_json.encode("utf-8"),
            )
        )

    def set_track_strip_insert_bypassed(
        self, track_id: int, insert_index: int, bypassed: bool, reset_on_bypass: bool = False
    ) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_insert_bypassed(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint(insert_index, "insert_index"),
                1 if bypassed else 0,
                1 if reset_on_bypass else 0,
            )
        )

    def set_track_strip_insert_param_by_name(
        self, track_id: int, insert_index: int, param_name: str, value: float
    ) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                _to_c_float(value, "value"),
            )
        )

    def set_track_strip_pan(self, track_id: int, pan: float) -> None:
        """Set a track strip's pan position (-1.0 hard left .. 1.0 hard right)."""
        _check(
            _get_lib().sonare_engine_set_track_strip_pan(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_float(pan, "pan"),
            )
        )

    def set_track_strip_pan_law(self, track_id: int, pan_law: PanLawInput) -> None:
        """Set a track strip's pan law (a ``PanLawName`` alias, enum, or int 0..3).

        Mono strips apply the law at centre; stereo Balance strips keep centre
        unity and use it only for the far-channel taper.
        """
        _check(
            _get_lib().sonare_engine_set_track_strip_pan_law(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _pan_law_value(pan_law),
            )
        )

    def set_track_strip_pan_mode(self, track_id: int, pan_mode: str | int) -> None:
        """Set a track strip's pan mode (name 'balance'/'stereo-pan'/'dual-pan', or int 0..2)."""
        _check(
            _get_lib().sonare_engine_set_track_strip_pan_mode(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _pan_mode_value(pan_mode),
            )
        )

    def set_track_strip_dual_pan(self, track_id: int, left_pan: float, right_pan: float) -> None:
        """Set a track strip's dual-pan positions for the left and right channels."""
        _check(
            _get_lib().sonare_engine_set_track_strip_dual_pan(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_float(left_pan, "left_pan"),
                _to_c_float(right_pan, "right_pan"),
            )
        )

    def set_track_strip_channel_delay_samples(self, track_id: int, delay_samples: int) -> None:
        """Set a track strip's alignment delay in samples, moving it later than the other lanes."""
        _check(
            _get_lib().sonare_engine_set_track_strip_channel_delay_samples(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_int(delay_samples, "delay_samples"),
            )
        )

    def set_master_strip_json(self, scene_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_master_strip_json(
                self._require_handle(),
                scene_json.encode("utf-8"),
            )
        )

    def set_master_strip_eq_band(self, band_index: int, band: Mapping[str, object] | str) -> None:
        _check(
            _get_lib().sonare_engine_set_master_strip_eq_band_json(
                self._require_handle(),
                _to_c_int(band_index, "band_index"),
                _band_json_arg(band),
            )
        )

    def set_master_strip_eq_band_json(self, band_index: int, band_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_master_strip_eq_band_json(
                self._require_handle(),
                _to_c_int(band_index, "band_index"),
                band_json.encode("utf-8"),
            )
        )

    def set_master_strip_insert_bypassed(
        self, insert_index: int, bypassed: bool, reset_on_bypass: bool = False
    ) -> None:
        _check(
            _get_lib().sonare_engine_set_master_strip_insert_bypassed(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                1 if bypassed else 0,
                1 if reset_on_bypass else 0,
            )
        )

    def set_master_strip_insert_param_by_name(
        self, insert_index: int, param_name: str, value: float
    ) -> None:
        _check(
            _get_lib().sonare_engine_set_master_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                _to_c_float(value, "value"),
            )
        )

    def set_bus_strip_insert_param_by_name(
        self, bus_id: int, insert_index: int, param_name: str, value: float
    ) -> None:
        """Realtime change of one bus-strip insert parameter, addressed by name.

        Bus-strip counterpart of :meth:`set_track_strip_insert_param_by_name`;
        ``bus_id`` must carry a strip configured via :meth:`set_bus_strip_json`.
        """
        _check(
            _get_lib().sonare_engine_set_bus_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                _to_c_float(value, "value"),
            )
        )

    def apply_track_strip_insert_param_by_name_now(
        self, track_id: int, insert_index: int, param_name: str, value: float
    ) -> bool:
        """Apply one track-strip insert parameter immediately, bypassing the queue.

        Runs on the calling (engine-owning) thread rather than going through
        the command ring, and is recorded as the parameter's manual base like
        :meth:`set_track_strip_insert_param_by_name`. An unknown track, insert
        or name is reported by a ``False`` return instead of raising; not safe
        concurrently with :meth:`process`.
        """
        out_applied = ctypes.c_int()
        _check(
            _get_lib().sonare_engine_apply_track_strip_insert_param_by_name_now(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                _to_c_float(value, "value"),
                ctypes.byref(out_applied),
            )
        )
        return bool(out_applied.value)

    def apply_master_strip_insert_param_by_name_now(
        self, insert_index: int, param_name: str, value: float
    ) -> bool:
        """Master-strip counterpart of :meth:`apply_track_strip_insert_param_by_name_now`."""
        out_applied = ctypes.c_int()
        _check(
            _get_lib().sonare_engine_apply_master_strip_insert_param_by_name_now(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                _to_c_float(value, "value"),
                ctypes.byref(out_applied),
            )
        )
        return bool(out_applied.value)

    def apply_bus_strip_insert_param_by_name_now(
        self, bus_id: int, insert_index: int, param_name: str, value: float
    ) -> bool:
        """Bus-strip counterpart of :meth:`apply_track_strip_insert_param_by_name_now`."""
        out_applied = ctypes.c_int()
        _check(
            _get_lib().sonare_engine_apply_bus_strip_insert_param_by_name_now(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                _to_c_float(value, "value"),
                ctypes.byref(out_applied),
            )
        )
        return bool(out_applied.value)

    def restore_track_strip_insert_param_by_name(
        self, track_id: int, insert_index: int, param_name: str, value: float
    ) -> None:
        """Restore a retained track-strip insert value exactly, without a ramp.

        Retires any in-flight smoother for the parameter and records the
        value as the manual base. Used to replay retained edits after
        :meth:`set_track_strip_json` replaced the strip. Raises if the track,
        insert, or name is unknown; not safe concurrently with :meth:`process`.
        """
        _check(
            _get_lib().sonare_engine_restore_track_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                _to_c_float(value, "value"),
            )
        )

    def restore_master_strip_insert_param_by_name(
        self, insert_index: int, param_name: str, value: float
    ) -> None:
        """Master-strip counterpart of :meth:`restore_track_strip_insert_param_by_name`."""
        _check(
            _get_lib().sonare_engine_restore_master_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                _to_c_float(value, "value"),
            )
        )

    def restore_bus_strip_insert_param_by_name(
        self, bus_id: int, insert_index: int, param_name: str, value: float
    ) -> None:
        """Bus-strip counterpart of :meth:`restore_track_strip_insert_param_by_name`."""
        _check(
            _get_lib().sonare_engine_restore_bus_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                _to_c_float(value, "value"),
            )
        )

    def clear_track_insert_parameter_bases(self, track_id: int) -> None:
        """Forget the remembered manual insert-parameter values of one track strip.

        Also discards its queued insert edits. Call before
        :meth:`set_track_strip_json` replaces the strip when its old values
        must not carry over; the setter never does this itself, since a queued
        edit may already target the new chain. Raises if the track is not a published
        lane; not safe concurrently with :meth:`process`.
        """
        _check(
            _get_lib().sonare_engine_clear_track_insert_parameter_bases(
                self._require_handle(), _to_c_uint32(track_id, "track_id")
            )
        )

    def clear_master_insert_parameter_bases(self) -> None:
        """Master-strip counterpart of :meth:`clear_track_insert_parameter_bases`."""
        _check(_get_lib().sonare_engine_clear_master_insert_parameter_bases(self._require_handle()))

    def clear_bus_insert_parameter_bases(self, bus_id: int) -> None:
        """Bus-strip counterpart of :meth:`clear_track_insert_parameter_bases`.

        Also accepts a bus identity that was already removed, covering every
        selector the identity held across remove/re-add.
        """
        _check(
            _get_lib().sonare_engine_clear_bus_insert_parameter_bases(
                self._require_handle(), _to_c_uint32(bus_id, "bus_id")
            )
        )

    def set_bus_strip_insert_bypassed(
        self, bus_id: int, insert_index: int, bypassed: bool, reset_on_bypass: bool = False
    ) -> None:
        """Toggle bypass for a bus-strip insert.

        Bus-strip counterpart of :meth:`set_track_strip_insert_bypassed`;
        ``bus_id`` must carry a strip configured via :meth:`set_bus_strip_json`.
        """
        _check(
            _get_lib().sonare_engine_set_bus_strip_insert_bypassed(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                1 if bypassed else 0,
                1 if reset_on_bypass else 0,
            )
        )

    def set_bus_strip_pan(self, bus_id: int, pan: float) -> None:
        """Set a bus strip's pan position (-1.0 hard left .. 1.0 hard right).

        Bus-strip counterpart of :meth:`set_track_strip_pan`; rejected on a
        surround bus.
        """
        _check(
            _get_lib().sonare_engine_set_bus_strip_pan(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_float(pan, "pan"),
            )
        )

    def set_bus_strip_pan_law(self, bus_id: int, pan_law: PanLawInput) -> None:
        """Set a bus strip's pan law (a ``PanLawName`` alias, enum, or int 0..3).

        Bus-strip counterpart of :meth:`set_track_strip_pan_law`.
        """
        _check(
            _get_lib().sonare_engine_set_bus_strip_pan_law(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _pan_law_value(pan_law),
            )
        )

    def set_bus_strip_pan_mode(self, bus_id: int, pan_mode: str | int) -> None:
        """Set a bus strip's pan mode (name 'balance'/'stereo-pan'/'dual-pan', or int 0..2).

        Bus-strip counterpart of :meth:`set_track_strip_pan_mode`.
        """
        _check(
            _get_lib().sonare_engine_set_bus_strip_pan_mode(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _pan_mode_value(pan_mode),
            )
        )

    def set_bus_strip_dual_pan(self, bus_id: int, left_pan: float, right_pan: float) -> None:
        """Set a bus strip's dual-pan positions for the left and right channels.

        Bus-strip counterpart of :meth:`set_track_strip_dual_pan`.
        """
        _check(
            _get_lib().sonare_engine_set_bus_strip_dual_pan(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_float(left_pan, "left_pan"),
                _to_c_float(right_pan, "right_pan"),
            )
        )

    def resolve_track_insert_automation_id(
        self, track_id: int, insert_index: int, param_name: str
    ) -> int:
        """Resolve a track-lane insert parameter to its reserved automation id.

        The returned id drives :meth:`set_automation_lane`,
        :meth:`set_parameter`, or :meth:`set_parameter_smoothed` exactly like a
        fader/pan id. Raises :class:`SonareError` if the track, insert, or name
        is unknown.

        This trio is how a mastering processor gets time-varying automation:
        the ``eq.*``, ``dynamics.*``, ``saturation.*``, ``spectral.*``,
        ``stereo.*``, ``maximizer.*`` and ``multiband.*`` processors are all
        available as strip inserts, so placing one on a strip and resolving its
        parameter here drives it at audio-block precision, live and offline
        alike. The whole-signal stages of the offline mastering chain
        (``repair.*``, ``loudness``, and the match stages) have no insert form
        and no automation id: they buffer the entire signal by construction and
        do not run on the realtime path.

        The returned id uses the track's current positional lane selector. When
        :meth:`set_track_lanes` successfully changes lane order or membership,
        the engine remaps already queued and published track automation by
        track id, but it cannot update a numeric id retained by the caller.
        Resolve every track insert id again after such a topology change before
        passing it to :meth:`set_automation_lane`, :meth:`set_parameter`, or
        :meth:`set_parameter_smoothed`. Use
        :meth:`set_track_strip_insert_param_by_name` when the operation needs a
        stable track identity. Master and bus insert ids are separate and are
        not invalidated by track-lane changes.
        """
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_engine_resolve_track_insert_automation_id(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def resolve_master_insert_automation_id(self, insert_index: int, param_name: str) -> int:
        """Resolve a master-strip insert parameter to its reserved automation id.

        Master-strip counterpart of :meth:`resolve_track_insert_automation_id`.
        """
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_engine_resolve_master_insert_automation_id(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def resolve_bus_insert_automation_id(
        self, bus_id: int, insert_index: int, param_name: str
    ) -> int:
        """Resolve a bus-strip insert parameter to its reserved automation id.

        Bus-strip counterpart of :meth:`resolve_track_insert_automation_id`;
        ``bus_id`` must carry a strip configured via :meth:`set_bus_strip_json`.
        """
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_engine_resolve_bus_insert_automation_id(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                param_name.encode("utf-8"),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)
