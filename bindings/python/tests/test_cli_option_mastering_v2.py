"""Focused option-routing coverage for mastering and mixing CLI handlers."""

from __future__ import annotations

import argparse
import json
import wave
from pathlib import Path
from types import SimpleNamespace

import pytest

from ._helpers import patch_mono_decode


def _identity_fit():
    """A fit that leaves the samples alone: the writer under test is the plain quantizer."""
    from libsonare._cli_output import _OutputFit

    return _OutputFit()


def _result(*, stages: list[str] | None = None, report: object | None = None) -> SimpleNamespace:
    return SimpleNamespace(
        samples=[0.1, -0.1],
        sample_rate=48_000,
        input_lufs=-18.0,
        output_lufs=-14.0,
        applied_gain_db=4.0,
        latency_samples=12,
        stages=stages or ["loudness"],
        report=report,
        declip_unreconstructed_runs=None,
    )


def test_mastering_preset_params_and_bits_reach_api_and_writer(
    monkeypatch, capsys, tmp_path
) -> None:
    import libsonare
    from libsonare import _cli_mastering, _cli_output, cli

    calls: dict[str, object] = {}
    patch_mono_decode(monkeypatch, lambda path: ([0.0], 44_100))

    def master_audio(*args, **kwargs):
        calls["preset"] = kwargs["preset"]
        calls["overrides"] = kwargs["overrides"]
        return _result(stages=["eq.tilt", "loudness"])

    monkeypatch.setattr(libsonare, "master_audio", master_audio)
    monkeypatch.setattr(
        _cli_output,
        "_write_wav",
        lambda path, samples, sample_rate, bits, fit: calls.update(
            writer=(path, sample_rate, bits)
        ),
    )

    output = tmp_path / "mastered.wav"
    args = cli._build_parser().parse_args(
        [
            "mastering",
            "input.wav",
            "--preset",
            "pop",
            "--params",
            "loudness.targetLufs=-13",
            "--bits",
            "24",
            "--output",
            str(output),
            "--json",
        ]
    )
    assert _cli_mastering.cmd_mastering(args) == 0

    assert calls["preset"] == "pop"
    assert calls["overrides"] == {"loudness": {"targetLufs": -13.0}}
    assert calls["writer"] == (str(output), 48_000, 24)
    payload = json.loads(capsys.readouterr().out)
    assert payload["mode"] == "preset"
    assert payload["preset"] == "pop"


def test_mastering_config_reads_the_document_and_forwards_params(monkeypatch, tmp_path) -> None:
    import libsonare
    from libsonare import _cli_mastering, cli

    calls: dict[str, object] = {}
    patch_mono_decode(monkeypatch, lambda path: ([0.0], 44_100))

    def master_audio(*args, **kwargs):
        assert kwargs["preset"] is None
        calls["config"] = kwargs["overrides"]
        return _result(stages=["loudness"])

    monkeypatch.setattr(libsonare, "master_audio", master_audio)
    config_path = tmp_path / "chain.json"
    config_path.write_text(
        json.dumps({"version": 2, "params": {"loudness.targetLufs": -15.0}}),
        encoding="utf-8",
    )
    args = cli._build_parser().parse_args(
        [
            "mastering",
            "input.wav",
            "--chain-config",
            str(config_path),
            "--params",
            "loudness.ceilingDbtp=-0.5",
        ]
    )

    assert _cli_mastering.cmd_mastering(args) == 0
    # The library reads the document into a complete chain; --params lands on top of it.
    config = calls["config"]
    assert isinstance(config, dict)
    loudness = config["loudness"]
    assert loudness["targetLufs"] == -15.0
    assert loudness["ceilingDbtp"] == -0.5


