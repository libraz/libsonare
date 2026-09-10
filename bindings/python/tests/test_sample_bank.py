"""Sample-bank binding tests: SampleBank, the SynthPatch sample fields, and the
sample_bank argument on Project.bounce_with_synth_instrument and
RealtimeEngine.set_synth_instrument."""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import (
    Project,
    RealtimeEngine,
    SampleBank,
    SonareError,
    SynthPatch,
    synth_enum_tables,
)
from libsonare._project import SYNTH_ENUM_TABLES

SAMPLE_RATE = 48000


def _tone(freq: float = 440.0, frames: int = 4800) -> np.ndarray:
    """A short mono float32 tone, loud enough that silence is unambiguous."""
    return (0.5 * np.sin(2.0 * np.pi * freq * np.arange(frames) / SAMPLE_RATE)).astype(np.float32)


def _one_note_project(note: int) -> Project:
    project = Project()
    project.set_sample_rate(float(SAMPLE_RATE))
    track, clip = project.add_midi_clip(0.0, 1.0)
    project.set_track_midi_destination(track, 0)
    project.set_midi_events(
        clip,
        [
            Project.midi_note_on(0.0, 0, 0, note, 100),
            Project.midi_note_off(0.5, 0, 0, note, 0),
        ],
    )
    return project


def _sample_patch(
    bank: SampleBank | None,
    *,
    set_index: int = 0,
    sample_level: float | None = None,
) -> SynthPatch:
    """A patch voicing the sample engine, carrying its own bank as the C ABI does."""
    return SynthPatch(
        engine_mode="sample",
        sample_set=set_index,
        sample_level=sample_level,
        sample_bank=bank,
    )


def _render(note: int, patch: SynthPatch) -> np.ndarray:
    project = _one_note_project(note)
    try:
        return project.bounce_with_synth_instrument(
            patch,
            total_frames=12000,
            num_channels=1,
            sample_rate=SAMPLE_RATE,
        )
    finally:
        project.close()


def _peak(audio: np.ndarray) -> float:
    return float(np.max(np.abs(audio)))


def _is_silent(audio: np.ndarray) -> bool:
    """Below anything a bounce of a sounding voice reaches."""
    return _peak(audio) < 1.0e-9


# -- the engine-mode table --------------------------------------------------


def test_engine_mode_table_carries_the_sample_engine() -> None:
    assert SYNTH_ENUM_TABLES["engine_modes"][-1] == "sample"
    assert synth_enum_tables()["engine_modes"] == SYNTH_ENUM_TABLES["engine_modes"]
    assert SynthPatch(engine_mode="sample")._to_c().engine_mode == 17
    assert SynthPatch._from_c(SynthPatch(engine_mode=17)._to_c()).engine_mode == "sample"


# -- lifecycle --------------------------------------------------------------


def test_bank_counts_what_was_added_and_closes_deterministically() -> None:
    with SampleBank() as bank:
        assert bank.sample_count == 0
        assert bank.set_count == 0
        first = bank.add_sample(_tone())
        second = bank.add_sample(_tone(220.0))
        assert (first, second) == (0, 1)
        assert bank.sample_count == 2
        # Sets are dense: appending to set 2 creates 0 and 1 empty.
        bank.add_zone(2, sample_index=first)
        assert bank.set_count == 3
    with pytest.raises(RuntimeError, match="SampleBank is closed"):
        bank.add_zone(0, sample_index=0)
    bank.close()  # idempotent


# -- argument rejections ----------------------------------------------------


def test_add_sample_rejects_bad_buffers_and_out_of_range_fields() -> None:
    with SampleBank() as bank:
        with pytest.raises(ValueError, match="must not be empty"):
            bank.add_sample(np.zeros(0, dtype=np.float32))
        with pytest.raises(ValueError, match="NaN or Inf"):
            bank.add_sample(np.array([0.0, np.nan], dtype=np.float32))
        with pytest.raises(ValueError, match="1-D buffer"):
            bank.add_sample(np.zeros((4, 2), dtype=np.float32))
        # ctypes would truncate these into the field silently.
        with pytest.raises(ValueError, match=r"root_key must be in \[0, 127\]"):
            bank.add_sample(_tone(), root_key=300)
        with pytest.raises(ValueError, match=r"loop_start must be in \[0, "):
            bank.add_sample(_tone(), loop_start=-1)
        with pytest.raises(ValueError, match="root_key must be an integer"):
            bank.add_sample(_tone(), root_key=60.5)  # type: ignore[arg-type]
        with pytest.raises(ValueError, match="source_rate must be a finite number"):
            bank.add_sample(_tone(), source_rate=float("inf"))
        with pytest.raises(ValueError, match="unknown sample loop mode"):
            bank.add_sample(_tone(), loop_mode="looping-forever")
        assert bank.sample_count == 0


