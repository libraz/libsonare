"""Tests for the offline mixing assistant in the Python binding.

The assistant's numeric output is DSP-dependent, so nothing here pins a gain, a
source class or a line count. What is asserted is the structure of the result
and the invariants the API promises: one track entry per input, a
strip for every track id, a scene the mixer can load, and an empty explanation
when every decision domain is switched off.
"""

from __future__ import annotations

import json
import math
import re

import numpy as np
import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

SAMPLE_RATE = 22050
# Tracks shorter than the assistant's usability floor are excluded from the
# suggestion, so the fixtures stay comfortably above it while remaining short
# enough for the default (non-slow) run.
DURATION_SEC = 0.6

_ALL_DOMAINS_OFF = {
    "enable_structure": False,
    "enable_gain": False,
    "enable_balance": False,
    "enable_eq": False,
    "enable_dynamics": False,
    "enable_image": False,
}


def _tone(freq: float, *, duration_sec: float = DURATION_SEC, amp: float = 0.3) -> np.ndarray:
    n = int(SAMPLE_RATE * duration_sec)
    t = np.arange(n, dtype=np.float32) / SAMPLE_RATE
    return (amp * np.sin(2.0 * np.pi * freq * t)).astype(np.float32)


def _percussive(freq: float, *, duration_sec: float = DURATION_SEC) -> np.ndarray:
    n = int(SAMPLE_RATE * duration_sec)
    t = np.arange(n, dtype=np.float32) / SAMPLE_RATE
    return (0.6 * np.sin(2.0 * np.pi * freq * t) * np.exp(-9.0 * (t % 0.25))).astype(np.float32)


def _noisy(freq: float, seed: int, *, duration_sec: float = DURATION_SEC) -> np.ndarray:
    rng = np.random.default_rng(seed)
    n = int(SAMPLE_RATE * duration_sec)
    return (
        _tone(freq, duration_sec=duration_sec, amp=0.15) + 0.05 * rng.standard_normal(n)
    ).astype(np.float32)


@pytest.fixture(scope="module")
def mono_tracks():
    """Three mono tracks of equal length."""
    from libsonare import MixAssistantTrack

    return [
        MixAssistantTrack("kick", [_percussive(55.0)], "kick"),
        MixAssistantTrack("bass", [_tone(110.0)], "bass"),
        MixAssistantTrack("keys", [_noisy(660.0, 7)], "keys"),
    ]


@pytest.fixture(scope="module")
def mixed_tracks():
    """A mono track and a stereo track in one call."""
    from libsonare import MixAssistantTrack

    return [
        MixAssistantTrack("kick", [_percussive(55.0)], "kick"),
        MixAssistantTrack("pad", [_noisy(660.0, 3), _noisy(664.0, 4)], "pad"),
    ]


@pytest.fixture(scope="module")
def mono_suggestion(mono_tracks):
    """The default suggestion over ``mono_tracks``, analysed once for the module."""
    from libsonare import mixing_assistant_suggest

    return mixing_assistant_suggest(mono_tracks, sample_rate=SAMPLE_RATE)


# Partial weights of the voice-like series below; index 0 is the fundamental.
_VOICE_PARTIALS = (0.30, 0.70, 0.90, 0.85, 0.80, 0.70, 0.60, 0.55, 0.50, 0.45, 0.40, 0.35)
_HIGH_PASS_PROCESSOR = "eq.cutFilter"
# The reasoning line a proposed high-pass writes, matched on its opening rather
# than on the measured share it goes on to quote.
_HIGH_PASS_REASON = "high-passed vox at 80 Hz, where "


def _voice_with_rumble(duration_sec: float = DURATION_SEC) -> np.ndarray:
    """A sustained voice-like harmonic series over 180 Hz with stand rumble under it.

    The rumble is the point of the fixture. The assistant proposes a high-pass only
    where the share of a track's energy below its class corner reads as residue --
    between 0.5% and 10% -- and the shared fixtures carry nothing at all under their
    corners, so on that material ``enable_high_pass`` changes nothing, the two
    documents come back identical, and the case would be satisfied by a binding that
    dropped the option. The 40 Hz tone puts about 2% of this track's energy below the
    80 Hz vocal corner, which is inside the window.
    """
    n = int(SAMPLE_RATE * duration_sec)
    t = np.arange(n, dtype=np.float64) / SAMPLE_RATE
    voice = np.zeros(n, dtype=np.float64)
    for index, weight in enumerate(_VOICE_PARTIALS):
        voice += weight * np.sin(2.0 * np.pi * 180.0 * (index + 1) * t)
    return (0.045 * voice + 0.014 * np.sin(2.0 * np.pi * 40.0 * t)).astype(np.float32)


