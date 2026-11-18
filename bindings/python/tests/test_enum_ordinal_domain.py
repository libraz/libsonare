"""A closed set takes its token spelling only; an ordinal never reaches the C ABI.

The C ABI is not uniform about what it does with an ordinal outside its field's
domain: where it substitutes a default, the caller gets a successful call that
did something other than what was asked, with nothing in the result to say so.
A ``bool`` is the same defect wearing a different type -- it satisfies
``isinstance(value, int)`` and would read as 0 or 1. So no ordinal is taken.

Each case opens with a positive control -- distinct tokens resolving
differently. Without it a resolver that ignored its argument would read as one
that accepted it, and the refusals below would mean nothing.
"""

from __future__ import annotations

from collections.abc import Callable, Sequence

import pytest

from libsonare import SonareValueError
from libsonare._project_model import _automation_target_kind_value, _track_kind_value
from libsonare._runtime import (
    _mode_values,
    _profile_value,
    _warp_mode_value,
)

from ._helpers import LIB_AVAILABLE

# Deliberately untyped in its argument: every case below drives a resolver with
# a value its own signature does not admit.
Resolver = Callable[..., int]

# (resolver, the field it names in its error, its tokens in C ordinal order,
# spellings it once took)
_FAMILIES: Sequence[tuple[Resolver, str, Sequence[str], Sequence[str]]] = (
    (
        _warp_mode_value,
        "warp mode",
        ("off", "repitch", "tempoSync", "timeStretch"),
        ("tempo-sync", "time-stretch", "Off"),
    ),
    (_track_kind_value, "track kind", ("audio", "midi", "aux"), ("Audio", "MIDI")),
    (
        _automation_target_kind_value,
        "automation target kind",
        ("opaque", "trackFaderDb", "trackPan"),
        ("track-fader-db", "track_pan", "OPAQUE"),
    ),
)

_FAMILY_IDS = [what.replace(" ", "_") for _, what, _, _ in _FAMILIES]
_PARAMS = ("resolve", "what", "tokens", "retired")


@pytest.mark.parametrize(_PARAMS, _FAMILIES, ids=_FAMILY_IDS)
def test_each_token_resolves_to_its_ordinal(
    resolve: Resolver, what: str, tokens: Sequence[str], retired: Sequence[str]
) -> None:
    assert [resolve(token) for token in tokens] == list(range(len(tokens))), what


@pytest.mark.parametrize(_PARAMS, _FAMILIES, ids=_FAMILY_IDS)
def test_an_ordinal_or_a_bool_is_a_type_error(
    resolve: Resolver, what: str, tokens: Sequence[str], retired: Sequence[str]
) -> None:
    for value in (0, 1, 99, -1, 1.0, True, False, None):
        with pytest.raises(TypeError, match=what):
            resolve(value)


@pytest.mark.parametrize(_PARAMS, _FAMILIES, ids=_FAMILY_IDS)
def test_a_retired_spelling_lists_the_tokens(
    resolve: Resolver, what: str, tokens: Sequence[str], retired: Sequence[str]
) -> None:
    for spelling in retired:
        with pytest.raises(SonareValueError, match=what) as excinfo:
            resolve(spelling)
        message = str(excinfo.value)
        assert repr(spelling) in message
        for token in tokens:
            assert repr(token) in message


def test_a_key_ordinal_is_never_accepted_in_place_of_its_token() -> None:
    """The key-detection resolvers take the token spelling only, never an ordinal."""
    # Positive control: both routes carry a legal token through.
    assert _mode_values(["minor"]) == [1] and _profile_value("krumhanslSchmuckler") == 0
    # Omitted is SONARE_KEY_PROFILE_AUTO; the same -1 given as an ordinal is refused.
    assert _profile_value(None) == -1
    with pytest.raises(TypeError, match="mode"):
        _mode_values([1])  # type: ignore[list-item]
    with pytest.raises(TypeError, match="key profile"):
        _profile_value(0)  # type: ignore[arg-type]
    with pytest.raises(TypeError, match="key profile"):
        _profile_value(-1)  # type: ignore[arg-type]


@pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")
def test_a_refused_warp_mode_never_reaches_the_arrangement() -> None:
    """The same guard seen from the public surface, against a serialized project."""
    from libsonare import Project

    project = Project()
    try:
        track = project.add_track("audio", "gtr")
        clip = project.add_clip(
            track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_channels=1, audio_sample_rate=48000
        )
        project.set_clip_warp_mode(clip, "tempoSync")
        by_name = project.to_json()
        project.set_clip_warp_mode(clip, "repitch")
        assert project.to_json() != by_name  # positive control: the mode reaches the JSON
        project.set_clip_warp_mode(clip, "tempoSync")
        assert project.to_json() == by_name

        for refused in (2, 4, True, "tempo-sync"):
            expected = SonareValueError if isinstance(refused, str) else TypeError
            with pytest.raises(expected, match="warp mode"):
                project.set_clip_warp_mode(clip, refused)  # type: ignore[arg-type]
        assert project.to_json() == by_name
    finally:
        project.close()


@pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")
def test_a_track_kind_is_a_token_on_add_and_on_change() -> None:
    from libsonare import Project

    project = Project()
    try:
        track = project.add_track("midi", "keys")
        project.set_track_kind(track, "aux")
        assert '"kind":2' in project.to_json()
        before = project.to_json()
        with pytest.raises(TypeError, match="track kind"):
            project.add_track(1, "x")  # type: ignore[arg-type]
        with pytest.raises(TypeError, match="track kind"):
            project.set_track_kind(track, 0)  # type: ignore[arg-type]
        assert project.to_json() == before
    finally:
        project.close()
