"""``project align-takes`` over a two-take document: what it writes and what it refuses.

The command's whole output is the document it writes, so the assertions are on the
warp maps and the clips that point at them rather than on the exit code alone. The
takes are continuous pitch glides at different rates: a held tone leaves the
alignment path unconstrained, so its anchors would say nothing about the rate
difference the command exists to encode.
"""

from __future__ import annotations

import json
import struct
import subprocess
import sys
import wave
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SAMPLE_RATE = 22050
# The warp mode an aligned clip must end up in; any other mode ignores the anchors.
WARP_MODE_TIME_STRETCH = 3

EXIT_INVALID_PARAMETER = 3
EXIT_INVALID_STATE = 9

_PAYLOAD_KEYS = {"reference_source", "take_count", "takes", "bytes"}
_TAKE_KEYS = {
    "source_id",
    "warp_ref_id",
    "clip_count",
    "anchor_count",
    "reference_frames",
    "take_frames",
    "mean_residual_frames",
}


def _glide(seconds: float, sample_rate: int = SAMPLE_RATE) -> np.ndarray:
    """A phase-continuous two-partial pitch glide rising eleven semitones."""
    n = int(sample_rate * seconds)
    frac = np.arange(n) / n
    freq = 261.63 * np.power(2.0, 11.0 * frac / 12.0)
    value = np.zeros(n)
    for partial, amplitude in ((1, 1.0), (2, 0.5)):
        value += amplitude * np.sin(np.cumsum(2.0 * np.pi * freq * partial / sample_rate))
    return 0.4 * value


def _write_wav(path: Path, samples: np.ndarray, sample_rate: int = SAMPLE_RATE) -> None:
    codes = (np.clip(samples, -1.0, 1.0) * 32767.0).astype(np.int16)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(struct.pack(f"<{len(codes)}h", *codes))


def _run_console(*args: str) -> subprocess.CompletedProcess[str]:
    script = Path(sys.executable).parent / "sonare"
    assert script.is_file(), f"installed console script is missing: {script}"
    return subprocess.run([str(script), *args], capture_output=True, text=True)


@pytest.fixture(scope="module")
def takes(tmp_path_factory: pytest.TempPathFactory) -> SimpleNamespace:
    """A document whose two clips reference a guide take and a slower one.

    Built through the project API and serialized, so the fixture's shape is the
    serializer's rather than a hand-written guess at it.
    """
    from libsonare import Project

    root = tmp_path_factory.mktemp("align-takes")
    reference, take = root / "reference.wav", root / "take.wav"
    _write_wav(reference, _glide(0.5))
    _write_wav(take, _glide(0.75))

    project = Project()
    try:
        project.set_sample_rate(SAMPLE_RATE)
        guide_track = project.add_track("audio", "guide")
        take_track = project.add_track("audio", "take")
        project.add_clip(guide_track, 0.0, 1.0, source_uri=reference.as_uri())
        project.add_clip(take_track, 0.0, 1.5, source_uri=take.as_uri())
        serialized = project.to_json_bytes()
    finally:
        project.close()

    path = root / "project.json"
    path.write_bytes(serialized)
    return SimpleNamespace(root=root, document=path, reference=reference, take=take)


def _variant(takes: SimpleNamespace, name: str, mutate) -> Path:
    """Write a variant of the fixture document with one field changed."""
    shape = json.loads(takes.document.read_bytes())
    mutate(shape)
    path = takes.root / name
    path.write_text(json.dumps(shape), encoding="utf-8")
    return path


@pytest.fixture(scope="module")
def aligned(takes: SimpleNamespace) -> SimpleNamespace:
    """The JSON-mode run every payload and document assertion reads."""
    output = takes.root / "aligned.json"
    result = _run_console(
        "project",
        "align-takes",
        "--in",
        str(takes.document),
        "-o",
        str(output),
        "--reference-source",
        "1",
        "--resolve-audio",
        "--json",
    )
    assert result.returncode == 0, result.stderr
    return SimpleNamespace(
        payload=json.loads(result.stdout),
        document=json.loads(output.read_bytes()),
        output=output,
    )