def _high_pass_owners(scene) -> list[str]:
    """Ids of the strips and buses carrying a high-pass insert, in scene order."""
    return [
        node["id"]
        for node in [*scene["strips"], *scene["buses"]]
        if any(insert["processor"] == _HIGH_PASS_PROCESSOR for insert in node["inserts"])
    ]


def _high_pass_lines(result) -> list[str]:
    """The explanation lines attributable to a proposed high-pass."""
    return [line for line in result.explanation if line.startswith(_HIGH_PASS_REASON)]


def _assert_document_shape(result, track_count: int) -> None:
    """Assert the result's structure for ``track_count`` inputs."""
    from libsonare import MixAssistantMixProfile, MixAssistantResult, MixAssistantTrackProfile

    assert isinstance(result, MixAssistantResult)
    assert isinstance(result.mix, MixAssistantMixProfile)
    assert result.mix.track_count == track_count
    assert len(result.tracks) == track_count
    assert all(isinstance(track, MixAssistantTrackProfile) for track in result.tracks)
    assert all(isinstance(line, str) for line in result.explanation)
    scene = result.scene
    assert set(scene) >= {"version", "strips", "buses", "connections"}


def test_source_class_names_resolve_to_their_index():
    from libsonare import (
        mixing_assistant_source_class_from_name,
        mixing_assistant_source_class_names,
    )

    names = mixing_assistant_source_class_names()
    assert names, "expected at least one mixing-assistant source class"
    for name in names:
        assert mixing_assistant_source_class_from_name(name) >= 0


def test_an_unknown_source_class_name_is_refused():
    from libsonare import (
        SonareValueError,
        mixing_assistant_source_class_from_name,
        mixing_assistant_source_class_names,
    )

    expected = "mixing_assistant_source_class_from_name: name must be one of " + ", ".join(
        mixing_assistant_source_class_names()
    )
    with pytest.raises(SonareValueError) as unknown:
        mixing_assistant_source_class_from_name("not-a-source-class")
    assert str(unknown.value) == expected
    with pytest.raises(TypeError) as wrong_type:
        mixing_assistant_source_class_from_name(3)  # type: ignore[arg-type]
    assert str(wrong_type.value) == expected


def test_suggest_over_mono_tracks(mono_tracks, mono_suggestion):
    _assert_document_shape(mono_suggestion, len(mono_tracks))
    assert mono_suggestion.explanation, "a usable multi-track set should be explained"
    strip_ids = {strip["id"] for strip in mono_suggestion.scene["strips"]}
    assert strip_ids >= {track.id for track in mono_tracks}


def test_reported_source_classes_are_declared(mono_suggestion):
    from libsonare import mixing_assistant_source_class_names

    names = set(mixing_assistant_source_class_names())
    assert {track.source for track in mono_suggestion.tracks} <= names


def test_suggest_over_mixed_mono_and_stereo_tracks(mixed_tracks):
    from libsonare import mixing_assistant_suggest

    result = mixing_assistant_suggest(mixed_tracks, sample_rate=SAMPLE_RATE)
    _assert_document_shape(result, len(mixed_tracks))
    channel_counts = {track.strip_id: track.num_channels for track in result.tracks}
    assert channel_counts["kick"] == 1
    assert channel_counts["pad"] == 2


def test_suggest_over_tracks_of_differing_lengths():
    from libsonare import MixAssistantTrack, mixing_assistant_suggest

    tracks = [
        MixAssistantTrack("kick", [_percussive(55.0, duration_sec=DURATION_SEC)], "kick"),
        MixAssistantTrack("bass", [_tone(110.0, duration_sec=DURATION_SEC * 0.75)], "bass"),
        MixAssistantTrack(
            "pad",
            [
                _noisy(660.0, 3, duration_sec=DURATION_SEC * 0.9),
                _noisy(664.0, 4, duration_sec=DURATION_SEC * 0.9),
            ],
            "pad",
        ),
    ]
    result = mixing_assistant_suggest(tracks, sample_rate=SAMPLE_RATE)
    _assert_document_shape(result, len(tracks))
    strip_ids = {strip["id"] for strip in result.scene["strips"]}
    assert strip_ids >= {track.id for track in tracks}


