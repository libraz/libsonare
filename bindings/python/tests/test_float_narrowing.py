"""A caller's float is refused, not saturated into a legal one.

A Python float is an IEEE double, so ``ctypes.c_float`` halves the exponent
range without raising: ``1e40`` arrives as ``inf`` and ``-1e40`` as ``-inf``.
That is not the integer family's wrap, and it is the same defect -- the value
the caller asked for is folded onto another legal one, and every field whose
contract reads a non-finite input as "unspecified" reads the folded value as a
deliberate request. A NaN or an infinity handed in directly lands on the same
reading, so it belongs to the same set.

Each case drives one argument behind one reader, and each opens with a positive
control: two legitimate values, at least one of them fractional, whose results
differ. Without it an accepted out-of-range value cannot be told from an
argument the entry point never reads.

The refusal is asserted by which argument it names, not by the fact that one was
raised. An entry point that ignores an argument would otherwise pass every case
on the refusal its neighbour produced.
"""

from __future__ import annotations

import re
from collections.abc import Sequence

import numpy as np
import pytest

import libsonare as ls
from libsonare import SonareValueError

SAMPLE_RATE = 22050

# Saturating: finite as a double, an infinity as a float32. The last two are
# integers, which the readers accept, and the larger has no double at all.
SATURATING = (1e40, -1e40, 3.5e38, -3.5e38, 10**40, 10**400)
NON_FINITE = (float("inf"), float("-inf"), float("nan"))
REFUSED = SATURATING + NON_FINITE


@pytest.fixture(scope="module")
def tone() -> np.ndarray:
    n = SAMPLE_RATE // 4
    return (0.3 * np.sin(2 * np.pi * 220 * np.arange(n) / SAMPLE_RATE)).astype(np.float32)


def _named_argument(message: str, candidates: Sequence[str]) -> str:
    """The one candidate the refusal names, as a whole word.

    ``midi`` inside ``scale_quantize_midi`` and inside ``reference_midi`` is not
    the argument ``midi``, so a plain substring test would let a refusal about
    one argument stand in for a refusal about another.
    """
    named = [c for c in candidates if re.search(rf"(?<![A-Za-z0-9_]){c}(?![A-Za-z0-9_])", message)]
    assert len(named) == 1, f"expected exactly one of {tuple(candidates)} in {message!r}: {named}"
    return named[0]


def _refuses(argument: str, others: Sequence[str], call, *args, **kwargs) -> None:
    """Assert the call is refused and that the refusal names ``argument``."""
    with pytest.raises(SonareValueError) as excinfo:
        call(*args, **kwargs)
    assert _named_argument(str(excinfo.value), (argument, *others)) == argument


def test_a_saturating_frequency_is_refused_rather_than_read_as_an_infinity() -> None:
    """The scalar reader, on the shortest path there is into the library."""
    assert ls.hz_to_mel(440.0) != ls.hz_to_mel(880.5)  # positive control
    for value in REFUSED:
        _refuses("hz", (), ls.hz_to_mel, value)


def test_a_saturating_midi_number_is_refused_on_the_argument_that_carried_it() -> None:
    """Two float arguments behind one reader, so each refusal has to name itself."""
    mask = 0b101010110101
    assert ls.scale_quantize_midi(0, mask, 61.4) != ls.scale_quantize_midi(0, mask, 66.7)
    assert ls.scale_quantize_midi(0, mask, 61.4, 69.0) != ls.scale_quantize_midi(
        0, mask, 61.4, 68.25
    )  # positive control on the second argument
    for value in REFUSED:
        _refuses("midi", ("reference_midi",), ls.scale_quantize_midi, 0, mask, value)
        _refuses("reference_midi", ("midi",), ls.scale_quantize_midi, 0, mask, 61.4, value)


