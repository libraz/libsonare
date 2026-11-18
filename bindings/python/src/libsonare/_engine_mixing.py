"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING, Literal, cast

from ._engine_conversions import _band_json_arg, _scene_entry_json
from ._errors import SonareValueError, _unknown_error
from ._ffi import SonareSurroundPan
from ._mixing import StripRef
from ._narrowing import _narrow_double
from ._runtime import (
    _UINT32_MAX,
    ChannelLayout,
    PanLaw,
    PanMode,
    SendTiming,
    SonareEngineBus,
    SonareEngineTrackLane,
    SonareEngineTrackSend,
    _channel_layout_value,
    _check,
    _get_lib,
    _mapping_keys,
    _narrow_int,
    _pan_law_value,
    _pan_mode_value,
    _send_timing_value,
    _sidechain_source_kind_value,
    _to_c_float,
    _to_c_int,
    _to_c_uint,
    _to_c_uint32,
    _token_arg,
    _utf8_arg,
)
from ._types_engine import (
    _SIDECHAIN_REFUSALS,
    MixSceneBusInput,
    MixSceneStripInput,
    SidechainCheck,
    SidechainRefusal,
    SidechainSource,
)
from .types import EqBandInput

StripAutomationKind = Literal["faderDb", "pan", "width"]
_STRIP_AUTOMATION_KINDS: tuple[str, ...] = ("faderDb", "pan", "width")


def _strip_target(fn: str, ref: StripRef) -> int | None:
    """Return the track id a strip reference names, or None for the master strip."""
    if isinstance(ref, str):
        if ref == "master":
            return None
        raise SonareValueError(f"{fn}: strip must be a track id or 'master', got {ref!r}")
    return _narrow_int(ref, "strip", 0, _UINT32_MAX)


def _track_only(fn: str, ref: StripRef) -> int:
    """Return the track id a strip reference names, refusing the master strip."""
    track_id = _strip_target(fn, ref)
    if track_id is None:
        raise SonareValueError(f"{fn}: the master strip has no {fn.removeprefix('set_')}")
    return track_id


_TRACK_SOURCE_KIND = 0


def _sidechain_source_pair(
    fn: str, source: SidechainSource | None, *, removable: bool
) -> tuple[int, int]:
    """Return the C ``(kind, id)`` of a sidechain source; ``None`` removes when ``removable``."""
    if source is None:
        if not removable:
            raise TypeError(f"{fn}: source must be a SidechainSource")
        return _TRACK_SOURCE_KIND, 0
    if not isinstance(source, SidechainSource):
        raise TypeError(f"{fn}: source must be a SidechainSource or None")
    kind = _sidechain_source_kind_value(source.kind)
    source_id = _narrow_int(source.id, "source.id", 0, _UINT32_MAX)
    if source_id == 0:
        raise SonareValueError(f"{fn}: source.id must not be 0 (pass None to remove the binding)")
    return kind, source_id


def _require_track_source(fn: str, kind: int) -> None:
    if kind != _TRACK_SOURCE_KIND:
        raise SonareValueError(f"{fn}: a track strip accepts only a source of kind 'track'")


def _optional_bus_id(value: object, name: str) -> int:
    """Return the C bus id of an optional bus: None is the master mix (0) and 0 is refused."""
    if value is None:
        return 0
    return _narrow_int(value, name, 1, _UINT32_MAX)


def _sidechain_check(refusal: int) -> SidechainCheck:
    # Code 0 is acceptance; refusal codes follow in token order.
    if refusal == 0:
        return SidechainCheck(ok=True, reason=None)
    if not 1 <= refusal <= len(_SIDECHAIN_REFUSALS):
        raise _unknown_error(f"unknown sidechain refusal code {refusal}")
    return SidechainCheck(ok=False, reason=cast(SidechainRefusal, _SIDECHAIN_REFUSALS[refusal - 1]))


_SEND_KEYS = ("bus_id", "level_db", "enabled", "send_timing")
_LANE_KEYS = ("track_id", "sends", "output_bus_id", "source_channel_layout")
_BUS_KEYS = ("bus_id", "gain_db", "channel_layout", "sends", "output_bus_id")