def test_suggest_without_tracks_yields_an_empty_suggestion():
    from libsonare import mixing_assistant_suggest

    result = mixing_assistant_suggest([], sample_rate=SAMPLE_RATE)
    _assert_document_shape(result, 0)
    assert result.explanation == []
    assert result.scene["strips"] == []


def test_suggest_with_a_silent_track():
    from libsonare import MixAssistantTrack, mixing_assistant_suggest

    n = int(SAMPLE_RATE * DURATION_SEC)
    tracks = [
        MixAssistantTrack("kick", [_percussive(55.0)], "kick"),
        MixAssistantTrack("silence", [np.zeros(n, dtype=np.float32)], "pad"),
    ]
    result = mixing_assistant_suggest(tracks, sample_rate=SAMPLE_RATE)
    _assert_document_shape(result, len(tracks))
    # A silent track carries no measurable level, so it is reported as excluded
    # rather than staged against the loudness target.
    excluded = {track.strip_id: track.usable for track in result.tracks}
    assert excluded["silence"] is False


@pytest.mark.parametrize("bad", [np.nan, np.inf, -np.inf])
def test_a_non_finite_track_is_excluded_as_itself_rather_than_as_silence(bad):
    from libsonare import MixAssistantTrack, mixing_assistant_suggest

    # A non-finite sample reaches the integrated loudness as -inf, where the
    # silence gate would claim it, and the caller would be told something about
    # the material instead of about the buffer they handed over.
    poisoned = _tone(110.0).copy()
    poisoned[1000] = bad
    n = int(SAMPLE_RATE * DURATION_SEC)
    tracks = [
        MixAssistantTrack("kick", [_percussive(55.0)], "kick"),
        MixAssistantTrack("poisoned", [poisoned], "bass"),
        MixAssistantTrack("silence", [np.zeros(n, dtype=np.float32)], "pad"),
    ]
    result = mixing_assistant_suggest(tracks, sample_rate=SAMPLE_RATE)
    by_id = {track.strip_id: track for track in result.tracks}

    assert by_id["poisoned"].usable is False
    assert by_id["poisoned"].exclusion_reason == "track has non-finite samples"
    # The silent track is the control: both are excluded, and the two reasons
    # stay distinguishable, so the assertion above cannot pass on any refusal.
    assert by_id["silence"].usable is False
    assert by_id["silence"].exclusion_reason == "track is silent"
    # A measurable track is carried, so one bad buffer does not fail the call.
    assert by_id["kick"].usable is True
    assert by_id["kick"].exclusion_reason is None


def test_the_suggested_scene_loads_into_a_mixer(mono_tracks, mono_suggestion):
    from libsonare import Mixer

    scene_json = json.dumps(mono_suggestion.scene)
    mixer = Mixer.from_json(scene_json, sample_rate=SAMPLE_RATE, max_block_size=256)
    try:
        assert mixer.strip_count >= len(mono_tracks)
    finally:
        mixer.close()


def test_every_domain_off_yields_no_explanation(mono_tracks):
    from libsonare import mixing_assistant_suggest

    result = mixing_assistant_suggest(mono_tracks, sample_rate=SAMPLE_RATE, **_ALL_DOMAINS_OFF)
    _assert_document_shape(result, len(mono_tracks))
    assert result.explanation == []


def test_every_domain_off_still_yields_a_loadable_scene(mono_tracks):
    from libsonare import mixing_assistant_suggest

    # The switches suppress decisions, not tracks: the scene still describes
    # every input so it remains a usable starting point for the mixer.
    scene = mixing_assistant_suggest(mono_tracks, sample_rate=SAMPLE_RATE, **_ALL_DOMAINS_OFF).scene
    assert {strip["id"] for strip in scene["strips"]} >= {track.id for track in mono_tracks}


@pytest.mark.parametrize(
    "domain",
    [
        "enable_structure",
        "enable_gain",
        "enable_balance",
        "enable_eq",
        "enable_dynamics",
        "enable_image",
    ],
)
def test_each_domain_switch_is_honoured(mono_tracks, mono_suggestion, domain):
    from libsonare import mixing_assistant_suggest

    result = mixing_assistant_suggest(mono_tracks, sample_rate=SAMPLE_RATE, **{domain: False})
    _assert_document_shape(result, len(mono_tracks))
    # A disabled domain is skipped rather than evaluated and discarded, so it
    # can only ever remove reasoning, never add any.
    assert len(result.explanation) <= len(mono_suggestion.explanation)


