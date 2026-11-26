"""Cross-binding public-input transaction corpus."""

from __future__ import annotations

import builtins
import dataclasses
import json
import math
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

import libsonare
from libsonare import (
    EngineMarker,
    EngineMarkerInput,
    MidiCcBinding,
    Project,
    ProjectMidiCcBindingKind,
    RealtimeEngine,
    SonareError,
    SonareValueError,
    StreamingRetune,
    mix_stereo,
)
from libsonare._field_spelling import surface_spelling

_CORPUS = json.loads(
    (Path(__file__).resolve().parents[3] / "tests/conformance/public_input_corpus.json").read_text()
)


def _marker_value(raw: dict[str, Any]) -> EngineMarker:
    marker_id = 0xFFFFFFFF if raw["id"] == "uint32_max" else raw["id"]
    ppq = {"nan": math.nan, "inf": math.inf}.get(raw["ppq"], raw["ppq"])
    return EngineMarker(ppq, id=marker_id, name=raw["name"])


def _snapshot(engine: RealtimeEngine) -> bytes:
    values = []
    for marker in engine.markers():
        values.append(
            [
                marker.id,
                marker.ppq,
                marker.name,
                marker.kind,
                marker.key_fifths,
                marker.key_minor,
            ]
        )
    return json.dumps(values, ensure_ascii=False, separators=(",", ":")).encode()


def _mix_option_value(raw: object) -> object:
    specials = {"nan": math.nan, "inf": math.inf, "neg_inf": -math.inf}
    return specials[raw] if isinstance(raw, str) and raw in specials else raw


@pytest.mark.parametrize(
    "case", _CORPUS["marker_transaction"]["cases"], ids=lambda case: case["id"]
)
def test_shared_marker_transaction_corpus(case: dict[str, Any]) -> None:
    initial = [_marker_value(raw) for raw in _CORPUS["marker_transaction"]["initial"]]
    candidate = [_marker_value(raw) for raw in case["markers"]]
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_markers(initial)
        before = _snapshot(engine)
        if case["accepted"]:
            engine.set_markers(candidate)
            stored = engine.markers()
            assert len(stored) == len(candidate)
            for actual, expected in zip(stored, candidate, strict=True):
                assert (actual.id, actual.ppq, actual.name) == (
                    expected.id,
                    expected.ppq,
                    expected.name,
                )
        else:
            with pytest.raises((ValueError, OverflowError, TypeError, RuntimeError)):
                engine.set_markers(candidate)
            assert _snapshot(engine) == before


@pytest.mark.parametrize("case", _CORPUS["mix_options"]["cases"], ids=lambda case: case["id"])
def test_shared_mix_option_corpus_rejects_and_recovers(case: dict[str, Any]) -> None:
    value = _mix_option_value(case["value"])
    options = (
        {"pan": [0.0], "pan_mode": value}
        if case["field"] == "pan_mode"
        else {case["field"]: [value]}
    )
    strips = [([0.25, -0.25], [0.25, -0.25])]

    with pytest.raises((ValueError, OverflowError, TypeError, RuntimeError)):
        mix_stereo(strips, sample_rate=48000, **options)

    recovered = mix_stereo(strips, sample_rate=48000)
    assert len(recovered.left) == 2
    assert len(recovered.right) == 2


