"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
import os
from collections.abc import Sequence
from pathlib import Path
from typing import BinaryIO

import numpy as np

from ._runtime import (
    ClipPageRequest,
    _check,
    _get_lib,
)

# Must match sonare::rt::kEngineAbiVersion (src/rt/command.h) and the WASM
# binding's EXPECTED_ENGINE_ABI_VERSION. A mismatch means the loaded native
# binary lays out engine structs differently than this wrapper expects.
EXPECTED_ENGINE_ABI_VERSION = 3
_CAPTURE_SOURCE_VALUES = {"output": 0, "input": 1}


class ClipPageProvider:
    """Host-supplied paged audio source for realtime clip streaming."""

    def __init__(self, num_channels: int, num_samples: int, page_frames: int) -> None:
        self._handle: ctypes.c_void_p | None = None
        handle = ctypes.c_void_p()
        _check(
            _get_lib().sonare_clip_page_provider_create(
                int(num_channels), int(num_samples), int(page_frames), ctypes.byref(handle)
            )
        )
        self._handle = handle

    def close(self) -> None:
        handle = getattr(self, "_handle", None)
        if handle is not None:
            _get_lib().sonare_clip_page_provider_destroy(handle)
            self._handle = None

    def destroy(self) -> None:
        self.close()

    def __enter__(self) -> ClipPageProvider:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise RuntimeError("ClipPageProvider is closed")
        return self._handle

    def supply(self, page_index: int, channels: Sequence[Sequence[float]]) -> None:
        if not channels:
            raise ValueError("channels must not be empty")
        frames = len(channels[0])
        if frames <= 0:
            raise ValueError("channels must not be empty")
        arrays: list[ctypes.Array[ctypes.c_float]] = []
        ptr_values: list[ctypes.POINTER(ctypes.c_float)] = []
        for channel in channels:
            if len(channel) != frames:
                raise ValueError("all channels must have the same length")
            array = (ctypes.c_float * frames)(*channel)
            arrays.append(array)
            ptr_values.append(ctypes.cast(array, ctypes.POINTER(ctypes.c_float)))
        ptr_type = ctypes.POINTER(ctypes.c_float) * len(ptr_values)
        ptrs = ptr_type(*ptr_values)
        _check(
            _get_lib().sonare_clip_page_provider_supply(
                self._require_handle(),
                int(page_index),
                ctypes.cast(ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
                len(ptr_values),
                frames,
            )
        )

    def clear(self, page_index: int) -> None:
        _check(_get_lib().sonare_clip_page_provider_clear(self._require_handle(), int(page_index)))


class FileClipPageProvider(ClipPageProvider):
    """File-backed float32 PCM page supplier for realtime clip streaming.

    The file format is raw little-endian interleaved float32 PCM.
    """

    def __init__(
        self,
        path: str | os.PathLike[str],
        *,
        num_channels: int,
        num_samples: int,
        page_frames: int,
        data_offset_bytes: int = 0,
    ) -> None:
        if num_channels <= 0 or num_samples <= 0 or page_frames <= 0:
            raise ValueError("num_channels, num_samples, and page_frames must be positive")
        super().__init__(num_channels, num_samples, page_frames)
        self._file: BinaryIO | None = None
        try:
            self._file = Path(path).open("rb")  # noqa: SIM115
        except BaseException:
            super().close()
            raise
        self.num_channels = int(num_channels)
        self.num_samples = int(num_samples)
        self.page_frames = int(page_frames)
        self.data_offset_bytes = int(data_offset_bytes)

    def close(self) -> None:
        file = getattr(self, "_file", None)
        if file is not None:
            file.close()
            self._file = None
        super().close()

    def supply_page(self, page_index: int) -> bool:
        if self._file is None:
            raise RuntimeError("FileClipPageProvider is closed")
        page = int(page_index)
        if page < 0:
            return False
        start_frame = page * self.page_frames
        if start_frame >= self.num_samples:
            return False
        frames = min(self.page_frames, self.num_samples - start_frame)
        frame_bytes = self.num_channels * np.dtype("<f4").itemsize
        self._file.seek(self.data_offset_bytes + start_frame * frame_bytes)
        raw = self._file.read(frames * frame_bytes)
        frames_read = len(raw) // frame_bytes
        if frames_read < frames:
            return False
        interleaved = np.frombuffer(raw[: frames_read * frame_bytes], dtype="<f4")
        channels = [interleaved[ch :: self.num_channels] for ch in range(self.num_channels)]
        self.supply(page, channels)
        return True

    def supply_request(self, request: ClipPageRequest) -> bool:
        return self.supply_page(int(request.sample) // self.page_frames)