def _marshal_sends(
    sends: Sequence[Mapping[str, object]], where: str
) -> ctypes.Array[SonareEngineTrackSend]:
    """Pack send mappings into a ``SonareEngineTrackSend`` array."""
    send_array = (SonareEngineTrackSend * len(sends))()
    for send_index, raw_send in enumerate(sends):
        send = _mapping_keys(raw_send, _SEND_KEYS, f"{where}[{send_index}]")
        send_array[send_index].bus_id = cast(int, send["bus_id"])
        send_array[send_index].level_db = _narrow_double(send.get("level_db", 0.0), "level_db")
        send_array[send_index].enabled = 1 if bool(send.get("enabled", True)) else 0
        # Absent timing is post-fader, the C default.
        timing = send.get("send_timing")
        send_array[send_index].send_timing = (
            _send_timing_value(cast(SendTiming, timing)) if timing is not None else 0
        )
    return send_array


class _EngineMixingMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

    def set_track_lanes(self, lanes: Sequence[Mapping[str, object]]) -> None:
        """Declare the mixer track lanes in their order.

        Raises SonareValueError when ``lanes`` holds more than
        :data:`MAX_TRACK_LANES` lanes.
        """
        raw = (SonareEngineTrackLane * len(lanes))()
        send_arrays: list[object] = []
        for i, lane in enumerate(lanes):
            # ctypes zero-inits source_channel_layout to 0 (mono); default to
            # stereo so callers that omit it keep the
            # prior stereo behavior.
            raw[i].source_channel_layout = 1
            where = f"set_track_lanes: lanes[{i}]"
            lane = _mapping_keys(lane, _LANE_KEYS, where)
            # Assigned unconverted so the struct's own narrowing sees the
            # caller value; int() would truncate a fraction past it.
            raw[i].track_id = cast(int, lane["track_id"])
            sends = cast(Sequence[Mapping[str, object]], lane.get("sends", []))
            if sends:
                send_array = _marshal_sends(sends, f"{where}.sends")
                raw[i].sends = send_array
                raw[i].send_count = len(sends)
                send_arrays.append(send_array)
            # Narrowed rather than coerced: int(0.5) is the 0 this field
            # reads as "stay on the master mix", so a fractional bus id
            # would leave the lane unrouted and report success.
            raw[i].output_bus_id = _optional_bus_id(
                lane.get("output_bus_id"), f"{where}.output_bus_id"
            )
            if "source_channel_layout" in lane:
                raw[i].source_channel_layout = _channel_layout_value(
                    cast(ChannelLayout, lane["source_channel_layout"])
                )
        _check(_get_lib().sonare_engine_set_track_lanes(self._require_handle(), raw, len(lanes)))

    def set_track_sends(self, track_id: int, sends: Sequence[Mapping[str, object]]) -> None:
        """Replace one track lane's sends, leaving its output bus and every other lane as published.

        Each send is a mapping with ``bus_id``, optional ``level_db`` (default 0),
        ``enabled`` (default True) and ``send_timing``. An empty sequence clears the
        lane's sends. Goes through the same validation as :meth:`set_track_lanes`,
        so an unchanged send keeps its ramp. Raises ``SonareValueError`` for an
        unknown track (the lane list is empty before the first :meth:`set_track_lanes`),
        an undeclared or duplicate send bus, or a level outside the allowed range.
        Control thread only; not concurrent with processing.
        """
        send_array = _marshal_sends(sends, "set_track_sends: sends") if sends else None
        _check(
            _get_lib().sonare_engine_set_track_sends(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                send_array,
                len(sends),
            )
        )

    def set_track_output_bus(self, track_id: int, bus_id: int | None) -> None:
        """Route one track lane to a bus, leaving its sends and every other lane as published.

        ``None`` returns the lane to the master mix; 0 is refused. Raises ``SonareValueError``
        for an unknown track or an undeclared bus. Control thread only; not concurrent
        with processing.
        """
        _check(
            _get_lib().sonare_engine_set_track_output_bus(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint32(_optional_bus_id(bus_id, "bus_id"), "bus_id"),
            )
        )

    def set_strip_sidechain(
        self, ref: StripRef, insert_index: int, source: SidechainSource | None
    ) -> None:
        """Key one insert of a strip from another signal; ``None`` removes the binding.

        For ducking/sidechainRouter inserts. A track strip accepts only a source of kind
        ``"track"``; the master strip takes a track lane or a bus, and for it
        ``insert_index`` counts the pre-fader inserts first, then the post-fader ones.
        A key naming the track itself, or a binding that would close a cycle across the
        bindings as a whole, raises and leaves the existing bindings unchanged. The key
        is taken after the source's lane strip and delay-compensated, so the result
        depends on neither lane order nor block size. Control thread only; not
        concurrent with processing.
        """
        fn = "set_strip_sidechain"
        track_id = _strip_target(fn, ref)
        kind, source_id = _sidechain_source_pair(fn, source, removable=True)
        if track_id is None:
            rc = _get_lib().sonare_engine_set_master_sidechain(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                _to_c_int(kind, "source.kind"),
                _to_c_uint32(source_id, "source.id"),
            )
        else:
            _require_track_source(fn, kind)
            rc = _get_lib().sonare_engine_set_lane_sidechain(
                self._require_handle(),
                _to_c_uint32(track_id, "strip"),
                _to_c_uint(insert_index, "insert_index"),
                _to_c_uint32(source_id, "source.id"),
            )
        _check(rc)

    def can_set_strip_sidechain(
        self, ref: StripRef, insert_index: int, source: SidechainSource
    ) -> SidechainCheck:
        """Report whether :meth:`set_strip_sidechain` would accept a binding.

        Changes nothing; same threading contract as the setter.
        """
        fn = "can_set_strip_sidechain"
        track_id = _strip_target(fn, ref)
        kind, source_id = _sidechain_source_pair(fn, source, removable=False)
        refusal = ctypes.c_int()
        if track_id is None:
            rc = _get_lib().sonare_engine_can_set_master_sidechain(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                _to_c_int(kind, "source.kind"),
                _to_c_uint32(source_id, "source.id"),
                ctypes.byref(refusal),
            )
        else:
            _require_track_source(fn, kind)
            rc = _get_lib().sonare_engine_can_set_lane_sidechain(
                self._require_handle(),
                _to_c_uint32(track_id, "strip"),
                _to_c_uint(insert_index, "insert_index"),
                _to_c_uint32(source_id, "source.id"),
                ctypes.byref(refusal),
            )
        _check(rc)
        return _sidechain_check(refusal.value)

    def set_bus_sidechain(
        self, bus_id: int, insert_index: int, source: SidechainSource | None
    ) -> None:
        """Key one insert of a bus strip from a track lane or another bus; ``None`` removes it.

        The key is taken before the source's lane fader or bus gain. Control thread
        only; not concurrent with processing.
        """
        kind, source_id = _sidechain_source_pair("set_bus_sidechain", source, removable=True)
        _check(
            _get_lib().sonare_engine_set_bus_sidechain(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                _to_c_int(kind, "source.kind"),
                _to_c_uint32(source_id, "source.id"),
            )
        )

    def can_set_bus_sidechain(
        self, bus_id: int, insert_index: int, source: SidechainSource
    ) -> SidechainCheck:
        """Report whether :meth:`set_bus_sidechain` would accept a binding."""
        kind, source_id = _sidechain_source_pair("can_set_bus_sidechain", source, removable=False)
        refusal = ctypes.c_int()
        _check(
            _get_lib().sonare_engine_can_set_bus_sidechain(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                _to_c_int(kind, "source.kind"),
                _to_c_uint32(source_id, "source.id"),
                ctypes.byref(refusal),
            )
        )
        return _sidechain_check(refusal.value)

    def set_track_buses(self, buses: Sequence[Mapping[str, object]]) -> None:
        raw = (SonareEngineBus * len(buses))()
        send_arrays: list[object] = []
        for i, raw_bus in enumerate(buses):
            where = f"set_track_buses: buses[{i}]"
            bus = _mapping_keys(raw_bus, _BUS_KEYS, where)
            # Assigned unconverted so the struct's own narrowing sees the caller
            # value; int() would truncate a fraction past it.
            raw[i].bus_id = cast(int, bus["bus_id"])
            raw[i].gain_db = _narrow_double(bus.get("gain_db", 0.0), "gain_db")
            # ctypes zero-inits channel_layout to 0 (mono); default to stereo
            # unless the caller specifies it.
            if "channel_layout" in bus:
                raw[i].channel_layout = _channel_layout_value(
                    cast(ChannelLayout, bus["channel_layout"])
                )
            else:
                raw[i].channel_layout = 1
            sends = cast(Sequence[Mapping[str, object]], bus.get("sends", []))
            if sends:
                send_array = _marshal_sends(sends, f"{where}.sends")
                raw[i].sends = send_array
                raw[i].send_count = len(sends)
                send_arrays.append(send_array)
            # Narrowed rather than coerced: int(0.5) is the 0 this field reads
            # as "stay on the master mix", so a fractional bus id would leave
            # the bus unrouted and report success.
            raw[i].output_bus_id = _optional_bus_id(
                bus.get("output_bus_id"), f"{where}.output_bus_id"
            )
        _check(_get_lib().sonare_engine_set_track_buses(self._require_handle(), raw, len(buses)))

    def set_bus_strip(self, bus_id: int, settings: MixSceneBusInput) -> None:
        """Replace a bus's strip with ``settings``; the bus must exist via :meth:`set_track_buses`.

        ``settings`` is a :data:`MixSceneBusInput` mapping; anything else raises ``TypeError``.
        """
        _check(
            _get_lib().sonare_engine_set_bus_strip_json(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _scene_entry_json(settings, "buses", "settings"),
            )
        )

    def set_strip(self, ref: StripRef, settings: MixSceneStripInput) -> None:
        """Replace a strip with ``settings``: a track id or ``"master"`` names the strip.

        ``settings`` is a :data:`MixSceneStripInput` mapping; anything else raises ``TypeError``.
        The mapping carries its own ``id``; the engine applies it to the addressed strip.
        """
        track_id = _strip_target("set_strip", ref)
        payload = _scene_entry_json(settings, "strips", "settings")
        if track_id is None:
            rc = _get_lib().sonare_engine_set_master_strip_json(self._require_handle(), payload)
        else:
            rc = _get_lib().sonare_engine_set_track_strip_json(
                self._require_handle(), _to_c_uint32(track_id, "strip"), payload
            )
        _check(rc)

    def set_strip_eq_band(self, ref: StripRef, band_index: int, band: EqBandInput) -> None:
        """Replace one equalizer band of a strip in realtime; a track id or ``"master"``."""
        fn = "set_strip_eq_band"
        track_id = _strip_target(fn, ref)
        if track_id is None:
            rc = _get_lib().sonare_engine_set_master_strip_eq_band_json(
                self._require_handle(), _to_c_int(band_index, "band_index"), _band_json_arg(band)
            )
        else:
            rc = _get_lib().sonare_engine_set_track_strip_eq_band_json(
                self._require_handle(),
                _to_c_uint32(track_id, "strip"),
                _to_c_int(band_index, "band_index"),
                _band_json_arg(band),
            )
        _check(rc)

    def set_bus_eq_band(self, bus_id: int, band_index: int, band: EqBandInput) -> None:
        """Replace one equalizer band of a bus strip in realtime."""
        _check(
            _get_lib().sonare_engine_set_bus_strip_eq_band_json(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_int(band_index, "band_index"),
                _band_json_arg(band),
            )
        )

    def set_strip_insert_bypassed(
        self, ref: StripRef, insert_index: int, bypassed: bool, reset_on_bypass: bool = False
    ) -> None:
        """Bypass or engage one strip insert; ``reset_on_bypass`` clears its state when bypassed.

        For the master strip ``insert_index`` counts the pre-fader inserts first, then the
        post-fader inserts.
        """
        track_id = _strip_target("set_strip_insert_bypassed", ref)
        if track_id is None:
            rc = _get_lib().sonare_engine_set_master_strip_insert_bypassed(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                1 if bypassed else 0,
                1 if reset_on_bypass else 0,
            )
        else:
            rc = _get_lib().sonare_engine_set_track_strip_insert_bypassed(
                self._require_handle(),
                _to_c_uint32(track_id, "strip"),
                _to_c_uint(insert_index, "insert_index"),
                1 if bypassed else 0,
                1 if reset_on_bypass else 0,
            )
        _check(rc)

    def set_bus_insert_bypassed(
        self, bus_id: int, insert_index: int, bypassed: bool, reset_on_bypass: bool = False
    ) -> None:
        """Bus counterpart of :meth:`set_strip_insert_bypassed`.

        ``bus_id`` must carry a strip configured via :meth:`set_bus_strip`.
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

    def set_strip_insert_param(
        self, ref: StripRef, insert_index: int, name: str, value: float
    ) -> None:
        """Change one strip insert parameter in realtime, addressed by its JSON-key name.

        Applied at the next block head through the engine command queue; safe during
        playback. Raises if the strip, insert or name is unknown, the parameter is not
        realtime-safe, or the queue is full.
        """
        track_id = _strip_target("set_strip_insert_param", ref)
        if track_id is None:
            rc = _get_lib().sonare_engine_set_master_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(name, "name"),
                _to_c_float(value, "value"),
            )
        else:
            rc = _get_lib().sonare_engine_set_track_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint32(track_id, "strip"),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(name, "name"),
                _to_c_float(value, "value"),
            )
        _check(rc)

    def set_bus_insert_param(self, bus_id: int, insert_index: int, name: str, value: float) -> None:
        """Bus counterpart of :meth:`set_strip_insert_param`.

        ``bus_id`` must carry a strip configured via :meth:`set_bus_strip`.
        """
        _check(
            _get_lib().sonare_engine_set_bus_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(name, "name"),
                _to_c_float(value, "value"),
            )
        )

    def apply_strip_insert_param_now(
        self, ref: StripRef, insert_index: int, name: str, value: float
    ) -> bool:
        """Apply one strip insert parameter immediately, bypassing the command queue.

        Runs on the calling (engine-owning) thread and is recorded as the parameter's
        manual base like :meth:`set_strip_insert_param`. An unknown strip, insert or
        name is reported by a ``False`` return instead of raising; not safe
        concurrently with :meth:`process`.
        """
        out_applied = ctypes.c_int()
        track_id = _strip_target("apply_strip_insert_param_now", ref)
        if track_id is None:
            rc = _get_lib().sonare_engine_apply_master_strip_insert_param_by_name_now(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(name, "name"),
                _to_c_float(value, "value"),
                ctypes.byref(out_applied),
            )
        else:
            rc = _get_lib().sonare_engine_apply_track_strip_insert_param_by_name_now(
                self._require_handle(),
                _to_c_uint32(track_id, "strip"),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(name, "name"),
                _to_c_float(value, "value"),
                ctypes.byref(out_applied),
            )
        _check(rc)
        return bool(out_applied.value)

    def apply_bus_insert_param_now(
        self, bus_id: int, insert_index: int, name: str, value: float
    ) -> bool:
        """Bus counterpart of :meth:`apply_strip_insert_param_now`."""
        out_applied = ctypes.c_int()
        _check(
            _get_lib().sonare_engine_apply_bus_strip_insert_param_by_name_now(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(name, "name"),
                _to_c_float(value, "value"),
                ctypes.byref(out_applied),
            )
        )
        return bool(out_applied.value)

    def restore_strip_insert_param(
        self, ref: StripRef, insert_index: int, name: str, value: float
    ) -> None:
        """Restore a retained strip insert value exactly, without a ramp.

        Retires any in-flight smoother for the parameter and records the value as the
        manual base. Used to replay retained edits after :meth:`set_strip`
        replaced the strip. Raises if the strip, insert or name is unknown; not safe
        concurrently with :meth:`process`.
        """
        track_id = _strip_target("restore_strip_insert_param", ref)
        if track_id is None:
            rc = _get_lib().sonare_engine_restore_master_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(name, "name"),
                _to_c_float(value, "value"),
            )
        else:
            rc = _get_lib().sonare_engine_restore_track_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint32(track_id, "strip"),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(name, "name"),
                _to_c_float(value, "value"),
            )
        _check(rc)

    def restore_bus_insert_param(
        self, bus_id: int, insert_index: int, name: str, value: float
    ) -> None:
        """Bus counterpart of :meth:`restore_strip_insert_param`."""
        _check(
            _get_lib().sonare_engine_restore_bus_strip_insert_param_by_name(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(name, "name"),
                _to_c_float(value, "value"),
            )
        )

    def clear_strip_insert_parameter_bases(self, ref: StripRef) -> None:
        """Forget the remembered manual insert-parameter values of one strip.

        Also discards its queued insert edits. Call before :meth:`set_strip`
        replaces the strip when its old values must not carry over; the setter never
        does this itself, since a queued edit may already target the new chain. Raises
        if a track is not a published lane; not safe concurrently with :meth:`process`.
        """
        track_id = _strip_target("clear_strip_insert_parameter_bases", ref)
        if track_id is None:
            rc = _get_lib().sonare_engine_clear_master_insert_parameter_bases(
                self._require_handle()
            )
        else:
            rc = _get_lib().sonare_engine_clear_track_insert_parameter_bases(
                self._require_handle(), _to_c_uint32(track_id, "strip")
            )
        _check(rc)

    def clear_bus_insert_parameter_bases(self, bus_id: int) -> None:
        """Bus counterpart of :meth:`clear_strip_insert_parameter_bases`.

        Also accepts a bus identity that was already removed, covering every
        selector the identity held across remove/re-add.
        """
        _check(
            _get_lib().sonare_engine_clear_bus_insert_parameter_bases(
                self._require_handle(), _to_c_uint32(bus_id, "bus_id")
            )
        )

    def set_strip_pan(self, ref: StripRef, pan: float) -> None:
        """Set a track strip's pan position (-1.0 hard left .. 1.0 hard right).

        The master strip has no pan setter here; drive its pan through
        :meth:`resolve_strip_automation_id`.
        """
        _check(
            _get_lib().sonare_engine_set_track_strip_pan(
                self._require_handle(),
                _to_c_uint32(_track_only("set_strip_pan", ref), "strip"),
                _to_c_float(pan, "pan"),
            )
        )

    def set_strip_pan_law(self, ref: StripRef, pan_law: PanLaw) -> None:
        """Set a track strip's pan law (a :data:`PanLaw` token such as ``"const3Db"``).

        Balance mode keeps centre unity and uses the law only for the far-channel
        taper; stereo-pan and dual-pan modes take the law's centre attenuation.
        A mono source is unity at centre under every law (see :data:`PanLaw`).
        """
        _check(
            _get_lib().sonare_engine_set_track_strip_pan_law(
                self._require_handle(),
                _to_c_uint32(_track_only("set_strip_pan_law", ref), "strip"),
                _pan_law_value(pan_law),
            )
        )

    def set_strip_pan_mode(self, ref: StripRef, pan_mode: PanMode) -> None:
        """Set a track strip's pan mode (a :data:`PanMode` token)."""
        _check(
            _get_lib().sonare_engine_set_track_strip_pan_mode(
                self._require_handle(),
                _to_c_uint32(_track_only("set_strip_pan_mode", ref), "strip"),
                _pan_mode_value(pan_mode),
            )
        )

    def set_strip_dual_pan(self, ref: StripRef, left_pan: float, right_pan: float) -> None:
        """Set a track strip's dual-pan positions for the left and right channels."""
        _check(
            _get_lib().sonare_engine_set_track_strip_dual_pan(
                self._require_handle(),
                _to_c_uint32(_track_only("set_strip_dual_pan", ref), "strip"),
                _to_c_float(left_pan, "left_pan"),
                _to_c_float(right_pan, "right_pan"),
            )
        )

    def set_strip_surround_pan(
        self,
        ref: StripRef,
        *,
        azimuth: float = 0.0,
        elevation: float = 0.0,
        divergence: float = 0.0,
        lfe: float = 0.0,
        distance: float = 1.0,
    ) -> None:
        """Set a track strip's surround pan position without a graph rebuild.

        Fields as in :meth:`Mixer.set_surround_pan`; a ``distance`` of 0 or less
        means the default of 1. Applied when the lane feeds a bus with more than
        two channels; non-finite or out-of-range ``azimuth`` / ``divergence`` /
        ``lfe`` raise :class:`SonareValueError` rather than being clamped.
        """
        track_id = _track_only("set_strip_surround_pan", ref)
        pan = SonareSurroundPan(
            azimuth=_to_c_float(azimuth, "azimuth"),
            elevation=_to_c_float(elevation, "elevation"),
            divergence=_to_c_float(divergence, "divergence"),
            lfe=_to_c_float(lfe, "lfe"),
            distance=_to_c_float(distance, "distance"),
        )
        _check(
            _get_lib().sonare_engine_set_track_strip_surround_pan(
                self._require_handle(), _to_c_uint32(track_id, "strip"), ctypes.byref(pan)
            )
        )

    def set_strip_channel_delay_samples(self, ref: StripRef, delay_samples: int) -> None:
        """Set a track strip's alignment delay in samples, moving it later than the other lanes."""
        _check(
            _get_lib().sonare_engine_set_track_strip_channel_delay_samples(
                self._require_handle(),
                _to_c_uint32(_track_only("set_strip_channel_delay_samples", ref), "strip"),
                _to_c_int(delay_samples, "delay_samples"),
            )
        )

    def set_bus_pan(self, bus_id: int, pan: float) -> None:
        """Set a bus strip's pan position (-1.0 hard left .. 1.0 hard right).

        Rejected on a surround (>2ch) bus, where pan is not defined.
        """
        _check(
            _get_lib().sonare_engine_set_bus_strip_pan(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_float(pan, "pan"),
            )
        )

    def set_bus_pan_law(self, bus_id: int, pan_law: PanLaw) -> None:
        """Set a bus strip's pan law (a :data:`PanLaw` token); rejected on a surround bus."""
        _check(
            _get_lib().sonare_engine_set_bus_strip_pan_law(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _pan_law_value(pan_law),
            )
        )

    def set_bus_pan_mode(self, bus_id: int, pan_mode: PanMode) -> None:
        """Set a bus strip's pan mode (a :data:`PanMode` token); rejected on a surround bus."""
        _check(
            _get_lib().sonare_engine_set_bus_strip_pan_mode(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _pan_mode_value(pan_mode),
            )
        )

    def set_bus_dual_pan(self, bus_id: int, left_pan: float, right_pan: float) -> None:
        """Set a bus strip's dual-pan positions; rejected on a surround bus."""
        _check(
            _get_lib().sonare_engine_set_bus_strip_dual_pan(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_float(left_pan, "left_pan"),
                _to_c_float(right_pan, "right_pan"),
            )
        )

    def resolve_strip_insert_automation_id(
        self, ref: StripRef, insert_index: int, param_name: str
    ) -> int:
        """Resolve a strip insert parameter to its reserved automation id.

        The returned id drives :meth:`set_automation_lane`,
        :meth:`set_parameter`, or :meth:`set_parameter_smoothed` exactly like a
        :meth:`resolve_strip_automation_id` id. Raises :class:`SonareValueError`
        if the strip, insert, or name is unknown.

        This is how a mastering processor gets time-varying automation:
        the ``eq.*``, ``dynamics.*``, ``saturation.*``, ``spectral.*``,
        ``stereo.*``, ``maximizer.*`` and ``multiband.*`` processors are all
        available as strip inserts, so placing one on a strip and resolving its
        parameter here drives it at audio-block precision, live and offline
        alike. The whole-signal stages of the offline mastering chain that have no insert form
        (``repair.declick``, ``repair.declip``, ``repair.trimSilence``, ``loudness``, and the match
        stages) have no automation id: they buffer the entire signal by construction and do not run
        on the realtime path. The four realtime repair inserts (``repair.decrackle``,
        ``repair.dehum``, ``repair.denoiseClassical``, ``repair.dereverbClassical``) load into a
        strip, but every one of their parameters is construction-only, so no automation id resolves
        and an edit needs a strip replacement.

        Track, bus and master insert ids share one lifetime rule: an id names
        its strip by identity and the kind of processor in its slot (for
        ``effects.gsEfx``, its EFX type too), so it survives lane and bus
        reorders and the removal of other strips, and stays valid until its
        track or bus is removed or its slot comes to hold another kind of
        processor. After that it applies nothing, its queued edits are dropped,
        and it is never reissued; resolve again for the new processor. A change
        that would take the engine past its 8192 insert-id entries is refused.
        """
        out_id = ctypes.c_uint32()
        track_id = _strip_target("resolve_strip_insert_automation_id", ref)
        if track_id is None:
            rc = _get_lib().sonare_engine_resolve_master_insert_automation_id(
                self._require_handle(),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(param_name, "param_name"),
                ctypes.byref(out_id),
            )
        else:
            rc = _get_lib().sonare_engine_resolve_track_insert_automation_id(
                self._require_handle(),
                _to_c_uint32(track_id, "strip"),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(param_name, "param_name"),
                ctypes.byref(out_id),
            )
        _check(rc)
        return int(out_id.value)

    def resolve_bus_insert_automation_id(
        self, bus_id: int, insert_index: int, param_name: str
    ) -> int:
        """Bus-strip counterpart of :meth:`resolve_strip_insert_automation_id`.

        ``bus_id`` must carry a strip configured via :meth:`set_bus_strip`.
        """
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_engine_resolve_bus_insert_automation_id(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _to_c_uint(insert_index, "insert_index"),
                _utf8_arg(param_name, "param_name"),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def resolve_strip_automation_id(self, ref: StripRef, kind: StripAutomationKind) -> int:
        """Resolve a strip's ``"faderDb"``, ``"pan"`` or, for the master only, ``"width"``.

        Same lifetime rule as :meth:`resolve_strip_insert_automation_id`: a track id
        names the track, not its lane position, so it keeps driving that track across
        :meth:`set_track_lanes` reorders and applies nothing once the track is removed;
        the master strip always exists, so its id never goes stale. Raises
        :class:`SonareValueError` when the track has no lane, the kind is unknown, or
        ``"width"`` is asked of a track.
        """
        fn = "resolve_strip_automation_id"
        track_id = _strip_target(fn, ref)
        token = _token_arg(kind, _STRIP_AUTOMATION_KINDS, "kind")
        out_id = ctypes.c_uint32()
        if track_id is None:
            rc = _get_lib().sonare_engine_resolve_master_automation_id(
                self._require_handle(), _utf8_arg(token, "kind"), ctypes.byref(out_id)
            )
        else:
            if token == "width":
                raise SonareValueError(f"{fn}: a track strip has no 'width' parameter")
            rc = _get_lib().sonare_engine_resolve_track_lane_automation_id(
                self._require_handle(),
                _to_c_uint32(track_id, "strip"),
                _utf8_arg(token, "kind"),
                ctypes.byref(out_id),
            )
        _check(rc)
        return int(out_id.value)

    def resolve_bus_automation_id(self, bus_id: int, param_name: str) -> int:
        """Resolve a bus's fader to its reserved automation id.

        ``param_name`` is ``"faderDb"``. Same lifetime rule as
        :meth:`resolve_strip_insert_automation_id`: the id names the bus, not its
        position in the bus list, so it keeps driving that bus across
        :meth:`set_track_buses` reorders and the removal of other buses, and
        applies nothing once the bus is removed. Raises :class:`SonareValueError` if
        the bus is not configured or the name is not ``"faderDb"``.
        """
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_engine_resolve_bus_automation_id(
                self._require_handle(),
                _to_c_uint32(bus_id, "bus_id"),
                _utf8_arg(param_name, "param_name"),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)
