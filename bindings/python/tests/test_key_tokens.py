"""Key mode, key profile and section type are string tokens, never ordinals."""

from __future__ import annotations

import math
from typing import get_args

import pytest

import libsonare
from libsonare import SonareValueError
from libsonare._runtime import _mode_value, _mode_values, _profile_value, _token_ordinal
from libsonare.types import KeyMode, KeyProfile, SectionType

from ._helpers import LIB_AVAILABLE

SR = 22050


def _chord_tone(seconds: float = 1.5) -> list[float]:
    freqs = (261.63, 329.63, 392.0)
    return [
        0.25 * sum(math.sin(2 * math.pi * f * i / SR) for f in freqs)
        for i in range(int(SR * seconds))
    ]


def test_token_tuples_are_the_literal_arguments_in_ordinal_order() -> None:
    assert get_args(KeyMode) == (
        "major",
        "minor",
        "dorian",
        "phrygian",
        "lydian",
        "mixolydian",
        "locrian",
    )
    assert get_args(KeyProfile)[0] == "krumhanslSchmuckler"
    assert len(get_args(KeyProfile)) == 7
    assert get_args(SectionType)[2] == "preChorus"
    assert get_args(SectionType)[-1] == "unknown"
    assert len(get_args(SectionType)) == 8


def test_every_token_resolves_to_its_ordinal() -> None:
    for ordinal, token in enumerate(get_args(KeyMode)):
        assert _mode_value(token) == ordinal  # type: ignore[arg-type]
    for ordinal, token in enumerate(get_args(KeyProfile)):
        assert _profile_value(token) == ordinal  # type: ignore[arg-type]
    assert _profile_value(None) == -1


@pytest.mark.parametrize("value", [0, 1, True, 1.5, None, b"major", ("major",)])
def test_a_non_string_mode_is_a_type_error(value: object) -> None:
    with pytest.raises(TypeError):
        _mode_value(value)  # type: ignore[arg-type]


@pytest.mark.parametrize("value", [0, 6, True, 1.5, b"temperley"])
def test_a_non_string_profile_is_a_type_error(value: object) -> None:
    with pytest.raises(TypeError):
        _profile_value(value)  # type: ignore[arg-type]


@pytest.mark.parametrize("value", ["Major", "MAJOR", "maj", "m", "min", "minor ", ""])
def test_an_unknown_or_legacy_mode_names_the_accepted_tokens(value: str) -> None:
    with pytest.raises(SonareValueError) as info:
        _mode_value(value)  # type: ignore[arg-type]
    for token in get_args(KeyMode):
        assert token in str(info.value)


@pytest.mark.parametrize(
    "value",
    ["ks", "krumhansl", "krumhansl-schmuckler", "keyfinder", "edmt", "faraldo-edma", "bellman"],
)
def test_an_unknown_or_legacy_profile_names_the_accepted_tokens(value: str) -> None:
    with pytest.raises(SonareValueError) as info:
        _profile_value(value)  # type: ignore[arg-type]
    for token in get_args(KeyProfile):
        assert token in str(info.value)


def test_a_mode_list_resolves_each_entry_and_names_a_group() -> None:
    assert _mode_values("detect_key", ["dorian", "locrian"]) == [2, 6]
    assert _mode_values("detect_key", "majorMinor") == [0, 1]
    assert _mode_values("detect_key", "all") == list(range(7))
    with pytest.raises(TypeError):
        _mode_values("detect_key", [1])  # type: ignore[list-item]
    with pytest.raises(SonareValueError):
        _mode_values("detect_key", "major-minor")


def test_the_resolver_returns_the_index_in_the_given_tokens() -> None:
    assert _token_ordinal("b", ("a", "b"), "probe") == 1
    with pytest.raises(SonareValueError, match="probe"):
        _token_ordinal("c", ("a", "b"), "probe")


@pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")
def test_scale_mask_for_c_major_is_the_numeric_mask() -> None:
    assert libsonare.scale_mask_for_mode(0, "major") == 0xAB5 == 2741
    with pytest.raises(TypeError):
        libsonare.scale_mask_for_mode(0, 0)  # type: ignore[arg-type]
    with pytest.raises(SonareValueError):
        libsonare.scale_mask_for_mode(0, "ionian")  # type: ignore[arg-type]


@pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")
def test_a_detected_key_mode_is_a_token() -> None:
    key = libsonare.detect_key(_chord_tone(), SR)
    assert key.mode in get_args(KeyMode)
    assert str(key).endswith(key.mode)
    candidates = libsonare.detect_key_candidates(_chord_tone(), SR, modes="all")
    assert candidates
    assert all(candidate.key.mode in get_args(KeyMode) for candidate in candidates)


@pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")
@pytest.mark.parametrize("profile", get_args(KeyProfile))
def test_every_key_profile_token_is_accepted_by_detect_key(profile: KeyProfile) -> None:
    key = libsonare.detect_key(_chord_tone(), SR, profile=profile)
    assert key.mode in get_args(KeyMode)


@pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")
def test_detect_key_refuses_an_ordinal_and_a_legacy_name() -> None:
    tone = _chord_tone(0.5)
    with pytest.raises(TypeError):
        libsonare.detect_key(tone, SR, profile=0)  # type: ignore[arg-type]
    with pytest.raises(SonareValueError, match="krumhanslSchmuckler"):
        libsonare.detect_key(tone, SR, profile="ks")  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        libsonare.detect_key(tone, SR, modes=[0])  # type: ignore[list-item]
    with pytest.raises(SonareValueError, match="locrian"):
        libsonare.detect_key(tone, SR, modes=["maj"])  # type: ignore[list-item]


@pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")
@pytest.mark.parametrize("fn_name", ["detect_key", "detect_key_candidates"])
@pytest.mark.parametrize("empty", [[], ()], ids=["list", "tuple"])
def test_an_empty_modes_set_is_refused_rather_than_read_as_the_default(
    fn_name: str, empty: list[KeyMode] | tuple[KeyMode, ...]
) -> None:
    fn = getattr(libsonare, fn_name)
    with pytest.raises(SonareValueError, match=f"^{fn_name}: modes must not be empty$"):
        fn(_chord_tone(0.5), SR, modes=empty)


@pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")
def test_section_types_are_tokens() -> None:
    tone = _chord_tone(6.0) + [0.5 * v for v in _chord_tone(6.0)]
    sections = libsonare.analyze_sections(tone, sample_rate=SR, min_section_sec=2.0)
    assert sections
    assert all(section.type in get_args(SectionType) for section in sections)
    result = libsonare.analyze(tone, SR)
    assert all(section.type in get_args(SectionType) for section in result.sections)
