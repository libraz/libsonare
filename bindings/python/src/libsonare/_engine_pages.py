"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
import os
from collections.abc import Sequence
from pathlib import Path
from typing import BinaryIO

import numpy as np

from ._runtime import (
    _INT64_MAX,
    _INT64_MIN,
    ClipPageRequest,
    SonareValueError,
    _check,
    _get_lib,
    _narrow_int,
    _planar_channel_arrays,
    _to_c_int,
    _to_c_int64,
)


class ClipPageProvider:
    """Host-supplied paged audio source for realtime clip streaming."""

    def __init__(self, num_channels: int, num_samples: int, page_frames: int) -> None:
        self._handle: ctypes.c_void_p | None = None
        handle = ctypes.c_void_p()
        _check(
            _get_lib().sonare_clip_page_provider_create(
                _to_c_int(num_channels, "num_channels"),
                _to_c_int64(num_samples, "num_samples"),
                _to_c_int64(page_frames, "page_frames"),
                ctypes.byref(handle),
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
        arrays, ptrs, frames = _planar_channel_arrays(channels)
        _check(
            _get_lib().sonare_clip_page_provider_supply(
                self._require_handle(),
                _to_c_int64(page_index, "page_index"),
                ctypes.cast(ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
                _to_c_int(len(arrays), "num_channels"),
                _to_c_int64(frames, "frames"),
            )
        )

    def clear(self, page_index: int) -> None:
        _check(
            _get_lib().sonare_clip_page_provider_clear(
                self._require_handle(), _to_c_int64(page_index, "page_index")
            )
        )


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
            raise SonareValueError("num_channels, num_samples, and page_frames must be positive")
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
        # Narrowed rather than coerced: this one never reaches the C provider, so
        # int(1024.5) would silently seek 1024 bytes in and read a page of
        # misaligned frames. Integrality only -- nothing here bounds the offset.
        self.data_offset_bytes = _narrow_int(
            data_offset_bytes, "data_offset_bytes", _INT64_MIN, _INT64_MAX
        )

    def close(self) -> None:
        file = getattr(self, "_file", None)
        if file is not None:
            file.close()
            self._file = None
        super().close()

    def supply_page(self, page_index: int) -> bool:
        if self._file is None:
            raise RuntimeError("FileClipPageProvider is closed")
        # Narrowed before the page arithmetic: int(2.5) is page 2, a whole page
        # of the wrong frames supplied under the caller's index.
        page = _narrow_int(page_index, "page_index", _INT64_MIN, _INT64_MAX)
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
        return self.supply_page(request.sample // self.page_frames)
