"""End-to-end CLI coverage: ``sections``, ``decompose-stems``, ``transcribe``, ``suggest-mix``.

The positive assertions read the content each command produced -- how many
sections, how many components and whether they sum back, which notes reached the
SMF, which tracks the suggested scene addresses -- because a suite that only
reads an exit status passes on a command that prints nothing.
"""

from __future__ import annotations

import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

SR = 22050

# The played sequence and the MIDI notes it must come back as, at A440.
_FREQUENCIES = (261.6256, 329.6276, 391.9954, 523.2511)
_EXPECTED_NOTES = (60, 64, 67, 72)

_NOTE_ON = 0x9


def _run_cli(args: list[str]) -> subprocess.CompletedProcess[str]:
    src_dir = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src_dir + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run(
        [sys.executable, "-m", "libsonare.cli", *args],
        capture_output=True,
        text=True,
        env=env,
    )


def _tone(freq: float, duration: float = 0.35, amp: float = 0.4) -> list[float]:
    """One faded sine note, so a segmenter sees an onset rather than a click."""
    count = int(SR * duration)
    fade = int(0.005 * SR)
    samples = []
    for index in range(count):
        envelope = 1.0
        if index < fade:
            envelope = index / fade
        elif index >= count - fade:
            envelope = (count - 1 - index) / fade
        samples.append(amp * envelope * math.sin(2.0 * math.pi * freq * index / SR))
    return samples


def _melody() -> list[float]:
    """The four-note sequence the transcription assertions read."""
    silence = [0.0] * int(SR * 0.05)
    samples: list[float] = []
    for freq in _FREQUENCIES:
        samples.extend(_tone(freq))
        samples.extend(silence)
    return samples


def _voice_like(duration: float = 1.0) -> list[float]:
    """A signal the source classifier reads as a vocal.

    A bare sine is classified as something with no effect send, so a scene built
    from one carries no delay return and every test about how the delay is
    voiced passes over an empty scene. The partial weights are what put the
    centroid and rolloff where a voice's are; this is not an attempt to sound
    like one.
    """
    weights = [0.30, 0.70, 0.90, 0.85, 0.80, 0.70, 0.60, 0.55, 0.50, 0.45, 0.40, 0.35]
    count = int(SR * duration)
    samples = []
    for index in range(count):
        value = 0.0
        for partial, weight in enumerate(weights):
            value += weight * math.sin(2.0 * math.pi * 180.0 * (partial + 1) * index / SR)
        samples.append(0.045 * value)
    return samples


def _write_wav(path: str, samples: list[float], sample_rate: int = SR) -> None:
    frames = bytearray()
    for sample in samples:
        frames += struct.pack("<h", int(round(max(-1.0, min(1.0, sample)) * 32767.0)))
    with wave.open(path, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(bytes(frames))


def _read_wav(path: str) -> tuple[list[float], int]:
    with wave.open(path, "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getsampwidth() == 2
        frames = wav.readframes(wav.getnframes())
        rate = wav.getframerate()
    values = struct.unpack(f"<{len(frames) // 2}h", frames)
    return [value / 32767.0 for value in values], rate


@pytest.fixture(scope="module")
def melody_wav() -> str:
    """One written take, shared by the commands that only read it."""
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "melody.wav")
        _write_wav(path, _melody())
        yield path


def test_sections_reports_the_boundaries_it_found(melody_wav: str) -> None:
    """Sections come back in time order, covering the take without gaps."""
    result = _run_cli(["sections", melody_wav, "--min-duration", "0.3", "--json"])
    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)

    sections = payload["sections"]
    assert payload["count"] == len(sections)
    # Two, because the boundary detector is given a minimum below the take's own
    # length: a single section would also satisfy every invariant below.
    assert len(sections) >= 2
    assert sections[0]["start"] == 0.0
    for section in sections:
        assert section["type"] in {
            "intro",
            "verse",
            "pre-chorus",
            "chorus",
            "bridge",
            "instrumental",
            "outro",
            "unknown",
        }
        assert section["end"] > section["start"]
        assert math.isfinite(section["energy"])
        assert math.isfinite(section["confidence"])
    for current, following in zip(sections, sections[1:], strict=False):
        assert following["start"] == pytest.approx(current["end"])


