"""CLI tests for the mix command: strip placement, block processing and input padding."""

from __future__ import annotations

import argparse
import json
from types import SimpleNamespace
from typing import Any

# ruff: noqa: F403,F405
from ._analyzer_helpers import *
from ._analyzer_helpers import _run_cli, _write_test_wav
from ._helpers import patch_mono_decode

# The scene a single-strip FakeMixer must be built from. The command reads the
# strip ids out of the scene text and hands the buffers to the compiled mixer, so
# a double whose strip_count disagrees with its own scene is a double that could
# not exist: "{}" declares no strips at all.
_ONE_STRIP_SCENE = '{"version":1,"strips":[{"id":"strip0"}],"buses":[],"connections":[]}'


def test_mix_strip_resamples_mono_once_then_copies_both_sides(monkeypatch) -> None:
    """A rate-mismatched mono stem shares one resampler result safely."""
    from libsonare import _cli_mixing

    calls: list[tuple[list[float], int, int]] = []
    monkeypatch.setattr(
        _cli_mixing, "_load_channels_or_downmix", lambda _path: ([[1.0, 2.0]], 44_100)
    )

    def resample(samples, source_rate, target_rate):
        calls.append((list(samples), source_rate, target_rate))
        return [10.0, 20.0]

    monkeypatch.setattr(_cli_mixing, "_resample", resample)
    left, right = _cli_mixing._load_strip_pair("stem.wav", 48_000)

    assert calls == [([1.0, 2.0], 44_100, 48_000)]
    assert left == [10.0, 20.0]
    assert right == [10.0, 20.0]
    assert left is not right


def test_mix_strip_resample_call_count_stereo_and_same_rate(monkeypatch) -> None:
    """Stereo keeps two resamplers while same-rate input keeps zero."""
    from libsonare import _cli_mixing

    calls: list[list[float]] = []
    monkeypatch.setattr(
        _cli_mixing, "_load_channels_or_downmix", lambda _path: ([[1.0], [2.0]], 44_100)
    )
    monkeypatch.setattr(
        _cli_mixing,
        "_resample",
        lambda samples, _source_rate, _target_rate: calls.append(list(samples)) or list(samples),
    )
    left, right = _cli_mixing._load_strip_pair("stereo.wav", 48_000)
    assert (left, right) == ([1.0], [2.0])
    assert calls == [[1.0], [2.0]]

    calls.clear()
    monkeypatch.setattr(_cli_mixing, "_load_channels_or_downmix", lambda _path: ([[3.0]], 48_000))
    left, right = _cli_mixing._load_strip_pair("mono.wav", 48_000)
    assert (left, right) == ([3.0], [3.0])
    assert left is not right
    assert calls == []


def test_mix_cli_resamples_inputs_to_mixer_rate(monkeypatch, tmp_path) -> None:
    """cmd_mix resamples each input to the mixer rate before mixing."""
    import libsonare
    from libsonare import cli

    captured: dict[str, object] = {}

    # A 44.1 kHz stem: mixing it untouched at the 48 kHz default would play fast.
    patch_mono_decode(monkeypatch, lambda path: ([0.0] * 441, 44100))

    class FakeMixer:
        @classmethod
        def from_scene_json(cls, scene_json, *, sample_rate, block_size):
            captured["sample_rate"] = sample_rate
            return cls()

        def scene_warnings(self) -> list[str]:
            return []

        def strip_count(self) -> int:
            return 1

        def compile(self) -> None:
            pass

        def process_stereo(self, left, right):
            captured["left_len"] = len(left[0])
            return SimpleNamespace(left=list(left[0]), right=list(right[0]))

        def tail_samples(self) -> int:
            return 0

        def close(self) -> None:
            pass

    monkeypatch.setattr(libsonare, "Mixer", FakeMixer)
    monkeypatch.setattr(libsonare, "mixing_scene_preset_json", lambda name: _ONE_STRIP_SCENE)
    args = argparse.Namespace(
        scene="",
        preset="demo",
        input=["stem.wav"],
        output=str(tmp_path / "out.wav"),
        sample_rate=48000,
        block_size=512,
        json=True,
    )
    assert cli.cmd_mix(args) == 0
    # 441 samples at 44.1 kHz resample to 480 samples at 48 kHz.
    assert captured["sample_rate"] == 48000
    assert captured["left_len"] == 480


