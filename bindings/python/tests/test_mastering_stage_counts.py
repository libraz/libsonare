"""The optional stage counts a mastering result carries, present only when their stage ran."""

from __future__ import annotations

import json
import math
import re
from pathlib import Path

import pytest

from ._analyzer_helpers import _run_cli, _write_test_wav

_ROOT = Path(__file__).resolve().parents[3]
_SAMPLE_RATE = 48000
_CLIP_THRESHOLD_DB = 20.0 * math.log10(0.47)


def _clipped_tone_with_long_run() -> list[float]:
    """A tone clipped flat at 0.5, with one held run longer than declip reconstructs."""
    samples = [
        max(-0.5, min(0.5, math.sin(2.0 * math.pi * 220.0 * i / _SAMPLE_RATE)))
        for i in range(_SAMPLE_RATE // 4)
    ]
    samples[2000:2800] = [0.5] * 800
    return samples


def test_a_declip_processor_reports_its_unreconstructed_runs() -> None:
    from libsonare import mastering_apply_processor

    result = mastering_apply_processor(
        "repair.declip",
        _clipped_tone_with_long_run(),
        sample_rate=_SAMPLE_RATE,
        params={"clipThresholdDb": _CLIP_THRESHOLD_DB},
    )
    assert result.declip_unreconstructed_runs is not None
    assert result.declip_unreconstructed_runs >= 1
    assert result.quantize_clipped_samples is None


def test_a_quantizing_processor_counts_samples_past_full_scale() -> None:
    from libsonare import mastering_apply_processor, mastering_apply_processor_stereo

    hot = [0.25, 1.5, -2.0, 1.0]
    mono = mastering_apply_processor(
        "final.bitDepth", hot, sample_rate=_SAMPLE_RATE, params={"targetBits": 16}
    )
    # Exactly 1.0 is not counted.
    assert mono.quantize_clipped_samples == 2
    assert mono.declip_unreconstructed_runs is None

    stereo = mastering_apply_processor_stereo(
        "final.bitDepth", hot, hot, sample_rate=_SAMPLE_RATE, params={"targetBits": 16}
    )
    assert stereo.quantize_clipped_samples == 4

    undithered = mastering_apply_processor(
        "final.dither", hot, sample_rate=_SAMPLE_RATE, params={"type": 0}
    )
    assert undithered.quantize_clipped_samples is None


def test_a_chain_reports_the_declip_count_only_when_declip_ran() -> None:
    from libsonare import master_audio

    samples = _clipped_tone_with_long_run()
    with_declip = master_audio(
        samples, sample_rate=_SAMPLE_RATE, overrides={"repair": {"declip": {"enabled": True}}}
    )
    assert with_declip.declip_unreconstructed_runs is not None
    assert not hasattr(with_declip, "quantize_clipped_samples")

    without = master_audio(
        samples, sample_rate=_SAMPLE_RATE, overrides={"repair": {"declip": {"enabled": False}}}
    )
    assert without.declip_unreconstructed_runs is None


def test_the_cli_gap_limit_is_the_core_declip_limit() -> None:
    from libsonare._cli_common import _DECLIP_MAX_GAP_SAMPLES

    header = (_ROOT / "src/mastering/repair/declip.h").read_text(encoding="utf-8")
    match = re.search(r"kDeclipMaxLpcGapSamples = (\d+);", header)
    assert match is not None
    assert int(match.group(1)) == _DECLIP_MAX_GAP_SAMPLES


def test_the_repair_cli_reports_unreconstructed_runs(tmp_path: Path) -> None:
    source = tmp_path / "clipped.wav"
    _write_test_wav(str(source), _clipped_tone_with_long_run(), _SAMPLE_RATE)
    output = tmp_path / "out.wav"
    args = [
        "repair",
        str(source),
        "--stages",
        "declip",
        "--params",
        f"repair.declip.clipThresholdDb={_CLIP_THRESHOLD_DB}",
        "-o",
        str(output),
    ]
    as_json = _run_cli([*args, "--json"])
    assert as_json.returncode == 0, as_json.stderr
    runs = json.loads(as_json.stdout)["unreconstructed_runs"]
    assert isinstance(runs, int)
    assert runs >= 1

    as_text = _run_cli(args)
    assert as_text.returncode == 0, as_text.stderr
    assert f"{runs} runs longer than 512 samples" in as_text.stdout


@pytest.mark.parametrize("processor", ["final.bitDepth", "final.outputChain"])
def test_the_final_quantizing_processors_refuse_a_clamp_key(processor: str) -> None:
    from libsonare import SonareValueError, mastering_apply_processor

    with pytest.raises(SonareValueError, match="unknown key params.clamp"):
        mastering_apply_processor(
            processor, [0.0] * 64, sample_rate=_SAMPLE_RATE, params={"clamp": 1}
        )