def test_sections_human_output_names_every_section(melody_wav: str) -> None:
    result = _run_cli(["sections", melody_wav, "--min-duration", "0.3"])
    assert result.returncode == 0, result.stderr
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    assert lines[0].startswith("  Sections (")
    assert len(lines) - 1 == int(lines[0].split("(")[1].split(")")[0])


def test_sections_refuses_an_out_of_domain_analysis_size(melody_wav: str) -> None:
    result = _run_cli(["sections", melody_wav, "--n-fft", "0", "--json"])
    assert result.returncode == 3
    assert result.stdout == ""


def test_sections_names_an_unparseable_option(melody_wav: str) -> None:
    """A value no float reads is refused by name before the pipeline runs."""
    result = _run_cli(["sections", melody_wav, "--min-duration", "wide"])
    assert result.returncode == 2
    assert "--min-duration" in result.stderr


def test_decompose_stems_writes_components_that_sum_back(melody_wav: str) -> None:
    """One file per component, and the masks sum to one, so the parts sum to the input."""
    with tempfile.TemporaryDirectory() as tmpdir:
        output = os.path.join(tmpdir, "stem.wav")
        result = _run_cli(
            ["decompose-stems", melody_wav, "-o", output, "--n-components", "3", "--json"]
        )
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)

        assert payload["count"] == 3
        assert payload["sample_rate"] == SR
        assert len(payload["components"]) == 3
        assert len(payload["energies"]) == 3
        assert all(energy > 0.0 for energy in payload["energies"])

        source, _rate = _read_wav(melody_wav)
        summed = [0.0] * payload["length"]
        for path in payload["components"]:
            assert os.path.exists(path)
            component, rate = _read_wav(path)
            assert rate == SR
            assert len(component) == payload["length"]
            for index, value in enumerate(component):
                summed[index] += value

        # The reconstruction is exact up to the STFT edge frames and the 16-bit
        # write, so compare on the interior against the input's own scale.
        margin = SR // 10
        interior = range(margin, min(len(source), len(summed)) - margin)
        peak = max(abs(source[index]) for index in interior)
        for index in interior:
            assert summed[index] == pytest.approx(source[index], abs=0.02 * peak)


def test_decompose_stems_requires_an_output(melody_wav: str) -> None:
    result = _run_cli(["decompose-stems", melody_wav, "--json"])
    assert result.returncode == 3
    assert "--output" in result.stderr


def test_decompose_stems_names_an_unknown_initialiser(melody_wav: str) -> None:
    with tempfile.TemporaryDirectory() as tmpdir:
        output = os.path.join(tmpdir, "stem.wav")
        result = _run_cli(["decompose-stems", melody_wav, "-o", output, "--init", "warm"])
        assert result.returncode == 3
        assert "--init" in result.stderr


def test_transcribe_writes_an_smf_whose_notes_read_back(melody_wav: str) -> None:
    """The file is a readable SMF, and the notes that were played are in it."""
    import libsonare

    with tempfile.TemporaryDirectory() as tmpdir:
        output = os.path.join(tmpdir, "take.mid")
        result = _run_cli(["transcribe", melody_wav, "-o", output, "--tempo-bpm", "120", "--json"])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)

        assert payload["output"] == output
        assert payload["note_count"] == len(_EXPECTED_NOTES)
        assert payload["tempo_bpm"] == pytest.approx(120.0)
        assert payload["bytes"] == os.path.getsize(output)

        project = libsonare.Project()
        try:
            clip_id = project.import_smf(Path(output).read_bytes())
            events = json.loads(project.to_json())["midi_content"][str(clip_id)]
        finally:
            project.close()

    starts = [
        (int(event["data0"]) >> 8) & 0x7F
        for event in events
        if ((int(event["data0"]) >> 20) & 0xF) == _NOTE_ON
    ]
    assert starts == list(_EXPECTED_NOTES)
    assert len(events) == 2 * payload["note_count"]


