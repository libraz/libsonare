"""A caller's number is refused, not folded into a legal one.

ctypes applies the C conversion on both routes into the library -- an argument
constructor and a struct field assignment -- so an out-of-range value arrives as
a different, legal number rather than as an error. The value that matters is not
the implausible one: ``2**31`` wraps negative and the core's own guards catch it,
while ``2**32 + 8`` lands on 8, which is a setting a caller could have asked for.

Each case drives one field behind one reader, and each opens with a positive
control -- two legitimate values whose results differ. Without it an accepted
out-of-range value cannot be told from a field the entry point never reads.
"""

from __future__ import annotations

import re

import numpy as np
import pytest

import libsonare as ls
from libsonare import SonareValueError

SAMPLE_RATE = 22050


@pytest.fixture(scope="module")
def tone() -> np.ndarray:
    n = SAMPLE_RATE // 2
    return (0.3 * np.sin(2 * np.pi * 220 * np.arange(n) / SAMPLE_RATE)).astype(np.float32)


def _mel(tone: np.ndarray, n_mels: int):
    return ls.mel_spectrogram(
        tone, sample_rate=SAMPLE_RATE, n_fft=512, hop_length=256, n_mels=n_mels
    )


def test_a_wrapped_count_is_refused_rather_than_read_as_a_smaller_one(tone) -> None:
    """The signed-int argument reader, driven where the wrap lands in domain."""
    assert _mel(tone, 8).n_mels != _mel(tone, 16).n_mels  # positive control
    for value in (2**32 + 8, 2**32 + 16, 2**32, 2**31, -(2**31) - 1):
        with pytest.raises(SonareValueError, match="n_mels"):
            _mel(tone, value)


def test_a_wrapped_struct_field_is_refused_rather_than_read_as_a_smaller_one() -> None:
    """The struct-field route: assignment applies the same conversion."""

    def frames(n_fft: int) -> int:
        analyzer = ls.StreamAnalyzer(
            ls.StreamConfig(sample_rate=8000, n_fft=n_fft, hop_length=16, n_mels=8)
        )
        analyzer.process([0.01 * i for i in range(512)])
        return analyzer.stats().total_frames

    assert frames(32) != frames(64)  # positive control
    for value in (2**32 + 32, 2**32 + 64, 2**32):
        with pytest.raises(SonareValueError, match="n_fft"):
            frames(value)


def test_a_negative_size_is_refused_rather_than_read_as_the_largest_one() -> None:
    """The unsigned readers: a negative wraps to the top of the range, not to zero."""
    with ls.StreamAnalyzer(
        ls.StreamConfig(sample_rate=8000, n_fft=32, hop_length=32, n_mels=8)
    ) as analyzer:
        analyzer.process([0.01 * i for i in range(512)])
        assert analyzer.read_frames(1).n_frames != analyzer.read_frames(3).n_frames
        for value in (-1, -2, 2**64, 2**64 + 2):
            with pytest.raises(SonareValueError, match="max_frames"):
                analyzer.read_frames(value)


def test_a_wrapped_bitmask_is_refused_rather_than_read_as_a_different_scale() -> None:
    """The 16-bit reader: 2**16 + mask is the same mask with a different meaning."""
    assert ls.scale_quantize_midi(0, 0b101010110101, 61.4) != ls.scale_quantize_midi(0, 1, 61.4)
    for value in (-1, 2**16, 2**16 + 0b101010110101):
        with pytest.raises(SonareValueError, match="mode_mask"):
            ls.scale_quantize_midi(0, value, 61.4)


def test_an_out_of_range_sample_position_is_refused(mixer_scene) -> None:
    """The 64-bit reader."""
    mixer_scene.schedule_fader_automation("vocal", 0, -6.0, 0)
    for value in (2**63, 2**63 + 2, -(2**63) - 1):
        with pytest.raises(SonareValueError, match="sample_pos"):
            mixer_scene.schedule_fader_automation("vocal", value, -6.0, 0)