def test_payload_is_the_closed_object_the_contract_compares(aligned: SimpleNamespace) -> None:
    """Every key is required and none beyond them is reported."""
    assert set(aligned.payload) == _PAYLOAD_KEYS
    assert aligned.payload["reference_source"] == 1
    assert aligned.payload["take_count"] == 1
    assert aligned.payload["bytes"] == aligned.output.stat().st_size
    (take,) = aligned.payload["takes"]
    assert set(take) == _TAKE_KEYS
    assert take["source_id"] == 2
    assert take["clip_count"] == 1
    # A take running half again as long as the reference produces more chroma
    # frames, which is the rate difference the anchors have to carry.
    assert take["take_frames"] > take["reference_frames"] > 0
    assert take["anchor_count"] >= 2


def test_the_written_warp_map_carries_the_anchors_under_the_takes_name(
    aligned: SimpleNamespace,
) -> None:
    """The map is named for its source and its anchors place the take under the guide."""
    (warp_map,) = aligned.document["warp_maps"]
    assert warp_map["id"] == 1
    assert warp_map["name"] == "take-2"
    assert len(warp_map["anchors"]) == aligned.payload["takes"][0]["anchor_count"]
    for index in range(1, len(warp_map["anchors"])):
        previous, current = warp_map["anchors"][index - 1], warp_map["anchors"][index]
        assert current["warp_sample"] > previous["warp_sample"]
        assert current["source_sample"] > previous["source_sample"]
    # The take is the longer signal, so its own axis has to advance faster than
    # the reference timeline it is placed under.
    last = warp_map["anchors"][-1]
    assert last["source_sample"] > last["warp_sample"]


def test_only_the_takes_clips_are_bound_and_they_are_bound_in_time_stretch(
    aligned: SimpleNamespace,
) -> None:
    """A map without a mode plays unaligned, so the mode is part of the job."""
    clips = {clip["id"]: clip for clip in aligned.document["clips"]}
    assert clips[2]["warp_ref_id"] == 1
    assert clips[2]["warp_mode"] == WARP_MODE_TIME_STRETCH
    # The reference is not a take: nothing about its clip may move.
    assert clips[1]["warp_ref_id"] == 0
    assert clips[1]["warp_mode"] == 0


def test_text_mode_reports_one_line_per_take_between_the_two_summaries(
    takes: SimpleNamespace,
) -> None:
    output = takes.root / "aligned-text.json"
    result = _run_console(
        "project",
        "align-takes",
        "--in",
        str(takes.document),
        "-o",
        str(output),
        "--reference-source",
        "1",
        "--resolve-audio",
    )

    assert result.returncode == 0, result.stderr
    first, take_line, last = result.stdout.splitlines()
    assert first == "Aligned 1 take(s) against source 1"
    assert take_line.startswith("  source 2 -> warp map 1: ")
    assert "mean residual " in take_line and " frames, 1 clip(s)" in take_line
    assert last == f"Wrote {output} ({output.stat().st_size} bytes)"


def test_an_existing_warp_map_is_neither_reused_nor_renumbered(takes: SimpleNamespace) -> None:
    """The new map takes the next free id above every id the document already carries."""
    variant = _variant(
        takes,
        "occupied.json",
        lambda doc: doc.__setitem__(
            "warp_maps",
            [
                {
                    "id": 7,
                    "name": "hand-made",
                    "anchors": [
                        {"warp_sample": 0.0, "source_sample": 0.0},
                        {"warp_sample": 1000.0, "source_sample": 1000.0},
                    ],
                }
            ],
        ),
    )
    output = takes.root / "occupied-out.json"

    result = _run_console(
        "project",
        "align-takes",
        "--in",
        str(variant),
        "-o",
        str(output),
        "--reference-source",
        "1",
        "--resolve-audio",
        "--json",
    )

    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["takes"][0]["warp_ref_id"] == 8
    written = json.loads(output.read_bytes())
    assert [(entry["id"], entry["name"]) for entry in written["warp_maps"]] == [
        (7, "hand-made"),
        (8, "take-2"),
    ]


def test_every_unbound_source_is_named_on_its_own_line(takes: SimpleNamespace) -> None:
    """Both the reference and the take are needed, so both are reported at once."""
    output = takes.root / "unbound.json"
    result = _run_console(
        "project",
        "align-takes",
        "--in",
        str(takes.document),
        "-o",
        str(output),
        "--reference-source",
        "1",
    )

    assert result.returncode == EXIT_INVALID_STATE, result.stderr
    for source_id, path in ((1, takes.reference), (2, takes.take)):
        assert f"source {source_id} ({path.as_uri()})" in result.stderr
        assert f"--audio {source_id}=FILE" in result.stderr
    assert not output.exists()