def test_mastering_assistant_enable_repair_explain_reaches_suggestion_and_chain(
    monkeypatch, capsys
) -> None:
    import libsonare
    from libsonare import _cli_mastering, cli

    calls: dict[str, object] = {}
    patch_mono_decode(monkeypatch, lambda path: ([0.0], 48_000))

    def suggest(*args, **kwargs):
        calls["assistant_params"] = kwargs["params"]
        return SimpleNamespace(
            chain_config={"loudness": {"targetLufs": -14.0}, "repair": {"declick": {"enabled": 1}}},
            explanation=["repair enabled"],
        )

    def master_audio(*args, **kwargs):
        assert kwargs["preset"] is None
        calls["config"] = kwargs["overrides"]
        return _result(stages=["repair.declick", "loudness"])

    monkeypatch.setattr(libsonare, "mastering_assistant_suggest", suggest)
    monkeypatch.setattr(libsonare, "master_audio", master_audio)
    args = cli._build_parser().parse_args(
        [
            "mastering",
            "input.wav",
            "--assistant",
            "--enable-repair",
            "--explain",
            "--params",
            "loudness.truePeakOversample=8",
            "--json",
        ]
    )

    assert _cli_mastering.cmd_mastering(args) == 0
    # Only what the command line named reaches the assistant. A supplied
    # loudness key is what marks the field explicit for a delivery target, and
    # every unnamed control keeps the library's own default.
    assert calls["assistant_params"] == {"enableRepair": True}
    assert calls["config"] == {
        "loudness": {"targetLufs": -14.0, "truePeakOversample": 8.0},
        "repair": {"declick": {"enabled": 1}},
    }
    assert json.loads(capsys.readouterr().out)["explanation"] == ["repair enabled"]


def test_mastering_semantic_selector_conflicts_are_usage_errors(capsys) -> None:
    from libsonare import cli

    with pytest.raises(SystemExit) as caught:
        cli._build_parser().parse_args(
            ["mastering", "input.wav", "--preset", "pop", "--chain-config", "chain.json"]
        )
    assert caught.value.code == 2
    assert "mutually exclusive" in capsys.readouterr().err


def test_mastering_processor_hands_a_stereo_source_to_the_facade_unmixed(
    monkeypatch, capsys, tmp_path
) -> None:
    """A two-channel source reaches the stereo facade as its own two channels.

    The channels here are deliberately distinct and asserted by value, because
    the shape this guards against passed every structural check it had: the
    command used to hand the mono downmix in as BOTH channels, so the facade saw
    a fabricated image, and a test that only asserted "the stereo facade was
    called" was green throughout. There is no ``--stereo`` to spell -- the file's
    channel count is the whole decision.
    """
    import libsonare
    from libsonare import _cli_common, _cli_mastering, _cli_output, cli

    calls: dict[str, object] = {}
    monkeypatch.setattr(_cli_common, "_source_channel_count", lambda path: 2)
    monkeypatch.setattr(
        _cli_common, "_load_audio_channels_known", lambda path, channels: ([[0.5], [-0.25]], 44_100)
    )
    monkeypatch.setattr(libsonare, "mastering_processor_catalog", lambda: [])

    def stereo(*args, **kwargs):
        calls["stereo"] = (args, kwargs)
        return SimpleNamespace(
            left=[0.1],
            right=[0.2],
            sample_rate=44_100,
            input_lufs=-18.0,
            output_lufs=-17.0,
            applied_gain_db=1.0,
            latency_samples=3,
            declip_unreconstructed_runs=None,
        )

    monkeypatch.setattr(libsonare, "mastering_apply_processor_stereo", stereo)
    monkeypatch.setattr(
        _cli_output,
        "_write_wav_stereo",
        lambda path, left, right, sample_rate, bits, fit: calls.update(
            writer=(left, right, sample_rate, bits)
        ),
    )
    output = tmp_path / "processor.wav"
    args = cli._build_parser().parse_args(
        [
            "mastering-processor",
            "--processor",
            "dynamics.compressor",
            "--bits",
            "24",
            "--output",
            str(output),
            "input.wav",
            "--json",
        ]
    )

    assert _cli_mastering.cmd_mastering_processor(args) == 0
    assert calls["stereo"][0][0] == "dynamics.compressor"
    # The two channels the file carries, not one of them twice.
    assert calls["stereo"][0][1] == [0.5]
    assert calls["stereo"][0][2] == [-0.25]
    # And the processed pair is written as a pair rather than folded back.
    assert calls["writer"] == ([0.1], [0.2], 44_100, 24)
    assert json.loads(capsys.readouterr().out)["stereo"] is True