def test_an_out_of_range_index_is_refused(mixer_scene) -> None:
    """The unsigned 32-bit readers behind an insert automation target."""
    mixer_scene.schedule_insert_automation(0, 0, 0, 0, 0.5, 0)
    for value in (-1, 2**32, 2**32 + 2):
        with pytest.raises(SonareValueError, match="insert_index"):
            mixer_scene.schedule_insert_automation(0, value, 0, 0, 0.5, 0)
        with pytest.raises(SonareValueError, match="param_id"):
            mixer_scene.schedule_insert_automation(0, 0, value, 0, 0.5, 0)


def _cc_sweep() -> list:
    """One 7-bit controller moving by 10, so a movement threshold is observable."""
    return [ls.Project.midi_cc(0.0, 0, 2, 74, 60), ls.Project.midi_cc(0.1, 0, 2, 74, 70)]


def test_a_wrapped_movement_threshold_is_refused_rather_than_read_as_a_smaller_one() -> None:
    """The 8-bit argument reader, on a field whose in-domain wrap changes the outcome."""
    events = _cc_sweep()
    assert ls.Project.midi_cc_learn(events, 77, min_movement=10) is not None  # positive control
    assert ls.Project.midi_cc_learn(events, 77, min_movement=11) is None
    for value in (2**8 + 7, 2**8, -1, 2**32):
        with pytest.raises(SonareValueError, match="min_movement"):
            ls.Project.midi_cc_learn(events, 77, min_movement=value)


def test_a_wrapped_group_is_refused_rather_than_read_as_a_different_port() -> None:
    """The same reader on the other field that reaches it from a caller."""
    binding = ls.Project.midi_cc_learn(_cc_sweep(), 77)
    assert binding is not None
    first = ls.Project.midi_param_to_cc([binding], 77, 0.5, 0)
    second = ls.Project.midi_param_to_cc([binding], 77, 0.5, 1)
    assert first is not None and second is not None and first != second  # positive control
    for value in (2**8 + 1, 2**8, -1, 2**32):
        with pytest.raises(SonareValueError, match="group"):
            ls.Project.midi_param_to_cc([binding], 77, 0.5, value)


def test_a_wrapped_param_id_is_refused_rather_than_read_as_a_bound_one() -> None:
    """The 32-bit argument reader: a wrap lands back on the id that is bound."""
    binding = ls.Project.midi_cc_learn(_cc_sweep(), 77)
    assert binding is not None
    assert ls.Project.midi_param_to_cc([binding], 77, 0.5, 0) is not None  # positive control
    assert ls.Project.midi_param_to_cc([binding], 78, 0.5, 0) is None
    for value in (2**32 + 77, 2**32, -1, 2**64):
        with pytest.raises(SonareValueError, match="param_id"):
            ls.Project.midi_param_to_cc([binding], value, 0.5, 0)


def test_a_wrapped_marker_kind_is_refused_rather_than_read_as_a_different_kind() -> None:
    """The struct route for an enum ordinal: the field's own type carries the bound."""
    with ls.RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_markers(
            [
                ls.EngineMarker(1, 0.0, "a", kind=ls.MarkerKind.CUE_POINT),
                ls.EngineMarker(2, 4.0, "b", kind=ls.MarkerKind.KEY_SIGNATURE),
            ]
        )
        assert engine.marker(1).kind != engine.marker(2).kind  # positive control
        for value in (2**8 + 3, 2**8, -1, 2**32):
            with pytest.raises(SonareValueError, match="kind"):
                engine.set_markers([ls.EngineMarker(3, 0.0, "c", kind=value)])


@pytest.fixture
def mixer_scene():
    scene = ls.mixing_scene_preset_json(ls.mixing_scene_preset_names()[0])
    mixer = ls.Mixer.from_scene_json(scene, sample_rate=48000, block_size=256)
    try:
        yield mixer
    finally:
        mixer.close()


@pytest.fixture
def two_track_project():
    """Two tracks and two clips, so an edit's id argument has something to select."""
    project = ls.Project()
    tracks = [project.add_track("audio", name) for name in ("a", "b")]
    clips = [
        project.add_clip(track, 0.0, 480.0, audio=[0.1, 0.2], audio_sample_rate=48000)
        for track in tracks
    ]
    return project, tracks, clips


