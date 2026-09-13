"""An enum ordinal outside its field's domain is refused, not forwarded to the C ABI.

The C ABI is not uniform about what it does with one: where it substitutes a
default, the caller gets a successful call that did something other than what was
asked, with nothing in the result to say so. A ``bool`` is the same defect wearing
a different type -- it satisfies ``isinstance(value, int)`` and resolves to the
ordinal 0 or 1, which is a legal value in every one of these tables.

The domain is the field's, not the spelling table's, and for every table here but
one they are the same set. The exception is the SF2 ``sampleModes`` field, whose
reserved 2 is a value a zone really carries and which the core reads as no loop.
That separates a documented alternative from a silent substitution: both arrive
at the same catch-all in the core, so the only place they can be told apart is
the entry point.

Each family opens with a positive control -- a name and an ordinal resolving
alike, and distinct names resolving differently. Without it a resolver that
ignored its argument would read as one that accepted it, and the refusals below
would mean nothing.
"""

from __future__ import annotations

from collections.abc import Callable, Sequence

import pytest

from libsonare import AutomationCurve, MeterTap, PanLaw, SendTiming, SonareValueError
from libsonare._project_model import _loop_mode_value, _track_kind_value
from libsonare._project_synth import (
    _sample_desc_loop_value,
    _sample_key_track_value,
    _sample_loop_value,
)
from libsonare._runtime import (
    _curve_value,
    _meter_tap_value,
    _mode_values,
    _pan_law_value,
    _pan_mode_value,
    _profile_value,
    _send_timing_value,
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
        _pan_mode_value,
        "pan mode",
        (("balance", 0), ("stereo-pan", 1), ("dual-pan", 2)),
        (),
        (3, 99, -1),
    ),
    (
        _pan_law_value,
        "pan law",
        (("const3db", 0), ("const4.5db", 1), ("const6db", 2), ("linear", 3)),
        (),
        (4, 99, -1),
    ),
    (_meter_tap_value, "meter tap", (("pre-fader", 0), ("post-fader", 1)), (), (2, 99, -1)),
    (_send_timing_value, "send timing", (("post-fader", 0), ("pre-fader", 1)), (), (2, 99, -1)),
    (
        _curve_value,
        "automation curve",
        (("linear", 0), ("exponential", 1), ("hold", 2), ("s-curve", 3)),
        (),
        (4, 99, -1),
    ),
    (
        _warp_mode_value,
        "warp mode",
        (("off", 0), ("repitch", 1), ("tempo-sync", 2), ("time-stretch", 3)),
        (),
        (4, 99, -1),
    ),
    (_track_kind_value, "track kind", (("audio", 0), ("midi", 1), ("aux", 2)), (), (3, 99, -1)),
    (_loop_mode_value, "loop mode", (("off", 0), ("loop", 1)), (), (2, 99, -1)),
    (
        _sample_loop_value,
        "sample loop mode",
        (("default", 0), ("none", 1), ("continuous", 2), ("key-down", 3)),
        (),
        (4, 99, -1),
    ),
    (
        _sample_key_track_value,
        "sample key track",
        (("default", 0), ("on", 1), ("off", 2)),
        (),
        (3, 99, -1),
    ),
    # The only table with an accepted ordinal it does not spell: SF2 sampleModes
    # is two bits wide, and its reserved 2 means no loop.
    (
        _sample_desc_loop_value,
        "sample loop mode",
        (("none", 0), ("continuous", 1), ("key-down", 3)),
        (2,),
        (4, 99, -1),
    ),
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
def test_an_ordinal_the_field_accepts_without_spelling_is_kept_as_it_stands(
    resolve: Resolver,
    what: str,
    legal: Sequence[tuple[str, int]],
    unspelled: Sequence[int],
    out_of_domain: Sequence[int],
) -> None:
    """It arrives as itself, not folded onto the spelling that shares its meaning.

    Folding it would be the resolver deciding what a reserved value means; the
    core is what reads it, and the ordinal is what carries it there.
    """
    for ordinal in unspelled:
        assert resolve(ordinal) == ordinal
        assert ordinal not in {value for _, value in legal}


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
        with pytest.raises(SonareValueError, match=what):
            resolve(value)


