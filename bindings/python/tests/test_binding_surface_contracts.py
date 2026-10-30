"""Argument refusal, automation-id lookup and ndarray result contracts of the Python facade."""

from __future__ import annotations

import math
import re
from pathlib import Path

import numpy as np
import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

_SRC = Path(__file__).resolve().parents[1] / "src" / "libsonare"


@pytest.mark.parametrize("module", ["_project_edit.py", "_project_mir.py"])
def test_project_edit_has_no_bare_float_coercion(module: str) -> None:
    text = (_SRC / module).read_text(encoding="utf-8")
    assert re.findall(r"(?:^|[^_.a-zA-Z])float\(", text, flags=re.MULTILINE) == []


@pytest.mark.parametrize("bad", ["1.0", True, None], ids=["str", "bool", "none"])
def test_project_ppq_arguments_refuse_non_numbers(bad: object) -> None:
    from libsonare import Project

    with Project() as project:
        with pytest.raises(TypeError):
            project.add_midi_clip(bad, 4.0)  # type: ignore[arg-type]
        with pytest.raises(TypeError):
            project.add_midi_clip(0.0, bad)  # type: ignore[arg-type]
        _, clip = project.add_midi_clip(0.0, 4.0)
        with pytest.raises(TypeError):
            project.split_clip(clip, bad)  # type: ignore[arg-type]
        with pytest.raises(TypeError):
            project.trim_clip(clip, bad, 1.0)  # type: ignore[arg-type]
        with pytest.raises(TypeError):
            project.move_clip(clip, bad)  # type: ignore[arg-type]
        with pytest.raises(TypeError):
            project.duplicate_clip(clip, bad)  # type: ignore[arg-type]
        with pytest.raises(TypeError):
            project.set_sample_rate(bad)  # type: ignore[arg-type]


@pytest.mark.parametrize("bad", [math.nan, math.inf])
def test_project_ppq_arguments_refuse_non_finite(bad: float) -> None:
    from libsonare import Project, SonareValueError

    with Project() as project:
        with pytest.raises(SonareValueError):
            project.add_midi_clip(bad, 4.0)
        _, clip = project.add_midi_clip(0.0, 4.0)
        with pytest.raises(SonareValueError):
            project.split_clip(clip, bad)
        with pytest.raises(SonareValueError):
            project.set_clip_comp_segments(clip, [(0.0, bad, 0)])
        with pytest.raises(SonareValueError):
            project.set_warp_map(1, [(bad, 0.0)])


def test_resolve_instrument_automation_id_is_destination_first_and_refuses_unknown_name() -> None:
    from libsonare import RealtimeEngine, SonareValueError, SynthPatch

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_synth_instrument(SynthPatch(), destination_id=3)
        first = engine.resolve_instrument_automation_id(3, "gain")
        assert engine.resolve_instrument_automation_id(3, "gain") == first
        assert engine.resolve_instrument_automation_id(3, "cutoffHz") != first
        with pytest.raises(SonareValueError):
            engine.resolve_instrument_automation_id(3, "noSuchParameter")


def test_resolve_master_unknown_name_is_a_value_error() -> None:
    from libsonare import RealtimeEngine, SonareValueError

    with (
        RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine,
        pytest.raises(SonareValueError),
    ):
        engine.resolve_master_automation_id("noSuchParameter")


def test_bounce_and_render_results_are_float32_arrays() -> None:
    from libsonare import EngineBounceOptions, RealtimeEngine

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.play()
        bounced = engine.bounce_offline(
            EngineBounceOptions(total_frames=256, block_size=128, num_channels=2)
        )
        assert isinstance(bounced.interleaved, np.ndarray)
        assert bounced.interleaved.dtype == np.float32
        assert bounced.interleaved.size == 512
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.play()
        planes = engine.render_offline([[0.0] * 256, [0.0] * 256], block_size=128)
        assert all(isinstance(p, np.ndarray) and p.dtype == np.float32 for p in planes)
        assert all(p.size == 256 for p in planes)