# The core assigns ids from 1, so every value here lands on a live object once
# converted: 2**32 + 1 and 1.5 both become 1, and 2**32 and 0.5 both become 0.
WRAPPED_IDS = (2**32 + 1, 2**32 + 2, 2**32, 1.5, 0.5, -1)

# The signed-int arguments, where -1 is legal and the wrap is what is not:
# 2**32 lands on 0 and 2**31 on the most negative int.
WRAPPED_INTS = (2**32, 2**32 + 1, 2**31, -(2**31) - 1, 1.5)

# The 8-bit arguments, whose range a plausible-looking value already leaves.
WRAPPED_BYTES = (2**8, 2**8 + 1, 2**32, -1, 1.5)


def _refuses(name: str, call, *args) -> None:
    """Assert the call is refused and that the refusal names ``name`` first.

    Anchored rather than matched loosely: ``track_id`` appears inside
    ``new_track_id``, so a substring test would let a refusal about one argument
    stand in for a refusal about its neighbour.
    """
    with pytest.raises(SonareValueError, match=rf"^{re.escape(name)} must be"):
        call(*args)


def test_a_wrapped_edit_id_is_refused_rather_than_selecting_another_object(
    two_track_project,
) -> None:
    """The project edit ops, where the wrap lands on an object the caller did not name.

    Each case opens with the same edit applied to each of the two ids: the
    results differ, so an entry point that ignored the id could not pass. The
    refusal is asserted by the id it names, since every op here takes exactly one.
    """
    project, (track_a, track_b), (clip_a, clip_b) = two_track_project

    def applied(edit, *args) -> str:
        edit(*args)
        result = project.to_json()
        project.undo()
        return result

    edits = (
        ("track_id", project.set_track_gain, track_a, track_b, (0.5,)),
        ("track_id", project.set_track_mute, track_a, track_b, (True,)),
        ("track_id", project.set_track_solo, track_a, track_b, (True,)),
        ("track_id", project.set_track_pan, track_a, track_b, (0.5,)),
        ("clip_id", project.remove_clip, clip_a, clip_b, ()),
        ("clip_id", project.set_clip_gain, clip_a, clip_b, (0.5,)),
        ("clip_id", project.set_clip_fade, clip_a, clip_b, (24.0, 48.0)),
        ("clip_id", project.set_clip_loop, clip_a, clip_b, ("loop", 240.0)),
    )
    for name, edit, first, second, rest in edits:
        assert applied(edit, first, *rest) != applied(edit, second, *rest)  # positive control
        for value in WRAPPED_IDS:
            _refuses(name, edit, value, *rest)


@pytest.fixture
def arrangement_project():
    """Two of everything the arrangement edit ops address by id.

    The spare pair is empty because the ops that retarget or retype a track
    refuse one that already holds a clip, and a case whose control cannot run is
    a case that proves nothing.
    """
    project = ls.Project()
    tracks = [project.add_track("audio", name) for name in ("a", "b")]
    clips = [
        project.add_clip(track, 0.0, 480.0, audio=[0.1 * (i + 1)] * 8, audio_sample_rate=48000)
        for i, track in enumerate(tracks)
    ]
    spare = [project.add_track("audio", name) for name in ("c", "d")]
    for warp_ref, stretch in ((3, 120.0), (4, 140.0)):
        project.set_warp_map(warp_ref, [(0.0, 0.0), (100.0, stretch)], f"w{warp_ref}")
    for clip, source in zip(clips, (1, 2), strict=True):
        project.set_clip_takes(clip, [{"id": 1, "source_id": source}])
    for track in tracks:
        project.add_automation_lane(track, 7, [(0.0, 0.25, 0)])
    return project, tracks, clips, spare


