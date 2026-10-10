"""CLI tests that stereo inputs stay stereo through master, mastering, mix, normalize and declip."""

from __future__ import annotations

import json

# ruff: noqa: F403,F405
from ._analyzer_helpers import *
from ._analyzer_helpers import _run_cli, _write_test_wav


def _write_stereo_wav(path: str, left: list[float], right: list[float], sample_rate: int) -> None:
    """Write stereo 16-bit PCM using only the standard library."""
    frames = bytearray()
    for l_value, r_value in zip(left, right, strict=True):
        frames += struct.pack("<h", int(round(max(-1.0, min(1.0, l_value)) * 32767.0)))
        frames += struct.pack("<h", int(round(max(-1.0, min(1.0, r_value)) * 32767.0)))
    with wave.open(path, "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate))
        wav.writeframes(bytes(frames))


def _side_energy(path: str) -> float:
    """Sum of |L-R| over a stereo file; zero exactly when the channels are equal."""
    with wave.open(path, "rb") as wav:
        assert wav.getnchannels() == 2
        frames = wav.readframes(wav.getnframes())
    values = struct.unpack(f"<{len(frames) // 2}h", frames)
    return sum(abs(values[index] - values[index + 1]) for index in range(0, len(values), 2))


def test_master_cli_keeps_a_stereo_input_stereo(tmp_path) -> None:
    """A stereo file is mastered as a pair, not folded to mono and written back.

    The channel count alone would pass on a mono result duplicated across two
    channels, so the side energy is read too: it must survive, and the stage
    list must carry a stage the mono chain has no way to run.
    """
    source = tmp_path / "mix.wav"
    output = tmp_path / "master.wav"
    length = 48000
    left = [0.2 * math.sin(2.0 * math.pi * 220.0 * i / 48000) for i in range(length)]
    right = [0.2 * math.sin(2.0 * math.pi * 330.0 * i / 48000) for i in range(length)]
    _write_stereo_wav(str(source), left, right, 48000)

    result = _run_cli(["master", str(source), "-o", str(output), "--preset", "pop", "--json"])

    assert result.returncode == 0, result.stderr
    with wave.open(str(output), "rb") as wav:
        assert wav.getnchannels() == 2
    assert _side_energy(str(output)) > 0
    assert any(stage.startswith("stereo.") for stage in json.loads(result.stdout)["stages"])
    # Nothing was dropped, so the downmix warning must not be printed.
    assert "downmixed to mono" not in result.stderr


@pytest.mark.parametrize(
    "selector",
    [
        pytest.param([], id="loudness"),
        pytest.param(["--preset", "pop"], id="preset"),
        pytest.param(["--assistant"], id="assistant"),
        pytest.param(["--report"], id="report"),
    ],
)
def test_mastering_cli_keeps_a_stereo_input_stereo(tmp_path, selector) -> None:
    """Every route through `mastering` carries a stereo source as a pair.

    Parametrized over the selectors rather than tested once, because the four
    reach the chain by different calls -- a preset, a suggested config, an
    explicit config and the standalone loudness path -- and the last one has no
    stereo entry of its own to inherit, so it is the one most likely to fall
    back to a fold without anything noticing.
    """
    source = tmp_path / "mix.wav"
    output = tmp_path / "master.wav"
    length = 48000
    left = [0.2 * math.sin(2.0 * math.pi * 220.0 * i / 48000) for i in range(length)]
    right = [0.2 * math.sin(2.0 * math.pi * 330.0 * i / 48000) for i in range(length)]
    _write_stereo_wav(str(source), left, right, 48000)

    argv = ["mastering", str(source), "-o", str(output), "--json", *selector]
    if selector == ["--report"]:
        argv += [str(tmp_path / "report.json")]
    result = _run_cli(argv)

    assert result.returncode == 0, result.stderr
    with wave.open(str(output), "rb") as wav:
        assert wav.getnchannels() == 2
    # A mono result duplicated across two channels would satisfy the count.
    assert _side_energy(str(output)) > 0
    assert "downmixed to mono" not in result.stderr


def test_master_cli_keeps_a_mono_input_mono(tmp_path) -> None:
    """The stereo path is taken from the source, not applied to everything."""
    source = tmp_path / "take.wav"
    output = tmp_path / "master.wav"
    _write_test_wav(str(source), _generate_sine(220, 48000, 1.0), 48000)

    result = _run_cli(["master", str(source), "-o", str(output), "--preset", "pop", "--json"])

    assert result.returncode == 0, result.stderr
    with wave.open(str(output), "rb") as wav:
        assert wav.getnchannels() == 1


def test_mix_cli_keeps_a_stereo_stem_stereo(tmp_path) -> None:
    """A stereo stem reaches the strip as its own two channels.

    Compared against the same stem folded to mono and written back out as two
    identical channels: that render has no side content at all, which is what
    the mixer used to receive for every stereo input.

    The scene is one strip straight to master rather than a built-in preset,
    because every preset carrying an effect return generates side content of its
    own -- a plate reverb decorrelates a mono input -- and the control would then
    measure the reverb rather than the stem.
    """
    scene = tmp_path / "direct.json"
    scene.write_text(
        json.dumps(
            {
                "version": 1,
                "strips": [{"id": "vocal"}],
                "buses": [{"id": "master", "role": "master"}],
                "connections": [{"source": "vocal", "destination": "master"}],
            }
        ),
        encoding="utf-8",
    )
    scene = str(scene)
    stereo_source = tmp_path / "vocal.wav"
    folded_source = tmp_path / "folded.wav"
    length = 24000
    left = [0.2 * math.sin(2.0 * math.pi * 220.0 * i / 48000) for i in range(length)]
    right = [0.2 * math.sin(2.0 * math.pi * 330.0 * i / 48000) for i in range(length)]
    _write_stereo_wav(str(stereo_source), left, right, 48000)
    mono = [0.5 * (a + b) for a, b in zip(left, right, strict=True)]
    _write_stereo_wav(str(folded_source), mono, mono, 48000)

    renders = {}
    for name, source in (("stereo", stereo_source), ("folded", folded_source)):
        output = tmp_path / f"{name}.wav"
        assert (
            _run_cli(
                # fmt: off
                [
                    "mix",
                    "--scene",
                    scene,
                    "--input",
                    f"vocal={source}",
                    "--sample-rate",
                    "48000",
                    "--output",
                    str(output),
                    "--json",
                ],
                # fmt: on
            ).returncode
            == 0
        )
        renders[name] = _side_energy(str(output))

    assert renders["folded"] == 0
    assert renders["stereo"] > 0


def test_eq_cli_equalizes_a_stereo_input_as_a_pair(tmp_path) -> None:
    """A band placed on the left channel moves the left channel only.

    The two channels carry different tones, so an EQ run on the downmix would
    move both channels or neither and could not produce this result.
    """
    source = tmp_path / "pair.wav"
    output = tmp_path / "eq.wav"
    length = 22050
    left = [0.1 * math.sin(2.0 * math.pi * 440.0 * i / 22050) for i in range(length)]
    right = [0.1 * math.sin(2.0 * math.pi * 277.0 * i / 22050) for i in range(length)]
    _write_stereo_wav(str(source), left, right, 22050)

    result = _run_cli(
        [
            "eq",
            str(source),
            "-o",
            str(output),
            "--type",
            "0",
            "--frequency-hz",
            "440",
            "--gain-db",
            "12",
            "--q",
            "1",
            "--placement",
            "1",
            "--json",
        ]
    )

    assert result.returncode == 0, result.stderr
    assert "downmixed to mono" not in result.stderr
    assert json.loads(result.stdout)["stereo"] is True
    with wave.open(str(output), "rb") as wav:
        assert wav.getnchannels() == 2
        frames = wav.readframes(wav.getnframes())
    values = struct.unpack(f"<{len(frames) // 2}h", frames)
    out_left = [v / 32767.0 for v in values[0::2]]
    out_right = [v / 32767.0 for v in values[1::2]]

    def tone(samples: list[float], frequency: float) -> float:
        omega = 2.0 * math.pi * frequency / 22050
        re = sum(x * math.cos(omega * i) for i, x in enumerate(samples))
        im = sum(x * math.sin(omega * i) for i, x in enumerate(samples))
        return math.hypot(re, im)

    # +12 dB is a factor of about 4; the untouched channel stays near unity.
    assert tone(out_left, 440.0) / tone(left, 440.0) > 3.0
    assert tone(out_right, 277.0) / tone(right, 277.0) == pytest.approx(1.0, abs=0.1)


def test_cli_warns_once_when_it_downmixes_a_stereo_input(tmp_path) -> None:
    """A command that is mono by nature says what it did to the channels.

    The native CLI prints this warning and the Python CLI printed nothing, so a
    user comparing the two front-ends saw a difference in what was processed
    where there was none.

    ``pitch-shift`` is the subject because it still folds a stereo source. When
    it grows a stereo form this case has to move to whichever command has not,
    rather than be deleted: the warning stays correct for every command that
    still downmixes, and a test that goes red here is naming its own successor.
    """
    stereo = tmp_path / "stereo.wav"
    mono = tmp_path / "mono.wav"
    length = 4800
    values = [0.2 * math.sin(2.0 * math.pi * 220.0 * i / 48000) for i in range(length)]
    _write_stereo_wav(str(stereo), values, [-v for v in values], 48000)
    _write_test_wav(str(mono), values, 48000)

    args = ["--semitones", "1.0", "--json"]
    noisy = _run_cli(["pitch-shift", str(stereo), "-o", str(tmp_path / "a.wav"), *args])
    quiet = _run_cli(["pitch-shift", str(mono), "-o", str(tmp_path / "b.wav"), *args])

    assert noisy.returncode == 0, noisy.stderr
    assert quiet.returncode == 0, quiet.stderr
    assert "2-channel input is downmixed to mono" in noisy.stderr
    assert "downmixed to mono" not in quiet.stderr


def test_normalize_carries_a_stereo_source_through_on_one_gain(tmp_path) -> None:
    """normalize keeps both channels and moves them by a single gain.

    Two things are asserted together because either alone passes for the wrong
    reason: a two-channel output could be a mono result written twice, and a
    correct level on the louder channel is what a per-channel gain produces too.
    What separates the linked entry from a per-channel loop is where the QUIET
    channel lands -- 12 dB under the target rather than on it -- so the control
    is the level a per-channel gain would have to produce.

    The downmix warning must also be absent: it is now false for this command,
    and a warning that describes something the command stopped doing is worse
    than none.
    """
    stereo = tmp_path / "stereo.wav"
    out = tmp_path / "out.wav"
    length = 9600
    loud = [0.5 * math.sin(2.0 * math.pi * 220.0 * i / 48000) for i in range(length)]
    quiet = [0.125 * math.sin(2.0 * math.pi * 330.0 * i / 48000) for i in range(length)]
    _write_stereo_wav(str(stereo), loud, quiet, 48000)

    run = _run_cli(["normalize", str(stereo), "-o", str(out), "--target-db", "-1", "--json"])
    assert run.returncode == 0, run.stderr
    assert "downmixed to mono" not in run.stderr

    with wave.open(str(out), "rb") as handle:
        assert handle.getnchannels() == 2
        frames = handle.getnframes()
        raw = handle.readframes(frames)
    values = struct.unpack(f"<{frames * 2}h", raw)
    left = [values[i] / 32767.0 for i in range(0, len(values), 2)]
    right = [values[i] / 32767.0 for i in range(1, len(values), 2)]

    def peak_db(channel: list[float]) -> float:
        return 20.0 * math.log10(max(abs(v) for v in channel))

    assert peak_db(left) == pytest.approx(-1.0, abs=0.01)
    # The control: a per-channel gain would put this channel on the target too.
    # It is 12 dB away instead, which is the gap the source was written with.
    assert peak_db(right) == pytest.approx(-13.04, abs=0.05)
    assert peak_db(left) - peak_db(right) == pytest.approx(12.04, abs=0.02)


def test_declip_cli_repairs_a_stereo_pair_as_a_pair(tmp_path) -> None:
    """declip runs the stereo entry, not the mono one twice.

    The stereo entry repairs the union of both channels' clipped runs, so the
    shallower channel's reconstruction is constrained by the deeper one's
    extent. That is what separates it from a per-channel loop, and it is what
    this reads: the right channel of the pair must differ from the same channel
    declipped on its own. The fixture clips the left hard and the right barely,
    which is the configuration the entry documents as the one that links.

    The threshold is below the written plateau on purpose: a level written as
    0.98 comes back from 16-bit as 0.97999, so a detector asked for 0.98 finds
    no clipping at all and every assertion below would pass over an untouched
    file.
    """
    from libsonare import mastering_repair_declip
    from libsonare.audio import Audio

    sample_rate = 22050
    length = 4410
    threshold = 0.98
    detect_at = 0.95
    left = [
        max(-threshold, min(threshold, 1.6 * math.sin(2.0 * math.pi * 220.0 * i / sample_rate)))
        for i in range(length)
    ]
    right = [
        max(
            -threshold,
            min(threshold, 1.02 * threshold * math.sin(2.0 * math.pi * 220.0 * i / sample_rate)),
        )
        for i in range(length)
    ]
    source = tmp_path / "clipped.wav"
    output = tmp_path / "declipped.wav"
    _write_stereo_wav(str(source), left, right, sample_rate)

    result = _run_cli(
        # fmt: off
        ["declip", str(source), "-o", str(output), "--clip-threshold", str(detect_at), "--json"],
        # fmt: on
    )
    assert result.returncode == 0, result.stderr

    with wave.open(str(output), "rb") as wav:
        assert wav.getnchannels() == 2
    assert _side_energy(str(output)) > 0

    # The control is this file's own right channel, declipped alone through the
    # mono entry: same samples in, so the only thing that can differ is whether
    # the other channel was in the call.
    with Audio.from_file_channel(str(source), 1) as channel:
        alone = list(
            mastering_repair_declip(channel.data, sample_rate=sample_rate, clip_threshold=detect_at)
        )
    with wave.open(str(output), "rb") as wav:
        frames = wav.readframes(wav.getnframes())
    rendered_right = [
        struct.unpack_from("<h", frames, offset)[0] / 32767.0 for offset in range(2, len(frames), 4)
    ]
    worst = max(abs(a - b) for a, b in zip(rendered_right, alone, strict=True))
    assert worst > 0.05
