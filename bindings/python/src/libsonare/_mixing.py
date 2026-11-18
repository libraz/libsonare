"""Mixing wrappers for libsonare."""

from __future__ import annotations

import ctypes
import json
import typing
from collections.abc import Sequence
from dataclasses import dataclass

import numpy as np

from ._errors import _not_supported, _require_open
from ._ffi import SonareMixGoniometerPoint, SonareSurroundPan
from ._ffi_types_analysis import SonareMixerMasterMeterConfig
from ._narrowing import _resolve_zero_default
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    AutomationCurve,
    ErrorCode,
    MeterTap,
    MixMeterSnapshot,
    MixResult,
    PanLaw,
    PanMode,
    SendTiming,
    SonareMixMeterSnapshot,
    SonareValueError,
    _check,
    _check_realtime,
    _curve_value,
    _from_c_float_array,
    _get_lib,
    _joined_names,
    _last_diagnostics,
    _last_error,
    _max_block_size,
    _meter_tap_value,
    _mix_meter_from_c,
    _narrow_int,
    _pan_law_value,
    _pan_mode_value,
    _send_timing_value,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int64,
    _to_c_size_t,
    _to_c_uint,
    _to_c_uint32,
    _token_arg,
    _utf8_arg,
    _validate_samples,
)
from ._types_engine import MixSceneDocument
from ._types_enums import _MIX_BUS_ROLES, MixBusRole
from .types import GoniometerPoint

# A strip is addressed either by its index in [0, strip_count()) or by its
# string id (as declared in the scene JSON).
StripRef = int | str
"""A mixer strip: its index in ``[0, strip_count())`` or its scene string id."""

# Upper bound on the goniometer working buffer, mirroring the strip ring
# (``sonare::mixing::ChannelStrip::kGoniometerCapacity``). One read can never
# return more than the ring holds, so capping here drops no point; it only stops
# a caller-supplied ``max_points`` from sizing a multi-gigabyte ctypes array.
_GONIOMETER_READ_CAP = 4096


@dataclass(frozen=True, slots=True, eq=False)
class MixerStereoResult:
    """Stereo master output of :meth:`Mixer.process_stereo`.

    Mirrors the Node/WASM ``{left, right, sampleRate}`` result shape.
    """

    left: np.ndarray
    """Left channel of the master output."""
    right: np.ndarray
    """Right channel of the master output."""
    sample_rate: int
    """Sample rate of the output in Hz."""


_MIN_SAMPLE_RATE = 8000
_MAX_SAMPLE_RATE = 384000