def test_a_wrapped_arrangement_id_is_refused_rather_than_selecting_another_object(
    arrangement_project,
) -> None:
    """The rest of the arrangement surface, one row per id argument.

    Each row is one lambda taking the value under test, so the positive control
    and the refusal drive the identical call shape and a row cannot assert about
    an argument its own edit never reads.
    """
    project, (track_a, track_b), (clip_a, clip_b), (spare_a, spare_b) = arrangement_project
    points = [(0.0, 0.5, 0)]
    segment = [{"start_ppq": 0.0, "end_ppq": 100.0, "take_id": 1}]

    def applied(edit, value) -> str:
        edit(value)
        result = project.to_json()
        project.undo()
        return result

    cases = (
        ("clip_id", lambda v: project.split_clip(v, 240.0), clip_a, clip_b),
        ("clip_id", lambda v: project.trim_clip(v, 0.0, 240.0), clip_a, clip_b),
        ("clip_id", lambda v: project.move_clip(v, 240.0), clip_a, clip_b),
        ("new_track_id", lambda v: project.move_clip(clip_a, 240.0, v), spare_a, spare_b),
        ("track_id", lambda v: project.set_track_kind(v, "aux"), spare_a, spare_b),
        ("clip_id", lambda v: project.set_clip_warp_ref(v, 3), clip_a, clip_b),
        ("warp_ref_id", lambda v: project.set_clip_warp_ref(clip_a, v), 3, 4),
        ("clip_id", lambda v: project.set_clip_warp_mode(v, "repitch"), clip_a, clip_b),
        ("warp_ref_id", lambda v: project.remove_warp_map(v), 3, 4),
        ("clip_id", lambda v: project.set_clip_takes(v, [{"id": 1}]), clip_a, clip_b),
        ("clip_id", lambda v: project.set_clip_comp_segments(v, segment), clip_a, clip_b),
        ("track_id", lambda v: project.set_track_midi_destination(v, 5), track_a, track_b),
        ("destination_id", lambda v: project.set_track_midi_destination(track_a, v), 5, 6),
        ("clip_id", lambda v: project.set_clip_source(v, 2), clip_a, clip_b),
        ("source_id", lambda v: project.set_clip_source(clip_a, v), 1, 2),
        ("clip_id", lambda v: project.duplicate_clip(v, 960.0), clip_a, clip_b),
        ("track_id", lambda v: project.remove_track(v), track_a, track_b),
        ("track_id", lambda v: project.rename_track(v, "zz"), track_a, track_b),
        ("track_id", lambda v: project.set_track_route(v, "strip"), track_a, track_b),
        ("source_id", lambda v: project.set_audio_source_metadata(v, "h", "r"), 1, 2),
        ("track_id", lambda v: project.add_automation_lane(v, 9, points), track_a, track_b),
        (
            "track_id",
            lambda v: project.add_automation_lane(v, 9, points, "track-pan"),
            track_a,
            track_b,
        ),
        ("track_id", lambda v: project.edit_automation_lane(v, 7, points), track_a, track_b),
        (
            "track_id",
            lambda v: project.edit_automation_lane(v, 7, points, "track-pan"),
            track_a,
            track_b,
        ),
        ("track_id", lambda v: project.remove_automation_lane(v, 7), track_a, track_b),
    )
    for name, edit, first, second in cases:
        assert applied(edit, first) != applied(edit, second), name  # positive control
        for value in WRAPPED_IDS:
            _refuses(name, edit, value)


def test_a_wrapped_take_id_is_refused_rather_than_read_as_take_zero() -> None:
    """The take list, whose own ``int()`` truncated in front of the checked field.

    The mapping branch coerced the id before the struct's range check saw it, so
    a fractional id became 0 and the list carried it as a real take.
    """
    project = ls.Project()
    track = project.add_track("audio", "a")
    clip = project.add_clip(track, 0.0, 480.0, audio=[0.1] * 8, audio_sample_rate=48000)

    def applied(take_id) -> str:
        project.set_clip_takes(clip, [{"id": take_id, "source_id": 1}])
        result = project.to_json()
        project.undo()
        return result

    assert applied(1) != applied(2)  # positive control
    for value in WRAPPED_IDS:
        _refuses("set_clip_takes: takes[0].id", applied, value)
        _refuses(
            "set_clip_takes: takes[0].source_id",
            lambda v: project.set_clip_takes(clip, [{"id": 1, "source_id": v}]),
            value,
        )