def test_planar_channel_arrays_copies_list_input_once_and_never_aliases_arrays(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from libsonare import _runtime

    copies: list[int] = []
    real_array = np.array

    def counting_array(*args: object, **kwargs: object) -> np.ndarray:
        copies.append(1)
        return real_array(*args, **kwargs)  # type: ignore[arg-type]

    monkeypatch.setattr(_runtime.np, "array", counting_array)
    _runtime._planar_channel_arrays([[0.5] * 64, [0.25] * 64])
    assert copies == []

    caller = np.full((2, 64), 0.5, dtype=np.float32)
    arrays, _, frames = _runtime._planar_channel_arrays(caller)
    assert frames == 64
    assert len(copies) == 2
    arrays[0][0] = 9.0
    assert caller[0, 0] == pytest.approx(0.5)


def test_cli_resample_returns_float32_array() -> None:
    from libsonare._cli_common import _resample

    tone = np.sin(np.linspace(0.0, 40.0, 4410)).astype(np.float32)
    out = _resample(tone, 44100, 22050)
    assert isinstance(out, np.ndarray) and out.dtype == np.float32
    assert abs(out.size - 2205) <= 2
    same = _resample(tone, 44100, 44100)
    assert same is not tone and np.array_equal(same, tone)


def test_cli_mean_abs_matches_the_definition() -> None:
    from libsonare._cli_common import _mean_abs

    assert _mean_abs([]) == 0.0
    assert _mean_abs(np.array([-1.0, 3.0], dtype=np.float32)) == pytest.approx(2.0)


def test_repair_full_scale_fit_scales_by_one_gain() -> None:
    from libsonare._cli_repair import _fit_repair_output_to_full_scale

    quiet = [0.5, -0.25]
    same, gain_db = _fit_repair_output_to_full_scale(quiet)
    assert same is quiet and gain_db == 0.0
    loud = np.array([2.0, -1.0, math.nan, 0.5], dtype=np.float32)
    fitted, gain_db = _fit_repair_output_to_full_scale(loud)
    assert gain_db == pytest.approx(20.0 * math.log10(0.5))
    assert fitted[0] == pytest.approx(1.0) and fitted[1] == pytest.approx(-0.5)
    assert math.isnan(fitted[2])


def test_source_span_destination_positions_are_required_and_zero_is_a_position() -> None:
    from libsonare import SonareValueError, VocalSetNoteSourceSpan
    from libsonare._ffi import SonareVocalOperation
    from libsonare._runtime import _get_lib
    from libsonare.vocal_edit import _operation_to_c

    lib = _get_lib()
    complete = VocalSetNoteSourceSpan(
        1,
        source_start_sample=100,
        source_end_sample=900,
        destination_start_sample=0,
        destination_length_samples=800,
    )
    raw, _ = _operation_to_c(lib, complete, 48000)
    assert isinstance(raw, SonareVocalOperation)
    assert raw.destination_start_sample == 0
    assert raw.destination_length_samples == 800

    for missing in ("destination_start_sample", "destination_length_samples"):
        fields = {
            "source_start_sample": 100,
            "source_end_sample": 900,
            "destination_start_sample": 0,
            "destination_length_samples": 800,
        }
        del fields[missing]
        with pytest.raises(SonareValueError, match=missing):
            _operation_to_c(lib, VocalSetNoteSourceSpan(1, **fields), 48000)

    seconds = VocalSetNoteSourceSpan(
        1,
        source_start_sample=100,
        source_end_sample=900,
        destination_start_sec=0.5,
        destination_length_samples=800,
    )
    assert _operation_to_c(lib, seconds, 48000)[0].destination_start_sample == 24000
    both = VocalSetNoteSourceSpan(
        1,
        source_start_sample=100,
        source_end_sample=900,
        destination_start_sample=0,
        destination_start_sec=0.5,
        destination_length_samples=800,
    )
    with pytest.raises(SonareValueError):
        _operation_to_c(lib, both, 48000)