def test_add_zone_rejects_what_the_binding_and_the_core_each_reject() -> None:
    with SampleBank() as bank:
        index = bank.add_sample(_tone())
        with pytest.raises(ValueError, match=r"key_hi must be in \[0, 255\]"):
            bank.add_zone(0, sample_index=index, key_hi=256)
        with pytest.raises(ValueError, match="pan_units must be a finite number"):
            bank.add_zone(0, sample_index=index, pan_units=float("nan"))
        with pytest.raises(ValueError, match=r"set_index must be in \[0, "):
            bank.add_zone(-1, sample_index=index)
        # Past the binding and rejected by the core.
        with pytest.raises(SonareError):
            bank.add_zone(0, sample_index=index + 1)
        with pytest.raises(SonareError):
            bank.add_zone(0, sample_index=index, key_lo=90, key_hi=30)
        with pytest.raises(SonareError):
            bank.add_zone(0, sample_index=index, vel_lo=100, vel_hi=10)
        with pytest.raises(SonareError):
            bank.add_zone(4096, sample_index=index)
        assert bank.set_count == 0


def test_patch_sample_enums_reject_unknown_spellings() -> None:
    with pytest.raises(ValueError, match="unknown sample loop mode"):
        SynthPatch(sample_loop="loop-forever")._to_c()
    with pytest.raises(ValueError, match="unknown sample key track"):
        SynthPatch(sample_key_track="sometimes")._to_c()


# -- the patch's sample block -----------------------------------------------


def test_patch_sample_fields_round_trip_through_the_c_struct() -> None:
    patch = SynthPatch(
        engine_mode="sample",
        sample_set=3,
        sample_level=0.75,
        sample_loop="key-down",
        sample_start_offset=0.25,
        sample_key_track="off",
    )
    c = patch._to_c()
    # Version 3 is what makes the core read the block at all, and every version
    # past it is a superset, so the block keeps being read.
    assert c.struct_version >= 3
    assert (c.sample_set, c.sample_loop, c.sample_key_track) == (3, 3, 2)
    back = SynthPatch._from_c(c)
    assert back.sample_set == 3
    assert back.sample_level == pytest.approx(0.75)
    assert back.sample_loop == "key-down"
    assert back.sample_start_offset == pytest.approx(0.25)
    assert back.sample_key_track == "off"

    # Set 0 is addressable without a presence bit, because only a sample patch
    # reads the block. An untouched patch spells "keep the base" as zero.
    plain = SynthPatch()._to_c()
    assert (plain.sample_set, plain.sample_loop, plain.sample_key_track) == (0, 0, 0)
    assert plain.sample_level == 0.0
    assert plain.sample_start_offset == 0.0

    for ordinal, name in enumerate(("default", "none", "continuous", "key-down")):
        assert SynthPatch(sample_loop=name)._to_c().sample_loop == ordinal
    for ordinal, name in enumerate(("default", "on", "off")):
        assert SynthPatch(sample_key_track=name)._to_c().sample_key_track == ordinal


# -- rendering --------------------------------------------------------------


def test_bounce_through_a_sample_patch_is_audible_and_deterministic() -> None:
    with SampleBank() as bank:
        bank.add_zone(0, sample_index=bank.add_sample(_tone(), root_key=60))
        patch = _sample_patch(bank)
        audio = _render(60, patch)
        assert audio.shape == (12000, 1)
        assert np.isfinite(audio).all()
        assert _peak(audio) > 0.0
        assert np.array_equal(audio, _render(60, patch))

    # A sample patch with no bank renders silence rather than failing, the same
    # way one naming a keymap set the bank lacks does.
    assert _is_silent(_render(60, _sample_patch(None)))
    with SampleBank() as empty:
        empty.add_zone(0, sample_index=empty.add_sample(_tone()))
        assert _is_silent(_render(60, _sample_patch(empty, set_index=9)))


def test_each_binding_carries_its_own_bank() -> None:
    """Two destinations, two banks, and neither one reaches the other.

    The bank is a field of the C binding struct, so this is the capability a
    single per-call argument could not express: the loud bank must reach only
    the destination it was bound to.
    """
    project = Project()
    try:
        project.set_sample_rate(float(SAMPLE_RATE))
        for destination, note in ((0, 60), (1, 72)):
            track, clip = project.add_midi_clip(0.0, 1.0)
            project.set_track_midi_destination(track, destination)
            project.set_midi_events(
                clip,
                [
                    Project.midi_note_on(0.0, 0, 0, note, 100),
                    Project.midi_note_off(0.5, 0, 0, note, 0),
                ],
            )

        with SampleBank() as loud, SampleBank() as silent:
            loud.add_zone(0, sample_index=loud.add_sample(_tone(), root_key=60))
            # Covers only key 60, so destination 1's note 72 finds nothing.
            silent.add_zone(0, sample_index=silent.add_sample(_tone()), key_lo=60, key_hi=60)

            def render(banks: tuple[SampleBank, SampleBank]) -> np.ndarray:
                return project.bounce_with_synth_instrument(
                    instruments=[
                        (0, _sample_patch(banks[0])),
                        (1, _sample_patch(banks[1])),
                    ],
                    total_frames=12000,
                    num_channels=1,
                    sample_rate=SAMPLE_RATE,
                )

            # Swapping which destination gets which bank changes the render, so
            # the two banks cannot be collapsing into one shared value.
            assert not np.array_equal(render((loud, silent)), render((silent, loud)))
            # Only destination 0's note is covered either way, so both renders
            # sound; one bank reaching both would be a different peak.
            assert _peak(render((loud, silent))) > 0.0
    finally:
        project.close()