def test_a_wrapped_source_audio_argument_is_refused_rather_than_replacing_another_source() -> None:
    """Registered PCM, whose arguments the serialized project does not carry.

    ``to_json`` shows neither the samples nor the rate they were registered at,
    so the control here is the rendered bounce: a control read off the JSON
    would have compared two identical strings and passed on any argument.
    """

    def bounced(source_id, channels, sample_rate) -> bytes:
        project = ls.Project()
        for index, name in enumerate(("a", "b")):
            track = project.add_track("audio", name)
            project.add_clip(
                track, 0.0, 480.0, audio=[0.1 * (index + 1)] * 8, audio_sample_rate=48000
            )
        project.set_source_audio(source_id, [0.9, 0.1] * 4, channels, sample_rate)
        return np.asarray(project.bounce(), dtype=np.float64).tobytes()

    assert bounced(1, 1, 48000) != bounced(2, 1, 48000)  # positive control
    assert bounced(1, 1, 48000) != bounced(1, 2, 48000)  # positive control
    assert bounced(1, 1, 44100) != bounced(1, 1, 48000)  # positive control
    for value in WRAPPED_IDS:
        _refuses("source_id", bounced, value, 1, 48000)
    for value in WRAPPED_INTS:
        _refuses("sample_rate", bounced, 1, 1, value)

    # ``channels`` divides the sample count, so for any non-empty audio the
    # local semantic half refuses a wrapped value before the narrowing is
    # reached. Both halves are driven rather than assumed: the modulo check on
    # its own message, the type check on the one path that reaches it.
    def empty(channels) -> None:
        ls.Project().set_source_audio(1, [], channels, 48000)

    for value in (2**32, 2**32 + 1, 1.5):
        _refuses("channels", empty, value)
    for value in (2**32, -1, 0):
        with pytest.raises(SonareValueError, match="^audio length must be a multiple of channels$"):
            bounced(1, value, 48000)


@pytest.fixture
def two_midi_clip_project():
    """Two MIDI clips carrying different note content."""
    project = ls.Project()
    clips = [project.add_midi_clip(0.0, 480.0)[1] for _ in range(2)]
    for clip, note in zip(clips, (60, 67), strict=True):
        project.set_midi_events(
            clip,
            [
                ls.Project.midi_note_on(0.0, 0, 0, note, 100),
                ls.Project.midi_note_off(240.0, 0, 0, note),
            ],
        )
    return project, clips


def test_a_wrapped_midi_argument_is_refused_rather_than_addressing_another_clip(
    two_midi_clip_project,
) -> None:
    """The MIDI surface, whose clip id and channel-voice scalars narrow onto three widths."""
    project, (clip_a, clip_b) = two_midi_clip_project
    events = [ls.Project.midi_note_on(0.0, 0, 0, 72, 100)]
    fx = '{"transpose_semitones":5}'

    def applied(edit, value) -> str:
        edit(value)
        result = project.to_json()
        project.undo()
        return result

    ids = (
        ("clip_id", lambda v: project.set_midi_events(v, events)),
        ("clip_id", lambda v: project.set_program(v, 5)),
        ("clip_id", lambda v: project.set_program_on_channel(v, 0, 0, 5)),
        ("clip_id", lambda v: project.set_midi_fx(v, fx)),
        ("clip_id", lambda v: project.bake_midi_fx(v, fx)),
    )
    for name, edit in ids:
        assert applied(edit, clip_a) != applied(edit, clip_b), name  # positive control
        for value in WRAPPED_IDS:
            _refuses(name, edit, value)

    scalars = (
        ("program", lambda v: project.set_program(clip_a, v), 5, 6, WRAPPED_INTS),
        ("bank", lambda v: project.set_program(clip_a, 5, v), 0, 1, WRAPPED_INTS),
        ("program", lambda v: project.set_program_on_channel(clip_a, 0, 0, v), 5, 6, WRAPPED_INTS),
        ("bank", lambda v: project.set_program_on_channel(clip_a, 0, 0, 5, v), 0, 1, WRAPPED_INTS),
        ("group", lambda v: project.set_program_on_channel(clip_a, v, 0, 5), 0, 1, WRAPPED_BYTES),
        ("channel", lambda v: project.set_program_on_channel(clip_a, 0, v, 5), 0, 1, WRAPPED_BYTES),
    )
    for name, edit, first, second, refused in scalars:
        assert applied(edit, first) != applied(edit, second), name  # positive control
        for value in refused:
            _refuses(name, edit, value)