def mixing_scene_preset_names() -> list[str]:
    """Return built-in mixer scene preset identifiers."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mixing_scene_preset_names"):
        raise _not_supported("libsonare was built without mixing support")
    return _joined_names(lib.sonare_mixing_scene_preset_names(), "mixing scene preset names")


def mixing_scene_preset(name: str) -> MixSceneDocument:
    """Return a built-in mixer scene preset as a scene document mapping.

    Raises ``TypeError`` for a non-string ``name`` and ``SonareValueError`` for a
    name :func:`mixing_scene_preset_names` does not list.
    """
    if not isinstance(name, str):
        raise TypeError(f"mixing_scene_preset: name must be a string, got {type(name).__name__}")
    names = mixing_scene_preset_names()
    if name not in names:
        raise SonareValueError(f"mixing_scene_preset: name must be one of {names}, got {name!r}")
    lib = _get_lib()
    json_ptr = ctypes.c_char_p()
    _check(lib.sonare_mixing_scene_preset_json(_utf8_arg(name, "name"), ctypes.byref(json_ptr)))
    try:
        text = ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else "{}"
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)
    return typing.cast(MixSceneDocument, json.loads(text))


class Mixer:
    """Scene-based persistent stereo mixer.

    Built from a scene JSON string (see :func:`mixing_scene_preset`), it
    routes per-strip stereo blocks through a compiled routing graph (sends,
    buses, inserts) into a stereo master.

    Strips are addressed by index in ``[0, strip_count())`` or by their string
    id. Strip handles are *borrowed*: they are owned by the native mixer and
    must not outlive it. This wrapper never exposes a raw handle and always
    resolves a fresh handle from the live mixer for each operation, so a strip
    method can only run while the mixer is open (:meth:`close` invalidates
    every strip). Do not retain results across a :meth:`close` call.
    """

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: int | None = None

    def __init__(
        self,
        handle: int,
        sample_rate: int,
        scene_warnings: list[str] | None = None,
    ) -> None:
        self._handle = handle
        self._sample_rate = sample_rate
        self._scene_warnings: list[str] = scene_warnings or []

    @classmethod
    def from_scene_json(
        cls, json: str, sample_rate: int, max_block_size: int | None = None
    ) -> Mixer:
        """Build a mixer from a scene JSON string.

        ``max_block_size`` bounds the frames of every process and drain call; an
        omitted value takes the library default and an explicit 0 is refused.

        A strip's meters are sized when the strip is built, so this is where
        their configuration is chosen: an optional ``"metering"`` object on the
        strip (``enabled`` / ``lufs`` / ``truePeak`` / ``truePeakOversample``)
        selects it, and leaving it out keeps the full default (LUFS + true peak
        at 4x, about 1.4 MB per strip at 48 kHz). ``{"enabled": false}`` drops
        both meters for a strip whose snapshots are never read.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_from_scene_json"):
            raise _not_supported("libsonare was built without mixing support")
        handle = lib.sonare_mixer_from_scene_json(
            _utf8_arg(json, "json"),
            _narrow_int(sample_rate, "sample_rate", _MIN_SAMPLE_RATE, _MAX_SAMPLE_RATE),
            _to_c_int(_resolve_zero_default(max_block_size, "max_block_size"), "max_block_size"),
        )
        if not handle:
            raise _last_error(
                int(ErrorCode.INVALID_PARAMETER), "failed to build mixer from scene JSON"
            )
        # Capture the load's warnings (e.g. insert params no processor read)
        # immediately, before any later C-ABI call overwrites them.
        warnings = [message for _, message, _ in _last_diagnostics()]
        return cls(int(handle), sample_rate, warnings)

    def scene_warnings(self) -> list[str]:
        """Non-fatal warnings captured when this mixer was built from scene JSON.

        One entry per channel-strip insert that was handed param keys no processor
        reads (a likely typo, or a key meant for a different processor), and one per
        send to a destination the scene does not declare. The scene still loaded;
        those keys and sends simply took no effect. Empty when nothing was ignored.
        Use :func:`mastering_insert_param_names` to discover the keys an insert
        accepts. An unknown scene key is not a warning: :meth:`from_scene_json`
        raises ``SonareError`` (``INVALID_FORMAT``) naming its path.
        """
        return list(self._scene_warnings)

    def compile(self) -> None:
        """Rebuild the graph while preserving automation position and queued events."""
        self._require()
        _check(_get_lib().sonare_mixer_compile(self._handle))

    @property
    def max_block_size(self) -> int:
        """The largest ``num_samples`` one process call accepts."""
        self._require()
        return _max_block_size(_get_lib().sonare_mixer_max_block_size, self._handle)

    @property
    def strip_count(self) -> int:
        """Return the number of strips in the mixer."""
        self._require()
        lib = _get_lib()
        if hasattr(lib, "sonare_mixer_get_strip_count"):
            out = ctypes.c_size_t()
            _check(lib.sonare_mixer_get_strip_count(self._handle, ctypes.byref(out)))
            return int(out.value)
        if not hasattr(lib, "sonare_mixer_strip_count"):
            raise _not_supported("libsonare was built without insert-automation support")
        return int(lib.sonare_mixer_strip_count(self._handle))

    def add_strip(
        self,
        id: str,
        *,
        enabled: bool = True,
        lufs: bool = True,
        true_peak: bool = True,
        true_peak_oversample: int | None = None,
    ) -> int:
        """Add a channel strip to the mixer topology.

        The metering keywords are the strip's ``"metering"`` object from scene
        JSON under their Python spelling, and the defaults are the full
        configuration :meth:`from_scene_json` applies to a strip that omits it
        (LUFS + true peak at 4x, about 1.4 MB per strip at 48 kHz).
        ``enabled=False`` drops both meters for a strip whose snapshots are never
        read. ``true_peak_oversample`` is 4 or 8; ``None`` keeps the
        library default (4), and any other value is refused.

        The routing graph is marked dirty; call :meth:`compile` (or
        :meth:`process_stereo`) to rebuild. Returns the new strip's index.

        Raises:
            RuntimeError: the id is already taken, or ``true_peak_oversample``
                is not 0 (default), 4 or 8.
        """
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_add_strip_ex"):
            raise _not_supported("libsonare was built without mixing support")
        # Returns the strip pointer rather than a SonareError, and NULL is the
        # whole failure signal -- the detail is in the thread-local error slot.
        # The pointer itself is mixer-owned and never surfaces: every strip op on
        # this facade addresses a strip by index or id.
        handle = lib.sonare_mixer_add_strip_ex(
            self._handle,
            _utf8_arg(id, "id"),
            1 if enabled else 0,
            1 if lufs else 0,
            1 if true_peak else 0,
            _to_c_int(
                _resolve_zero_default(true_peak_oversample, "true_peak_oversample"),
                "true_peak_oversample",
            ),
        )
        if not handle:
            raise _last_error(int(ErrorCode.INVALID_PARAMETER), f"failed to add strip {id!r}")
        return self.strip_by_id(id)

    def add_bus(self, id: str, role: MixBusRole = "aux") -> None:
        """Add a bus to the mixer topology.

        ``role`` is one of ``"master"``, ``"aux"``, or ``"submix"`` and defaults
        to ``"aux"``; an unknown token raises :class:`SonareValueError` and a
        non-string ``TypeError``. The routing graph is marked dirty; call
        :meth:`compile` (or :meth:`process_stereo`) to rebuild.
        """
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_add_bus"):
            raise _not_supported("libsonare was built without mixer bus support")
        _check(
            lib.sonare_mixer_add_bus(
                self._handle,
                _utf8_arg(id, "id"),
                _utf8_arg(_token_arg(role, _MIX_BUS_ROLES, "role"), "role"),
            )
        )

    def remove_bus(self, id: str) -> None:
        """Remove a bus by id from the mixer topology."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_remove_bus"):
            raise _not_supported("libsonare was built without mixer bus support")
        _check(lib.sonare_mixer_remove_bus(self._handle, _utf8_arg(id, "id")))

    def set_output_bus(self, source_id: str, bus_id: str) -> None:
        """Send a strip or bus into a submix bus, or back to the master.

        ``source_id`` is a strip or a non-master bus; ``bus_id`` is a declared bus
        or the master id the compile resolves (the literal ``"master"`` is not an
        alias when the master bus has another id). Every connection leaving
        ``source_id`` is replaced, including a strip-to-strip edge authored in
        scene JSON. Lazy like :meth:`add_bus`: nothing compiles in this call, so
        call :meth:`compile` afterwards. A refused call (unknown id, master as
        source, self-route, an edit closing a cycle) raises ``SonareValueError``
        naming the ids and leaves the topology unchanged.
        Control thread only.

        Two calls route a strip through a submix to the master::

            mixer.set_output_bus("a", "sub")
            mixer.set_output_bus("sub", "master")
            mixer.compile()
        """
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_set_output_bus"):
            raise _not_supported("libsonare was built without mixer bus support")
        _check(
            lib.sonare_mixer_set_output_bus(
                self._handle, _utf8_arg(source_id, "source_id"), _utf8_arg(bus_id, "bus_id")
            )
        )

    @property
    def bus_count(self) -> int:
        """Return the number of buses in the mixer topology."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_bus_count"):
            raise _not_supported("libsonare was built without mixer bus support")
        out = ctypes.c_size_t()
        _check(lib.sonare_mixer_bus_count(self._handle, ctypes.byref(out)))
        return int(out.value)

    def add_vca_group(
        self, id: str, gain_db: float = 0.0, members: Sequence[str] | None = None
    ) -> None:
        """Add a VCA group with the given id, gain offset, and strip members."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_add_vca_group"):
            raise _not_supported("libsonare was built without mixer VCA support")
        member_list = list(members or [])
        if member_list:
            member_array = (ctypes.c_char_p * len(member_list))(
                *[_utf8_arg(m, "m") for m in member_list]
            )
            member_ptr = ctypes.cast(member_array, ctypes.POINTER(ctypes.c_char_p))
        else:
            member_ptr = None
        _check(
            lib.sonare_mixer_add_vca_group(
                self._handle,
                _utf8_arg(id, "id"),
                _to_c_float(gain_db, "gain_db"),
                member_ptr,
                ctypes.c_size_t(len(member_list)),
            )
        )

    def remove_vca_group(self, id: str) -> None:
        """Remove a VCA group by id from the mixer topology."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_remove_vca_group"):
            raise _not_supported("libsonare was built without mixer VCA support")
        _check(lib.sonare_mixer_remove_vca_group(self._handle, _utf8_arg(id, "id")))

    def set_vca_group_gain_db(self, id: str, gain_db: float) -> None:
        """Set an existing VCA group's gain in dB."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_set_vca_group_gain_db"):
            raise _not_supported("libsonare was built without mixer VCA support")
        _check(
            lib.sonare_mixer_set_vca_group_gain_db(
                self._handle,
                _utf8_arg(id, "id"),
                _to_c_float(gain_db, "gain_db"),
            )
        )

    def set_vca_group_members(self, id: str, members: Sequence[str]) -> None:
        """Replace an existing VCA group's strip membership."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_set_vca_group_members"):
            raise _not_supported("libsonare was built without mutable mixer VCA support")
        member_list = list(members)
        if member_list:
            member_array = (ctypes.c_char_p * len(member_list))(
                *[_utf8_arg(member, "member") for member in member_list]
            )
            member_ptr = ctypes.cast(member_array, ctypes.POINTER(ctypes.c_char_p))
        else:
            member_ptr = None
        _check(
            lib.sonare_mixer_set_vca_group_members(
                self._handle,
                _utf8_arg(id, "id"),
                member_ptr,
                ctypes.c_size_t(len(member_list)),
            )
        )

    @property
    def vca_group_count(self) -> int:
        """Return the number of VCA groups in the mixer topology."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_vca_group_count"):
            raise _not_supported("libsonare was built without mixer VCA support")
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
            raise _not_supported("libsonare was built without strip-handle support")
        if isinstance(strip, str):
            handle = lib.sonare_mixer_strip_by_id(self._handle, _utf8_arg(strip, "strip"))
            if not handle:
                raise SonareValueError(f"mixer strip id not found: {strip!r}")
        else:
            handle = lib.sonare_mixer_strip_at(self._handle, _to_c_size_t(strip, "strip"))
            if not handle:
                raise SonareValueError(f"mixer strip index out of range: {strip}")
        return ctypes.c_void_p(int(handle))

    def strip_by_id(self, id: str) -> int:
        """Return the index of the strip with ``id``.

        Raises :class:`SonareValueError` if no strip with that id exists. The returned index
        is stable for the lifetime of the current topology.
        """
        self._require()
        lib = _get_lib()
        target = lib.sonare_mixer_strip_by_id(self._handle, _utf8_arg(id, "id"))
        if not target:
            raise SonareValueError(f"mixer strip id not found: {id!r}")
        for index in range(self.strip_count):
            handle = lib.sonare_mixer_strip_at(self._handle, _to_c_size_t(index, "index"))
            if handle and int(handle) == int(target):
                return index
        raise SonareValueError(f"mixer strip id not found: {id!r}")

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

    def set_pan_law(self, strip: StripRef, pan_law: PanLaw) -> None:
        """Set a strip's pan law (a :data:`PanLaw` token such as ``"const3Db"``).

        Balance mode keeps centre unity and uses the law only for the far-channel
        taper; stereo-pan and dual-pan modes take the law's centre attenuation.
        A mono source is unity at centre under every law (see :data:`PanLaw`).
        """
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_pan_law(handle, ctypes.c_int(_pan_law_value(pan_law))))

    def set_channel_delay_samples(self, strip: StripRef, delay_samples: int) -> None:
        """Set a per-strip channel delay in samples, moving it later than the other strips."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_set_channel_delay_samples(
                handle, _to_c_int(delay_samples, "delay_samples")
            )
        )

    def set_vca_offset_db(self, strip: StripRef, offset_db: float) -> None:
        """Set a strip's direct VCA trim in dB (persisted as the scene's ``vcaOffsetDb``)."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_set_vca_offset_db(handle, _to_c_float(offset_db, "offset_db"))
        )

    def set_dual_pan(self, strip: StripRef, left: float, right: float) -> None:
        """Set independent left/right pan positions in [-1, 1] for a strip (dual-pan mode).

        Raises:
            SonareValueError: If either position is outside [-1, 1] or non-finite.
        """
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_set_dual_pan(
                handle, _to_c_float(left, "left"), _to_c_float(right, "right")
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
        reserved. Applied when the engine's track mixer renders this strip's
        lane into a destination with more than two channels; the stereo-only
        :class:`Mixer` block entry points ignore it.

        Raises:
            SonareValueError: If ``azimuth`` is outside [-180, 180], ``divergence``
                or ``lfe`` is outside [0, 1], or any field is non-finite; nothing
                is clamped.
        """
        handle = self._strip_handle(strip)
        pan = SonareSurroundPan(
            azimuth=_to_c_float(azimuth, "azimuth"),
            elevation=_to_c_float(elevation, "elevation"),
            divergence=_to_c_float(divergence, "divergence"),
            lfe=_to_c_float(lfe, "lfe"),
            distance=_to_c_float(distance, "distance"),
        )
        _check(_get_lib().sonare_strip_set_surround_pan(handle, ctypes.byref(pan)))

    def set_fader_db(self, strip: StripRef, db: float) -> None:
        """Set a strip's fader gain in dB (takes effect without a recompile)."""
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_fader_db(handle, _to_c_float(db, "db")))

    def set_input_trim_db(self, strip: StripRef, db: float) -> None:
        """Set a strip's input trim in dB."""
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_input_trim_db(handle, _to_c_float(db, "db")))

    def set_pan(self, strip: StripRef, pan: float, pan_mode: PanMode | None = None) -> None:
        """Set a strip's pan position.

        Pan modes: ``balance`` (default) keeps the stereo image, attenuates only
        the channel away from the pan direction and is unity at centre under
        every law; ``stereoPan`` folds the pair to mono and applies the law's
        centre attenuation (-3.01, -4.52, -6.02 and 0 dB for the four laws);
        ``dualPan`` pans the left and right inputs independently (default -1
        and +1, a pass-through) with the same centre attenuation.

        ``pan_mode`` is a :data:`PanMode` token, or ``None`` to keep the
        strip's current pan mode.

        Raises:
            SonareValueError: If ``pan`` is outside [-1, 1] or non-finite.
        """
        handle = self._strip_handle(strip)
        mode = -1 if pan_mode is None else _pan_mode_value(pan_mode)
        _check(
            _get_lib().sonare_strip_set_pan(
                handle, _to_c_float(pan, "pan"), _to_c_int(mode, "mode")
            )
        )

    def set_width(self, strip: StripRef, width: float) -> None:
        """Set a strip's stereo width in [0, 2] (0 mono, 1 unchanged, 2 widest).

        Raises:
            SonareValueError: If ``width`` is outside [0, 2] or non-finite.
        """
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_width(handle, _to_c_float(width, "width")))

    def set_muted(self, strip: StripRef, muted: bool) -> None:
        """Set a strip's mute state (takes effect without a recompile)."""
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_set_muted(handle, ctypes.c_int(1 if muted else 0)))

    def settle(self, strip: StripRef) -> None:
        """Snap a strip's gain smoothers to the values already set on it.

        Input trim, fader, pan and width are smoothed over roughly 5 ms so a
        live fader move does not click. That smoothing is wrong for an offline
        render of a finite buffer: the strip opens at its previous values and
        glides to the configured ones, so the head of the output carries a
        level and image sweep that a caller who only set a fader and a pan
        never asked for. Calling ``settle`` after configuring a strip and
        before the first :meth:`process_stereo` makes the very first output
        sample already sit at the converged value.

        Unlike a reset it clears nothing -- scheduled automation, meters and
        insert state are untouched, and the serialized scene is unchanged,
        because settling only moves the smoothers to values the scene already
        records.

        Args:
            strip: Strip index or id.

        Example:
            >>> mixer.set_fader_db("vocal", -3.0)  # doctest: +SKIP
            >>> mixer.set_width("vocal", 1.2)  # doctest: +SKIP
            >>> mixer.settle("vocal")  # doctest: +SKIP
            >>> mix = mixer.process_stereo(left, right)  # doctest: +SKIP
        """
        handle = self._strip_handle(strip)
        _check(_get_lib().sonare_strip_settle(handle))

    def add_send(
        self,
        strip: StripRef,
        send_id: str,
        destination_bus_id: str,
        send_db: float = 0.0,
        timing: SendTiming = "postFader",
    ) -> int:
        """Add a send from a strip to a bus and return its send index.

        ``timing`` is ``"postFader"`` (default) or ``"preFader"``. Call
        :meth:`compile` after adding sends before processing.
        """
        handle = self._strip_handle(strip)
        index_out = ctypes.c_size_t()
        _check(
            _get_lib().sonare_strip_add_send(
                handle,
                _utf8_arg(send_id, "send_id"),
                _utf8_arg(destination_bus_id, "destination_bus_id"),
                _to_c_float(send_db, "send_db"),
                ctypes.c_int(_send_timing_value(timing)),
                ctypes.byref(index_out),
            )
        )
        return int(index_out.value)

    def set_send_db(self, strip: StripRef, index: int, db: float) -> None:
        """Set the level in dB of an existing send on a strip (by send index)."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_set_send_db(
                handle, _to_c_size_t(index, "index"), _to_c_float(db, "db")
            )
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
            raise _not_supported("libsonare was built without strip remove_send support")
        _check(lib.sonare_strip_remove_send(handle, _to_c_uint32(index, "index")))

    def strip_meter(self, strip: StripRef, tap: MeterTap = "postFader") -> MixMeterSnapshot:
        """Read a per-strip meter snapshot at the given tap point.

        Args:
            strip: Strip index or id.
            tap: ``"preFader"`` or ``"postFader"`` (default).

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

    def bus_meter(self, bus_id: str) -> MixMeterSnapshot:
        """Read the post-insert meter for a compiled bus, including master."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_bus_meter"):
            raise _not_supported("libsonare was built without bus meter support")
        snapshot = SonareMixMeterSnapshot()
        _check(
            lib.sonare_mixer_bus_meter(
                self._handle, _utf8_arg(bus_id, "bus_id"), ctypes.byref(snapshot)
            )
        )
        return _mix_meter_from_c(snapshot)

    def configure_meter(
        self,
        enabled: bool = True,
        *,
        true_peak_oversample: int = 4,
        measure_lufs: bool = True,
        measure_true_peak: bool = True,
    ) -> None:
        """Turn the master-output meter on or off and choose what it measures.

        While enabled, every process and drain call meters the master output,
        and :meth:`flush_meters` folds its pending true-peak stretch too.

        Args:
            enabled: Whether the master output is metered.
            true_peak_oversample: True-peak oversampling factor, 4 or 8.
            measure_lufs: Whether loudness is measured.
            measure_true_peak: Whether true peak is measured.

        Raises:
            TypeError: If a flag is not a bool.
            SonareValueError: If ``true_peak_oversample`` is not 4 or 8 while enabled.
        """
        self._require()
        flags = {
            "enabled": enabled,
            "measure_lufs": measure_lufs,
            "measure_true_peak": measure_true_peak,
        }
        for name, value in flags.items():
            if not isinstance(value, bool):
                raise TypeError(f"configure_meter: {name} must be a bool")
        lib = _get_lib()
        config = SonareMixerMasterMeterConfig()
        lib.sonare_mixer_master_meter_config_init(ctypes.byref(config))
        config.enabled = int(enabled)
        config.measure_lufs = int(measure_lufs)
        config.measure_true_peak = int(measure_true_peak)
        config.true_peak_oversample = _narrow_int(
            true_peak_oversample, "true_peak_oversample", _C_INT_MIN, _C_INT_MAX
        )
        _check(lib.sonare_mixer_configure_master_meter(self._handle, ctypes.byref(config)))

    def meter_snapshot(self) -> MixMeterSnapshot:
        """Read the master-output meter.

        Raises:
            SonareError: ``InvalidState`` before :meth:`configure_meter` first enables it.
        """
        self._require()
        snapshot = SonareMixMeterSnapshot()
        _check_realtime(_get_lib().sonare_mixer_master_meter(self._handle, ctypes.byref(snapshot)))
        return _mix_meter_from_c(snapshot)

    def strip_non_finite_discard_count(self, strip: StripRef) -> int:
        """Return the blocks in which the strip discarded recursive state.

        Advisory telemetry, and the only thing that separates a degraded
        strip from a clean one. A discard returns the affected state to its
        post-reset value, so the strip recovers in silence and the output
        stays finite and in range while carrying samples unrelated to the
        input; nothing else reports that this happened.

        Covers the strip's own state, its EQ, every insert it owns and both
        of its meters; none of those is separately addressable here, so a
        discard inside one is observable only through this number -- and a
        meter that loses its loudness window then reports the floor, which
        is what a genuinely silent strip reports, so nothing else
        distinguishes the two. The unit is one processed block, never a
        channel and never an insert, so a stereo block that discards on both
        channels adds one. Cumulative since the strip was created and never
        cleared, so two readings bracket a span of audio.
        """
        handle = self._strip_handle(strip)
        lib = _get_lib()
        if not hasattr(lib, "sonare_strip_non_finite_discard_count"):
            raise _not_supported("libsonare was built without strip discard-count support")
        out = ctypes.c_uint32()
        _check_realtime(lib.sonare_strip_non_finite_discard_count(handle, ctypes.byref(out)))
        return int(out.value)

    def bus_non_finite_discard_count(self, bus_id: str) -> int:
        """Return the blocks in which the bus discarded recursive state.

        Same contract as :meth:`strip_non_finite_discard_count`, for a bus:
        covers every insert the bus owns and its meter, neither of which is
        separately addressable here. The count lives with the bus rather
        than with the compiled node, so it is cumulative across graph
        recompiles and an unrelated edit elsewhere in the mixer does not
        reset it.

        A bus's DSP record is created by the first compile, so a bus that
        has been declared with :meth:`add_bus` but never compiled raises
        the same way an unknown bus id does, rather than reading as zero.
        """
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_bus_non_finite_discard_count"):
            raise _not_supported("libsonare was built without bus discard-count support")
        out = ctypes.c_uint32()
        _check_realtime(
            lib.sonare_mixer_bus_non_finite_discard_count(
                self._handle, _utf8_arg(bus_id, "bus_id"), ctypes.byref(out)
            )
        )
        return int(out.value)

    def read_goniometer_latest(self, strip: StripRef, max_points: int) -> list[GoniometerPoint]:
        """Read up to ``max_points`` of the latest goniometer (vectorscope) data.

        ``max_points`` is a request rather than an allocation size: a value
        beyond the strip's goniometer ring simply returns every point the ring
        holds. A non-integer or negative value raises
        :class:`SonareValueError`.
        """
        if isinstance(max_points, bool) or not isinstance(max_points, int):
            raise SonareValueError("max_points must be a non-negative integer")
        if max_points < 0:
            raise SonareValueError("max_points must be a non-negative integer")
        # Bound the working buffer by the ring, not by the caller's number: a
        # ctypes array sized straight from an arbitrary count is how a request
        # for 10**12 points becomes a multi-gigabyte allocation. Same shape as
        # the Node and WASM facades.
        capped = min(max_points, _GONIOMETER_READ_CAP)
        if capped == 0:
            return []
        handle = self._strip_handle(strip)
        buffer = (SonareMixGoniometerPoint * capped)()
        count = _get_lib().sonare_strip_read_goniometer_latest(
            handle,
            ctypes.cast(buffer, ctypes.POINTER(SonareMixGoniometerPoint)),
            _to_c_size_t(capped, "capped"),
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
        curve: AutomationCurve = "linear",
    ) -> None:
        """Schedule a sample-accurate fader (dB) automation event on a strip."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_schedule_fader_automation(
                handle,
                _to_c_int64(sample_pos, "sample_pos"),
                _to_c_float(fader_db, "fader_db"),
                ctypes.c_int(_curve_value(curve)),
            )
        )

    def schedule_pan_automation(
        self,
        strip: StripRef,
        sample_pos: int,
        pan: float,
        curve: AutomationCurve = "linear",
    ) -> None:
        """Schedule a sample-accurate pan automation event on a strip."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_schedule_pan_automation(
                handle,
                _to_c_int64(sample_pos, "sample_pos"),
                _to_c_float(pan, "pan"),
                ctypes.c_int(_curve_value(curve)),
            )
        )

    def schedule_width_automation(
        self,
        strip: StripRef,
        sample_pos: int,
        width: float,
        curve: AutomationCurve = "linear",
    ) -> None:
        """Schedule a sample-accurate stereo-width automation event on a strip."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_schedule_width_automation(
                handle,
                _to_c_int64(sample_pos, "sample_pos"),
                _to_c_float(width, "width"),
                ctypes.c_int(_curve_value(curve)),
            )
        )

    def schedule_send_automation(
        self,
        strip: StripRef,
        send_index: int,
        sample_pos: int,
        db: float,
        curve: AutomationCurve = "linear",
    ) -> None:
        """Schedule a sample-accurate send-level (dB) automation event on a strip."""
        handle = self._strip_handle(strip)
        _check(
            _get_lib().sonare_strip_schedule_send_automation(
                handle,
                _to_c_size_t(send_index, "send_index"),
                _to_c_int64(sample_pos, "sample_pos"),
                _to_c_float(db, "db"),
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
        curve: AutomationCurve = "linear",
    ) -> None:
        """Schedule a sample-accurate insert-parameter automation event.

        Args:
            strip_index: Strip index in ``[0, strip_count())`` or strip id.
            insert_index: Index into the strip's combined pre/post insert chain.
            param_id: Processor-specific parameter id.
            sample_pos: Absolute sample position from the start of processing.
            value: Target parameter value.
            curve: :data:`AutomationCurve` token (``"linear"``,
                ``"exponential"``, ``"hold"`` or ``"sCurve"``).
        """
        handle = self._strip_handle(strip_index)
        lib = _get_lib()
        if not hasattr(lib, "sonare_strip_schedule_insert_automation"):
            raise _not_supported("libsonare was built without insert-automation support")
        _check(
            lib.sonare_strip_schedule_insert_automation(
                handle,
                _to_c_uint(insert_index, "insert_index"),
                _to_c_uint(param_id, "param_id"),
                _to_c_int64(sample_pos, "sample_pos"),
                _to_c_float(value, "value"),
                ctypes.c_int(_curve_value(curve)),
            )
        )

    def process_stereo(
        self,
        left_inputs: Sequence[Sequence[float]],
        right_inputs: Sequence[Sequence[float]],
    ) -> MixerStereoResult:
        """Mix one block of per-strip stereo audio into the stereo master.

        ``left_inputs[i]`` / ``right_inputs[i]`` are strip ``i``'s left and right planes.
        Returns a :class:`MixerStereoResult` (``left``, ``right``,
        ``sample_rate``) matching the Node/WASM ``{left, right, sampleRate}``
        shape. With no input strips the master is silent (an empty block), to
        match the C ABI which accepts ``input_count=0``. Fewer inputs than strips
        leave the remaining strips silent; more raise ``SonareValueError``.
        """
        self._require()
        if len(left_inputs) != len(right_inputs):
            raise SonareValueError("left_inputs and right_inputs must have the same length")
        strip_count = self.strip_count
        if len(left_inputs) > strip_count:
            raise SonareValueError(f"got {len(left_inputs)} inputs for a {strip_count}-strip mixer")

        left_arrays: list[ctypes.Array[ctypes.c_float]] = []
        right_arrays: list[ctypes.Array[ctypes.c_float]] = []
        length: int | None = None
        for index, (left, right) in enumerate(zip(left_inputs, right_inputs, strict=True)):
            # Name the strip as well as the side: what the caller passed is a
            # sequence of strips, so the position is what locates the bad one.
            left_array, left_length = _to_c_float_array(left, arg_name=f"left_inputs[{index}]")
            right_array, right_length = _to_c_float_array(right, arg_name=f"right_inputs[{index}]")
            if left_length != right_length:
                raise SonareValueError("left and right channel lengths must match")
            if length is None:
                length = left_length
            elif left_length != length:
                raise SonareValueError("all strips must have the same length")
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
                _to_c_size_t(length, "length"),
            )
        )
        return MixerStereoResult(
            left=_from_c_float_array(out_left, length),
            right=_from_c_float_array(out_right, length),
            sample_rate=int(self._sample_rate),
        )

    @property
    def tail_samples(self) -> int:
        """Return the longest audible reverb/delay path to the master, in samples.

        This is how many additional samples should be drained with
        :meth:`drain_tail_stereo` after the last input block to capture the
        decaying effect tails (reverb, delay) left on the master routing path.
        """
        self._require()
        out = ctypes.c_int()
        _check(_get_lib().sonare_mixer_tail_samples(self._handle, ctypes.byref(out)))
        return int(out.value)

    @property
    def latency_samples(self) -> int:
        """Return the compiled mixer graph's latency in samples.

        Use this to align dry/wet material against the mixer output, mirroring
        the C ABI ``sonare_mixer_latency_samples`` accessor.
        """
        self._require()
        out = ctypes.c_int()
        _check(_get_lib().sonare_mixer_latency_samples(self._handle, ctypes.byref(out)))
        return int(out.value)

    def drain_tail_stereo(self, num_samples: int | float) -> MixerStereoResult:
        """Drain ``num_samples`` of trailing effect-tail audio with no new input.

        Pushes silence through the compiled graph so the stereo master keeps
        emitting the decaying reverb / delay tails. ``num_samples`` must be a
        positive integer no larger than the mixer's maximum block size; the
        library refuses a longer drain. Returns a :class:`MixerStereoResult`
        matching :meth:`process_stereo`.
        """
        self._require()
        # The integrality half is the shared narrowing rather than a `float`
        # test, which a numpy scalar walks straight past.
        count = _narrow_int(num_samples, "num_samples", 1, _C_INT_MAX)
        out_left = (ctypes.c_float * count)()
        out_right = (ctypes.c_float * count)()
        _check(
            _get_lib().sonare_mixer_drain_tail_stereo(
                self._handle,
                out_left,
                out_right,
                _to_c_size_t(count, "count"),
            )
        )
        return MixerStereoResult(
            left=_from_c_float_array(out_left, count),
            right=_from_c_float_array(out_right, count),
            sample_rate=int(self._sample_rate),
        )

    def flush_meters(self) -> None:
        """Mark the end of the signal for every strip, bus and master meter.

        The true-peak reading runs one reconstruction-filter group delay behind
        the block; this reads that pending stretch against silence and folds it
        into the snapshots, so ``true_peak_dbtp`` covers the whole signal. Call it
        after the last block of an offline render. A later process call starts
        a new signal.
        """
        self._require()
        _check(_get_lib().sonare_mixer_flush_meters(self._handle))

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

    def __enter__(self) -> Mixer:
        """Return the object itself for ``with`` use."""
        return self

    def __exit__(self, *_exc: object) -> None:
        """Close the object on leaving the ``with`` block; never suppresses exceptions."""
        self.close()

    def __del__(self) -> None:
        self.close()

    def _require(self) -> None:
        _require_open(self._handle, "Mixer")


