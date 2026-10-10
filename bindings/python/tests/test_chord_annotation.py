"""Analysis chord / key -> annotation conversion on the Python surface.

The mapping itself is the core's (tests/arrangement/c_abi_edit_ops_test.cpp covers
all 25 qualities); what is Python's own is the name -> ordinal step, the
``bass=None`` convention, the refusals by field name, and that every result is
accepted by ``annotate_chords`` and survives serialization.
"""

from __future__ import annotations

import pytest

from libsonare import (
    Chord,
    Mode,
    PitchClass,
    Project,
    SonareValueError,
    chord_symbol_from_analysis,
    key_mode_from_analysis,
)
from libsonare._analysis_music import _CHORD_QUALITY_NAMES

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")


def chord(quality: str, root: int = 0, bass: int | None = None) -> Chord:
    return Chord(
        PitchClass(root), quality, 0.0, 1.0, 1.0, bass=None if bass is None else PitchClass(bass)
    )


@pytest.mark.parametrize(
    ("quality", "annotation_quality", "extensions"),
    [
        ("minor", 2, []),
        ("dominant7", 5, [7]),
        ("halfDim7", 6, [7]),
        ("sus4", 7, [4]),
        ("dominant13", 5, [7, 9, 13]),
        ("unknown", 0, []),
    ],
)
def test_named_qualities_map_to_their_family_and_degrees(
    quality: str, annotation_quality: int, extensions: list[int]
) -> None:
    result = chord_symbol_from_analysis(chord(quality, root=7))
    assert result == {
        "root_pc": 7,
        "quality": annotation_quality,
        "extensions": extensions,
        "slash_bass_pc": 255,
    }


def test_a_bass_below_the_root_is_a_slash_and_bass_equal_to_root_is_not() -> None:
    assert chord_symbol_from_analysis(chord("major", root=0, bass=4))["slash_bass_pc"] == 4
    assert chord_symbol_from_analysis(chord("major", root=0, bass=0))["slash_bass_pc"] == 255
    assert chord_symbol_from_analysis(chord("major", root=0, bass=None))["slash_bass_pc"] == 255


def test_every_quality_is_accepted_by_annotate_chords_and_survives_serialization() -> None:
    assert len(_CHORD_QUALITY_NAMES) == 25
    project = Project()
    entries = []
    for ordinal, name in _CHORD_QUALITY_NAMES.items():
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


@pytest.mark.parametrize("mode", list(Mode))
def test_key_modes_are_offset_by_one_and_accepted_by_annotate_keys(mode: Mode) -> None:
    converted = key_mode_from_analysis(mode)
    assert converted == int(mode) + 1
    project = Project()
    project.annotate_keys([(0.0, 480.0, 0, converted)])
    serialized = project.to_json()
    assert Project.from_json(serialized).to_json() == serialized


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


@pytest.mark.parametrize("mode", [7, -1])
def test_an_out_of_range_mode_is_refused_naming_the_field(mode: int) -> None:
    with pytest.raises(SonareValueError, match="mode"):
        key_mode_from_analysis(mode)


def test_a_non_integer_mode_is_a_type_error() -> None:
    with pytest.raises(TypeError):
        key_mode_from_analysis("major")  # type: ignore[arg-type]
