"""Analysis chord / key -> annotation conversion on the Python surface.

The mapping itself is the core's (tests/arrangement/c_abi_edit_ops_test.cpp covers
all 25 qualities); what is Python's own is the name -> ordinal step, the
``bass=None`` convention, the refusals by field name, and that every result is
accepted by ``annotate_chords`` and survives serialization.
"""

from __future__ import annotations

from types import MappingProxyType
from typing import get_args

import pytest

from libsonare import (
    CHORD_QUALITIES,
    Chord,
    KeyMode,
    PitchClass,
    Project,
    ProjectChordSymbol,
    ProjectKeySegment,
    SonareValueError,
    chord_symbol_from_analysis,
    key_mode_from_analysis,
)

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")


def chord(quality: str, root: int = 0, bass: int | None = None) -> Chord:
    return Chord(
        PitchClass(root), quality, 0.0, 1.0, 1.0, bass=None if bass is None else PitchClass(bass)
    )


@pytest.mark.parametrize(
    ("quality", "annotation_quality", "extensions"),
    [
        ("minor", "minor", []),
        ("dominant7", "dominant", [7]),
        ("halfDim7", "halfDiminished", [7]),
        ("sus4", "suspended", [4]),
        ("dominant13", "dominant", [7, 9, 13]),
        ("unknown", "unknown", []),
    ],
)
def test_named_qualities_map_to_their_family_and_degrees(
    quality: str, annotation_quality: str, extensions: list[int]
) -> None:
    result = chord_symbol_from_analysis(chord(quality, root=7))
    assert result == {
        "root_pc": 7,
        "quality": annotation_quality,
        "extensions": extensions,
    }


def test_a_bass_below_the_root_is_a_slash_and_bass_equal_to_root_is_not() -> None:
    assert chord_symbol_from_analysis(chord("major", root=0, bass=4))["slash_bass_pc"] == 4
    assert "slash_bass_pc" not in chord_symbol_from_analysis(chord("major", root=0, bass=0))
    assert "slash_bass_pc" not in chord_symbol_from_analysis(chord("major", root=0, bass=None))


def test_every_quality_is_accepted_by_annotate_chords_and_survives_serialization() -> None:
    assert len(CHORD_QUALITIES) == 25
    project = Project()
    entries = []
    for ordinal, name in enumerate(CHORD_QUALITIES):
        entries.append(
            {
                "start_ppq": float(ordinal),
                "end_ppq": float(ordinal) + 1.0,
                **chord_symbol_from_analysis(
                    chord(name, root=ordinal % 12, bass=(ordinal + 3) % 12)
                ),
            }
        )
    project.annotate_chords(entries)
    serialized = project.to_json()
    assert Project.from_json(serialized).to_json() == serialized


@pytest.mark.parametrize("mode", get_args(KeyMode))
def test_key_modes_keep_their_name_and_are_accepted_by_annotate_keys(mode: KeyMode) -> None:
    converted = key_mode_from_analysis(mode)
    assert converted == mode
    project = Project()
    project.annotate_keys([{"start_ppq": 0.0, "end_ppq": 480.0, "tonic_pc": 0, "mode": converted}])
    serialized = project.to_json()
    assert Project.from_json(serialized).to_json() == serialized


def test_project_key_annotation_reader_returns_typed_segments_in_stored_order() -> None:
    project = Project()
    try:
        first = ProjectKeySegment(start_ppq=0.0, end_ppq=480.0, tonic_pc=None, mode="unknown")
        second = ProjectKeySegment(start_ppq=480.0, end_ppq=960.0, tonic_pc=11, mode="locrian")
        assert project.key_annotations() == []
        project.annotate_keys([second, first])
        assert project.key_annotations() == [second, first]
    finally:
        project.close()


def test_project_chord_annotation_reader_copies_extensions_and_omits_absent_numerals() -> None:
    project = Project()
    try:
        absent = ProjectChordSymbol(
            start_ppq=0.0,
            end_ppq=480.0,
            root_pc=None,
            quality="unknown",
            extensions=[],
            slash_bass_pc=None,
            modulation_boundary=False,
        )
        labeled = ProjectChordSymbol(
            start_ppq=480.0,
            end_ppq=960.0,
            root_pc=7,
            quality="halfDiminished",
            extensions=[7, 9, 13],
            slash_bass_pc=4,
            modulation_boundary=True,
            roman_numeral="iiø7",
        )
        single_extension = ProjectChordSymbol(
            start_ppq=960.0,
            end_ppq=1200.0,
            root_pc=2,
            quality="minor",
            extensions=[7],
            slash_bass_pc=None,
            modulation_boundary=False,
        )
        assert project.chord_annotations() == []
        project.annotate_chords([absent, labeled, single_extension])
        expected_empty = {
            "start_ppq": single_extension["start_ppq"],
            "end_ppq": single_extension["end_ppq"],
            "root_pc": single_extension["root_pc"],
            "quality": single_extension["quality"],
            "extensions": single_extension["extensions"],
            "slash_bass_pc": single_extension["slash_bass_pc"],
            "modulation_boundary": single_extension["modulation_boundary"],
        }
        snapshot = project.chord_annotations()
        assert snapshot == [absent, labeled, expected_empty]
        project.annotate_chords([])
        assert project.chord_annotations() == []
        assert snapshot == [absent, labeled, expected_empty]
    finally:
        project.close()