@pytest.mark.parametrize(
    ("option", "value"),
    [
        ("target_track_lufs", -12.0),
        ("suggestion_strength", 0.5),
        ("eq_max_cut_db", 2.0),
        ("mix_bus_headroom_dbtp", -3.0),
        ("n_fft", 1024),
        ("hop_length", 256),
    ],
)
def test_numeric_options_are_accepted(mono_tracks, option, value):
    from libsonare import mixing_assistant_suggest

    result = mixing_assistant_suggest(mono_tracks, sample_rate=SAMPLE_RATE, **{option: value})
    _assert_document_shape(result, len(mono_tracks))


def test_high_pass_is_proposed_only_once_the_switch_is_on():
    from libsonare import MixAssistantTrack, mixing_assistant_suggest

    # A local fixture rather than the shared one: this case needs low-frequency
    # residue that would move unrelated assertions elsewhere.
    tracks = [MixAssistantTrack("vox", [_voice_with_rumble()], "Lead Vox")]
    off = mixing_assistant_suggest(tracks, sample_rate=SAMPLE_RATE)
    on = mixing_assistant_suggest(tracks, sample_rate=SAMPLE_RATE, enable_high_pass=True)

    _assert_document_shape(off, len(tracks))
    _assert_document_shape(on, len(tracks))
    # Off by default: the measurement is not taken at all, so no filter and no
    # line about one.
    assert _high_pass_owners(off.scene) == []
    assert _high_pass_lines(off) == []
    # On: the vocal track's 80 Hz corner earns a pre-fader filter of its own.
    assert _high_pass_owners(on.scene) == ["vox"]
    assert len(_high_pass_lines(on)) == 1


def test_target_track_lufs_moves_gain_staging(mono_tracks):
    from libsonare import mixing_assistant_suggest

    quiet = mixing_assistant_suggest(mono_tracks, sample_rate=SAMPLE_RATE, target_track_lufs=-30.0)
    loud = mixing_assistant_suggest(mono_tracks, sample_rate=SAMPLE_RATE, target_track_lufs=-6.0)
    quiet_trims = [strip["inputTrimDb"] for strip in quiet.scene["strips"]]
    loud_trims = [strip["inputTrimDb"] for strip in loud.scene["strips"]]
    assert quiet_trims and len(quiet_trims) == len(loud_trims)
    # A louder target can only ever ask for more input gain, never less.
    assert all(low <= high for low, high in zip(quiet_trims, loud_trims, strict=True))


def test_unset_options_keep_the_library_defaults(mono_tracks, mono_suggestion):
    from libsonare import mixing_assistant_suggest

    # Restating a default on this side would mask a change to the core's own
    # default, so an unset option must not reach the param array at all.
    explicit = mixing_assistant_suggest(
        mono_tracks,
        sample_rate=SAMPLE_RATE,
        target_track_lufs=-18.0,
        suggestion_strength=1.0,
        eq_max_cut_db=4.0,
        mix_bus_headroom_dbtp=-6.0,
    )
    assert explicit.scene == mono_suggestion.scene


def test_empty_track_id_is_rejected():
    from libsonare import MixAssistantTrack, SonareValueError, mixing_assistant_suggest

    with pytest.raises(SonareValueError, match=r"tracks\[0\]\.id"):
        mixing_assistant_suggest([MixAssistantTrack("", [_tone(110.0)])], sample_rate=SAMPLE_RATE)


def test_duplicate_track_ids_are_rejected():
    from libsonare import MixAssistantTrack, SonareValueError, mixing_assistant_suggest

    tracks = [MixAssistantTrack("bass", [_tone(110.0)]), MixAssistantTrack("bass", [_tone(220.0)])]
    with pytest.raises(
        SonareValueError,
        match=r"^mixing_assistant_suggest: tracks\[1\]\.id must be unique among tracks$",
    ):
        mixing_assistant_suggest(tracks, sample_rate=SAMPLE_RATE)


