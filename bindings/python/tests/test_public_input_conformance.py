"""Cross-binding public-input transaction corpus."""

from __future__ import annotations

import dataclasses
import json
import math
import re
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