def test_mix_cli_processes_blocks_partial_block_and_tail(monkeypatch, tmp_path) -> None:
    """The CLI keeps one mixer alive across bounded blocks and drains its tail."""
    import libsonare
    from libsonare import cli

    block_lengths: list[int] = []
    drain_lengths: list[int] = []
    patch_mono_decode(monkeypatch, lambda _path: ([0.25] * 513, 48000))

    class FakeMixer:
        @classmethod
        def from_scene_json(cls, _scene_json, *, sample_rate, block_size):
            assert sample_rate == 48000
            assert block_size == 512
            return cls()

        def scene_warnings(self) -> list[str]:
            return []

        def strip_count(self) -> int:
            return 1

        def compile(self) -> None:
            pass

        def process_stereo(self, left, right):
            block_lengths.append(len(left[0]))
            return SimpleNamespace(left=list(left[0]), right=list(right[0]))

        def tail_samples(self) -> int:
            return 515

        def drain_tail_stereo(self, count):
            drain_lengths.append(count)
            return SimpleNamespace(left=[0.0] * count, right=[0.0] * count)

        def close(self) -> None:
            pass

    monkeypatch.setattr(libsonare, "Mixer", FakeMixer)
    monkeypatch.setattr(libsonare, "mixing_scene_preset_json", lambda _name: _ONE_STRIP_SCENE)
    output = tmp_path / "mix.wav"
    args = argparse.Namespace(
        scene="",
        preset="demo",
        input=["stem.wav"],
        output=str(output),
        sample_rate=48000,
        block_size=512,
        json=True,
    )

    assert cli.cmd_mix(args) == 0
    assert block_lengths == [512, 1]
    assert drain_lengths == [512, 3]
    with wave.open(str(output), "rb") as wav:
        assert wav.getnchannels() == 2
        assert wav.getnframes() == 1028


def _scene_file(tmp_path, preset: str) -> str:
    """Write a built-in preset out as a scene file the mix command can read."""
    result = _run_cli(["mixing-preset", "--preset", preset])
    assert result.returncode == 0, result.stderr
    path = tmp_path / f"{preset}.json"
    path.write_text(result.stdout, encoding="utf-8")
    return str(path)


def test_mix_cli_feeds_only_the_strips_an_input_names(tmp_path) -> None:
    """A scene may carry a strip no file feeds, which is what a send-fed return is.

    `vocalReverbSend` is the minimal shape of every assistant-suggested scene:
    one strip a caller has audio for and one return strip fed by a send. Before
    inputs could be addressed by id this needed a silent WAV per return.
    """
    scene = _scene_file(tmp_path, "vocalReverbSend")
    source = tmp_path / "take.wav"
    output = tmp_path / "mix.wav"
    _write_test_wav(str(source), [0.1] * 600, 48000)

    result = _run_cli(
        # fmt: off
        ["mix", "--scene", scene, "--input", f"vocal={source}", "--output", str(output), "--json"],
        # fmt: on
    )

    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["strip_count"] == 2
    with wave.open(str(output), "rb") as wav:
        assert wav.getnframes() > 0


def test_mix_cli_naming_a_strip_matches_feeding_the_rest_silence(tmp_path) -> None:
    """The addressed form is the silent-filler workaround, not an approximation.

    An unfed strip and a strip fed digital silence must reach the master as the
    same thing, so the two renders are compared as bytes. Anything less would
    leave open that addressing a subset quietly changes the mix.
    """
    scene = _scene_file(tmp_path, "vocalReverbSend")
    source = tmp_path / "take.wav"
    silence = tmp_path / "vocal-verb-return.wav"
    _write_test_wav(str(source), [0.1] * 600, 48000)
    _write_test_wav(str(silence), [0.0] * 600, 48000)

    addressed = tmp_path / "addressed.wav"
    filled = tmp_path / "filled.wav"
    assert (
        _run_cli(
            # fmt: off
            [
                "mix",
                "--scene",
                scene,
                "--input",
                f"vocal={source}",
                "--output",
                str(addressed),
                "--json",
            ],
            # fmt: on
        ).returncode
        == 0
    )
    assert (
        _run_cli(
            # fmt: off
            [
                "mix",
                "--scene",
                scene,
                "--input",
                f"vocal={source}",
                "--input",
                f"vocal-verb-return={silence}",
                "--output",
                str(filled),
                "--json",
            ],
            # fmt: on
        ).returncode
        == 0
    )

    assert addressed.read_bytes() == filled.read_bytes()


def test_mix_cli_resolves_a_bare_path_by_its_base_name(tmp_path) -> None:
    """A bare path whose base name is a strip id addresses that strip.

    This is what makes `suggest-mix --input vocal=take.wav` and the mix that
    follows it one flow: the assistant defaults a track's id to the same base
    name, so the file that produced a strip also feeds it.
    """
    scene = _scene_file(tmp_path, "vocalReverbSend")
    source = tmp_path / "vocal.wav"
    output = tmp_path / "mix.wav"
    _write_test_wav(str(source), [0.1] * 600, 48000)

    result = _run_cli(
        ["mix", "--scene", scene, "--input", str(source), "--output", str(output), "--json"]
    )

    assert result.returncode == 0, result.stderr