def test_a_wrapped_midi_query_argument_is_refused_rather_than_counting_another_clip(
    two_midi_clip_project,
) -> None:
    """The read-only MIDI queries, whose result rather than the project is the control."""
    project, (clip_a, clip_b) = two_midi_clip_project
    project.set_midi_events(
        clip_b,
        [
            ls.Project.midi_note_on(0.0, 0, 0, 67, 100),
            ls.Project.midi_note_off(240.0, 0, 0, 67),
            ls.Project.midi_note_on(240.0, 0, 0, 69, 100),
        ],
    )
    fx = '{"transpose_semitones":5}'

    queries = (
        ("clip_id", lambda v: project.preview_midi_fx_count(v, fx)),
        ("clip_id", lambda v: project.validate_midi_notes(v)),
        ("clip_id", lambda v: project.bake_midi_fx(v, fx, with_source_index=True)),
    )
    for name, query in queries:
        assert query(clip_a) != query(clip_b), name  # positive control
        for value in WRAPPED_IDS:
            _refuses(name, query, value)


def test_a_wrapped_gm_lookup_argument_is_refused_rather_than_naming_another_entry() -> None:
    """The static GM tables, where a wrap returns a name instead of ``None``.

    These take no handle and mutate nothing, so an out-of-range program used to
    come back as program 0's name -- an answer, and a wrong one, where the
    contract promises ``None``.
    """
    lookups = (
        ("program", ls.Project.gm_instrument_name, (0,), (1,), 0),
        ("family", ls.Project.gm_family_name, (0,), (1,), 0),
        ("family", ls.Project.gm_family_first_program, (0,), (1,), 0),
        ("note", ls.Project.gm_drum_name, (36,), (38,), 0),
        ("bank_lsb", ls.Project.gm2_drum_set_name, (0,), (8,), 0),
        ("controller", ls.Project.midi_cc_name, (7,), (10,), 0),
        ("index", ls.Project.per_note_controller_name, (1,), (2,), 0),
        ("bank_lsb", ls.Project.gm2_instrument_name, (0, 0), (1, 0), 0),
        ("program", ls.Project.gm2_instrument_name, (0, 0), (0, 1), 1),
        ("bank_lsb", ls.Project.gm2_drum_name, (0, 27), (48, 27), 0),
        ("note", ls.Project.gm2_drum_name, (0, 36), (0, 38), 1),
    )
    for name, lookup, first, second, position in lookups:
        assert lookup(*first) != lookup(*second), name  # positive control
        for value in WRAPPED_INTS:
            args = list(first)
            args[position] = value
            _refuses(name, lookup, *args)


def test_a_wrapped_bank_program_argument_is_refused_rather_than_emitting_another_event() -> None:
    """The pure lowering helper, whose five scalars span two C widths."""
    base = (0.0, 0, 0, 0, 0, 0)
    scalars = (
        ("group", 1, WRAPPED_BYTES),
        ("channel", 2, WRAPPED_BYTES),
        ("bank_msb", 3, WRAPPED_INTS),
        ("bank_lsb", 4, WRAPPED_INTS),
        ("program", 5, WRAPPED_INTS),
    )
    for name, position, refused in scalars:
        moved = list(base)
        moved[position] = 1
        assert ls.Project.midi_bank_program(*moved) != ls.Project.midi_bank_program(*base), name
        for value in refused:
            args = list(base)
            args[position] = value
            _refuses(name, ls.Project.midi_bank_program, *args)