def test_mastering_processor_rejects_the_removed_stereo_flag() -> None:
    """``--stereo`` is gone from both front-ends, so the parser must refuse it.

    Asserted rather than left to the option inventory: the contract compares the
    two front-ends' declared options against each other, so a flag surviving in
    both would agree with itself and be reported as parity.
    """
    from libsonare import cli

    with pytest.raises(SystemExit):
        cli._build_parser().parse_args(
            ["mastering-processor", "--processor", "eq.equalizer", "--stereo", "input.wav"]
        )


def test_mastering_processor_stereo_only_catalog_auto_routes(monkeypatch, capsys) -> None:
    import libsonare
    from libsonare import _cli_mastering

    calls: list[str] = []
    patch_mono_decode(monkeypatch, lambda path: ([0.0], 44_100))
    monkeypatch.setattr(
        libsonare,
        "mastering_processor_catalog",
        lambda: [SimpleNamespace(id="stereo.imager", stereo_only=True)],
    )
    monkeypatch.setattr(
        libsonare,
        "mastering_apply_processor_stereo",
        lambda name, *args, **kwargs: (
            calls.append(name)
            or SimpleNamespace(
                left=[0.1],
                right=[0.1],
                sample_rate=44_100,
                input_lufs=-18.0,
                output_lufs=-17.0,
                applied_gain_db=1.0,
                latency_samples=3,
                declip_unreconstructed_runs=None,
            )
        ),
    )
    args = argparse.Namespace(
        file="input.wav",
        processor="stereo.imager",
        params="",
        bits=16,
        stereo=False,
        output="",
        json=True,
    )
    assert _cli_mastering.cmd_mastering_processor(args) == 0
    assert calls == ["stereo.imager"]
    assert json.loads(capsys.readouterr().out)["stereo"] is True


def test_eq_params_and_shortcut_conflict_is_a_usage_refusal(capsys) -> None:
    """A shortcut spelled alongside --params is refused before any read, default-valued or not.

    Driven through the parser rather than a hand-built namespace: whether a
    shortcut was supplied is something only argv answers.
    """
    from libsonare import cli

    for shortcut, name in ((["--frequency-hz", "1500"], "frequency-hz"), (["--q", "1.0"], "q")):
        with pytest.raises(SystemExit) as exc_info:
            cli._build_parser().parse_args(
                ["eq", "input.wav", "--params", "band0.gainDb=1", *shortcut]
            )
        assert exc_info.value.code == 2
        assert f"--{name} cannot be combined with --params" in capsys.readouterr().err


def test_mastering_pair_analyze_forwards_params(monkeypatch) -> None:
    import libsonare
    from libsonare import _cli_mastering, audio

    calls: dict[str, object] = {}
    monkeypatch.setattr(
        _cli_mastering,
        "_read_audio_bounded",
        lambda path: path.encode(),
    )
    decoded = {
        b"source.wav": SimpleNamespace(sample_rate=44_100, channels=[[0.0]]),
        b"reference.wav": SimpleNamespace(sample_rate=44_100, channels=[[0.1]]),
    }
    monkeypatch.setattr(audio, "decode_channels", lambda data: decoded[data])

    def analyze(*args, **kwargs):
        calls["args"] = args
        calls["params"] = kwargs["params"]
        return libsonare.MatchEstimateReferenceDelaySamplesResult(delay_samples=0.0)

    monkeypatch.setattr(libsonare, "mastering_analyze_pair", analyze)
    args = argparse.Namespace(
        file="source.wav",
        reference="reference.wav",
        analysis="spectralDifference",
        params="windowMs=120",
    )
    assert _cli_mastering.cmd_mastering_pair_analyze(args) == 0
    assert calls["params"] == {"windowMs": 120.0}
    assert calls["args"] == ("spectralDifference", [[0.0]], [[0.1]])


def test_mixing_preset_uses_stable_default(monkeypatch, capsys) -> None:
    """Omitting ``--preset`` still resolves to the advertised default.

    Driven through the parser rather than a hand-built namespace: ``--preset``
    declares its default in argparse, so ``preset=None`` is a state the CLI
    cannot produce, and asserting on it pinned a handler-side fallback that was
    unreachable. What the contract actually promises is this end-to-end path.
    """
    import libsonare
    from libsonare import _cli_mixing, cli

    calls: list[str] = []
    monkeypatch.setattr(
        libsonare,
        "mixing_scene_preset",
        lambda name: calls.append(name) or {},
    )
    args = cli._build_parser().parse_args(["mixing-preset"])
    assert _cli_mixing.cmd_mixing_preset(args) == 0
    assert calls == ["vocalReverbSend"]
    assert capsys.readouterr().out == "{}\n"