def test_non_finite_samples_are_excluded_not_rejected():
    """A NaN sample is degenerate audio, classified by the core like the other surfaces.

    The exclusion reason is the assertion: a track excluded for the wrong
    reason is the same defect as one rejected outright.
    """
    from libsonare import MixAssistantTrack, mixing_assistant_suggest

    samples = _tone(110.0).copy()
    samples[17] = np.nan
    result = mixing_assistant_suggest(
        [MixAssistantTrack("bass", [samples]), MixAssistantTrack("pad", [_tone(440.0)])],
        sample_rate=SAMPLE_RATE,
    )
    profiles = {track.strip_id: track for track in result.tracks}
    assert profiles["bass"].usable is False
    assert profiles["bass"].exclusion_reason == "track has non-finite samples"
    # The rest of the call survives: one bad track is isolated, not fatal.
    assert profiles["pad"].usable is True


def test_non_finite_samples_in_the_right_channel_are_excluded():
    """The stereo path reaches the same classification as the mono one."""
    from libsonare import MixAssistantTrack, mixing_assistant_suggest

    right = _tone(440.0).copy()
    right[23] = np.inf
    result = mixing_assistant_suggest(
        [MixAssistantTrack("pad", [_tone(440.0), right])], sample_rate=SAMPLE_RATE
    )
    profile = result.tracks[0]
    assert profile.usable is False
    assert profile.exclusion_reason == "track has non-finite samples"


@pytest.mark.parametrize("planes", [0, 3])
def test_a_track_with_no_planes_or_more_than_two_is_rejected(planes):
    from libsonare import MixAssistantTrack, SonareValueError, mixing_assistant_suggest

    tracks = [MixAssistantTrack("pad", [_tone(440.0)] * planes)]
    with pytest.raises(SonareValueError, match=r"tracks\[0\]\.channels"):
        mixing_assistant_suggest(tracks, sample_rate=SAMPLE_RATE)


def test_channels_given_as_a_bare_plane_is_a_type_error():
    """A single array is not read as a list of planes, one per sample row."""
    from libsonare import MixAssistantTrack, mixing_assistant_suggest

    tracks = [MixAssistantTrack("pad", _tone(440.0))]  # type: ignore[arg-type]
    with pytest.raises(TypeError, match=r"tracks\[0\]\.channels"):
        mixing_assistant_suggest(tracks, sample_rate=SAMPLE_RATE)


def test_mismatched_channel_lengths_are_rejected():
    from libsonare import MixAssistantTrack, SonareValueError, mixing_assistant_suggest

    tracks = [
        MixAssistantTrack("pad", [_tone(440.0), _tone(440.0, duration_sec=DURATION_SEC * 0.5)])
    ]
    with pytest.raises(SonareValueError, match="equal lengths"):
        mixing_assistant_suggest(tracks, sample_rate=SAMPLE_RATE)


def test_non_positive_sample_rate_is_rejected():
    from libsonare import MixAssistantTrack, SonareValueError, mixing_assistant_suggest

    with pytest.raises(SonareValueError, match="sample_rate"):
        mixing_assistant_suggest([MixAssistantTrack("bass", [_tone(110.0)])], sample_rate=0)


def test_non_finite_option_is_rejected():
    from libsonare import MixAssistantTrack, SonareValueError, mixing_assistant_suggest

    with pytest.raises(SonareValueError, match="finite"):
        mixing_assistant_suggest(
            [MixAssistantTrack("bass", [_tone(110.0)])],
            sample_rate=SAMPLE_RATE,
            target_track_lufs=float("nan"),
        )


@pytest.mark.parametrize("tempo", [5.0, 1000.0])
def test_tempo_outside_the_musical_range_is_rejected(tempo):
    """A mistyped tempo comes back named rather than as a musical-looking delay.

    The range is the core's and is deliberately not mirrored here, so the
    refusal arrives as the plain core error the sibling options raise for a
    value they cannot use (``n_fft=-4`` is the same shape). The binding's own
    argument validation raises the same class but spells the field ``tempo_bpm``,
    which is what tells a second copy of the bounds on this side apart.
    """
    from libsonare import MixAssistantTrack, SonareValueError, mixing_assistant_suggest

    with pytest.raises(SonareValueError, match="tempoBpm") as caught:
        mixing_assistant_suggest(
            [MixAssistantTrack("bass", [_tone(110.0)])],
            sample_rate=SAMPLE_RATE,
            tempo_bpm=tempo,
        )
    # The core names the field in its wire spelling; the facade's own check says
    # ``tempo_bpm``.
    assert "tempo_bpm" not in str(caught.value)