def test_mix_cli_pads_inputs_of_different_lengths(tmp_path) -> None:
    """A part that enters late is not a reason to truncate the song to it."""
    scene = _scene_file(tmp_path, "drumBusSubgroup")
    long_take = tmp_path / "kick.wav"
    short_take = tmp_path / "snare.wav"
    output = tmp_path / "mix.wav"
    _write_test_wav(str(long_take), [0.1] * 4096, 48000)
    _write_test_wav(str(short_take), [0.1] * 600, 48000)

    result = _run_cli(
        # fmt: off
        [
            "mix",
            "--scene",
            scene,
            "--input",
            f"kick={long_take}",
            "--input",
            f"snare={short_take}",
            "--output",
            str(output),
            "--json",
        ],
        # fmt: on
    )

    assert result.returncode == 0, result.stderr
    # The longest input survives whole rather than being cut to the shortest.
    assert json.loads(result.stdout)["rendered_samples"] >= 4096


@pytest.mark.parametrize(
    ("inputs", "message"),
    [
        (["absent={source}"], "which the scene does not have"),
        (["vocal={source}", "vocal={source}"], "more than once"),
        (["vocal={source}", "{source}"], "all name a strip or all be positional"),
    ],
)
def test_mix_cli_refuses_an_input_it_cannot_place(tmp_path, inputs, message) -> None:
    """Every way of naming a strip wrongly is refused by name, not guessed at."""
    scene = _scene_file(tmp_path, "vocalReverbSend")
    source = tmp_path / "take.wav"
    output = tmp_path / "mix.wav"
    _write_test_wav(str(source), [0.1] * 600, 48000)

    argv = ["mix", "--scene", scene]
    for entry in inputs:
        argv += ["--input", entry.format(source=source)]
    result = _run_cli([*argv, "--output", str(output), "--json"])

    assert result.returncode == 3
    assert message in result.stderr
    assert not output.exists()


def test_mix_cli_rejects_output_without_inputs(tmp_path) -> None:
    """An explicit output never succeeds without producing an artifact."""
    output = tmp_path / "missing.wav"
    result = _run_cli(["mix", "--preset", "vocalReverbSend", "-o", str(output)])

    assert result.returncode == 3
    assert "requires at least one --input" in result.stderr
    assert not output.exists()


def test_mix_cli_real_mixer_handles_multiple_blocks_and_stems(tmp_path) -> None:
    """The subprocess path renders >block-size input through the real mixer."""
    first = tmp_path / "first.wav"
    second = tmp_path / "second.wav"
    output = tmp_path / "mix.wav"
    _write_test_wav(str(first), [0.1] * 513, 48000)
    _write_test_wav(str(second), [0.05] * 513, 48000)

    result = _run_cli(
        [
            "mix",
            "--preset",
            "vocalReverbSend",
            "--input",
            str(first),
            "--input",
            str(second),
            "--block-size",
            "512",
            "--sample-rate",
            "48000",
            "--output",
            str(output),
            "--json",
        ]
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert payload["rendered_samples"] >= 513
    with wave.open(str(output), "rb") as wav:
        assert wav.getnchannels() == 2
        assert wav.getnframes() == payload["rendered_samples"]


def test_suggest_mix_tempo_auto_decodes_each_input_once(monkeypatch) -> None:
    """``--tempo-bpm auto`` measures the loaded first track instead of decoding it again."""
    import numpy as np

    import libsonare
    from libsonare import _cli_mixing, cli

    left = np.array([0.25, -0.5, 1.0], dtype=np.float32)
    right = np.array([0.75, 0.5, -0.125], dtype=np.float32)
    decodes: list[str] = []

    def load_channels(path):
        decodes.append(path)
        if path == "stereo.wav":
            return [left.copy(), right.copy()], 44_100
        return [np.zeros(3, dtype=np.float32)], 48_000

    def load_mono(path):
        decodes.append(path)
        return list(0.5 * (left + right)), 44_100

    measured: list[tuple[list[float], int]] = []

    def detect_bpm(samples, sample_rate):
        measured.append((list(np.asarray(samples, dtype=np.float32)), sample_rate))
        return 96.0

    scene_calls: list[tuple[list[Any], dict[str, Any]]] = []
    monkeypatch.setattr(_cli_mixing, "_load_channels_or_downmix", load_channels)
    monkeypatch.setattr(cli, "_load_audio", load_mono)
    monkeypatch.setattr(libsonare, "detect_bpm", detect_bpm)
    monkeypatch.setattr(
        libsonare,
        "suggest_mix_scene",
        lambda tracks, sample_rate, **options: scene_calls.append((tracks, options)) or {},
    )
    args = argparse.Namespace(
        input=["vox=stereo.wav", "bass=mono.wav"],
        params=None,
        tempo_bpm="auto",
        sample_rate=48_000,
        scene_out=None,
    )

    assert _cli_mixing.cmd_suggest_mix(args) == 0
    assert decodes == ["stereo.wav", "mono.wav"]
    assert measured == [(list(np.float32(0.5) * (left + right)), 44_100)]
    tracks, options = scene_calls[0]
    assert options["tempo_bpm"] == 96.0
    assert [track.track_id for track in tracks] == ["vox", "bass"]
    assert tracks[0].right is not None
    assert tracks[1].right is None