def test_transcribe_reports_the_tempo_it_detected(melody_wav: str) -> None:
    """Leaving the tempo out detects one, and says which was used."""
    with tempfile.TemporaryDirectory() as tmpdir:
        output = os.path.join(tmpdir, "take.mid")
        result = _run_cli(["transcribe", melody_wav, "-o", output, "--json"])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert payload["tempo_bpm"] > 0.0
        assert math.isfinite(payload["tempo_bpm"])
        assert payload["note_count"] > 0


def test_transcribe_applies_a_fixed_velocity(melody_wav: str) -> None:
    """A fixed velocity reaches the written file rather than the measured level."""
    import libsonare

    with tempfile.TemporaryDirectory() as tmpdir:
        output = os.path.join(tmpdir, "take.mid")
        result = _run_cli(
            [
                "transcribe",
                melody_wav,
                "-o",
                output,
                "--tempo-bpm",
                "120",
                "--fixed-velocity",
                "77",
                "--json",
            ]
        )
        assert result.returncode == 0, result.stderr

        project = libsonare.Project()
        try:
            clip_id = project.import_smf(Path(output).read_bytes())
            events = json.loads(project.to_json())["midi_content"][str(clip_id)]
        finally:
            project.close()

    velocities = {
        int(event["data0"]) & 0x7F
        for event in events
        if ((int(event["data0"]) >> 20) & 0xF) == _NOTE_ON
    }
    assert velocities == {77}


def test_transcribe_refuses_a_non_positive_tempo(melody_wav: str) -> None:
    with tempfile.TemporaryDirectory() as tmpdir:
        output = os.path.join(tmpdir, "take.mid")
        result = _run_cli(["transcribe", melody_wav, "-o", output, "--tempo-bpm", "0"])
        assert result.returncode == 3
        assert "--tempo-bpm" in result.stderr
        assert not os.path.exists(output)


def test_suggest_mix_addresses_every_track_it_was_given() -> None:
    """The scene, the measurements and the explanation all name the tracks passed in."""
    with tempfile.TemporaryDirectory() as tmpdir:
        bass = os.path.join(tmpdir, "bass.wav")
        lead = os.path.join(tmpdir, "lead.wav")
        _write_wav(bass, _tone(110.0, 1.0, 0.5))
        _write_wav(lead, _tone(880.0, 1.0, 0.25))

        result = _run_cli(
            ["suggest-mix", "--input", bass, "--input", f"top={lead}", "--sample-rate", str(SR)]
        )
    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)

    assert set(payload) == {"scene", "tracks", "mix", "explanation"}
    # A bare path takes the file's own name; an ID= prefix overrides it.
    assert {track["stripId"] for track in payload["tracks"]} == {"bass", "top"}
    assert {strip["id"] for strip in payload["scene"]["strips"]} >= {"bass", "top"}
    assert payload["explanation"]
    assert any("bass" in line for line in payload["explanation"])


def test_suggest_mix_forwards_a_param_to_the_suggestion() -> None:
    """A param the caller spelled changes the suggestion it explains."""
    with tempfile.TemporaryDirectory() as tmpdir:
        track = os.path.join(tmpdir, "gtr.wav")
        _write_wav(track, _tone(220.0, 1.0, 0.5))

        def _explanation(*params: str) -> list[str]:
            result = _run_cli(["suggest-mix", "--input", track, "--sample-rate", str(SR), *params])
            assert result.returncode == 0, result.stderr
            return json.loads(result.stdout)["explanation"]

        default = _explanation()
        staged = _explanation("--params", "targetTrackLufs=-24")

    assert default != staged
    assert any("-24.0 LUFS" in line for line in staged)