def test_a_saturating_mel_band_edge_is_refused_on_its_own_argument(tone) -> None:
    """The analysis path, where fmin and fmax reach the same C call together."""

    def mel(**kwargs):
        return ls.mel_spectrogram(
            tone, sample_rate=SAMPLE_RATE, n_fft=512, hop_length=256, n_mels=8, **kwargs
        )

    assert mel(fmin=0.0) != mel(fmin=200.5)  # positive control
    assert mel(fmax=4000.0) != mel(fmax=6000.25)  # positive control
    for value in REFUSED:
        _refuses("fmin", ("fmax",), mel, fmin=value)
        _refuses("fmax", ("fmin",), mel, fmax=value)


def test_a_saturating_normalization_target_is_refused(tone) -> None:
    """The offline effects path."""
    quiet = ls.normalize(tone, SAMPLE_RATE, -20.0)
    loud = ls.normalize(tone, SAMPLE_RATE, -6.5)
    assert max(quiet) != max(loud)  # positive control
    for value in REFUSED:
        _refuses("target_db", (), ls.normalize, tone, SAMPLE_RATE, value)


def test_a_saturating_pitch_shift_is_refused(tone) -> None:
    """The editing path, whose argument is a musical quantity rather than a gain."""
    assert ls.pitch_shift(tone, SAMPLE_RATE, 2.0) != ls.pitch_shift(tone, SAMPLE_RATE, 4.5)
    for value in REFUSED:
        _refuses("semitones", (), ls.pitch_shift, tone, SAMPLE_RATE, value)


def test_a_saturating_fader_is_refused(mixer_scene) -> None:
    """The handle path: the value reaches a live strip rather than a one-shot call."""
    mixer_scene.set_fader_db("vocal", -6.0)
    mixer_scene.set_fader_db("vocal", -3.25)  # positive control: both are accepted
    for value in REFUSED:
        _refuses("db", (), mixer_scene.set_fader_db, "vocal", value)


def test_a_saturating_automation_value_is_refused(mixer_scene) -> None:
    """The scheduling path, where the float sits among four integer readers."""
    mixer_scene.schedule_insert_automation(0, 0, 0, 0, 0.5, 0)
    mixer_scene.schedule_insert_automation(0, 0, 0, 0, 0.75, 0)  # positive control
    for value in REFUSED:
        _refuses(
            "value",
            ("insert_index", "param_id", "sample_pos"),
            mixer_scene.schedule_insert_automation,
            0,
            0,
            0,
            0,
            value,
            0,
        )


def test_a_saturating_controller_value_is_refused() -> None:
    """The MIDI path, where the argument is already a unit-range quantity."""
    events = [ls.Project.midi_cc(0.0, 0, 2, 74, 60), ls.Project.midi_cc(0.1, 0, 2, 74, 70)]
    binding = ls.Project.midi_cc_learn(events, 77)
    assert binding is not None
    first = ls.Project.midi_param_to_cc([binding], 77, 0.25, 0)
    second = ls.Project.midi_param_to_cc([binding], 77, 0.75, 0)
    assert first is not None and second is not None and first != second  # positive control
    for value in REFUSED:
        _refuses(
            "unit_value",
            ("param_id", "group"),
            ls.Project.midi_param_to_cc,
            [binding],
            77,
            value,
            0,
        )


def test_a_saturating_retune_control_is_refused_on_the_argument_that_carried_it() -> None:
    """The streaming path, whose native refusal cannot say which control was wrong."""
    with ls.StreamingRetune(semitones=2.0, mix=1.0) as retune:
        retune.prepare(float(SAMPLE_RATE), 256)
        assert retune.config()["semitones"] == pytest.approx(2.0)  # positive control
        retune.set_config(semitones=-4.5)
        assert retune.config()["semitones"] == pytest.approx(-4.5)  # positive control
        for value in REFUSED:
            _refuses("semitones", ("mix",), retune.set_config, semitones=value)
            _refuses("mix", ("semitones",), retune.set_config, mix=value)
    for value in REFUSED:
        _refuses("semitones", ("mix",), ls.StreamingRetune, semitones=value)
        _refuses("mix", ("semitones",), ls.StreamingRetune, mix=value)