@pytest.mark.parametrize(
    ("resolve", "member"),
    (
        (_curve_value, AutomationCurve.S_CURVE),
        (_pan_law_value, PanLaw.LINEAR_0DB),
        (_meter_tap_value, MeterTap.POST_FADER),
        (_send_timing_value, SendTiming.PRE_FADER),
    ),
)
def test_an_enum_member_still_resolves_to_its_own_ordinal(resolve: Resolver, member: int) -> None:
    """The enum branch runs before the integer one, so validation cannot shadow it."""
    assert resolve(member) == int(member)


@pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")
def test_the_reserved_sample_loop_ordinal_renders_as_no_loop() -> None:
    """The reserved 2 reaches the core and is read there, not dropped on the way.

    Accepting an ordinal and ignoring it are indistinguishable at the resolver,
    so the separation has to be made where the value is consumed: 2 must render
    like the unlooped 0 and unlike the continuous 1.
    """
    import numpy as np

    from libsonare import Project, SampleBank, SynthPatch

    sample_rate = 48000
    frames = 2400  # 50 ms, far shorter than the note, so a loop is audible past it
    tone = (0.5 * np.sin(2.0 * np.pi * 440.0 * np.arange(frames) / sample_rate)).astype(np.float32)

    def render(loop_mode: int) -> np.ndarray:
        project = Project()
        project.set_sample_rate(float(sample_rate))
        track, clip = project.add_midi_clip(0.0, 4.0)
        project.set_track_midi_destination(track, 0)
        project.set_midi_events(
            clip,
            [
                Project.midi_note_on(0.0, 0, 0, 60, 100),
                Project.midi_note_off(2.0, 0, 0, 60, 0),
            ],
        )
        try:
            with SampleBank() as bank:
                index = bank.add_sample(
                    tone,
                    root_key=60,
                    source_rate=float(sample_rate),
                    loop_start=0,
                    loop_end=frames,
                    loop_mode=loop_mode,
                )
                bank.add_zone(0, sample_index=index)
                out = project.bounce_with_synth_instrument(
                    SynthPatch(engine_mode="sample", sample_set=0, sample_bank=bank),
                    total_frames=12000,
                    num_channels=1,
                    sample_rate=sample_rate,
                )
        finally:
            project.close()
        return np.asarray(out, dtype=np.float32)

    unlooped, looped, reserved = render(0), render(1), render(2)
    # Two positive controls: the sample sounded at all, and the field reaches the
    # render -- without the second, every render being equal would satisfy this.
    assert float(np.max(np.abs(unlooped))) > 0.0
    assert not np.array_equal(looped, unlooped)
    # Whole buffer, not a windowed level: the unlooped renders are silent in the
    # tail either way, so only sample-for-sample identity separates "read as no
    # loop" from "produced silence for some other reason".
    assert np.array_equal(reserved, unlooped)


def test_a_key_enum_ordinal_is_bounded_by_its_enum_rather_than_by_a_name_table() -> None:
    """The key-detection resolvers reach the C ABI through IntEnum, not a spelling table.

    They take the integer route before any name lookup, so the table's coverage
    is not what stops an out-of-range ordinal there -- the constructor is.
    """
    # Positive control: both routes carry a legal value through.
    assert _mode_values([1]) == [1] and _profile_value(None) == 0  # type: ignore[list-item]
    with pytest.raises(ValueError, match="Mode"):
        _mode_values([99])  # type: ignore[list-item]
    with pytest.raises(ValueError, match="KeyProfile"):
        _profile_value(99)  # type: ignore[arg-type]


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
            with pytest.raises(SonareValueError, match="warp mode"):
                project.set_clip_warp_mode(clip, ordinal)
        assert project.to_json() == by_name
    finally:
        project.close()
