"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING

from ._engine_conversions import _band_json_arg
from ._runtime import (
    PanLaw,
    SendTiming,
    SonareEngineBus,
    SonareEngineTrackLane,
    SonareEngineTrackSend,
    _check,
    _get_lib,
    _pan_law_value,
    _pan_mode_value,
    _send_timing_value,
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
                raw[i].track_id = int(lane["track_id"] if "track_id" in lane else lane["trackId"])
                sends = lane.get("sends", [])
                if sends:
                    send_array = (SonareEngineTrackSend * len(sends))()
                    for send_index, send in enumerate(sends):
                        if not isinstance(send, Mapping):
                            raise TypeError("track lane send must be a mapping")
                        send_array[send_index].bus_id = int(
                            send["bus_id"] if "bus_id" in send else send["busId"]
                        )
                        send_array[send_index].level_db = float(
                            send["level_db"] if "level_db" in send else send.get("levelDb", 0.0)
                        )
                        send_array[send_index].enabled = 1 if bool(send.get("enabled", True)) else 0
                        # Default to post-fader so callers that omit the timing
                        # key keep the prior behavior.
                        timing = send.get("timing", send.get("send_timing", send.get("sendTiming")))
                        send_array[send_index].send_timing = (
                            _send_timing_value(timing)
                            if timing is not None
                            else int(SendTiming.POST_FADER)
                        )
                    raw[i].sends = send_array
                    raw[i].send_count = len(sends)
                    send_arrays.append(send_array)
                raw[i].output_bus_id = int(
                    lane["output_bus_id"] if "output_bus_id" in lane else lane.get("outputBusId", 0)
                )
                if "source_channel_layout" in lane or "sourceChannelLayout" in lane:
                    raw[i].source_channel_layout = int(
                        lane["source_channel_layout"]
                        if "source_channel_layout" in lane
                        else lane["sourceChannelLayout"]
                    )
            else:
                raw[i].track_id = int(lane)
        _check(_get_lib().sonare_engine_set_track_lanes(self._require_handle(), raw, len(lanes)))

    def set_lane_sidechain(self, track_id: int, insert_index: int, source_track_id: int) -> None:
        """Key one insert of a lane strip from another lane's post-strip audio.

        Sidechain for ducking/sidechainRouter inserts; ``source_track_id`` 0
        removes the binding.
        """
        _check(
            _get_lib().sonare_engine_set_lane_sidechain(
                self._require_handle(), int(track_id), int(insert_index), int(source_track_id)
            )
        )

    def set_track_buses(self, buses: Sequence[Mapping[str, object]]) -> None:
        raw = (SonareEngineBus * len(buses))()
        for i, bus in enumerate(buses):
            raw[i].bus_id = int(bus["bus_id"] if "bus_id" in bus else bus["busId"])
            raw[i].gain_db = float(bus["gain_db"] if "gain_db" in bus else bus.get("gainDb", 0.0))
            # ctypes zero-inits channel_layout to 0 (mono); default to stereo
            # (ChannelLayout.STEREO) unless the caller specifies it.
            if "channel_layout" in bus or "channelLayout" in bus:
                raw[i].channel_layout = int(
                    bus["channel_layout"] if "channel_layout" in bus else bus["channelLayout"]
                )
            else:
                raw[i].channel_layout = 1
        _check(_get_lib().sonare_engine_set_track_buses(self._require_handle(), raw, len(buses)))

    def set_bus_strip_json(self, bus_id: int, scene_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_bus_strip_json(
                self._require_handle(),
                int(bus_id),
                scene_json.encode("utf-8"),
            )
        )

    def set_track_strip_json(self, track_id: int, scene_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_json(
                self._require_handle(),
                int(track_id),
                scene_json.encode("utf-8"),
            )
        )

    def set_track_strip_eq_band(
        self, track_id: int, band_index: int, band: Mapping[str, object] | str
    ) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_eq_band_json(
                self._require_handle(),
                int(track_id),
                int(band_index),
                _band_json_arg(band),
            )
        )

    def set_track_strip_eq_band_json(self, track_id: int, band_index: int, band_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_eq_band_json(
                self._require_handle(),
                int(track_id),
                int(band_index),
                band_json.encode("utf-8"),
            )
        )

    def set_track_strip_insert_bypassed(
        self, track_id: int, insert_index: int, bypassed: bool, reset_on_bypass: bool = False
    ) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_insert_bypassed(
                self._require_handle(),
                int(track_id),
                int(insert_index),
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
                int(track_id),
                int(insert_index),
                param_name.encode("utf-8"),
                float(value),
            )
        )

    def set_track_strip_pan(self, track_id: int, pan: float) -> None:
        """Set a track strip's pan position (-1.0 hard left .. 1.0 hard right)."""
        _check(
            _get_lib().sonare_engine_set_track_strip_pan(
                self._require_handle(),
                int(track_id),
                float(pan),
            )
        )

    def set_track_strip_pan_law(self, track_id: int, pan_law: PanLaw | str | int) -> None:
        """Set a track strip's pan law (``PanLaw`` enum, name, or int 0..3)."""
        _check(
            _get_lib().sonare_engine_set_track_strip_pan_law(
                self._require_handle(),
                int(track_id),
                _pan_law_value(pan_law),
            )
        )

    def set_track_strip_pan_mode(self, track_id: int, pan_mode: str | int) -> None:
        """Set a track strip's pan mode (name 'balance'/'stereo-pan'/'dual-pan', or int 0..2)."""
        _check(
            _get_lib().sonare_engine_set_track_strip_pan_mode(
                self._require_handle(),
                int(track_id),
                _pan_mode_value(pan_mode),
            )
        )

    def set_track_strip_dual_pan(self, track_id: int, left_pan: float, right_pan: float) -> None:
        """Set a track strip's dual-pan positions for the left and right channels."""
        _check(
            _get_lib().sonare_engine_set_track_strip_dual_pan(
                self._require_handle(),
                int(track_id),
                float(left_pan),
                float(right_pan),
            )
        )

    def set_track_strip_channel_delay_samples(self, track_id: int, delay_samples: int) -> None:
        """Set a track strip's inter-channel delay in samples (Haas widening)."""
        _check(
            _get_lib().sonare_engine_set_track_strip_channel_delay_samples(
                self._require_handle(),
                int(track_id),
                int(delay_samples),
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
                int(band_index),
                _band_json_arg(band),
            )
        )

    def set_master_strip_eq_band_json(self, band_index: int, band_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_master_strip_eq_band_json(
                self._require_handle(),
                int(band_index),
                band_json.encode("utf-8"),
            )
        )

    def set_master_strip_insert_bypassed(
        self, insert_index: int, bypassed: bool, reset_on_bypass: bool = False
    ) -> None:
        _check(
            _get_lib().sonare_engine_set_master_strip_insert_bypassed(
                self._require_handle(),
                int(insert_index),
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
                int(insert_index),
                param_name.encode("utf-8"),
                float(value),
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
                int(bus_id),
                int(insert_index),
                param_name.encode("utf-8"),
                float(value),
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
                int(bus_id),
                int(insert_index),
                1 if bypassed else 0,
                1 if reset_on_bypass else 0,
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
        """
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_engine_resolve_track_insert_automation_id(
                self._require_handle(),
                int(track_id),
                int(insert_index),
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
                int(insert_index),
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
                int(bus_id),
                int(insert_index),
                param_name.encode("utf-8"),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)