def test_an_all_zero_zone_covers_the_whole_keyboard() -> None:
    """The C ABI's promise for an untouched rectangle: every key, every velocity.

    Velocity is the half that cannot be inferred -- a literal reading of
    ``vel_lo == vel_hi == 0`` would cover no playable velocity at all.
    """
    with SampleBank() as whole:
        whole.add_zone(
            0,
            sample_index=whole.add_sample(_tone()),
            key_lo=0,
            key_hi=0,
            vel_lo=0,
            vel_hi=0,
        )
        for note in (24, 60, 96):
            assert _peak(_render(note, _sample_patch(whole))) > 0.0

    # A deliberate range sets at least one bound and is honoured exactly, so
    # the promise above cannot be swallowing one.
    with SampleBank() as narrow:
        narrow.add_zone(0, sample_index=narrow.add_sample(_tone()), key_lo=60, key_hi=60)
        assert _peak(_render(60, _sample_patch(narrow))) > 0.0
        assert _is_silent(_render(24, _sample_patch(narrow)))
        assert _is_silent(_render(96, _sample_patch(narrow)))


def _sounds(note: int, **zone: int) -> bool:
    """Whether a one-zone bank built from ``zone`` sounds for ``note`` at velocity 100."""
    with SampleBank() as bank:
        bank.add_zone(0, sample_index=bank.add_sample(_tone()), **zone)
        return _peak(_render(note, _sample_patch(bank))) > 0.0


def test_narrowing_one_bound_leaves_every_other_bound_whole() -> None:
    """Each bound stands alone: setting one must not collapse any of the rest.

    This is the FACADE's contract. Every bound has an explicit non-sentinel
    default here (``key_hi=127`` / ``vel_lo=1`` / ``vel_hi=127``), so what it
    pins is that those defaults reach the core intact -- it would catch someone
    "simplifying" them to 0 and leaving the behaviour to the C sentinels. The
    sentinels themselves are the next test.
    """
    # Notes render at velocity 100, so a velocity range that lost either end
    # shows up as silence.
    assert _sounds(60, key_lo=48) and not _sounds(24, key_lo=48)
    assert _sounds(60, key_hi=72) and not _sounds(96, key_hi=72)
    assert _sounds(60, vel_lo=64)  # an empty vel_hi would mute this
    assert not _sounds(60, vel_hi=64)  # 100 is above the ceiling


def test_a_zero_bound_reads_as_that_bound_being_unset() -> None:
    """The C ABI's per-bound sentinels, reached by passing 0 explicitly.

    An upper bound of zero reads as 127 and ``vel_lo`` of zero as 1; ``key_lo``
    of zero is simply the lowest key and needs no rule. Unlike the test above,
    these values do go through the core's defaulting, because the facade passes
    a zero straight down.
    """
    # key_hi=0 with a raised floor: the case an all-or-nothing rule rejected as
    # inverted instead of reading as 48..127.
    assert _sounds(60, key_lo=48, key_hi=0) and not _sounds(24, key_lo=48, key_hi=0)
    # vel_lo=0 reads as 1, so the ceiling still decides: 100 is above 64.
    assert not _sounds(60, vel_lo=0, vel_hi=64)
    assert _sounds(60, vel_lo=0, vel_hi=127)
    # vel_hi=0 reads as 127, so a velocity-100 note is inside 64..127.
    assert _sounds(60, vel_lo=64, vel_hi=0)


def test_zone_gain_and_patch_level_change_what_is_heard() -> None:
    with SampleBank() as loud, SampleBank() as quiet:
        loud.add_zone(0, sample_index=loud.add_sample(_tone()), gain=1.0)
        quiet.add_zone(0, sample_index=quiet.add_sample(_tone()), gain=0.25)
        loud_peak = _peak(_render(60, _sample_patch(loud)))
        assert _peak(_render(60, _sample_patch(quiet))) < loud_peak
        assert _peak(_render(60, _sample_patch(loud, sample_level=0.25))) < loud_peak