def test_mixing_preset_default_matches_the_native_cli() -> None:
    """Both CLIs advertise the same default, from their own declarations.

    The two surfaces declare it separately, so a change to one is invisible to
    the other; this is the only place the pair is compared.
    """
    from pathlib import Path

    from libsonare import cli

    action = next(
        action
        for action in cli._build_parser()
        ._subparsers._group_actions[0]  # noqa: SLF001
        .choices["mixing-preset"]
        ._actions
        if action.dest == "preset"
    )
    assert action.default == "vocalReverbSend"

    native = Path(__file__).resolve().parents[3] / "tools" / "cli" / "sonare_cli_registry.cpp"
    text = native.read_text(encoding="utf-8")
    assert 'string_value("preset", "vocalReverbSend")' in text


def test_python_wav_writer_emits_24_bit_header(tmp_path) -> None:
    from libsonare._cli_common import _write_wav

    output = tmp_path / "24-bit.wav"
    _write_wav(str(output), [0.0, 1.0], 48_000, 24, fit=_identity_fit())
    with wave.open(str(output), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getframerate() == 48_000
        assert wav.getsampwidth() == 3
        assert wav.getnframes() == 2


def test_native_handler_source_consumes_bits_and_rejects_eq_conflicts() -> None:
    tools = Path(__file__).parents[3] / "tools" / "cli"
    text = (tools / "sonare_cli_mastering.cpp").read_text(encoding="utf-8")
    assert 'args.get_int("bits", 16)' in text
    registry = (tools / "sonare_cli_registry.cpp").read_text(encoding="utf-8")
    assert '" cannot be combined with --params"' in registry
    assert "args.source_channels == 2 || is_stereo_only_processor(processor)" in text
    # Negatively too, because the positive assertion above is satisfied by a
    # substring: a surviving `args.has("stereo")` would sit beside it happily and
    # keep routing on a flag that no longer exists on either front-end.
    assert 'args.has("stereo")' not in text


def test_mastering_cli_reports_a_blocked_loudness_target(capsys, monkeypatch) -> None:
    """The default JSON payload carries the flag, and it agrees with the API.

    Peak-normalized material asked for -6 LUFS under a -3 dBTP ceiling cannot
    reach the target, and the CLI is the only machine-readable way to learn that
    without switching to the ``--report`` code path.
    """
    import math

    import libsonare
    from libsonare import _cli_mastering, cli

    sample_rate = 48_000
    samples = [
        math.sin(2.0 * math.pi * 440.0 * index / sample_rate) for index in range(sample_rate)
    ]
    patch_mono_decode(monkeypatch, lambda path: (samples, sample_rate))

    args = cli._build_parser().parse_args(
        [
            "mastering-normalize-loudness",
            "input.wav",
            "--target-lufs",
            "-6",
            "--ceiling-dbtp",
            "-3",
            "--json",
        ]
    )
    assert _cli_mastering.cmd_mastering_normalize_loudness(args) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["loudness_target_limited"] is True

    # The same input through the named-processor entry point must agree: these
    # are two views of one computation, not two independent estimates.
    processed = libsonare.mastering_apply_processor(
        "maximizer.loudnessOptimize",
        samples,
        sample_rate=sample_rate,
        params={"targetLufs": -6.0, "ceilingDbtp": -3.0},
    )
    assert processed.loudness_target_limited is True


def test_target_platform_moves_the_loudness_the_assistant_masters_to(capsys, monkeypatch) -> None:
    """The delivery target decides the loudness, and it is reachable from the CLI.

    Read from the rendered output loudness rather than from a flag round-trip:
    with the target unreachable, someone mastering for EBU R128 broadcast
    silently got the -14 LUFS streaming convention -- a 9 dB error under exit 0.
    """
    import math

    from libsonare import _cli_mastering, cli

    sample_rate = 22_050
    samples = [
        0.5 * math.sin(2.0 * math.pi * 440.0 * index / sample_rate) for index in range(sample_rate)
    ]
    patch_mono_decode(monkeypatch, lambda path: (samples, sample_rate))

    def master_to(platform: str | None) -> float:
        argv = ["mastering", "input.wav", "--assistant", "--json"]
        if platform is not None:
            argv += ["--target-platform", platform]
        args = cli._build_parser().parse_args(argv)
        assert _cli_mastering.cmd_mastering(args) == 0
        return json.loads(capsys.readouterr().out)["output_lufs"]

    streaming = master_to(None)
    assert -14.5 < streaming < -13.5
    # Named explicitly, the default target lands in the same place as the
    # omitted one.
    assert master_to("streaming") == pytest.approx(streaming)
    assert -23.5 < master_to("broadcast") < -22.5
    assert -16.5 < master_to("podcast") < -15.5
    assert -9.5 < master_to("club") < -8.5


def test_preset_names_the_base_the_assistant_starts_from(monkeypatch, capsys) -> None:
    """Alongside --assistant, --preset is the suggestion's base, not a second chain."""
    import math

    from libsonare import _cli_mastering, cli

    sample_rate = 22_050
    samples = [
        0.5 * math.sin(2.0 * math.pi * 440.0 * index / sample_rate) for index in range(sample_rate)
    ]
    patch_mono_decode(monkeypatch, lambda path: (samples, sample_rate))

    def explanation(*extra: str) -> list[str]:
        argv = ["mastering", "input.wav", "--assistant", "--explain", "--json", *extra]
        args = cli._build_parser().parse_args(argv)
        assert _cli_mastering.cmd_mastering(args) == 0
        return json.loads(capsys.readouterr().out)["explanation"]

    assert "base preset: streaming" in explanation()
    assert "base preset: classical" in explanation("--preset", "classical")
    # A restoration preset leaves level alone, which the suggestion never does.
    with pytest.raises(Exception, match="vinyl"):
        explanation("--preset", "vinyl")


def test_assistant_controls_are_refused_without_assistant(capsys) -> None:
    """Options that only reach an AssistantConfig field are refused, not dropped."""
    from libsonare import cli

    for option in (
        ["--enable-repair"],
        ["--explain"],
        ["--target-lufs", "-12"],
        ["--ceiling-dbtp", "-2"],
        ["--target-platform", "broadcast"],
        ["--no-streaming-safe"],
        ["--speech-mono-amount", "0.5"],
    ):
        with pytest.raises(SystemExit) as caught:
            cli._build_parser().parse_args(["mastering", "input.wav", "--preset", "pop", *option])
        assert caught.value.code == 2
        assert f"error: {option[0]} requires --assistant" in capsys.readouterr().err


@pytest.mark.parametrize(
    ("option", "value", "key"),
    [
        ("--target-lufs", "-14.0", "targetLufs"),
        ("--ceiling-dbtp", "-1.0", "ceilingDbtp"),
    ],
)
def test_a_loudness_option_carrying_its_default_still_reaches_the_assistant(
    monkeypatch, option, value, key
) -> None:
    """Naming a flag is what makes it explicit, not the number it carries.

    The assistant treats an unsupplied loudness field as free for the delivery
    target to fill, so a caller who pinned the target to the documented default
    silently got the target's own figure instead of the one they asked for.
    """
    import libsonare
    from libsonare import _cli_mastering, cli

    calls: dict[str, object] = {}
    patch_mono_decode(monkeypatch, lambda path: ([0.0], 48_000))
    monkeypatch.setattr(
        libsonare,
        "mastering_assistant_suggest",
        lambda *args, **kwargs: (
            calls.update(params=kwargs["params"])
            or SimpleNamespace(chain_config={}, explanation=[])
        ),
    )
    monkeypatch.setattr(libsonare, "master_audio", lambda *args, **kwargs: _result())

    def suggestion_params(*extra: str) -> dict[str, object]:
        args = cli._build_parser().parse_args(
            ["mastering", "input.wav", "--assistant", "--json", *extra]
        )
        assert _cli_mastering.cmd_mastering(args) == 0
        return dict(calls["params"])  # type: ignore[arg-type]

    assert key not in suggestion_params()
    assert suggestion_params(f"{option}={value}")[key] == float(value)
    # The separate-token spelling is the same request.
    assert suggestion_params(option, value)[key] == float(value)


@pytest.mark.parametrize(
    ("option", "value"),
    [
        ("--target-lufs", "-14.0"),
        ("--ceiling-dbtp", "-1.0"),
    ],
)
def test_a_loudness_target_carrying_its_default_is_refused_without_assistant(
    capsys, option, value
) -> None:
    """The loudness targets reach only the assistant, so a preset chain refuses them."""
    from libsonare import cli

    with pytest.raises(SystemExit) as caught:
        cli._build_parser().parse_args(
            ["mastering", "input.wav", "--preset", "pop", f"{option}={value}"]
        )
    assert caught.value.code == 2
    assert f"{option} requires --assistant" in capsys.readouterr().err


def test_mastering_needs_a_chain_and_names_the_loudness_command(capsys) -> None:
    """With no chain selector the command is refused rather than normalising loudness."""
    from libsonare import cli

    for extra in ([], ["--params", "loudness.targetLufs=-18"]):
        with pytest.raises(SystemExit) as caught:
            cli._build_parser().parse_args(["mastering", "input.wav", *extra])
        assert caught.value.code == 2
        assert "mastering-normalize-loudness" in capsys.readouterr().err
    # The oversampling factor is a chain parameter on mastering, not a flag.
    with pytest.raises(SystemExit):
        cli._build_parser().parse_args(
            ["mastering", "input.wav", "--assistant", "--true-peak-oversample", "8"]
        )


@pytest.mark.parametrize(
    "option",
    ["--target-platform=streaming", "--speech-mono-amount=1.0"],
)
def test_an_assistant_option_carrying_its_default_is_refused_without_assistant(
    capsys, option
) -> None:
    """The refusal follows the flag, not the value it happens to carry."""
    from libsonare import cli

    with pytest.raises(SystemExit) as caught:
        cli._build_parser().parse_args(["mastering", "input.wav", "--preset", "pop", option])
    assert caught.value.code == 2
    assert "requires --assistant" in capsys.readouterr().err


def test_presence_gated_mastering_options_match_the_native_handler() -> None:
    """Both CLIs answer "was this spelled?" over the same option names.

    The two lists are declared separately, so one surface gaining a flag is
    invisible to the other; this is the only place the pair is compared.
    """
    from libsonare import _cli_mastering

    native = Path(__file__).resolve().parents[3] / "tools" / "cli" / "sonare_cli_mastering.cpp"
    text = native.read_text(encoding="utf-8")
    for name in (
        "enable-repair",
        "explain",
        "target-platform",
        "no-streaming-safe",
        "speech-mono-amount",
        "target-lufs",
        "ceiling-dbtp",
    ):
        assert f'args.has("{name}")' in text or f'"{name}",' in text

    shortcuts = set(_cli_mastering._EQ_SHORTCUT_NAMES)
    assert shortcuts
    for name in shortcuts:
        assert f'"{name}"' in text


def test_unknown_target_platform_is_rejected_against_the_delivery_table() -> None:
    """The accepted names are the delivery-target table's rows, not a free string."""
    from libsonare import cli

    with pytest.raises(SystemExit) as raised:
        cli._build_parser().parse_args(
            ["mastering", "input.wav", "--assistant", "--target-platform", "bogus"]
        )
    assert raised.value.code == 2


def test_no_streaming_safe_reaches_the_suggester(capsys, monkeypatch) -> None:
    """prefer_streaming_safe defaults to true, so the reachable control turns it off."""
    import math
    import random

    from libsonare import _cli_mastering, cli

    # The material has to carry the defect the flag governs. Repair stages are
    # selected from measurement, so on a clean tone neither setting says anything
    # about repair, the two explanations come out identical, and the assertions
    # below stop distinguishing the settings while still reading as a test of them.
    sample_rate = 22_050
    rng = random.Random(7)
    samples = [
        0.5 * math.sin(2.0 * math.pi * 440.0 * index / sample_rate) + rng.uniform(-0.3, 0.3)
        for index in range(sample_rate * 3)
    ]
    patch_mono_decode(monkeypatch, lambda path: (samples, sample_rate))

    def explanation(*extra: str) -> list[str]:
        args = cli._build_parser().parse_args(
            [
                "mastering",
                "input.wav",
                "--assistant",
                "--enable-repair",
                "--explain",
                "--json",
                *extra,
            ]
        )
        assert _cli_mastering.cmd_mastering(args) == 0
        return json.loads(capsys.readouterr().out)["explanation"]

    # The same substring the C++ CLI test pins, so the two surfaces assert on one
    # phrase rather than each on its own paraphrase of it. Both settings mention
    # the noise floor, so only the clause naming the preference separates them.
    safe_lines = explanation()
    assert any("streaming-safe was asked for" in line for line in safe_lines)

    open_lines = explanation("--no-streaming-safe")
    assert not any("streaming-safe was asked for" in line for line in open_lines)
    # Both settings select denoise; the preference decides the noise estimator,
    # which this output does not carry. Asserting its absence here would say the
    # stage had been dropped, which is what it used to do and no longer does.
    assert any("the noise floor is loud under the programme" in line for line in open_lines)


@pytest.mark.parametrize(
    ("option", "first_rejected", "last_accepted"),
    [
        ("type", 10, 9),
        ("coeff-mode", 2, 1),
        ("placement", 5, 4),
        ("phase-mode", 4, 3),
        ("resolution", 6, 5),
    ],
)
def test_eq_refuses_an_enumerator_index_outside_its_enumeration(
    option, first_rejected, last_accepted, capsys
) -> None:
    """An index past the enumeration maps to the first enumerator without this.

    The switches behind these options answer an unrecognized index with their
    first enumerator, so a mistyped `--type 999` applied a peak filter and exited
    0 with a normal JSON payload.
    """
    from libsonare import cli

    parser = cli._build_parser()
    for value in (first_rejected, -1):
        with pytest.raises(SystemExit) as exc_info:
            parser.parse_args(["eq", "input.wav", f"--{option}={value}", "--json"])
        assert exc_info.value.code == 2
        assert f"argument --{option}: must be one of " in capsys.readouterr().err

    # The last enumerator is inside the domain, so it must get past validation
    # rather than be refused by an off-by-one bound.
    args = parser.parse_args(["eq", "input.wav", f"--{option}", str(last_accepted), "--json"])
    assert getattr(args, option.replace("-", "_")) == last_accepted


def test_eq_names_and_refuses_a_params_key_the_processor_does_not_read(monkeypatch, capsys) -> None:
    """A supplied key no config builder probes took no effect at all."""
    from libsonare import _cli_mastering, cli

    patch_mono_decode(monkeypatch, lambda path: ([0.0] * 2048, 48_000))
    with pytest.raises(SystemExit) as exc_info:
        cli._build_parser().parse_args(
            ["eq", "input.wav", "--params", "band0.bogusKey=42", "--json"]
        )
    assert exc_info.value.code == 2
    assert "unknown --params key for eq.equalizer: band0.bogusKey" in capsys.readouterr().err

    # A key the processor does read still runs.
    good = cli._build_parser().parse_args(
        ["eq", "input.wav", "--params", "band0.gainDb=3", "--json"]
    )
    assert _cli_mastering.cmd_eq(good) == 0


def _suggest(monkeypatch, capsys, *extra: str) -> dict[str, object]:
    import math

    from libsonare import _cli_mastering, cli

    sample_rate = 22_050
    samples = [
        0.5 * math.sin(2.0 * math.pi * 440.0 * index / sample_rate) for index in range(sample_rate)
    ]
    monkeypatch.setattr(cli, "_load_audio", lambda path: (samples, sample_rate))
    args = cli._build_parser().parse_args(["mastering-suggest", "input.wav", *extra])
    assert _cli_mastering.cmd_mastering_suggest(args) == 0
    return json.loads(capsys.readouterr().out)


@pytest.mark.parametrize(
    "extra",
    [
        ("--preset", "classical"),
        ("--params", "preset=classical"),
    ],
)
def test_mastering_suggest_takes_the_preset_by_name(monkeypatch, capsys, extra) -> None:
    """As an option or a --params key, the preset is a name on the command line."""
    assert "base preset: classical" in _suggest(monkeypatch, capsys, *extra)["explanation"]


@pytest.mark.parametrize(
    "extra",
    [
        ("--target-platform", "broadcast"),
        ("--params", "targetPlatform=broadcast"),
    ],
)
def test_mastering_suggest_takes_the_delivery_target_by_name(monkeypatch, capsys, extra) -> None:
    payload = _suggest(monkeypatch, capsys, *extra)
    assert payload["chain_config"]["params"]["loudness.targetLufs"] == -23  # type: ignore[index]


def test_mastering_suggest_named_options_reach_the_assistant(monkeypatch, capsys) -> None:
    """The options `mastering --assistant` declares mean the same thing here."""
    payload = _suggest(monkeypatch, capsys, "--target-lufs", "-12", "--ceiling-dbtp", "-2")
    params = payload["chain_config"]["params"]  # type: ignore[index]
    assert params["loudness.targetLufs"] == -12
    assert params["loudness.ceilingDbtp"] == -2


@pytest.mark.parametrize(
    ("extra", "match"),
    [
        (("--preset", "classical", "--params", "preset=classical"), "set the same value"),
        (("--target-lufs", "-12", "--params", "targetLufs=-12"), "set the same value"),
        (("--target-platform", "club", "--params", "targetPlatform=club"), "set the same value"),
        (("--params", "novalue"), "must be a comma-separated list of key=value entries"),
        (("--params", "notAKey=1"), "unknown key notAKey (expected "),
    ],
)
def test_mastering_suggest_refuses_a_contradiction_before_the_read(
    monkeypatch, capsys, extra, match
) -> None:
    with pytest.raises(SystemExit) as exc_info:
        _suggest(monkeypatch, capsys, *extra)
    assert exc_info.value.code == 2
    assert match in capsys.readouterr().err


@pytest.mark.parametrize("options", [(), ("--target-platform", "club")])
def test_mastering_suggest_refuses_an_unknown_params_key_from_argv(
    monkeypatch, capsys, options
) -> None:
    from libsonare import cli

    monkeypatch.setattr(cli, "_load_audio", lambda path: ([0.0] * 2048, 48_000))
    monkeypatch.setattr(
        "sys.argv",
        ["sonare", "mastering-suggest", "input.wav", *options, "--params", "target_platform=club"],
    )
    with pytest.raises(SystemExit) as exc_info:
        cli.main()
    assert exc_info.value.code == 2
    assert (
        "unknown key target_platform (expected ceilingDbtp, enableRepair, "
        "preferStreamingSafe, preset, speechMonoAmount, targetLufs, targetPlatform)"
    ) in capsys.readouterr().err


_SUGGEST_PRESETS = (
    "preset must be one of pop, edm, acoustic, hipHop, aiMusic, speech, streaming, youtube, "
    "broadcast, podcast, audiobook, cinema, jpop, ambient, lofi, classical, drumAndBass, "
    "techno, metal, trap, rnb, jazz, kpop, trance, gameOst"
)
_SUGGEST_PLATFORMS = (
    "targetPlatform must be one of streaming, youtube, broadcast, podcast, audiobook, "
    "cinema, club, cd"
)


@pytest.mark.parametrize(
    ("params", "match"),
    [
        ("preset=3", _SUGGEST_PRESETS),
        ("preset=vinyl", _SUGGEST_PRESETS),
        ("targetPlatform=2", _SUGGEST_PLATFORMS),
        ("targetPlatform=typo", _SUGGEST_PLATFORMS),
        ("targetLufs=invalid", "invalid numeric value for parameter 'targetLufs'"),
        ("ceilingDbtp=1e100", "invalid numeric value for parameter 'ceilingDbtp'"),
        ("speechMonoAmount=nan", "invalid numeric value for parameter 'speechMonoAmount'"),
    ],
)
def test_mastering_suggest_refuses_invalid_param_values_before_read(
    monkeypatch, capsys, tmp_path, params, match
) -> None:
    from libsonare import cli

    monkeypatch.setattr(
        "sys.argv",
        ["sonare", "mastering-suggest", str(tmp_path / "absent.wav"), "--params", params],
    )
    with pytest.raises(SystemExit) as exc_info:
        cli.main()
    assert exc_info.value.code == 2
    assert capsys.readouterr().err.rstrip().endswith(match)
