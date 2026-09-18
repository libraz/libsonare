"""``project bounce`` over a comped document: binding its audio, and the seam.

Project JSON references audio by URI / storage handle only -- the core never
opens either -- so a document with audio clips renders from nothing until a host
supplies PCM. These cover the two ways this front-end supplies it and the
refusal it reports when neither was used.
"""

from __future__ import annotations

import json
import struct
import subprocess
import sys
import wave
from pathlib import Path
from types import SimpleNamespace

import pytest

SAMPLE_RATE = 48000
# One PPQ beat is 0.5 s at this tempo, so the four-beat clip renders 2 s.
TEMPO_BPM = 120.0
# Where the comp lane hands over from the first take to the second.
SEAM_PPQ = 2.0
CROSSFADE_PPQ = 0.5
SEAM_FRAME = int(SEAM_PPQ * 60.0 / TEMPO_BPM * SAMPLE_RATE)

# Each take is a constant at the opposite sign, so the comp seam is the only
# place the rendered signal can move at all: the crossfade shows up as a ramp
# and its absence as a one-frame full-scale step.
TAKE_LEVELS = (0.5, -0.5)

# The invalid-state exit code, which is the class an unresolved audio source
# already carried when the C ABI refused the render generically.
EXIT_INVALID_STATE = 9
EXIT_INVALID_PARAMETER = 3


def _write_dc_wav(path: Path, level: float, seconds: float = 3.0) -> None:
    """Write a mono PCM16 file holding one constant sample value."""
    code = struct.pack("<h", int(level * 32767))
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(code * int(seconds * SAMPLE_RATE))


def _read_channel(path: Path, channel: int = 0) -> tuple[list[float], int]:
    """Read one channel of a PCM16 WAV as floats, with its sample rate."""
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        sample_rate = wav.getframerate()
        raw = wav.readframes(wav.getnframes())
    codes = struct.unpack(f"<{len(raw) // 2}h", raw)
    return [codes[index] / 32768.0 for index in range(channel, len(codes), channels)], sample_rate


def _largest_step(samples: list[float]) -> tuple[float, int]:
    """The largest frame-to-frame jump in a signal, and where it happens."""
    steps = [abs(samples[index + 1] - samples[index]) for index in range(len(samples) - 1)]
    largest = max(steps)
    return largest, steps.index(largest)


def _console_script() -> Path:
    script = Path(sys.executable).parent / "sonare"
    assert script.is_file(), f"installed console script is missing: {script}"
    return script


def _run_console(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(_console_script()), *args], capture_output=True, text=True)


@pytest.fixture
def comped(tmp_path: Path) -> SimpleNamespace:
    """A document whose one clip comps two URI-referenced takes across a seam.

    The takes are built through the project API and the document is then
    serialized, so the fixture's shape is the serializer's rather than a
    hand-written guess at it. Neither take's PCM survives that round trip, which
    is exactly the state the CLI has to resolve.
    """
    from libsonare import Project

    takes = []
    for index, level in enumerate(TAKE_LEVELS, start=1):
        path = tmp_path / f"take{index}.wav"
        _write_dc_wav(path, level)
        takes.append(path)

    project = Project()
    try:
        project.set_sample_rate(SAMPLE_RATE)
        project.set_tempo_segments([{"start_ppq": 0.0, "bpm": TEMPO_BPM}])
        track = project.add_track("audio", "comp")
        clip = project.add_clip(track, 0.0, 4.0, source_uri=takes[0].as_uri())
        # A clip is the only way to mint a source, so the second take arrives on
        # a scratch track that is removed once the comp lane references it.
        scratch = project.add_track("audio", "scratch")
        scratch_clip = project.add_clip(scratch, 0.0, 1.0, source_uri=takes[1].as_uri())
        project.set_clip_takes(
            clip,
            [
                {"id": 1, "source_id": 1, "name": "a"},
                {"id": 2, "source_id": 2, "name": "b"},
            ],
            active_take_id=1,
        )
        project.set_clip_comp_segments(
            clip,
            [
                {"start_ppq": 0.0, "end_ppq": SEAM_PPQ, "take_id": 1, "crossfade_ppq": 0.0},
                {
                    "start_ppq": SEAM_PPQ,
                    "end_ppq": 4.0,
                    "take_id": 2,
                    "crossfade_ppq": CROSSFADE_PPQ,
                },
            ],
        )
        project.remove_clip(scratch_clip)
        project.remove_track(scratch)
        document = project.to_json_bytes()
    finally:
        project.close()

    path = tmp_path / "comped.json"
    path.write_bytes(document)
    return SimpleNamespace(
        document=path,
        takes=takes,
        uris=[take.as_uri() for take in takes],
        output=tmp_path / "bounced.wav",
        tmp_path=tmp_path,
    )


def _rewritten(comped: SimpleNamespace, name: str, mutate) -> Path:
    """Write a variant of the fixture document with one field changed."""
    document = json.loads(comped.document.read_bytes())
    mutate(document)
    path = comped.tmp_path / name
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def test_bounce_binds_named_audio_sources_and_renders_signal(comped: SimpleNamespace) -> None:
    """``--audio <source_id>=FILE`` reaches the render, not just the exit code."""
    result = _run_console(
        "project",
        "bounce",
        "--in",
        str(comped.document),
        "-o",
        str(comped.output),
        "--audio",
        f"1={comped.takes[0]}",
        "--audio",
        f"2={comped.takes[1]}",
    )

    assert result.returncode == 0, result.stderr
    samples, sample_rate = _read_channel(comped.output)
    assert sample_rate == SAMPLE_RATE
    # An output full of silence is not a render that worked, which is why the
    # assertion is on the signal rather than on the exit code alone.
    assert max(abs(value) for value in samples) > 0.4