def test_a_saturating_project_edit_control_is_refused_on_its_own_argument() -> None:
    """The project edit ops, whose own finiteness check ran before the conversion.

    A double past the float32 range is finite, so it passed that check and
    saturated on the way to ``c_float``; the core then refused the infinity
    without naming the argument that carried it.
    """
    project = ls.Project()
    track = project.add_track("audio", "a")
    clip = project.add_clip(track, 0.0, 480.0, audio=[0.1, 0.2], audio_sample_rate=48000)

    def applied(edit, *args) -> str:
        edit(*args)
        result = project.to_json()
        project.undo()
        return result

    largest = 3.4028234663852886e38
    # Positive controls: the bound is the float32 range, not a guess at a
    # plausible gain, so the largest representable value is still accepted.
    assert applied(project.set_track_gain, track, 0.25) != applied(
        project.set_track_gain, track, largest
    )
    assert applied(project.set_track_pan, track, -1.0) != applied(
        project.set_track_pan, track, 0.75
    )
    assert applied(project.set_clip_gain, clip, 0.25) != applied(
        project.set_clip_gain, clip, largest
    )
    for value in REFUSED:
        _refuses("gain", ("track_id",), project.set_track_gain, track, value)
        _refuses("pan", ("track_id",), project.set_track_pan, track, value)
        _refuses("gain", ("clip_id",), project.set_clip_gain, clip, value)


def test_a_representable_extreme_is_still_accepted() -> None:
    """The bound is the float32 range, not a guess at a plausible one."""
    largest = 3.4028234663852886e38
    assert ls.hz_to_mel(largest) == ls.hz_to_mel(largest)
    assert np.isfinite(ls.hz_to_mel(-largest))


@pytest.fixture
def mixer_scene():
    scene = ls.mixing_scene_preset_json(ls.mixing_scene_preset_names()[0])
    mixer = ls.Mixer.from_scene_json(scene, sample_rate=48000, block_size=256)
    try:
        yield mixer
    finally:
        mixer.close()


def test_a_non_finite_double_argument_is_refused_by_name() -> None:
    """The two ``c_double`` arguments, which had no narrowing path at all.

    Both sat in a bare ``ctypes.c_double(...)`` while every sibling argument on
    the same call was narrowed, because the family had no double half to route
    them through. A Python float is already an IEEE double, so the missing
    guard was never about range -- it was that a NaN or an infinity reached the
    core unexamined.

    Measured, and it is why this case does not assert the float32/double
    distinction: the core's own ``ppq`` domain is far inside the float32 range
    (1e6 is accepted, 3.5e38 is refused by the core with INVALID_PARAMETER), so
    no value distinguishes a double narrower from a float32 one here. The
    observable change at these two sites is the finiteness refusal, and the
    type match is for the conversion's own correctness rather than for reach.
    """
    events = [ls.Project.midi_cc(0.0, 0, 2, 74, 60), ls.Project.midi_cc(0.1, 0, 2, 74, 70)]
    binding = ls.Project.midi_cc_learn(events, 77)
    assert binding is not None
    # Positive control: the argument under test reaches the core and is used.
    assert ls.Project.midi_param_to_cc([binding], 77, 0.25, 0, 480.0) is not None
    for value in NON_FINITE:
        _refuses(
            "ppq",
            ("param_id", "unit_value", "group"),
            ls.Project.midi_param_to_cc,
            [binding],
            77,
            0.25,
            0,
            value,
        )

    with ls.StreamingRetune(semitones=2.0, mix=1.0) as retune:
        retune.prepare(float(SAMPLE_RATE), 256)  # positive control
        for value in NON_FINITE:
            _refuses("sample_rate", ("max_block_size",), retune.prepare, value, 256)