@pytest.mark.parametrize("tempo", [0.0, -120.0])
def test_a_non_positive_tempo_is_refused(tempo):
    """Absence is ``None``; the C entry's 0 sentinel is not a tempo a caller can pass."""
    from libsonare import MixAssistantTrack, SonareValueError, mixing_assistant_suggest

    with pytest.raises(SonareValueError, match="tempo_bpm"):
        mixing_assistant_suggest(
            [MixAssistantTrack("bass", [_tone(110.0)])],
            sample_rate=SAMPLE_RATE,
            tempo_bpm=tempo,
        )


def test_a_track_profile_reports_absent_facts_as_none():
    from libsonare import MixAssistantTrack, mixing_assistant_suggest

    n = int(SAMPLE_RATE * DURATION_SEC)
    tracks = [
        MixAssistantTrack("bass", [_tone(110.0)]),
        MixAssistantTrack("silence", [np.zeros(n, dtype=np.float32)], "pad"),
    ]
    bass, silence = mixing_assistant_suggest(tracks, sample_rate=SAMPLE_RATE).tracks
    # No name was given, and a usable track has no exclusion to explain.
    assert bass.name is None
    assert bass.exclusion_reason is None
    assert bass.integrated_lufs is not None
    assert set(bass.band_occupancy) == {"sub", "low", "lowMid", "mid", "highMid", "high", "air"}
    assert sum(bass.band_occupancy.values()) == pytest.approx(1.0, abs=1e-3)
    # A silent track reads -inf loudness, and says why it was set aside.
    assert silence.name == "pad"
    assert silence.integrated_lufs == -math.inf
    assert silence.exclusion_reason == "track is silent"


@pytest.mark.parametrize(
    ("kwargs", "text"),
    [
        ({"n_fft": 1023}, "n_fft must be an even integer in [2, 524288]"),
        ({"n_fft": 3.5}, "n_fft must be an even integer in [2, 524288]"),
        ({"n_fft": 2**40}, "n_fft must be an even integer in [2, 524288]"),
        ({"hop_length": 0}, "hop_length must be an integer in [1, 2147483647]"),
        ({"hop_length": 2.5}, "hop_length must be an integer in [1, 2147483647]"),
        ({"hop_length": 2**40}, "hop_length must be an integer in [1, 2147483647]"),
    ],
)
def test_the_stft_sizes_are_refused_by_the_core_reader_under_the_keyword(kwargs, text):
    from libsonare import MixAssistantTrack, SonareValueError, mixing_assistant_suggest

    with pytest.raises(SonareValueError, match=rf"^mixing_assistant_suggest: {re.escape(text)}$"):
        mixing_assistant_suggest(
            [MixAssistantTrack("bass", [_tone(110.0)])], sample_rate=SAMPLE_RATE, **kwargs
        )


@pytest.mark.parametrize(
    ("name", "text"),
    [
        ("n_fft", "n_fft rescaled to sample_rate must fit the largest FFT size"),
        ("hop_length", "hop_length rescaled to sample_rate must fit the largest hop length"),
    ],
)
def test_a_rescaling_relation_names_the_keyword(name, text):
    from libsonare import MixAssistantTrack, SonareValueError, mixing_assistant_suggest

    value = 524288 if name == "n_fft" else 2**31 - 1
    with pytest.raises(SonareValueError, match=rf"^mixing_assistant_suggest: {re.escape(text)}$"):
        mixing_assistant_suggest(
            [MixAssistantTrack("bass", [np.zeros(9600, np.float32)], sample_rate=96000)],
            sample_rate=96000,
            **{name: value},
        )


@pytest.mark.parametrize("name", ("n_fft", "hop_length"))
@pytest.mark.parametrize("value", (True, np.bool_(True)))
def test_a_bool_stft_size_is_a_type_error(name, value):
    from libsonare import MixAssistantTrack, mixing_assistant_suggest

    with pytest.raises(TypeError, match=rf"^mixing_assistant_suggest: {name} must be an integer$"):
        mixing_assistant_suggest(
            [MixAssistantTrack("bass", [_tone(110.0)])],
            sample_rate=SAMPLE_RATE,
            **{name: value},
        )