def _python_field(case: dict[str, Any]) -> str:
    """The corpus field is the Node spelling; the Python keyword is its snake case."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", case["field"]).lower()


def _midi_cc_binding(case: dict[str, Any]) -> MidiCcBinding:
    return MidiCcBinding(**{"cc_number": 7, "param_id": 1, _python_field(case): case["value"]})


_MIDI_CC_CASES = _CORPUS["midi_cc_binding"]["cases"]


def test_midi_cc_binding_corpus_has_both_outcomes() -> None:
    assert len(_MIDI_CC_CASES) >= 11
    assert {case["accepted"] for case in _MIDI_CC_CASES} == {True, False}


# Each byte field with the binding kind that reads it and an in-range value the
# library accepts there (a 14-bit LSB controller is the MSB's pair, 32..63).
_MIDI_CC_BYTE_FIELDS: dict[str, tuple[ProjectMidiCcBindingKind, int]] = {
    "cc_number": ("controlChange7", 127),
    "cc_lsb_number": ("controlChange14", 33),
    "selector_msb": ("rpn", 127),
    "selector_lsb": ("nrpn", 127),
}


def _binding_field(case: dict[str, Any], subject: str = "binding") -> str:
    return f"{subject}.{_python_field(case)}"


@pytest.mark.parametrize("field", sorted(_MIDI_CC_BYTE_FIELDS))
@pytest.mark.parametrize("value", [128, 255])
def test_bind_midi_cc_refuses_a_byte_above_the_midi_data_range(field: str, value: int) -> None:
    # The C fields are uint8, so 128..255 fit the struct and only the MIDI domain refuses them.
    kind, accepted = _MIDI_CC_BYTE_FIELDS[field]
    binding = MidiCcBinding(param_id=1, kind=kind, **{"cc_number": 1, field: value})
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        with pytest.raises(
            SonareValueError,
            match=(
                rf"^RealtimeEngine\.bind_midi_cc: binding\.{field} "
                r"must be an integer in \[0, 127\]$"
            ),
        ):
            engine.bind_midi_cc(binding)
        assert len(engine.midi_cc_bindings()) == 0
        engine.bind_midi_cc(
            MidiCcBinding(param_id=1, kind=kind, **{"cc_number": 1, field: accepted})
        )
        assert len(engine.midi_cc_bindings()) == 1


@pytest.mark.parametrize("case", _MIDI_CC_CASES, ids=lambda case: case["id"])
def test_bind_midi_cc_byte_fields_follow_the_corpus(case: dict[str, Any]) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        if case["accepted"]:
            engine.bind_midi_cc(_midi_cc_binding(case))
            assert len(engine.midi_cc_bindings()) == 1
        else:
            with pytest.raises(
                SonareValueError,
                match=rf"^\S+: {re.escape(_binding_field(case))} must be an integer in \[0, ",
            ):
                engine.bind_midi_cc(_midi_cc_binding(case))
            assert len(engine.midi_cc_bindings()) == 0


@pytest.mark.parametrize("case", _MIDI_CC_CASES, ids=lambda case: case["id"])
def test_midi_cc_to_breakpoint_byte_fields_follow_the_corpus(case: dict[str, Any]) -> None:
    event = libsonare.midi_cc(0.0, 0, 0, 7, 64)
    if case["accepted"]:
        libsonare.midi_cc_to_breakpoint([_midi_cc_binding(case)], event)
    else:
        field = _binding_field(case, "bindings[0]")
        with pytest.raises(
            SonareValueError,
            match=rf"^\S+: {re.escape(field)} must be an integer in \[0, ",
        ):
            libsonare.midi_cc_to_breakpoint([_midi_cc_binding(case)], event)


@pytest.mark.parametrize("case", _MIDI_CC_CASES, ids=lambda case: case["id"])
def test_midi_param_to_cc_byte_fields_follow_the_corpus(case: dict[str, Any]) -> None:
    if case["accepted"]:
        libsonare.midi_param_to_cc([_midi_cc_binding(case)], 1, 0.5, 0)
    else:
        field = _binding_field(case, "bindings[0]")
        with pytest.raises(
            SonareValueError,
            match=rf"^\S+: {re.escape(field)} must be an integer in \[0, ",
        ):
            libsonare.midi_param_to_cc([_midi_cc_binding(case)], 1, 0.5, 0)


_STREAMING = _CORPUS["streaming_contract"]
# The facade folds the band counts into the matrix shapes and the flags into None.
_STREAM_FOLDED = {"n_mels", "n_chroma", "feature_flags"}
_STREAM_CHORD_FIELDS = {"chord_root", "chord_quality", "chord_confidence"}
_STREAM_FEATURE_FIELDS = {
    "mel": {"mel"},
    "chroma": {"chroma"} | _STREAM_CHORD_FIELDS,
    "onset": {"onset_strength"},
    "spectral": {"spectral_centroid", "spectral_flatness"},
}


def _small_analyzer(**options: Any) -> Any:
    from libsonare import StreamAnalyzer

    return StreamAnalyzer(sample_rate=8000, n_fft=256, hop_length=64, n_mels=8, **options)


def _stream_tone(n_samples: int) -> list[float]:
    return [0.5 * math.sin(2.0 * math.pi * 440.0 * i / 8000) for i in range(n_samples)]


def test_stream_feature_flags_follow_the_corpus() -> None:
    from libsonare import streaming

    bits = {
        "mel": streaming._FEATURE_MEL,
        "chroma": streaming._FEATURE_CHROMA,
        "onset": streaming._FEATURE_ONSET,
        "spectral": streaming._FEATURE_SPECTRAL,
    }
    assert bits == _STREAMING["feature_flags"]


def test_stream_frames_fields_follow_the_corpus() -> None:
    import dataclasses

    from libsonare import StreamFrames

    fields = {field.name for field in dataclasses.fields(StreamFrames)}
    expected = set(_STREAMING["frame_fields"]) - _STREAM_FOLDED
    assert fields - _STREAM_CHORD_FIELDS == expected


def test_stream_output_format_is_the_float_default_and_unreachable() -> None:
    import ctypes
    import inspect

    from libsonare import StreamAnalyzer
    from libsonare._ffi_types_streaming import SonareStreamConfig
    from libsonare._runtime import _get_lib

    lib = _get_lib()
    raw = SonareStreamConfig()
    assert raw.output_format == _STREAMING["output_format_default"]
    # The analyzer never offers the generic output format, so a legacy one cannot be asked for.
    assert "output_format" not in inspect.signature(StreamAnalyzer).parameters
    for legacy in _STREAMING["legacy_output_formats_rejected"]:
        raw.sample_rate = 8000
        raw.output_format = legacy
        handle = ctypes.c_void_p()
        assert lib.sonare_stream_analyzer_create(ctypes.byref(raw), ctypes.byref(handle)) != 0
        assert not handle.value


@pytest.mark.parametrize("feature", sorted(_STREAMING["feature_flags"]))
def test_stream_disabled_feature_reads_as_none(feature: str) -> None:
    assert _STREAMING["disabled_feature_shape"] == "empty"
    switches = {f"compute_{name}": name != feature for name in _STREAM_FEATURE_FIELDS}
    with _small_analyzer(**switches) as analyzer:
        analyzer.process(_stream_tone(4096))
        frames = analyzer.read_frames(1024)
    assert frames.n_frames > 0
    for name, fields in _STREAM_FEATURE_FIELDS.items():
        for field in fields:
            value = getattr(frames, field)
            if name == feature:
                assert value is None, field
            else:
                assert value is not None and value.shape[-1] == frames.n_frames, field


def test_stream_overflow_drops_the_newest_frames() -> None:
    import numpy as np

    assert _STREAMING["overflow_policy"] == "drop_newest"
    tone = _stream_tone(4096)
    with _small_analyzer() as unbounded:
        unbounded.process(tone)
        reference = unbounded.read_frames(1024)
    cap = 3
    with _small_analyzer(max_pending_frames=cap) as bounded:
        bounded.process(tone)
        stats = bounded.stats()
        kept = bounded.read_frames(1024)
    assert reference.n_frames > cap
    assert stats.dropped_output_frames == reference.n_frames - cap
    assert kept.n_frames == cap
    # The retained frames are the oldest ones, sample for sample.
    np.testing.assert_array_equal(kept.timestamps_sec, reference.timestamps_sec[:cap])
    np.testing.assert_array_equal(kept.rms_energy, reference.rms_energy[:cap])


_MARKER_ID_CASES = json.loads(
    (
        Path(__file__).resolve().parents[3] / "tests/conformance/marker_id_allocation.json"
    ).read_text()
)["cases"]


def _marker_entry(raw: dict[str, Any]) -> EngineMarkerInput:
    return EngineMarkerInput(raw["ppq"], id=raw.get("id"))


@pytest.mark.parametrize("case", _MARKER_ID_CASES, ids=lambda case: case["name"])
def test_marker_id_allocation_follows_the_corpus(case: dict[str, Any]) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        for index, step in enumerate(case["steps"]):
            entries = [_marker_entry(raw) for raw in step["set"]]
            if step.get("refused"):
                with pytest.raises(SonareValueError):
                    engine.set_markers(entries)
            else:
                stored = engine.set_markers(entries)
                assert [marker.id for marker in stored] == step["ids"], index
                assert [marker.ppq for marker in stored] == [raw["ppq"] for raw in step["set"]]
            assert [marker.id for marker in engine.markers()] == step["stored"], index


def test_a_marker_takes_ppq_first_and_every_other_field_by_keyword() -> None:
    with pytest.raises(TypeError):
        EngineMarkerInput(1, 0.0)  # type: ignore[call-arg, arg-type]
    assert EngineMarkerInput(480.0).id is None


def test_a_stored_marker_carries_its_id_and_an_input_may_omit_it() -> None:
    with pytest.raises(TypeError):
        EngineMarker(480.0)  # type: ignore[call-arg]
    assert {f.name for f in dataclasses.fields(EngineMarker)} == {
        f.name for f in dataclasses.fields(EngineMarkerInput)
    }


def test_markers_read_back_from_the_engine_set_again_keep_their_ids() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        stored = engine.set_markers(
            [
                EngineMarkerInput(0.0, id=7, name="intro"),
                EngineMarkerInput(480.0),
                EngineMarker(960.0, id=3),
            ]
        )
        assert [marker.id for marker in stored] == [7, 1, 3]
        assert engine.set_markers(engine.markers()) == engine.markers()
        assert [marker.id for marker in engine.markers()] == [7, 1, 3]


def test_project_markers_keep_explicit_ids_across_a_round_trip() -> None:
    project = Project()
    ids = [
        project.set_marker(0.0, marker_id=5, name="verse"),
        project.set_marker(480.0),
        project.set_marker(960.0, marker_id=9, kind="cuePoint"),
    ]
    before = project.markers()
    assert [marker.id for marker in before] == ids
    restored = Project.from_json(project.to_json())
    assert restored.markers() == before
    assert restored.set_marker(1440.0) > max(ids)


_GM_REFUSALS: dict[str, type[Exception]] = {"range": SonareValueError, "type": TypeError}


@pytest.mark.parametrize("case", _CORPUS["gm_lookup"]["cases"], ids=lambda case: case["id"])
def test_gm_lookup_follows_the_corpus(case: dict[str, Any]) -> None:
    lookup = getattr(libsonare, case["lookup"])
    if "refused" in case:
        refusal = f"{case['key']} {case['predicate']}"
        with pytest.raises(
            _GM_REFUSALS[case["refused"]],
            match=rf"^{case['lookup']}: {re.escape(refusal)}$",
        ):
            lookup(**case["args"])
    elif case.get("absent"):
        assert lookup(**case["args"]) is None
    elif "value" in case:
        assert lookup(**case["args"]) == case["value"]
    else:
        assert lookup(**case["args"]) == case["name"]


_NUMBER_SPLIT = _CORPUS["number_reader_split"]


def _corpus_number(raw: object) -> object:
    if isinstance(raw, dict) and "$number" in raw:
        return {"nan": math.nan, "inf": math.inf, "neg_inf": -math.inf}[raw["$number"]]
    return raw


@pytest.mark.parametrize("case", _NUMBER_SPLIT["cases"], ids=lambda case: case["id"])
def test_number_reader_split_follows_the_corpus(case: dict[str, Any]) -> None:
    fn = getattr(libsonare, _NUMBER_SPLIT["fn"])
    args = {**_NUMBER_SPLIT["base"], case["key"]: _corpus_number(case["value"])}
    if case.get("accepted"):
        fn(**args)
        return
    refusal = f"{case['key']} {case['predicate']}"
    with pytest.raises(
        _GM_REFUSALS[case["refused"]],
        match=rf"^{_NUMBER_SPLIT['fn']}: {re.escape(refusal)}$",
    ):
        fn(**args)


_RETUNE = _CORPUS["streaming_retune"]
_RETUNE_ENTRIES = {"constructor": "StreamingRetune", "set_config": "StreamingRetune.set_config"}


@pytest.mark.parametrize("entry", _RETUNE["entries"])
@pytest.mark.parametrize("case", _RETUNE["cases"], ids=lambda case: case["id"])
def test_streaming_retune_controls_follow_the_corpus(case: dict[str, Any], entry: str) -> None:
    base = dict(_RETUNE["base"])
    value = _corpus_number(case["value"])
    refusal = None
    if "refused" in case:
        text = f"{_RETUNE_ENTRIES[entry]}: {case['key']} {case['predicate']}"
        refusal = pytest.raises(_GM_REFUSALS[case["refused"]], match=rf"^{re.escape(text)}$")
    if entry == "constructor":
        if refusal is None:
            with StreamingRetune(**{**base, case["key"]: value}) as retune:
                assert retune.config()[case["key"]] == value
        else:
            with refusal:
                StreamingRetune(**{**base, case["key"]: value})
        return
    with StreamingRetune(**base) as retune:
        if refusal is None:
            retune.set_config(**{case["key"]: value})
            assert retune.config()[case["key"]] == value
        else:
            with refusal:
                retune.set_config(**{case["key"]: value})
            config = retune.config()
            assert (config["pitch_semitones"], config["mix"]) == (
                base["pitch_semitones"],
                base["mix"],
            )


_TRIM = _CORPUS["trim_detector"]
_TRIM_SIGNAL = [0.0] * 480 + [0.5] * 480 + [0.0] * 480


def _trim_call(entry: str, **kwargs: object) -> object:
    fn = getattr(libsonare, entry)
    channels = (_TRIM_SIGNAL, _TRIM_SIGNAL) if entry.endswith("_stereo") else (_TRIM_SIGNAL,)
    return fn(*channels, _TRIM["sample_rate"], **kwargs)


@pytest.mark.parametrize("entry", _TRIM["entries"])
@pytest.mark.parametrize("case", _TRIM["cases"], ids=lambda case: case["id"])
def test_trim_detector_controls_follow_the_corpus(case: dict[str, Any], entry: str) -> None:
    kwargs = {"mode": _TRIM["mode"][case["key"]], case["key"]: _corpus_number(case["value"])}
    if case.get("accepted"):
        _trim_call(entry, **kwargs)
        return
    text = f"{entry}: {case['key']} {case['predicate']}"
    with pytest.raises(_GM_REFUSALS[case["refused"]], match=rf"^{re.escape(text)}$"):
        _trim_call(entry, **kwargs)


_ROOM = _CORPUS["room_morph_stereo"]
_ROOM_IMPULSE = [1.0] + [0.0] * 1999


@pytest.mark.parametrize("case", _ROOM["cases"], ids=lambda case: case["id"])
def test_room_morph_stereo_spacing_follows_the_corpus(case: dict[str, Any]) -> None:
    fn = getattr(libsonare, _ROOM["fn"])
    kwargs = {**_ROOM["room"], **case.get("room", {}), case["key"]: _corpus_number(case["value"])}
    if case.get("accepted"):
        result = fn(_ROOM_IMPULSE, _ROOM_IMPULSE, **kwargs)
        assert len(result.left) == len(result.right) > 0
        return
    text = re.escape(f"{_ROOM['fn']}: {case['key']} {case['predicate']}")
    # A relation refusal goes on to name the operands it relates, each in the surface's spelling.
    pattern = rf"^{text} for .+$" if case.get("relation") else rf"^{text}$"
    with pytest.raises(_GM_REFUSALS[case["refused"]], match=pattern):
        fn(_ROOM_IMPULSE, _ROOM_IMPULSE, **kwargs)


_SCENE = _CORPUS["scene_insert_refusal"]


@pytest.mark.parametrize("case", _SCENE["cases"], ids=lambda case: case["id"])
def test_scene_insert_refusal_follows_the_corpus(case: dict[str, Any]) -> None:
    scene = json.loads(json.dumps(_SCENE["scene"]))
    scene["strips"][0]["inserts"][0]["params"][case["param"]] = case["value"]
    text = json.dumps(scene)
    if case.get("accepted"):
        libsonare.Mixer.from_json(text, **_SCENE["options"]).close()
        return
    with pytest.raises(SonareError) as refused:
        libsonare.Mixer.from_json(text, **_SCENE["options"])
    assert refused.value.code_name == case["code_name"]
    assert not isinstance(refused.value, SonareValueError)
    assert str(refused.value) == f"[{int(refused.value.code)}] Mixer.from_json: {case['message']}"


_BUS_ROLE = _CORPUS["mixer_bus_role"]


@pytest.mark.parametrize("case", _BUS_ROLE["cases"], ids=lambda case: case["id"])
def test_mixer_bus_role_follows_the_corpus(case: dict[str, Any]) -> None:
    mixer = libsonare.Mixer(sample_rate=48000)
    args = ("corpus",) if case["role"] is None else ("corpus", case["role"])
    if "stored" not in case:
        with pytest.raises(SonareValueError, match=re.escape(_BUS_ROLE["message"])):
            mixer.add_bus(*args)
        return
    mixer.add_bus(*args)
    buses = {bus["id"]: bus for bus in json.loads(mixer.to_json())["buses"]}
    assert buses["corpus"]["role"] == case["stored"]


_PROJECT_ENUMS = _CORPUS["project_document_enum_tokens"]


@pytest.mark.parametrize("case", _PROJECT_ENUMS["cases"], ids=lambda case: case["id"])
def test_project_document_enums_follow_the_corpus(case: dict[str, Any]) -> None:
    text = json.dumps(case["document"])
    if case.get("accepted"):
        libsonare.Project.from_json(text).close()
        return
    with pytest.raises(SonareError) as refused:
        libsonare.Project.from_json(text)
    assert refused.value.code_name == _PROJECT_ENUMS["code_name"]
    assert not isinstance(refused.value, SonareValueError)
    assert case["message"] in str(refused.value)


_MEL = _CORPUS["mel_family"]


@pytest.mark.parametrize("entry", _MEL["entries"])
@pytest.mark.parametrize("case", _MEL["cases"], ids=lambda case: case["id"])
def test_mel_family_follows_the_corpus(case: dict[str, Any], entry: str) -> None:
    if entry not in case.get("only", _MEL["entries"]):
        pytest.skip(f"{case['id']} is not a {entry} case")
    args = {**_MEL["base"][entry], **case["args"]}
    kwargs = {surface_spelling(entry, k): _corpus_number(v) for k, v in args.items()}
    fn = getattr(libsonare, entry)
    if case.get("accepted"):
        result = fn(**kwargs)
        expect = case["expect"][entry]
        if "shape" in expect:
            assert list(result.shape) == expect["shape"]
        if "values" in expect:
            assert result.tolist() == expect["values"]
        return
    names = {k: surface_spelling(entry, k) for k in _MEL["base"][entry] | case["args"]}
    text = f"{entry}: {case['predicate'].format(**names)}"
    with pytest.raises(_GM_REFUSALS[case["refused"]], match=rf"^{re.escape(text)}$"):
        fn(**kwargs)


_DECLIP_LINK = _CORPUS["declip_link_channels"]


@pytest.mark.parametrize("case", _DECLIP_LINK["cases"], ids=lambda case: case["id"])
def test_declip_link_channels_follow_the_corpus(case: dict[str, Any]) -> None:
    n = _DECLIP_LINK["samples"]
    signal = [0.5 * math.sin(2 * math.pi * 440 * i / _DECLIP_LINK["sample_rate"]) for i in range(n)]
    fn = getattr(libsonare, case["entry"])
    planes = [signal] if case["entry"] == "mastering_repair_declip" else [signal, signal]
    call = lambda: fn(  # noqa: E731
        *planes, _DECLIP_LINK["sample_rate"], link_channels=case["link_channels"]
    )
    if case["accepted"]:
        call()
        return
    with pytest.raises(TypeError) as refused:
        call()
    assert type(refused.value).__name__ == _DECLIP_LINK["error"]["python"]


_ONE_SHOT_RESULTS = json.loads(
    (
        Path(__file__).resolve().parents[3] / "tests/conformance/public_result_corpus.json"
    ).read_text()
)["one_shot_results"]
_ONE_SHOT_SIGNAL_PATH = (
    Path(__file__).resolve().parents[3] / "tests/conformance/public_result_signals.json"
)
_ONE_SHOT_SIGNALS: dict[str, list[float]] = (
    json.loads(_ONE_SHOT_SIGNAL_PATH.read_text()) if _ONE_SHOT_SIGNAL_PATH.exists() else {}
)


def _one_shot_fixture(name: str) -> list[float]:
    if name == "silence_excerpt":
        return [0.0] * 8192
    if name == "impulsive":
        return [1.0 if i == 64 or 256 <= i < 260 else (i % 23 - 11) / 128 for i in range(512)]
    if name == "historical_excerpt":
        return _one_shot_fixture("historical_left")[:8192]
    if name in _ONE_SHOT_SIGNALS:
        return [float(value) for value in _ONE_SHOT_SIGNALS[name]]
    frames = _ONE_SHOT_RESULTS["frames"]
    if name in {"empty", "tagged_wav"}:
        return []
    if name in {"silence", "profile_silence"}:
        return [0.0] * frames
    if name not in {"tone", "clipped", "gapped"}:
        raise AssertionError(f"unknown one-shot fixture: {name}")

    def tone_sample(i: int) -> float:
        return ((i % 109 - 54) + (i % 47 - 23) * 0.5) / (512 if i < frames // 2 else 256)

    return [
        0.0
        if name == "gapped" and not frames // 2 <= i < frames // 2 + 256
        else 1.0
        if name == "clipped" and i % 109 < 8
        else tone_sample(i)
        for i in range(frames)
    ]


def _one_shot_signal(name: str, fallback: list[float]) -> list[float]:
    return _one_shot_fixture(name) if name in _ONE_SHOT_SIGNALS else fallback


def _interleave(left: list[float], right: list[float]) -> list[float]:
    if len(left) != len(right):
        raise AssertionError(f"stereo fixture lengths differ: {len(left)} and {len(right)}")
    return [value for pair in zip(left, right, strict=True) for value in pair]


def _one_shot_call(
    entry: str,
    samples: list[float],
    fixture: str | None = None,
    midi_kwargs: dict[str, Any] | None = None,
    case: dict[str, Any] | None = None,
) -> Any:
    if not entry.startswith("sonare_"):
        raise AssertionError(f"one-shot corpus entry is not a C symbol: {entry}")
    if entry in {
        "sonare_midi_cc_learn",
        "sonare_midi_cc_to_breakpoint",
        "sonare_midi_route_events",
        "sonare_midi_param_to_cc",
    }:
        events = [libsonare.midi_cc(0.5, 1, 2, 74, 17), libsonare.midi_cc(1, 1, 2, 74, 101)]
        if entry == "sonare_midi_cc_learn":
            return libsonare.midi_cc_learn(events, 9)
        if entry == "sonare_midi_route_events":
            return libsonare.midi_route_events(events)
        bindings = [libsonare.MidiCcBinding(cc_number=74, channel=2, param_id=9)]
        if entry == "sonare_midi_cc_to_breakpoint":
            return libsonare.midi_cc_to_breakpoint(bindings, events[1])
        return libsonare.midi_param_to_cc(bindings, 9, 0.75, 1)
    rate = _ONE_SHOT_RESULTS["sample_rate"]
    if entry in {
        "sonare_assign_note_targets",
        "sonare_merge_notes",
        "sonare_split_note",
    }:
        import numpy as np

        f0_hz = np.full(32, 440.0, dtype=np.float32)
        if entry == "sonare_merge_notes":
            f0_hz[16:] = 660.0
        voiced = np.ones(32, dtype=np.int32)
        frame_rate = rate / 256
        notes = libsonare.extract_notes(samples, rate, f0_hz, frame_rate, voiced=voiced)
        if entry == "sonare_assign_note_targets":
            target = libsonare.NoteTarget(start_sec=0.0, end_sec=1.0, target_midi=60.0)
            return libsonare.assign_note_targets(notes, rate, [target])
        notes[0].edit.amplitude_envelope = np.asarray([0.25, 0.5, 1.0], dtype=np.float32)
        if entry == "sonare_split_note":
            return libsonare.split_note(
                samples, rate, f0_hz, frame_rate, notes, 0, 16, voiced=voiced
            )
        return libsonare.merge_notes(samples, rate, f0_hz, frame_rate, notes, 0, 1, voiced=voiced)
    if entry == "sonare_mixer_process_stereo":
        return libsonare.mix_stereo([(samples, [value * 0.5 for value in reversed(samples)])], rate)
    if entry == "sonare_playback_render_interleaved":
        return libsonare.render_playback(
            samples,
            num_channels=2,
            sample_rate=rate,
            config={"target": {"kind": "speakers", "layout": "stereo"}},
        )
    if entry == "sonare_capability_catalog_json":
        if not libsonare.capabilities().features.mastering:
            pytest.skip("capability catalog needs mastering")
        return libsonare.capability_catalog()
    if entry == "sonare_analyze_json":
        return libsonare.analyze(samples, rate)
    if entry == "sonare_mastering_amp_preset_catalog":
        return libsonare.mastering_amp_preset_catalog()
    if entry == "sonare_mastering_preset_params_json":
        return libsonare.mastering_preset_params("pop")
    if entry == "sonare_mastering_chain_config_from_json":
        return libsonare.mastering_chain_config_from_json('{"params":{}}')
    if entry == "sonare_mastering_insert_param_info":
        return libsonare.mastering_insert_param_info("eq.parametric", sample_rate=48000)
    if entry == "sonare_mixing_scene_preset_json":
        return libsonare.mixing_scene_preset("vocalReverbSend")
    if entry == "sonare_synth_engine_param_info":
        return libsonare.synth_engine_param_info("subtractive")
    if entry == "sonare_synth_enum_names":
        return libsonare.synth_enum_tables()
    if entry == "sonare_synth_preset_patch":
        return libsonare.synth_preset_patch("sine")
    if entry == "sonare_synth_patch_param_info":
        return libsonare.synth_patch_param_info()
    if entry == "sonare_waveform_peak_pyramid":
        return libsonare.waveform_peak_pyramid(samples, 1, samples_per_bucket_levels=[1, 2, 4])
    if entry in {"sonare_decode_channels", "sonare_read_audio_tags"}:
        encoded = libsonare.encode_audio(
            [[0.25, -0.25, 0.75, -0.5], [-0.125, 0.375, 0.5, -0.75]],
            22050,
            "wav",
            tags={"title": "Morning", "artist": "Ada"},
        )
        return (
            libsonare.decode_channels(encoded)
            if entry == "sonare_decode_channels"
            else libsonare.read_audio_tags(encoded)
        )
    if entry == "sonare_chord_symbol_from_analysis":
        chord = libsonare.Chord(
            root=0, quality="major", start_sec=0.0, end_sec=1.0, confidence=1.0, bass=0, name="C"
        )
        return libsonare.chord_symbol_from_analysis(chord)
    if entry == "sonare_realtime_voice_changer_preset_config":
        config = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
        with libsonare.RealtimeVoiceChanger(
            rate, num_channels=1, config=config, max_block_size=128
        ) as changer:
            return json.loads(changer.config_json())
    if entry == "sonare_realtime_voice_changer_preset_json":
        return json.loads(libsonare.realtime_voice_changer_preset_json("neutralMonitor"))
    if entry == "sonare_realtime_voice_changer_validate_preset_json":
        document = libsonare.realtime_voice_changer_preset_json("neutralMonitor")
        result = libsonare.validate_realtime_voice_changer_preset_json(document)
        normalized = result.get("normalizedJson")
        assert isinstance(normalized, str)
        return {**result, "normalizedJson": json.loads(normalized)}
    if entry == "sonare_split_silence_common":
        return libsonare.split_silence_common_with_report([samples, samples[: len(samples) // 2]])
    if entry == "sonare_align_take_to_reference":
        return libsonare.align_take_to_reference(samples, samples, rate)
    if entry == "sonare_mastering_repair_dereverb_apply_room_estimate":
        import numpy as np

        return libsonare.mastering_repair_dereverb_config_for_room(
            libsonare.RoomEstimate(
                volume=100.0,
                length_m=None,
                width_m=None,
                height_m=None,
                drr_db=None,
                confidence=1.0,
                band_absorption=np.empty(0, dtype=np.float32),
                rt60_bands=np.asarray([0.125, 0.25, 0.375, 0.625], dtype=np.float32),
            )
        )
    if entry == "sonare_room_geometry_from_estimate":
        import numpy as np

        return libsonare.room_geometry_from_estimate(
            libsonare.RoomEstimate(
                volume=None,
                length_m=14,
                width_m=5,
                height_m=6,
                drr_db=None,
                confidence=1,
                band_absorption=np.asarray([0.25, 0.5, 0.75], dtype=np.float32),
                rt60_bands=np.empty(0, dtype=np.float32),
            )
        )
    if entry == "sonare_synthesize_rir":
        return libsonare.synthesize_rir(sample_rate=rate, max_sec=0.12)
    if entry == "sonare_room_morph":
        return libsonare.room_morph(samples, rate, 7, 5, 3, max_sec=0.12)
    if entry == "sonare_room_morph_stereo":
        return libsonare.room_morph_stereo(samples, samples[::-1], rate, 7, 5, 3, max_sec=0.12)
    if entry == "sonare_lufs_series_interleaved":
        right = (
            [0.0] * len(samples)
            if fixture == "silence"
            else _one_shot_signal("historical_right", samples)
        )
        return libsonare.lufs_series_interleaved(_interleave(samples, right), 2, rate)
    if entry == "sonare_decompose_note_pitch":
        f0 = [0.0] * 7 if fixture == "empty" else [440.0, 441.0, 442.0, 0.0, 438.0, 437.0, 440.0]
        return libsonare.decompose_note_pitch(f0, 100, None if fixture == "empty" else 440)
    if entry == "sonare_note_targets_from_smf":
        return libsonare.note_targets_from_smf(
            bytes.fromhex("4d546864000000060000000100604d54726b0000000c00903c5060803c0000ff2f00")
        )
    if entry == "sonare_decompose":
        spectrum = [3, 2, 1, 2, 3, 2, 1, 2, 4, 3, 2, 1, 2, 4, 3, 1, 2, 4, 4, 1, 3, 2, 1, 3]
        return libsonare.decompose(spectrum, 4, 6, 2)
    if entry == "sonare_decompose_stems_linked":
        return libsonare.decompose_stems_linked(
            [samples, _one_shot_fixture("historical_right")], rate
        )
    if entry == "sonare_estimate_meter_json":
        import numpy as np

        return libsonare.estimate_meter(
            libsonare.BeatObservations(accent=np.asarray(samples, dtype=np.float32))
        )
    function_name = entry.removeprefix("sonare_")
    function = getattr(libsonare, function_name, None)
    if function is None and entry == "sonare_normalize_rms_stereo":
        function = libsonare.normalize_stereo
    if function is None and entry == "sonare_mastering_process":
        function = libsonare.mastering
    if function is None and entry == "sonare_mastering_analyze_pair_stereo":
        function = libsonare.mastering_analyze_pair
    if function is None:
        raise AssertionError(f"no Python one-shot route for {entry}")
    if midi_kwargs is not None:
        return function(**midi_kwargs)
    if entry.startswith("sonare_segment_") or entry in {"sonare_mel_to_stft", "sonare_mfcc_to_mel"}:
        import numpy as np

        if case is None or "input" not in case:
            raise AssertionError(f"missing matrix input for {entry}")
        source = case["input"]

        def matrix(name: str) -> Any:
            item = source[name]
            return np.asarray(item["data"], dtype=np.float32).reshape(item["rows"], item["cols"])

        routes = {
            "sonare_segment_cross_similarity": lambda: function(matrix("x"), matrix("y")),
            "sonare_segment_recurrence_matrix": lambda: function(matrix("data")),
            "sonare_segment_recurrence_to_lag": lambda: function(matrix("recurrence")),
            "sonare_segment_lag_to_recurrence": lambda: function(matrix("lag")),
            "sonare_segment_subsegment": lambda: function(matrix("data"), source["boundaries"]),
            "sonare_segment_agglomerative": lambda: function(matrix("data"), source["k"]),
            "sonare_segment_path_enhance": lambda: function(matrix("recurrence"), source["win"]),
            "sonare_mel_to_stft": lambda: function(matrix("mel"), sr=22050, n_fft=64),
            "sonare_mfcc_to_mel": lambda: function(matrix("coefficients")),
        }
        return routes[entry]()

    sample_rate = _ONE_SHOT_RESULTS["sample_rate"]
    zeroes = [0.0] * len(samples)
    left = samples
    right = zeroes if fixture == "silence" else _one_shot_signal("historical_right", samples)
    reference = _one_shot_signal("historical_reference", samples)
    reference_right = (
        zeroes
        if fixture == "silence"
        else _one_shot_signal("historical_reference_right", reference)
    )

    if entry in {
        "sonare_mastering_repair_declick_stereo",
        "sonare_mastering_repair_declip_stereo",
        "sonare_mastering_repair_decrackle_stereo",
    }:
        return function(samples, samples[::-1], sample_rate)
    if entry == "sonare_mastering_assistant_suggest_stereo":
        return function(samples, samples, sample_rate)
    if entry == "sonare_mastering_repair_apply":
        return function([samples], sample_rate, stages=[libsonare.DeclickStage()])
    if entry in {
        "sonare_mastering_repair_dehum_stereo",
        "sonare_mastering_repair_denoise_classical_stereo",
        "sonare_mastering_repair_dereverb_classical_stereo",
    }:
        return function(samples, samples, sample_rate)
    if entry in {
        "sonare_mastering_repair_denoise_classical_linked",
        "sonare_mastering_repair_dereverb_classical_linked",
    }:
        return function([samples, samples], sample_rate)
    if entry == "sonare_mastering_repair_analyze":
        return function([left, right], sample_rate)
    if entry == "sonare_mastering_streaming_preview_stereo":
        return function(left, right, sample_rate)
    if entry == "sonare_mastering_streaming_preview":
        return function(samples, sample_rate)
    if entry == "sonare_mastering_repair_trim_silence":
        return function(samples, sample_rate)
    if entry == "sonare_mastering_repair_trim_silence_stereo":
        return function(samples, samples, sample_rate)
    if entry in {"sonare_piptrack", "sonare_reassigned_spectrogram"}:
        return function(samples, sample_rate, n_fft=512, hop_length=128)
    if entry == "sonare_master_audio":
        return libsonare.master_audio(samples, sample_rate, overrides={})
    if entry == "sonare_master_audio_stereo":
        return libsonare.master_audio_stereo(samples, samples, sample_rate, overrides={})
    if entry == "sonare_normalize_stereo":
        return function(samples, samples, sample_rate)
    if entry == "sonare_normalize_rms_stereo":
        return libsonare.normalize_stereo(samples, samples, sample_rate, mode="rms")
    if entry == "sonare_mastering_process":
        return function(samples, sample_rate)
    if entry == "sonare_mastering_ab_match_loudness":
        return function(samples, reference, sample_rate)
    if entry == "sonare_mastering_ab_match_loudness_stereo":
        return function(samples, right, reference, reference_right, sample_rate)
    if entry == "sonare_mastering_apply_processor":
        return function("utility.gain", samples, sample_rate)
    if entry == "sonare_mastering_apply_processor_stereo":
        return function("utility.gain", samples, samples, sample_rate)
    if entry == "sonare_mastering_apply_pair_processor":
        return function("match.abCrossfade", samples, samples[::-1], sample_rate)
    if entry == "sonare_mastering_apply_pair_processor_stereo":
        return function(
            "match.abCrossfade",
            samples,
            samples[::-1],
            samples[::-1],
            [sample * 0.5 for sample in reversed(samples)],
            sample_rate,
        )
    if entry == "sonare_trim_silence":
        return function(samples)
    if entry == "sonare_mastering_audio_profile" and fixture in {
        "profile_silence",
        "clipped",
        "historical_reference_right",
    }:
        return function(samples, sample_rate, {"detectDefects": True})
    analysis_entries = {
        "sonare_analyze_bpm",
        "sonare_analyze_rhythm",
        "sonare_analyze_melody",
        "sonare_analyze_sections",
        "sonare_analyze_impulse_response",
        "sonare_detect_acoustic",
        "sonare_estimate_room",
    }
    if entry in analysis_entries:
        return function(samples, sample_rate)
    if entry == "sonare_frame_signal":
        return function(samples, 4, 2)
    if entry == "sonare_mastering_audio_profile_stereo":
        return function(left, right, sample_rate)
    if entry == "sonare_streaming_loudness_gain":
        return function(samples, sample_rate)
    if entry == "sonare_streaming_loudness_gain_stereo":
        return function(left, right, sample_rate)
    if entry == "sonare_mastering_insert_timing":
        return function("eq.parametric", sample_rate=sample_rate)
    if entry in {
        "sonare_mastering_repair_detect_clicks",
        "sonare_mastering_repair_detect_crackle",
        "sonare_mastering_repair_detect_hum",
        "sonare_mastering_repair_detect_noise_floor",
        "sonare_mastering_repair_detect_reverb",
        "sonare_mastering_repair_detect_trim_range",
    }:
        return function(samples, sample_rate)
    if entry == "sonare_mastering_repair_detect_trim_range_stereo":
        return function(left, right, sample_rate)
    if entry in {"sonare_mastering_analyze_pair", "sonare_mastering_analyze_pair_stereo"}:
        if entry.endswith("_stereo"):
            source: list[float] | list[list[float]] = [left, right]
            reference_audio: list[float] | list[list[float]] = [reference, reference_right]
        else:
            source = left
            reference_audio = reference
        return function("match.tonalBalance", source, reference_audio, sample_rate)
    if entry == "sonare_mastering_analyze_stereo":
        return function("stereo.monoCompatCheck", left, right, sample_rate)
    if entry == "sonare_metering_detect_clipping":
        return function(samples, sample_rate)
    if entry in {"sonare_metering_vectorscope", "sonare_metering_phase_scope"}:
        return function(left, right, sample_rate)
    if entry == "sonare_metering_spectrum":
        return function(samples, sample_rate)
    if entry == "sonare_metering_spectrum_frame":
        return function(samples, sample_rate)
    if entry == "sonare_lufs_interleaved":
        return function(_interleave(left, right), 2, sample_rate)
    if entry == "sonare_waveform_peaks":
        return function(_interleave(left, right), 2)
    if entry == "sonare_mixing_assistant_suggest":
        from libsonare import MixAssistantTrack

        tracks = [
            MixAssistantTrack("tone", [samples]),
            MixAssistantTrack("silent", [zeroes]),
        ]
        return function(tracks, sample_rate=sample_rate)
    return function(samples, sample_rate)


def _camel_field(name: str) -> str:
    head, *tail = name.split("_")
    return head + "".join(part[:1].upper() + part[1:] for part in tail)


def _json_result_value(value: Any) -> Any:
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        fields: dict[str, Any] = {}
        for field in dataclasses.fields(value):
            field_value = getattr(value, field.name)
            if field_value is not None:
                field_name = _camel_field(field.name)
                fields[field_name] = _json_result_value(field_value)
        return fields
    if isinstance(value, dict):
        return {
            _camel_field(str(key)): _json_result_value(item)
            for key, item in value.items()
            if item is not None
        }
    if isinstance(value, (list, tuple)):
        return [_json_result_value(item) for item in value]
    tolist = getattr(value, "tolist", None)
    if callable(tolist):
        return _json_result_value(tolist())
    item = getattr(value, "item", None)
    if callable(item):
        return _json_result_value(item())
    return value


def _one_shot_public_result(entry: str, result: Any) -> Any:
    if entry == "sonare_playback_render_interleaved":
        assert result.ndim == 2
        return {"samples": result.reshape(-1).tolist(), "numChannels": int(result.shape[1])}
    if entry in {
        "sonare_segment_cross_similarity",
        "sonare_segment_recurrence_matrix",
        "sonare_segment_recurrence_to_lag",
        "sonare_segment_lag_to_recurrence",
        "sonare_segment_path_enhance",
        "sonare_mel_to_stft",
        "sonare_mfcc_to_mel",
    }:
        import numpy as np

        a = np.asarray(result)
        return {"rows": int(a.shape[0]), "cols": int(a.shape[1]), "data": a.reshape(-1).tolist()}
    if entry == "sonare_frame_signal":
        rows, cols = result.shape
        return {
            "data": [float(value) for value in result.reshape(-1).tolist()],
            "rows": int(rows),
            "cols": int(cols),
        }
    if entry == "sonare_waveform_peak_pyramid":
        converted = _json_result_value(result)
        for report, wire in zip(result, converted, strict=True):
            channels = int(report.num_channels)
            buckets = int(report.bucket_count)
            for field in ("min", "max"):
                flat = wire[field]
                wire[field] = [flat[i * buckets : (i + 1) * buckets] for i in range(channels)]
            wire.pop("numChannels")
        return converted
    if entry == "sonare_waveform_peaks":
        channels = int(result.num_channels)
        buckets = int(result.bucket_count)
        if channels != 2:
            raise AssertionError(f"waveform_peaks fixture expected two channels, got {channels}")
        converted = _json_result_value(result)
        for field in ("min", "max"):
            flat = converted[field]
            converted[field] = [
                flat[index * buckets : (index + 1) * buckets] for index in range(channels)
            ]
        # The common view stores channel-major planes as nested channel arrays;
        # the channel count is already represented by that layout.
        converted.pop("numChannels")
        return converted
    converted = _json_result_value(result)
    if entry in {
        "sonare_mastering_repair_declick_stereo",
        "sonare_mastering_repair_declip_stereo",
        "sonare_mastering_repair_decrackle_stereo",
        "sonare_mastering_repair_dehum_stereo",
        "sonare_mastering_repair_denoise_classical_stereo",
        "sonare_mastering_repair_dereverb_classical_stereo",
        "sonare_mastering_repair_trim_silence_stereo",
    }:
        left = converted.pop("left")
        right = converted.pop("right")
        assert converted.pop("length") == len(left) == len(right)
        converted["channels"] = [left, right]
        if "leftReport" in converted:
            converted["reports"] = [converted.pop("leftReport"), converted.pop("rightReport")]
        else:
            converted["reports"] = [converted.pop("report")]
        if entry == "sonare_mastering_repair_trim_silence_stereo":
            converted["ranges"] = [converted.pop("leftRange"), converted.pop("rightRange")]
    if entry in {
        "sonare_mastering_repair_denoise_classical_linked",
        "sonare_mastering_repair_dereverb_classical_linked",
    }:
        converted["reports"] = [converted.pop("report")]
    if entry in {
        "sonare_cqt",
        "sonare_hybrid_cqt",
        "sonare_pseudo_cqt",
        "sonare_vqt",
        "sonare_bass_chroma",
        "sonare_chroma_cens",
        "sonare_chroma_cqt",
    }:
        field = (
            "features"
            if entry in {"sonare_bass_chroma", "sonare_chroma_cens", "sonare_chroma_cqt"}
            else "magnitude"
        )
        array = getattr(result, field)
        rows, cols = array.shape
        converted[field] = {"rows": rows, "cols": cols, "data": array.ravel().tolist()}
    if entry == "sonare_hpss":
        length = converted.pop("length")
        assert length == len(converted["harmonic"]) == len(converted["percussive"])
    if entry == "sonare_decompose":
        for field in ("w", "h"):
            array = getattr(result, field)
            rows, cols = array.shape
            converted[field] = {"rows": rows, "cols": cols, "data": array.ravel().tolist()}
    if entry in {"sonare_decompose_stems", "sonare_decompose_stems_linked"}:
        for field in ("w", "h"):
            converted[field] = getattr(result, field).ravel().tolist()
    if entry == "sonare_auto_tune":
        converted["key"].update(name=result.key.name, shortName=result.key.short_name)
    if entry == "sonare_room_geometry_from_estimate":
        for key in ("source", "listener"):
            converted[key] = dict(zip(("x", "y", "z"), converted[key], strict=True))
    if entry == "sonare_align_take_to_reference":
        converted["anchors"] = [
            dict(zip(("warpSample", "sourceSample"), pair, strict=True))
            for pair in converted["anchors"]
        ]
    if entry == "sonare_split_silence_common":
        converted["intervals"] = [item for pair in converted["intervals"] for item in pair]
    if entry == "sonare_mixing_assistant_suggest":
        for track, wire in zip(result.tracks, converted["tracks"], strict=True):
            # The C wire and the JS result use an empty display name for the unnamed track.
            assert track.name != ""
            if track.name is None:
                wire["name"] = ""
    elif entry == "sonare_metering_detect_clipping":
        converted["clippedRatio"] = converted.pop("clippingRatio")
    for field in {
        "sonare_chroma": ("features",),
        "sonare_mel_spectrogram": ("power", "db"),
        "sonare_mfcc": ("coefficients",),
        "sonare_stft": ("magnitude", "power", "db", "phase"),
        "sonare_piptrack": ("pitches", "magnitudes"),
        "sonare_reassigned_spectrogram": ("magnitude", "times", "frequencies"),
    }.get(entry, ()):
        if field in converted:
            array = getattr(result, field)
            rows, cols = array.shape
            converted[field] = {"rows": rows, "cols": cols, "data": array.ravel().tolist()}
    return converted


def _catalog_without_null_fields(value: Any) -> Any:
    if isinstance(value, list):
        return [_catalog_without_null_fields(item) for item in value]
    if isinstance(value, dict):
        return {
            key: _catalog_without_null_fields(field)
            for key, field in value.items()
            if field is not None
        }
    return value


def _facade_result_oracle(entry: str, value: Any) -> Any:
    if entry == "sonare_capability_catalog_json":
        return _catalog_without_null_fields(value)
    if entry == "sonare_detect_key":
        return {key: item for key, item in value.items() if key not in {"name", "shortName"}}
    if entry == "sonare_analyze_json":
        key = dict(value["key"])
        key.pop("name")
        key.pop("shortName")
        if key["mode"] == 1:
            key["mode"] = "minor"
        chords = []
        for chord in value["chords"]:
            chord = dict(chord)
            chord.pop("durationSec")
            if chord["quality"] == 0:
                chord["quality"] = "major"
            chords.append(chord)
        melody = dict(value["melody"])
        melody["points"] = melody.pop("pitches")
        sections = [
            {**section, "type": "unknown" if section["type"] == 7 else section["type"]}
            for section in value["sections"]
        ]
        return {**value, "key": key, "chords": chords, "melody": melody, "sections": sections}
    if entry in {"sonare_normalize_stereo", "sonare_normalize_rms_stereo"}:
        return {"left": value["left"], "right": value["right"], "length": len(value["left"])}
    if entry == "sonare_mastering_insert_param_info":
        return _catalog_without_null_fields(value)
    if entry in {"sonare_mastering_assistant_suggest", "sonare_mastering_assistant_suggest_stereo"}:
        return {**value, "chainConfig": value["chainConfig"]["params"]}
    if entry in {"sonare_mastering_preset_params_json", "sonare_mastering_chain_config_from_json"}:
        return value["params"]
    if entry in {"sonare_synth_engine_param_info", "sonare_synth_patch_param_info"}:
        # The Python record declares absent integer flags as False.
        return [item if item["type"] == "enum" else {"integer": False, **item} for item in value]
    return value


def _expected_array_scale(expected: list[Any]) -> float:
    values: list[float] = []

    def collect(value: Any) -> None:
        if isinstance(value, bool):
            return
        if isinstance(value, (int, float)):
            values.append(abs(float(value)))
        elif isinstance(value, list):
            for child in value:
                collect(child)

    collect(expected)
    return max(values, default=0.0)


_INTEGER_RESULT_FIELDS = {
    "bandFloorBand",
    "first",
    "group",
    "harmonics",
    "lastExclusive",
    "nMels",
    "endSample",
    "flatRunCount",
    "flatSampleCount",
    "frame",
    "frameStride",
    "hopLength",
    "index",
    "length",
    "longestFlatRunSamples",
    "longestRunSamples",
    "runCount",
    "clippedSamples",
    "onsetSample",
    "offsetSample",
    "frameStart",
    "frameEnd",
    "sampleRate",
    "startSample",
    "rows",
    "cols",
    "nFft",
    "bucketCount",
    "samplesPerBucket",
    "numerator",
    "denominator",
    "numChannels",
    "unison",
    "polyphony",
    "sampleSet",
    "count",
    "sampleCount",
}


def _result_field(path: str) -> str:
    return path.rsplit(".", 1)[-1].split("[", 1)[0]


def _path_tolerance(
    tolerances: dict[str, float] | None, path: str, relative_tolerance: float, scale: float
) -> float:
    if tolerances is None:
        return relative_tolerance * scale
    if path == "result":
        normalized = "result"
    elif path.startswith("result."):
        normalized = "result." + re.sub(r"\[\d+\]", "[]", path[len("result.") :])
    elif path.startswith("result["):
        normalized = "result" + re.sub(r"\[\d+\]", "[]", path[len("result") :])
    else:
        raise AssertionError(f"result path is not rooted at result: {path}")
    candidates = [normalized]
    while candidates[-1].endswith("[]"):
        candidates.append(candidates[-1][:-2])
    for candidate in candidates:
        if candidate in tolerances:
            return tolerances[candidate]
    return tolerances.get("*", 0.0) * scale


def _assert_result_matches(
    actual: Any,
    expected: Any,
    relative_tolerance: float,
    path: str,
    tolerances: dict[str, float] | None = None,
) -> None:
    if isinstance(expected, dict):
        assert isinstance(actual, dict), f"{path}: expected an object, got {type(actual).__name__}"
        assert set(actual) == set(expected), (
            f"{path}: result fields differ; actual={sorted(actual)}, expected={sorted(expected)}"
        )
        for key, expected_value in expected.items():
            _assert_result_matches(
                actual[key], expected_value, relative_tolerance, f"{path}.{key}", tolerances
            )
        return
    if isinstance(expected, list):
        assert isinstance(actual, list), f"{path}: expected an array, got {type(actual).__name__}"
        assert len(actual) == len(expected), (
            f"{path}: array lengths differ; actual={len(actual)}, expected={len(expected)}"
        )
        scale = _expected_array_scale(expected)
        for index, (actual_value, expected_value) in enumerate(zip(actual, expected, strict=True)):
            item_path = f"{path}[{index}]"
            if isinstance(expected_value, (dict, list)):
                _assert_result_matches(
                    actual_value, expected_value, relative_tolerance, item_path, tolerances
                )
            else:
                _assert_numeric_or_exact(
                    actual_value, expected_value, relative_tolerance, scale, item_path, tolerances
                )
        return
    _assert_numeric_or_exact(actual, expected, relative_tolerance, None, path, tolerances)


def _assert_numeric_or_exact(
    actual: Any,
    expected: Any,
    relative_tolerance: float,
    scale: float | None,
    path: str,
    tolerances: dict[str, float] | None,
) -> None:
    if isinstance(expected, str) and expected in {"NaN", "Infinity", "-Infinity"}:
        assert type(actual) is float, f"{path}: expected a number, got {actual!r}"
        matches = math.isnan(actual) if expected == "NaN" else actual == float(expected)
        assert matches, f"{path}: actual={actual!r}, expected={expected}"
    elif isinstance(expected, bool):
        assert type(actual) is bool and actual is expected, (
            f"{path}: actual={actual!r}, expected={expected!r}"
        )
    elif isinstance(expected, (int, float)):
        if _result_field(path) in _INTEGER_RESULT_FIELDS:
            assert type(actual) is int and actual == expected, (
                f"{path}: expected exact integer {expected!r}, got {actual!r}"
            )
        else:
            assert isinstance(actual, (int, float)) and not isinstance(actual, bool), (
                f"{path}: expected numeric value, got {type(actual).__name__}"
            )
            comparison_scale = abs(float(expected)) if scale is None else scale
            bound = _path_tolerance(tolerances, path, relative_tolerance, comparison_scale)
            assert abs(float(actual) - float(expected)) <= bound, (
                f"{path}: actual={actual!r}, expected={expected!r}, scale={comparison_scale!r}"
            )
    else:
        assert type(actual) is type(expected) and actual == expected, (
            f"{path}: actual={actual!r}, expected={expected!r}"
        )


@pytest.mark.parametrize(
    "case",
    [
        pytest.param(case, marks=pytest.mark.slow) if case.get("slow") else case
        for case in _ONE_SHOT_RESULTS["cases"]
        if "python" in case.get("surfaces", ("python",))
    ],
    ids=lambda case: case["id"],
)
def test_public_one_shot_results_follow_the_shared_corpus(case: dict[str, Any]) -> None:
    fixture = case["fixture"]
    samples = [] if "midi" in case else _one_shot_fixture(fixture)
    result = _one_shot_call(case["entry"], samples, fixture, case.get("midi"), case)
    actual = _one_shot_public_result(case["entry"], result)
    if case.get("catalogReference") == "all":
        catalog = Path(__file__).resolve().parents[3] / "tools/capability-catalog.json"
        expected = json.loads(catalog.read_text(encoding="utf-8"))
        capabilities = libsonare.capabilities()
        expected["encode"]["ffmpeg"] = list(capabilities.encode.ffmpeg)
        for enabled, group in (
            (capabilities.features.arrangement, "synth"),
            (capabilities.features.mixing, "mixingScene"),
            (capabilities.features.voice_changer, "voiceChanger"),
            (capabilities.features.playback, "playbackRoom"),
        ):
            if not enabled:
                expected["presets"][group] = []
    else:
        expected = case["result"]
    _assert_result_matches(
        actual,
        _facade_result_oracle(case["entry"], expected),
        _ONE_SHOT_RESULTS["relative_tolerance"],
        "result",
        case.get("tolerances"),
    )


@pytest.mark.parametrize("case", _ONE_SHOT_RESULTS["refusals"], ids=lambda case: case["id"])
def test_public_one_shot_refusals_follow_the_shared_corpus(
    case: dict[str, Any],
) -> None:
    expected_exception = getattr(
        builtins, case["python_class"], getattr(libsonare, case["python_class"], None)
    )
    assert expected_exception is not None
    with pytest.raises(expected_exception) as refused:
        fixture = case["fixture"]
        samples = [] if "midi" in case else _one_shot_fixture(fixture)
        _one_shot_call(case["entry"], samples, fixture, case.get("midi"))
    assert type(refused.value).__name__ == case["python_class"]
    if case["code"] is None:
        assert not isinstance(refused.value, SonareError)
    else:
        assert isinstance(refused.value, SonareError)
        assert int(refused.value.code) == case["code"]


def test_public_result_corpus_covers_python_exports() -> None:
    root = Path(__file__).resolve().parents[3]
    subprocess.run(
        [
            sys.executable,
            str(root / "tests/conformance/check_result_corpus_coverage.py"),
            "python",
        ],
        check=True,
    )
