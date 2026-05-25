"""Mixing wrappers for libsonare."""

from __future__ import annotations

from ._runtime import *  # noqa: F403


def mixing_scene_preset_names() -> list[str]:
    """Return built-in mixer scene preset identifiers."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mixing_scene_preset_names"):
        raise RuntimeError("libsonare was built without mixing support")
    raw = lib.sonare_mixing_scene_preset_names()
    return raw.decode("utf-8").splitlines() if raw else []


def mixing_scene_preset_json(preset: str) -> str:
    """Return the JSON scene template for a built-in mixer preset."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mixing_scene_preset_json"):
        raise RuntimeError("libsonare was built without mixing support")
    json_ptr = ctypes.c_void_p()
    rc = lib.sonare_mixing_scene_preset_json(preset.encode("utf-8"), ctypes.byref(json_ptr))
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
    buses, inserts) into a stereo master. Insert-parameter automation is
    scheduled by strip index; the underlying strip handles are never exposed.
    """

    def __init__(self, handle: int, sample_rate: int, block_size: int) -> None:
        self._handle: int | None = handle
        self._sample_rate = sample_rate
        self._block_size = block_size

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
        return cls(int(handle), sample_rate, block_size)

    def compile(self) -> None:
        """Rebuild and compile the routing graph from the current scene."""
        self._require()
        _check(_get_lib().sonare_mixer_compile(self._handle))

    def strip_count(self) -> int:
        """Return the number of strips in the mixer."""
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_strip_count"):
            raise RuntimeError("libsonare was built without insert-automation support")
        return int(lib.sonare_mixer_strip_count(self._handle))

    def schedule_insert_automation(
        self,
        strip_index: int,
        insert_index: int,
        param_id: int,
        sample_pos: int,
        value: float,
        curve: int = 0,
    ) -> None:
        """Schedule a sample-accurate insert-parameter automation event.

        Args:
            strip_index: Strip index in ``[0, strip_count())``.
            insert_index: Index into the strip's combined pre/post insert chain.
            param_id: Processor-specific parameter id.
            sample_pos: Absolute sample position from the start of processing.
            value: Target parameter value.
            curve: ``0`` for linear, ``1`` for exponential interpolation.
        """
        self._require()
        lib = _get_lib()
        if not hasattr(lib, "sonare_mixer_strip_at"):
            raise RuntimeError("libsonare was built without insert-automation support")
        strip = lib.sonare_mixer_strip_at(self._handle, ctypes.c_size_t(strip_index))
        if not strip:
            raise IndexError("mixer strip index out of range")
        _check(
            lib.sonare_strip_schedule_insert_automation(
                ctypes.c_void_p(int(strip)),
                ctypes.c_uint(insert_index),
                ctypes.c_uint(param_id),
                ctypes.c_int64(sample_pos),
                ctypes.c_float(value),
                ctypes.c_int(curve),
            )
        )

    def process_stereo(
        self,
        left_channels: Sequence[Sequence[float]],
        right_channels: Sequence[Sequence[float]],
    ) -> tuple[list[float], list[float]]:
        """Mix one block of per-strip stereo audio into the stereo master.

        ``left_channels[i]`` / ``right_channels[i]`` are strip ``i``'s channels.
        Returns the ``(left, right)`` master output.
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

        if length is None:
            raise ValueError("at least one strip is required")

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
        return (
            [float(out_left[i]) for i in range(length)],
            [float(out_right[i]) for i in range(length)],
        )

    def to_scene_json(self) -> str:
        """Serialize the current scene (strips, buses, sends, connections)."""
        self._require()
        lib = _get_lib()
        json_ptr = ctypes.c_void_p()
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
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mixer_create"):
        raise RuntimeError("libsonare was built without mixing support")
    if not strips:
        raise ValueError("at least one strip is required")

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