def mix_stereo(
    strips: Sequence[tuple[Sequence[float], Sequence[float]]],
    sample_rate: int,
    fader_db: Sequence[float] | None = None,
    pan: Sequence[float] | None = None,
    pan_mode: Sequence[PanMode] | PanMode = "balance",
    width: Sequence[float] | None = None,
    muted: Sequence[bool] | None = None,
    input_trim_db: Sequence[float] | None = None,
) -> MixResult:
    """Render a small stereo mixer scene from per-strip left/right buffers.

    Args:
        strips: Sequence of ``(left, right)`` sample buffers. All buffers must
            have the same length, that length must be non-zero, and every
            sample must be finite.
        sample_rate: Sample rate in Hz, an integer in ``[8000, 384000]``.
        fader_db: Optional per-strip fader values in dB.
        pan: Optional per-strip pan values in ``[-1, 1]``.
        pan_mode: Either one mode for all strips or per-strip modes:
            ``"balance"``, ``"stereoPan"``, or ``"dualPan"``. Applied whether or
            not ``pan`` is given; a mode-only call pans from centre.
        width: Optional per-strip stereo width values.
        muted: Optional per-strip mute flags.
        input_trim_db: Optional per-strip input trim values in dB.

    Each per-strip option may be shorter than ``strips``; the remaining strips
    keep their defaults, as on Node and WASM. An option longer than ``strips``
    raises :class:`SonareValueError` naming it.

    Note:
        The per-strip meters in the result expose integrating loudness fields
        (``momentary_lufs``, ``short_term_lufs``, ``integrated_lufs``) whose
        windows a short one-shot mix does not fill, so they read the ``-120`` dB
        floor sentinel; drive a strip over a streaming session when a meaningful
        loudness number is needed. ``true_peak_dbtp`` is not an integrator — it is
        a max-hold over the block just processed, so it is valid immediately and
        floors only on digital silence. This call mixes the whole signal in one
        block and flushes the meters after it, so the reported true peak
        matches :func:`metering_true_peak_dbtp` over the same signal exactly.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mixer_create"):
        raise _not_supported("libsonare was built without mixing support")
    if not strips:
        raise SonareValueError("mix_stereo: at least one strip is required")
    c_sample_rate = _narrow_int(sample_rate, "sample_rate", _MIN_SAMPLE_RATE, _MAX_SAMPLE_RATE)

    # A per-strip option may be shorter than the strip list (the remaining
    # strips keep their defaults, as on Node and WASM) but never longer: the
    # surplus entries would belong to no strip.
    n_strips = len(strips)
    mode_list = (
        pan_mode if isinstance(pan_mode, Sequence) and not isinstance(pan_mode, str) else None
    )
    for name, opt in (
        ("input_trim_db", input_trim_db),
        ("fader_db", fader_db),
        ("pan", pan),
        ("pan_mode", mode_list),
        ("width", width),
        ("muted", muted),
    ):
        if opt is not None and len(opt) > n_strips:
            raise SonareValueError(
                f"mix_stereo: '{name}' has more entries than strips ({n_strips})"
            )

    left_arrays: list[ctypes.Array[ctypes.c_float]] = []
    right_arrays: list[ctypes.Array[ctypes.c_float]] = []
    length: int | None = None
    for index, (left, right) in enumerate(strips):
        # A NaN loses every comparison the peak meter makes, so an unguarded
        # non-finite strip reads as the -120 dB silence floor rather than as a
        # fault; an infinity propagates into the master sum and the meter.
        left_buf = _validate_samples(
            "mix_stereo", left, arg_name=f"strips[{index}] left", allow_empty=True
        )
        right_buf = _validate_samples(
            "mix_stereo", right, arg_name=f"strips[{index}] right", allow_empty=True
        )
        left_array, left_length = _to_c_float_array(left_buf)
        right_array, right_length = _to_c_float_array(right_buf)
        if left_length != right_length:
            raise SonareValueError("left and right channel lengths must match")
        if length is None:
            length = left_length
        elif left_length != length:
            raise SonareValueError("all strips must have the same length")
        left_arrays.append(left_array)
        right_arrays.append(right_array)

    assert length is not None
    if length == 0:
        # Every strip shares one length, so a zero here means the whole scene is
        # empty. The C ABI treats a zero-frame block as a valid no-op, which
        # would answer with an empty mix and meters reading the silence floor —
        # indistinguishable from a real mix of silence.
        raise SonareValueError("mix_stereo: every strip is empty; there is no audio to mix")

    mixer_handle = ctypes.c_void_p()
    _check(
        lib.sonare_mixer_create(
            c_sample_rate, _to_c_int(length, "length"), ctypes.byref(mixer_handle)
        )
    )
    mixer = mixer_handle.value

    try:
        strip_handles: list[ctypes.c_void_p] = []
        for index in range(len(strips)):
            handle = lib.sonare_mixer_add_strip(mixer, _utf8_arg(f"strip{index}", "strip_id"))
            if not handle:
                raise _last_error(int(ErrorCode.UNKNOWN), "failed to add mixer strip")
            strip_handles.append(ctypes.c_void_p(handle))

            if input_trim_db is not None and index < len(input_trim_db):
                _check(
                    lib.sonare_strip_set_input_trim_db(
                        strip_handles[-1],
                        _to_c_float(input_trim_db[index], f"input_trim_db[{index}]"),
                    )
                )
            if fader_db is not None and index < len(fader_db):
                _check(
                    lib.sonare_strip_set_fader_db(
                        strip_handles[-1], _to_c_float(fader_db[index], f"fader_db[{index}]")
                    )
                )
            # pan and pan_mode are independent options: either one alone has to
            # reach the strip. Reading pan_mode only inside `if pan is not None`
            # dropped a mode-only request silently. The C ABI takes the pair
            # together, so the missing half is the value the strip this loop
            # just created already carries -- centre (0.0) for an absent
            # position, and Balance for the default mode.
            if mode_list is None:
                mode = typing.cast("PanMode", pan_mode)
            else:
                mode = mode_list[index] if index < len(mode_list) else "balance"
            _check(
                lib.sonare_strip_set_pan(
                    strip_handles[-1],
                    _to_c_float(
                        pan[index] if pan is not None and index < len(pan) else 0.0, f"pan[{index}]"
                    ),
                    ctypes.c_int(_pan_mode_value(mode)),
                )
            )
            if width is not None and index < len(width):
                _check(
                    lib.sonare_strip_set_width(
                        strip_handles[-1], _to_c_float(width[index], f"width[{index}]")
                    )
                )
            if muted is not None and index < len(muted):
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
            _to_c_size_t(length, "length"),
        )
        _check(rc)
        # The whole signal was one block: fold the meters' pending group delay in.
        _check(lib.sonare_mixer_flush_meters(mixer))

        meters: list[MixMeterSnapshot] = []
        for handle in strip_handles:
            snapshot = SonareMixMeterSnapshot()
            _check(lib.sonare_strip_meter(handle, ctypes.byref(snapshot)))
            meters.append(_mix_meter_from_c(snapshot))

        return MixResult(
            left=_from_c_float_array(out_left, length),
            right=_from_c_float_array(out_right, length),
            sample_rate=int(sample_rate),
            meters=meters,
        )
    finally:
        lib.sonare_mixer_destroy(mixer)