def test_resolve_audio_opens_the_documents_own_file_uris(comped: SimpleNamespace) -> None:
    """``--resolve-audio`` needs no per-source argument to produce the same signal."""
    result = _run_console(
        "project",
        "bounce",
        "--in",
        str(comped.document),
        "-o",
        str(comped.output),
        "--resolve-audio",
    )

    assert result.returncode == 0, result.stderr
    samples, _ = _read_channel(comped.output)
    assert max(abs(value) for value in samples) > 0.4


def test_bounce_names_every_unresolved_source_and_its_uri(comped: SimpleNamespace) -> None:
    """The refusal says which sources are missing and how to supply them."""
    result = _run_console(
        "project", "bounce", "--in", str(comped.document), "-o", str(comped.output)
    )

    assert result.returncode == EXIT_INVALID_STATE, result.stderr
    for source_id, uri in enumerate(comped.uris, start=1):
        # Both halves of the address: the id the binding takes, and the URI that
        # says which file the document meant.
        assert f"source {source_id} ({uri})" in result.stderr
        assert f"--audio {source_id}=FILE" in result.stderr
    assert "--resolve-audio" in result.stderr
    assert not comped.output.exists()


def test_resolve_audio_refuses_a_non_file_scheme_by_name(comped: SimpleNamespace) -> None:
    """A scheme this front-end cannot open is named, never skipped."""
    remote = "https://example.invalid/take2.wav"
    document = _rewritten(
        comped, "remote.json", lambda doc: doc["sources"][1].__setitem__("uri", remote)
    )

    result = _run_console(
        "project", "bounce", "--in", str(document), "-o", str(comped.output), "--resolve-audio"
    )

    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
    assert f"source 2 ({remote})" in result.stderr
    assert "file://" in result.stderr
    assert not comped.output.exists()


def test_a_bound_take_keeps_its_source_channels(tmp_path: Path) -> None:
    """A stereo take reaches the render as a pair, not as its downmix.

    The two channels sit at different levels, so a downmix on the way in would
    show up as one level on both sides of the output.
    """
    from libsonare import Project

    take = tmp_path / "stereo.wav"
    left, right = 0.5, -0.25
    with wave.open(str(take), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        frame = struct.pack("<hh", int(left * 32767), int(right * 32767))
        wav.writeframes(frame * SAMPLE_RATE)

    document_path = tmp_path / "stereo.json"
    project = Project()
    try:
        project.set_sample_rate(SAMPLE_RATE)
        project.add_clip(project.add_track("audio", "t"), 0.0, 2.0, source_uri=take.as_uri())
        document_path.write_bytes(project.to_json_bytes())
    finally:
        project.close()

    output = tmp_path / "stereo-out.wav"
    result = _run_console(
        "project", "bounce", "--in", str(document_path), "-o", str(output), "--resolve-audio"
    )

    assert result.returncode == 0, result.stderr
    rendered_left, _ = _read_channel(output, 0)
    rendered_right, _ = _read_channel(output, 1)
    assert max(abs(value) for value in rendered_left) == pytest.approx(abs(left), abs=0.002)
    assert max(abs(value) for value in rendered_right) == pytest.approx(abs(right), abs=0.002)


def test_bounce_refuses_an_audio_binding_no_source_answers_to(comped: SimpleNamespace) -> None:
    """An id the document does not carry is refused under that id."""
    result = _run_console(
        "project",
        "bounce",
        "--in",
        str(comped.document),
        "-o",
        str(comped.output),
        "--audio",
        f"99={comped.takes[0]}",
    )

    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
    assert "no audio source 99" in result.stderr


def test_comp_segment_seam_crossfades_where_the_document_asks_for_one(
    comped: SimpleNamespace,
) -> None:
    """A comped seam arrives as a ramp, and without a crossfade as a full step.

    The two documents differ in one field, so the step is attributable to the
    crossfade alone; the zero-crossfade run is what shows the metric can reach a
    failing value at all, and where.
    """
    stepped = _rewritten(
        comped,
        "stepped.json",
        lambda doc: doc["clips"][0]["comp_segments"][1].__setitem__("crossfade_ppq", 0.0),
    )

    rendered = {}
    for name, document in (("faded", comped.document), ("stepped", stepped)):
        output = comped.tmp_path / f"{name}.wav"
        result = _run_console(
            "project", "bounce", "--in", str(document), "-o", str(output), "--resolve-audio"
        )
        assert result.returncode == 0, result.stderr
        samples, _ = _read_channel(output)
        rendered[name] = _largest_step(samples)

    stepped_size, stepped_frame = rendered["stepped"]
    faded_size, _ = rendered["faded"]
    # The control: an uncrossfaded handover between two takes a full scale apart
    # jumps the whole way in one frame, at the seam and nowhere else.
    assert stepped_size > 0.9
    assert abs(stepped_frame - SEAM_FRAME) <= 2
    # The crossfade spreads that same handover, so nothing steps.
    assert faded_size < 0.05
