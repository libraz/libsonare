"""Persistent MIDI destination instruments on the Python project facade."""

from __future__ import annotations

from typing import Any, cast

import pytest

from libsonare import (
    BuiltinSynthConfig,
    ErrorCode,
    Project,
    ProjectDestinationInstrument,
    ProjectInstrument,
    Sf2InstrumentConfig,
    SonareError,
    SonareValueError,
    SynthPatch,
    synth_enum_tables,
    synth_patch_param_info,
)
from libsonare._types_capabilities import SynthPatchEnumParamInfo


def test_synth_patch_catalog_lists_enum_tokens_from_shared_tables() -> None:
    rows = {row.name: row for row in synth_patch_param_info()}
    enum_rows = {name: row for name, row in rows.items() if row.type == "enum"}
    tables = synth_enum_tables()
    expected = {
        "waveform": tables["waveforms"],
        "filterModel": tables["filter_models"],
        "filterOutput": tables["filter_outputs"],
        "body": tables["body_types"],
        "retrigger": tables["retriggers"],
        "lfoShape": tables["lfo_shapes"],
        "sampleLoop": tables["sample_loop_modes"],
        "sampleKeyTrack": tables["sample_key_tracks"],
    }
    assert set(enum_rows) == set(expected)
    for name, values in expected.items():
        row = enum_rows[name]
        assert isinstance(row, SynthPatchEnumParamInfo)
        assert row.values == values
        assert row.default == values[0]


def test_project_destination_instruments_round_trip_all_kinds_and_enum_fields() -> None:
    rows = {row.name: row for row in synth_patch_param_info() if row.type == "enum"}
    field_names = {
        "waveform": "waveform",
        "filterModel": "filter_model",
        "filterOutput": "filter_output",
        "body": "body",
        "retrigger": "retrigger",
        "lfoShape": "lfo_shape",
        "sampleLoop": "sample_loop",
        "sampleKeyTrack": "sample_key_track",
    }
    patch_fields = {
        field_names[name]: next(token for token in row.values if token != row.default)
        for name, row in rows.items()
    }
    patch = SynthPatch(
        **cast(Any, patch_fields),
        engine_mode="bowedString",
        engine_params={"bowForce": 0.35},
        gain=0.0,
        one_shot=False,
        mod_routings=(),
    )
    sparse_patch = SynthPatch(mod_routings=None)
    instruments = {
        5: ProjectInstrument(
            kind="synth",
            patch=patch,
            use_gm_programs=True,
            bank_msb=0,
            bank_lsb=0,
            program=0,
        ),
        7: ProjectInstrument(kind="synth", patch=sparse_patch, use_gm_programs=False),
        9: ProjectInstrument(
            kind="builtin",
            config=BuiltinSynthConfig(
                waveform="triangle",
                gain=0.75,
                attack_ms=8.0,
                decay_ms=85.0,
                sustain=0.5,
                release_ms=250.0,
                polyphony=13,
            ),
        ),
        11: ProjectInstrument(
            kind="sf2",
            config=Sf2InstrumentConfig(
                gain=0.875,
                polyphony=39,
                prefer_model_for_modeled_families=True,
                clear_bank_rig=True,
                gs_efx_realization="classic",
            ),
            sound_font_uri="memory://font/example.sf2",
        ),
    }

    with Project() as project:
        for destination_id, instrument in instruments.items():
            track_id = project.add_track("midi")
            project.set_track_midi_destination(track_id, destination_id)
            project.set_destination_instrument(destination_id, instrument)

        serialized = project.to_json()
        actual = project.destination_instruments()
        assert [row.destination_id for row in actual] == list(instruments)
        by_destination = {row.destination_id: row for row in actual}
        assert by_destination == {
            destination_id: ProjectDestinationInstrument(
                **vars(instrument), destination_id=destination_id
            )
            for destination_id, instrument in instruments.items()
        }
        assert by_destination[5].patch is not None
        assert by_destination[5].patch.mod_routings == ()
        assert by_destination[5].patch.engine_params == {"bowForce": 0.35}
        assert by_destination[5].patch.gain == 0.0
        assert by_destination[5].patch.one_shot is False
        assert by_destination[5].use_gm_programs is True
        assert by_destination[7].patch is not None
        assert by_destination[7].patch.mod_routings is None
        assert by_destination[5].bank_msb == 0
        assert by_destination[5].bank_lsb == 0
        assert by_destination[5].program == 0

        with pytest.raises(SonareValueError, match=r"instrument\.destination_id must match"):
            project.set_destination_instrument(7, by_destination[5])
        assert project.destination_instruments() == actual
        for row in actual:
            project.set_destination_instrument(row.destination_id, row)
        assert project.destination_instruments() == actual
        assert project.to_json() == serialized


def test_project_destination_instrument_unknown_id_names_destination() -> None:
    with Project() as project, pytest.raises(SonareValueError, match="destination_id"):
        project.set_destination_instrument(
            999,
            ProjectInstrument(kind="synth", patch=SynthPatch()),
        )


def test_project_destination_instrument_refusals_name_the_config() -> None:
    with Project() as project:
        project.add_track("midi")
        with pytest.raises(SonareValueError, match=r"instrument\.config\.gain"):
            project.set_destination_instrument(
                0, ProjectInstrument(kind="builtin", config=BuiltinSynthConfig(gain=5.0))
            )


def test_project_destination_instrument_refuses_a_callback() -> None:
    class CallbackInstrument:
        def render(self) -> None:
            raise AssertionError("a persistent setter must not render")

    with Project() as project:
        project.add_track("midi")
        with pytest.raises(SonareError) as caught:
            project.set_destination_instrument(0, CallbackInstrument())  # type: ignore[arg-type]
        assert caught.value.code == ErrorCode.NOT_SUPPORTED
        assert caught.value.reason == "notPersistable"
