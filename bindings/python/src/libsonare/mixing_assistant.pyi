from collections.abc import Sequence
from typing import Any, NamedTuple, TypeAlias

import numpy as np

FloatSamples: TypeAlias = Sequence[float] | list[float] | np.ndarray[Any, Any]

class MixTrackInput(NamedTuple):
    track_id: str
    left: FloatSamples
    right: FloatSamples | None = None
    name: str | None = None

def suggest_mix_scene(
    tracks: Sequence[MixTrackInput],
    *,
    sample_rate: int,
    target_track_lufs: float | None = None,
    suggestion_strength: float | None = None,
    eq_max_cut_db: float | None = None,
    mix_bus_headroom_dbtp: float | None = None,
    enable_structure: bool | None = None,
    enable_gain: bool | None = None,
    enable_balance: bool | None = None,
    enable_eq: bool | None = None,
    enable_dynamics: bool | None = None,
    enable_image: bool | None = None,
    enable_high_pass: bool | None = None,
    n_fft: int | None = None,
    hop_length: int | None = None,
) -> dict[str, Any]: ...
def suggest_mix_scene_json(
    tracks: Sequence[MixTrackInput],
    *,
    sample_rate: int,
    target_track_lufs: float | None = None,
    suggestion_strength: float | None = None,
    eq_max_cut_db: float | None = None,
    mix_bus_headroom_dbtp: float | None = None,
    enable_structure: bool | None = None,
    enable_gain: bool | None = None,
    enable_balance: bool | None = None,
    enable_eq: bool | None = None,
    enable_dynamics: bool | None = None,
    enable_image: bool | None = None,
    enable_high_pass: bool | None = None,
    n_fft: int | None = None,
    hop_length: int | None = None,
) -> str: ...
def mix_source_class_names() -> list[str]: ...
def mix_source_class_from_name(name: str) -> int: ...