def test_suggest_mix_names_an_unknown_param() -> None:
    with tempfile.TemporaryDirectory() as tmpdir:
        track = os.path.join(tmpdir, "gtr.wav")
        _write_wav(track, _tone(220.0, 0.5, 0.5))
        result = _run_cli(
            ["suggest-mix", "--input", track, "--sample-rate", str(SR), "--params", "loudness=-9"]
        )
    assert result.returncode == 3
    assert "loudness" in result.stderr
    assert result.stdout == ""


def test_suggest_mix_scene_out_is_what_the_mixer_reads() -> None:
    """--scene-out writes the scene in the form `mix --scene` loads.

    The file is written from the document already in hand rather than through a
    second analysis, so the check that matters is that the two agree: the scene
    on disk must equal what the core's scene-only entry point serializes for the
    same tracks, and `mix` must then render it.
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        lead = os.path.join(tmpdir, "leadVox.wav")
        scene_path = os.path.join(tmpdir, "scene.json")
        rendered = os.path.join(tmpdir, "mix.wav")
        _write_wav(lead, _voice_like())

        result = _run_cli(
            # fmt: off
            ["suggest-mix", "--input", lead, "--sample-rate", str(SR), "--scene-out", scene_path],
            # fmt: on
        )
        assert result.returncode == 0, result.stderr

        with open(scene_path, encoding="utf-8") as handle:
            written = json.load(handle)
        assert written == json.loads(result.stdout)["scene"]

        from libsonare import MixTrackInput, suggest_mix_scene_json
        from libsonare.audio import Audio

        # Loaded through the same decoder the CLI uses. Reconstructing the
        # samples here instead compares two different inputs -- a scale factor
        # one count apart moves the measured loudness and with it the staging,
        # so the scenes differ for a reason that has nothing to do with the
        # serializer under test.
        with Audio.from_file(lead) as audio:
            samples = audio.data
        # The core's own serializer is the referent: a scene this file rendered
        # itself would only be checked against this file.
        # The id is also the name hint, which is what the CLI passes: leaving it
        # out here changes the classification confidence and with it the balance.
        assert written == json.loads(
            suggest_mix_scene_json(
                [MixTrackInput("leadVox", samples, name="leadVox")], sample_rate=SR
            )
        )

        rendering = _run_cli(
            # fmt: off
            [
                "mix",
                "--scene",
                scene_path,
                "--input",
                f"leadVox={lead}",
                "--sample-rate",
                str(SR),
                "--output",
                rendered,
                "--json",
            ],
            # fmt: on
        )
        assert rendering.returncode == 0, rendering.stderr


def test_suggest_mix_tempo_option_and_param_are_one_value() -> None:
    """--tempo-bpm reaches the same field as --params tempoBpm=, so naming both is refused."""
    with tempfile.TemporaryDirectory() as tmpdir:
        lead = os.path.join(tmpdir, "leadVox.wav")
        _write_wav(lead, _voice_like())

        def _scene(*extra: str) -> dict:
            result = _run_cli(["suggest-mix", "--input", lead, "--sample-rate", str(SR), *extra])
            assert result.returncode == 0, result.stderr
            return json.loads(result.stdout)["scene"]

        assert _scene("--tempo-bpm", "90") == _scene("--params", "tempoBpm=90")
        assert _scene("--tempo-bpm", "90") != _scene()

        clash = _run_cli(
            # fmt: off
            [
                "suggest-mix",
                "--input",
                lead,
                "--sample-rate",
                str(SR),
                "--tempo-bpm",
                "90",
                "--params",
                "tempoBpm=100",
            ],
            # fmt: on
        )
    assert clash.returncode == 3
    assert "same value" in clash.stderr


def test_suggest_mix_requires_an_input() -> None:
    result = _run_cli(["suggest-mix", "--sample-rate", str(SR)])
    assert result.returncode == 3
    assert "--input" in result.stderr