def test_resolve_audio_refuses_a_non_file_scheme_by_name(takes: SimpleNamespace) -> None:
    """A scheme this front-end cannot open is named, never folded into "no path"."""
    remote = "https://example.invalid/take.wav"
    variant = _variant(
        takes, "remote.json", lambda doc: doc["sources"][1].__setitem__("uri", remote)
    )
    output = takes.root / "remote-out.json"

    result = _run_console(
        "project",
        "align-takes",
        "--in",
        str(variant),
        "-o",
        str(output),
        "--reference-source",
        "1",
        "--resolve-audio",
    )

    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
    assert f"source 2 ({remote})" in result.stderr
    assert "file://" in result.stderr
    assert "--audio 2=FILE" in result.stderr
    assert not output.exists()


def test_a_missing_output_is_an_invalid_parameter_not_a_usage_error(
    takes: SimpleNamespace,
) -> None:
    """Every command here that writes a file refuses an absent -o the same way."""
    result = _run_console(
        "project", "align-takes", "--in", str(takes.document), "--reference-source", "1"
    )

    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
    assert "requires --output" in result.stderr


def test_a_document_with_no_take_is_a_refused_request_not_an_empty_success(
    takes: SimpleNamespace,
) -> None:
    """An aligned document with nothing aligned would be indistinguishable from this."""
    variant = _variant(
        takes,
        "no-takes.json",
        lambda doc: doc.__setitem__(
            "clips", [clip for clip in doc["clips"] if clip["source_id"] == 1]
        ),
    )
    output = takes.root / "no-takes-out.json"

    result = _run_console(
        "project",
        "align-takes",
        "--in",
        str(variant),
        "-o",
        str(output),
        "--reference-source",
        "1",
        "--resolve-audio",
    )

    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
    assert "no takes to align" in result.stderr
    assert not output.exists()


def test_a_take_at_another_rate_is_refused_naming_both_rates(takes: SimpleNamespace) -> None:
    """One chroma frame grid cannot span two rates, so the pair is refused before the call."""
    other_rate = takes.root / "take-16k.wav"
    _write_wav(other_rate, _glide(0.75, 16000), 16000)
    variant = _variant(
        takes,
        "rate-mismatch.json",
        lambda doc: doc["sources"][1].__setitem__("uri", other_rate.as_uri()),
    )
    output = takes.root / "rate-mismatch-out.json"

    result = _run_console(
        "project",
        "align-takes",
        "--in",
        str(variant),
        "-o",
        str(output),
        "--reference-source",
        "1",
        "--resolve-audio",
    )

    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
    assert "16000" in result.stderr and str(SAMPLE_RATE) in result.stderr
    assert not output.exists()


def test_a_reference_that_is_not_an_audio_source_is_refused_under_its_id(
    takes: SimpleNamespace,
) -> None:
    """A MIDI source has no file to decode, so it cannot be the reference."""
    from libsonare import Project

    path = takes.root / "with-midi.json"
    project = Project.from_json(takes.document.read_bytes())
    try:
        project.add_midi_clip(0.0, 4.0)
        path.write_bytes(project.to_json_bytes())
    finally:
        project.close()
    midi_ids = [
        source["id"] for source in json.loads(path.read_bytes())["sources"] if source["kind"]
    ]
    assert midi_ids, "the fixture grew no MIDI source, so the refusal is unreachable"

    result = _run_console(
        "project",
        "align-takes",
        "--in",
        str(path),
        "-o",
        str(takes.root / "with-midi-out.json"),
        "--reference-source",
        str(midi_ids[0]),
        "--resolve-audio",
    )

    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
    assert f"source {midi_ids[0]} is not an audio source" in result.stderr


def test_a_resolution_the_core_refuses_is_passed_through_rather_than_restated(
    takes: SimpleNamespace,
) -> None:
    """The chroma folds onto twelve pitch classes, and the core owns that domain."""
    result = _run_console(
        "project",
        "align-takes",
        "--in",
        str(takes.document),
        "-o",
        str(takes.root / "bins-out.json"),
        "--reference-source",
        "1",
        "--resolve-audio",
        "--bins-per-octave",
        "13",
    )

    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