def test_a_looped_sample_outlasts_the_frames_it_holds() -> None:
    """A continuous loop keeps sounding past the sample's own length.

    The window is inside the held note, so what separates the two renders is
    the loop rather than the amplitude envelope's release.
    """
    frames = 2400  # 50 ms at 48 kHz; a note held far longer than that.
    held = slice(9600, 12000)  # 200-250 ms, four sample-lengths in.

    def render(loop_mode: str | int) -> np.ndarray:
        project = Project()
        project.set_sample_rate(float(SAMPLE_RATE))
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
                    _tone(frames=frames),
                    root_key=60,
                    source_rate=float(SAMPLE_RATE),
                    loop_start=0,
                    loop_end=frames,
                    loop_mode=loop_mode,
                )
                bank.add_zone(0, sample_index=index)
                return project.bounce_with_synth_instrument(
                    _sample_patch(bank),
                    total_frames=12000,
                    num_channels=1,
                    sample_rate=SAMPLE_RATE,
                )
        finally:
            project.close()

    looped = render("continuous")
    unlooped = render("none")
    assert _peak(looped[held]) > 0.0
    # An order of magnitude, so a resonator or bus tail on the unlooped render
    # cannot satisfy this the way a bare inequality would.
    assert _peak(looped[held]) > 10.0 * _peak(unlooped[held])


# -- the realtime engine ----------------------------------------------------

BLOCK = 128
DESTINATION = 7


def _play_live(
    patch: SynthPatch | str,
    *,
    sample_bank: SampleBank | None = None,
    close_after_binding: SampleBank | None = None,
) -> np.ndarray:
    """Bind a sample patch to a prepared engine and render 8 blocks of one note."""
    engine = RealtimeEngine()
    try:
        engine.prepare(float(SAMPLE_RATE), BLOCK, 16, 16)
        engine.set_synth_instrument(patch, destination_id=DESTINATION, sample_bank=sample_bank)
        if close_after_binding is not None:
            close_after_binding.close()
        engine.push_midi_note_on(DESTINATION, 0, 0, 60, 100)
        silence = [[0.0] * BLOCK, [0.0] * BLOCK]
        # One block is shorter than the amp envelope's attack, so take the peak
        # across several rather than asking the first one to be audible.
        return np.concatenate([np.asarray(engine.process(silence)) for _ in range(8)], axis=1)
    finally:
        engine.close()


def test_engine_reads_the_bank_off_the_patch_like_the_bounce_does() -> None:
    with SampleBank() as bank:
        bank.add_zone(0, sample_index=bank.add_sample(_tone(), root_key=60))
        assert _peak(_play_live(_sample_patch(bank))) > 0.0
    # The same patch with no bank is accepted and renders silence, matching what
    # the bounce does.
    assert _is_silent(_play_live(_sample_patch(None)))


def test_engine_sample_bank_argument_serves_the_case_a_patch_field_cannot() -> None:
    """A bare preset-name string has no field to carry a bank, so the argument
    exists for it -- and wins over the patch's own field when both are given."""
    with SampleBank() as bank, SampleBank() as empty:
        bank.add_zone(0, sample_index=bank.add_sample(_tone(), root_key=60))
        empty.add_sample(_tone())  # a sample but no zone: nothing covers a note

        # The patch field is empty and the argument carries the real bank.
        assert _peak(_play_live(_sample_patch(empty), sample_bank=bank)) > 0.0
        # The reverse: the argument's empty bank overrides the patch's good one.
        assert _is_silent(_play_live(_sample_patch(bank), sample_bank=empty))


def test_the_engine_takes_a_share_of_the_bank() -> None:
    """Closing the caller's handle right after binding must not silence the voice.

    The engine holds a share rather than the caller's handle, so this is the
    difference between a working host and a use-after-free that happens to
    sound right on the day it is written.
    """
    bank = SampleBank()
    bank.add_zone(0, sample_index=bank.add_sample(_tone(), root_key=60))
    audio = _play_live(_sample_patch(bank), close_after_binding=bank)
    assert _peak(audio) > 0.0
    assert np.isfinite(audio).all()


def test_a_closed_bank_is_rejected_on_both_hosts() -> None:
    bank = SampleBank()
    bank.add_zone(0, sample_index=bank.add_sample(_tone()))
    bank.close()
    engine = RealtimeEngine()
    try:
        engine.prepare(float(SAMPLE_RATE), BLOCK, 16, 16)
        with pytest.raises(RuntimeError, match="SampleBank is closed"):
            engine.set_synth_instrument(_sample_patch(bank), destination_id=DESTINATION)
    finally:
        engine.close()
    with pytest.raises(RuntimeError, match="SampleBank is closed"):
        _render(60, _sample_patch(bank))
