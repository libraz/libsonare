"""An enum ordinal outside its field's domain is refused, not forwarded to the C ABI.

The C ABI is not uniform about what it does with one: where it substitutes a
default, the caller gets a successful call that did something other than what was
asked, with nothing in the result to say so. A ``bool`` is the same defect wearing
a different type -- it satisfies ``isinstance(value, int)`` and resolves to the
ordinal 0 or 1, which is a legal value in every one of these tables.

Each family opens with a positive control -- a name and an ordinal resolving
alike, and distinct names resolving differently. Without it a resolver that
ignored its argument would read as one that accepted it, and the refusals below
would mean nothing.
"""

from __future__ import annotations

from collections.abc import Callable, Sequence

import pytest

from libsonare import SonareValueError
from libsonare._project_model import _track_kind_value
from libsonare._runtime import (
    _mode_values,
    _profile_value,
    _warp_mode_value,
)

from ._helpers import LIB_AVAILABLE

# Deliberately untyped in its argument: every case below drives a resolver with
# a value its own signature does not admit.
Resolver = Callable[..., int]

# (resolver, the field it names in its error, legal (name, ordinal) pairs,
# accepted ordinals no spelling covers, ordinals outside the field's domain)
_Family = tuple[Resolver, str, Sequence[tuple[str, int]], Sequence[int], Sequence[int]]

_FAMILIES: Sequence[_Family] = (
    (
        _warp_mode_value,
        "warp mode",
        (("off", 0), ("repitch", 1), ("tempo-sync", 2), ("time-stretch", 3)),
        (),
        (4, 99, -1),
    ),
    (_track_kind_value, "track kind", (("audio", 0), ("midi", 1), ("aux", 2)), (), (3, 99, -1)),
)

_FAMILY_IDS = [
    what.replace(" ", "_") + f"_{index}" for index, (_, what, _, _, _) in enumerate(_FAMILIES)
]


_PARAMS = ("resolve", "what", "legal", "unspelled", "out_of_domain")


@pytest.mark.parametrize(_PARAMS, _FAMILIES, ids=_FAMILY_IDS)
def test_a_name_and_its_ordinal_resolve_alike(
    resolve: Resolver,
    what: str,
    legal: Sequence[tuple[str, int]],
    unspelled: Sequence[int],
    out_of_domain: Sequence[int],
) -> None:
    """The positive control: the resolver reads its argument rather than ignoring it."""
    for name, ordinal in legal:
        assert resolve(name) == resolve(ordinal) == ordinal, what
    assert len({resolve(name) for name, _ in legal}) == len(legal), f"{what} collapses its names"


@pytest.mark.parametrize(_PARAMS, _FAMILIES, ids=_FAMILY_IDS)
def test_an_ordinal_outside_the_domain_is_refused(
    resolve: Resolver,
    what: str,
    legal: Sequence[tuple[str, int]],
    unspelled: Sequence[int],
    out_of_domain: Sequence[int],
) -> None:
    """The refusal names the field, quotes the value, and lists what would be taken."""
    for ordinal in out_of_domain:
        with pytest.raises(SonareValueError, match=what) as excinfo:
            resolve(ordinal)
        message = str(excinfo.value)
        assert str(ordinal) in message
        assert "expected one of" in message
        for name, _ in legal:
            assert repr(name) in message


@pytest.mark.parametrize(_PARAMS, _FAMILIES, ids=_FAMILY_IDS)
def test_a_bool_is_refused_rather_than_read_as_zero_or_one(
    resolve: Resolver,
    what: str,
    legal: Sequence[tuple[str, int]],
    unspelled: Sequence[int],
    out_of_domain: Sequence[int],
) -> None:
    """Both spellings, because 0 and 1 are legal ordinals in every table here."""
    for value in (True, False):
        with pytest.raises(TypeError, match=what):
            resolve(value)


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
def test_a_refused_ordinal_never_reaches_the_arrangement() -> None:
    """The same guard seen from the public surface, against a serialized project."""
    from libsonare import Project

    project = Project()
    try:
        track = project.add_track("audio", "gtr")
        clip = project.add_clip(
            track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_sample_rate=48000
        )
        project.set_clip_warp_mode(clip, "tempo-sync")
        by_name = project.to_json()
        project.set_clip_warp_mode(clip, 1)
        assert project.to_json() != by_name  # positive control: the mode reaches the JSON
        project.set_clip_warp_mode(clip, 2)
        assert project.to_json() == by_name

        for ordinal in (4, 99, -1, True):
            expected = TypeError if isinstance(ordinal, bool) else SonareValueError
            with pytest.raises(expected, match="warp mode"):
                project.set_clip_warp_mode(clip, ordinal)
        assert project.to_json() == by_name
    finally:
        project.close()