def test_an_explicit_empty_roman_numeral_is_refused_by_name() -> None:
    project = Project()
    try:
        with pytest.raises(
            SonareValueError,
            match=r"^Project\.annotate_chords: chords\[0\]\.roman_numeral must not be empty$",
        ):
            project.annotate_chords(
                [
                    {
                        "start_ppq": 0.0,
                        "end_ppq": 480.0,
                        "quality": "major",
                        "roman_numeral": "",
                    }
                ]
            )
        assert project.chord_annotations() == []
    finally:
        project.close()


def test_annotate_chords_reads_immutable_rows_and_extensions() -> None:
    row = MappingProxyType({"start_ppq": 0.0, "end_ppq": 4.0, "extensions": (7, 9)})
    with Project() as project:
        project.annotate_chords((row,))
        assert project.chord_annotations()[0]["extensions"] == [7, 9]
    assert row["extensions"] == (7, 9)


@pytest.mark.parametrize("value", [{"root": 0}, "major", None, (0, "major")])
def test_a_non_chord_is_a_type_error(value: object) -> None:
    with pytest.raises(TypeError):
        chord_symbol_from_analysis(value)  # type: ignore[arg-type]


@pytest.mark.parametrize(
    ("bad", "field"),
    [
        (Chord(12, "major", 0.0, 1.0, 1.0), "chord.root"),  # type: ignore[arg-type]
        (Chord(0, "major", 0.0, 1.0, 1.0, bass=12), "chord.bass"),  # type: ignore[arg-type]
        (Chord(0, "nonsense", 0.0, 1.0, 1.0), "chord.quality"),  # type: ignore[arg-type]
    ],
)
def test_an_unrecognised_value_is_refused_naming_the_field(bad: Chord, field: str) -> None:
    with pytest.raises(SonareValueError, match=field.replace(".", r"\.")):
        chord_symbol_from_analysis(bad)


@pytest.mark.parametrize("mode", ["Major", "maj", "whole-tone"])
def test_an_unknown_mode_is_refused_naming_the_field(mode: str) -> None:
    with pytest.raises(SonareValueError, match="mode"):
        key_mode_from_analysis(mode)  # type: ignore[arg-type]


@pytest.mark.parametrize("mode", [0, 1, True])
def test_a_non_string_mode_is_a_type_error(mode: object) -> None:
    with pytest.raises(TypeError):
        key_mode_from_analysis(mode)  # type: ignore[arg-type]


def test_an_unknown_root_or_tonic_and_no_slash_bass_are_absent() -> None:
    project = Project()
    project.annotate_chords(
        [
            {"start_ppq": 0.0, "end_ppq": 480.0, "quality": "unknown"},
            {"start_ppq": 480.0, "end_ppq": 960.0, "root_pc": None, "slash_bass_pc": None},
        ]
    )
    project.annotate_keys(
        [
            {"start_ppq": 0.0, "end_ppq": 480.0, "tonic_pc": None, "mode": "unknown"},
            {"start_ppq": 480.0, "end_ppq": 960.0},
        ]
    )


@pytest.mark.parametrize("field", ["root_pc", "slash_bass_pc"])
@pytest.mark.parametrize("value", [255, 12, -1])
def test_a_chord_pitch_class_outside_0_to_11_is_refused_by_name(field: str, value: int) -> None:
    with pytest.raises(SonareValueError, match=f"chords\\[0\\].{field}"):
        Project().annotate_chords(
            [{"start_ppq": 0.0, "end_ppq": 480.0, "root_pc": 0, "quality": "major", field: value}]
        )


@pytest.mark.parametrize("value", [255, 12, -1])
def test_a_key_tonic_outside_0_to_11_is_refused_by_name(value: int) -> None:
    with pytest.raises(SonareValueError, match="keys\\[0\\].tonic_pc"):
        Project().annotate_keys(
            [{"start_ppq": 0.0, "end_ppq": 480.0, "tonic_pc": value, "mode": "major"}]
        )


@pytest.mark.parametrize("value", [0, 1, True])
def test_an_ordinal_quality_or_mode_is_a_type_error(value: object) -> None:
    with pytest.raises(TypeError, match=r"chords\[0\]\.quality"):
        Project().annotate_chords([{"start_ppq": 0.0, "end_ppq": 480.0, "quality": value}])
    with pytest.raises(TypeError, match=r"keys\[0\]\.mode"):
        Project().annotate_keys([{"start_ppq": 0.0, "end_ppq": 480.0, "mode": value}])


@pytest.mark.parametrize("value", ["half_diminished", "Major", "dim"])
def test_an_unknown_quality_or_mode_token_is_refused_by_name(value: str) -> None:
    with pytest.raises(SonareValueError, match=r"chords\[0\]\.quality"):
        Project().annotate_chords([{"start_ppq": 0.0, "end_ppq": 480.0, "quality": value}])
    with pytest.raises(SonareValueError, match=r"keys\[0\]\.mode"):
        Project().annotate_keys([{"start_ppq": 0.0, "end_ppq": 480.0, "mode": value}])


def test_a_key_segment_tuple_is_a_type_error() -> None:
    with pytest.raises(TypeError, match=r"keys\[0\] must be a mapping"):
        Project().annotate_keys([(0.0, 480.0, 0, "major")])  # type: ignore[list-item]
