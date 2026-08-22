"""Tests for the offline mixing assistant in the Python binding.

The assistant's numeric output is DSP-dependent, so nothing here pins a gain, a
source class or a line count. What is asserted is the structure of the result
document and the invariants the API promises: one track entry per input, a
strip for every track id, a scene the mixer can load, and an empty explanation
when every decision domain is switched off.
"""

from __future__ import annotations

import json

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
    from libsonare import MixTrackInput

    return [
        MixTrackInput("kick", _percussive(55.0), None, "kick"),
        MixTrackInput("bass", _tone(110.0), None, "bass"),
        MixTrackInput("keys", _noisy(660.0, 7), None, "keys"),
    ]


@pytest.fixture(scope="module")
def mixed_tracks():
    """A mono track and a stereo track in one call."""
    from libsonare import MixTrackInput

    return [
        MixTrackInput("kick", _percussive(55.0), None, "kick"),
        MixTrackInput("pad", _noisy(660.0, 3), _noisy(664.0, 4), "pad"),
    ]


@pytest.fixture(scope="module")
def mono_suggestion(mono_tracks):
    """The default suggestion over ``mono_tracks``, analysed once for the module."""
    from libsonare import suggest_mix_scene

    return suggest_mix_scene(mono_tracks, sample_rate=SAMPLE_RATE)


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
    return [line for line in result["explanation"] if line.startswith(_HIGH_PASS_REASON)]


def _assert_document_shape(result, track_count: int) -> None:
    """Assert the result document's structure for ``track_count`` inputs."""
    assert set(result) == {"scene", "tracks", "mix", "explanation"}
    assert len(result["tracks"]) == track_count
    assert all(isinstance(line, str) for line in result["explanation"])
    scene = result["scene"]
    assert set(scene) >= {"version", "strips", "buses", "connections"}


def test_source_class_names_resolve_to_their_index():
    from libsonare import mix_source_class_from_name, mix_source_class_names

    names = mix_source_class_names()
    assert names, "expected at least one mixing-assistant source class"
    for name in names:
        assert mix_source_class_from_name(name) >= 0


def test_unknown_source_class_name_resolves_to_minus_one():
    from libsonare import mix_source_class_from_name

    assert mix_source_class_from_name("not-a-source-class") == -1


def test_suggest_over_mono_tracks(mono_tracks, mono_suggestion):
    _assert_document_shape(mono_suggestion, len(mono_tracks))
    assert mono_suggestion["explanation"], "a usable multi-track set should be explained"
    strip_ids = {strip["id"] for strip in mono_suggestion["scene"]["strips"]}
    assert strip_ids >= {track.track_id for track in mono_tracks}


def test_reported_source_classes_are_declared(mono_suggestion):
    from libsonare import mix_source_class_names

    names = set(mix_source_class_names())
    assert {track["source"] for track in mono_suggestion["tracks"]} <= names


def test_suggest_over_mixed_mono_and_stereo_tracks(mixed_tracks):
    from libsonare import suggest_mix_scene

    result = suggest_mix_scene(mixed_tracks, sample_rate=SAMPLE_RATE)
    _assert_document_shape(result, len(mixed_tracks))
    channel_counts = {track["stripId"]: track["channelCount"] for track in result["tracks"]}
    assert channel_counts["kick"] == 1
    assert channel_counts["pad"] == 2


def test_suggest_over_tracks_of_differing_lengths():
    from libsonare import MixTrackInput, suggest_mix_scene

    tracks = [
        MixTrackInput("kick", _percussive(55.0, duration_sec=DURATION_SEC), None, "kick"),
        MixTrackInput("bass", _tone(110.0, duration_sec=DURATION_SEC * 0.75), None, "bass"),
        MixTrackInput(
            "pad",
            _noisy(660.0, 3, duration_sec=DURATION_SEC * 0.9),
            _noisy(664.0, 4, duration_sec=DURATION_SEC * 0.9),
            "pad",
        ),
    ]
    result = suggest_mix_scene(tracks, sample_rate=SAMPLE_RATE)
    _assert_document_shape(result, len(tracks))
    strip_ids = {strip["id"] for strip in result["scene"]["strips"]}
    assert strip_ids >= {track.track_id for track in tracks}


def test_suggest_without_tracks_yields_an_empty_suggestion():
    from libsonare import suggest_mix_scene

    result = suggest_mix_scene([], sample_rate=SAMPLE_RATE)
    _assert_document_shape(result, 0)
    assert result["explanation"] == []
    assert result["scene"]["strips"] == []


def test_suggest_with_a_silent_track():
    from libsonare import MixTrackInput, suggest_mix_scene

    n = int(SAMPLE_RATE * DURATION_SEC)
    tracks = [
        MixTrackInput("kick", _percussive(55.0), None, "kick"),
        MixTrackInput("silence", np.zeros(n, dtype=np.float32), None, "pad"),
    ]
    result = suggest_mix_scene(tracks, sample_rate=SAMPLE_RATE)
    _assert_document_shape(result, len(tracks))
    # A silent track carries no measurable level, so it is reported as excluded
    # rather than staged against the loudness target.
    excluded = {track["stripId"]: track["usable"] for track in result["tracks"]}
    assert excluded["silence"] is False


def test_scene_json_matches_the_scene_in_the_full_result(mono_tracks, mono_suggestion):
    from libsonare import suggest_mix_scene_json

    scene = json.loads(suggest_mix_scene_json(mono_tracks, sample_rate=SAMPLE_RATE))
    assert scene == mono_suggestion["scene"]


def test_scene_json_loads_into_a_mixer(mono_tracks):
    from libsonare import Mixer, suggest_mix_scene_json

    scene_json = suggest_mix_scene_json(mono_tracks, sample_rate=SAMPLE_RATE)
    mixer = Mixer.from_scene_json(scene_json, sample_rate=SAMPLE_RATE, block_size=256)
    try:
        assert mixer.strip_count() >= len(mono_tracks)
    finally:
        mixer.close()


def test_every_domain_off_yields_no_explanation(mono_tracks):
    from libsonare import suggest_mix_scene

    result = suggest_mix_scene(mono_tracks, sample_rate=SAMPLE_RATE, **_ALL_DOMAINS_OFF)
    _assert_document_shape(result, len(mono_tracks))
    assert result["explanation"] == []


def test_every_domain_off_still_yields_a_loadable_scene(mono_tracks):
    from libsonare import suggest_mix_scene_json

    # The switches suppress decisions, not tracks: the scene still describes
    # every input so it remains a usable starting point for the mixer.
    scene = json.loads(
        suggest_mix_scene_json(mono_tracks, sample_rate=SAMPLE_RATE, **_ALL_DOMAINS_OFF)
    )
    assert {strip["id"] for strip in scene["strips"]} >= {track.track_id for track in mono_tracks}


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
    from libsonare import suggest_mix_scene

    result = suggest_mix_scene(mono_tracks, sample_rate=SAMPLE_RATE, **{domain: False})
    _assert_document_shape(result, len(mono_tracks))
    # A disabled domain is skipped rather than evaluated and discarded, so it
    # can only ever remove reasoning, never add any.
    assert len(result["explanation"]) <= len(mono_suggestion["explanation"])


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
    from libsonare import suggest_mix_scene

    result = suggest_mix_scene(mono_tracks, sample_rate=SAMPLE_RATE, **{option: value})
    _assert_document_shape(result, len(mono_tracks))


def test_high_pass_is_proposed_only_once_the_switch_is_on():
    from libsonare import MixTrackInput, suggest_mix_scene

    # A local fixture rather than the shared one: this case needs low-frequency
    # residue that would move unrelated assertions elsewhere.
    tracks = [MixTrackInput("vox", _voice_with_rumble(), None, "Lead Vox")]
    off = suggest_mix_scene(tracks, sample_rate=SAMPLE_RATE)
    on = suggest_mix_scene(tracks, sample_rate=SAMPLE_RATE, enable_high_pass=True)

    _assert_document_shape(off, len(tracks))
    _assert_document_shape(on, len(tracks))
    # Off by default: the measurement is not taken at all, so no filter and no
    # line about one.
    assert _high_pass_owners(off["scene"]) == []
    assert _high_pass_lines(off) == []
    # On: the vocal track's 80 Hz corner earns a pre-fader filter of its own.
    assert _high_pass_owners(on["scene"]) == ["vox"]
    assert len(_high_pass_lines(on)) == 1


def test_target_track_lufs_moves_gain_staging(mono_tracks):
    from libsonare import suggest_mix_scene

    quiet = suggest_mix_scene(mono_tracks, sample_rate=SAMPLE_RATE, target_track_lufs=-30.0)
    loud = suggest_mix_scene(mono_tracks, sample_rate=SAMPLE_RATE, target_track_lufs=-6.0)
    quiet_trims = [strip["inputTrimDb"] for strip in quiet["scene"]["strips"]]
    loud_trims = [strip["inputTrimDb"] for strip in loud["scene"]["strips"]]
    assert quiet_trims and len(quiet_trims) == len(loud_trims)
    # A louder target can only ever ask for more input gain, never less.
    assert all(low <= high for low, high in zip(quiet_trims, loud_trims, strict=True))


def test_unset_options_keep_the_library_defaults(mono_tracks, mono_suggestion):
    from libsonare import suggest_mix_scene

    # Restating a default on this side would mask a change to the core's own
    # default, so an unset option must not reach the param array at all.
    explicit = suggest_mix_scene(
        mono_tracks,
        sample_rate=SAMPLE_RATE,
        target_track_lufs=-18.0,
        suggestion_strength=1.0,
        eq_max_cut_db=4.0,
        mix_bus_headroom_dbtp=-6.0,
    )
    assert explicit["scene"] == mono_suggestion["scene"]


def test_empty_track_id_is_rejected():
    from libsonare import MixTrackInput, SonareValueError, suggest_mix_scene

    with pytest.raises(SonareValueError, match="track_id"):
        suggest_mix_scene([MixTrackInput("", _tone(110.0))], sample_rate=SAMPLE_RATE)


def test_duplicate_track_ids_are_rejected():
    from libsonare import MixTrackInput, SonareValueError, suggest_mix_scene

    tracks = [MixTrackInput("bass", _tone(110.0)), MixTrackInput("bass", _tone(220.0))]
    with pytest.raises(SonareValueError, match="duplicate"):
        suggest_mix_scene(tracks, sample_rate=SAMPLE_RATE)


def test_non_finite_samples_are_rejected():
    from libsonare import MixTrackInput, SonareValueError, suggest_mix_scene

    samples = _tone(110.0).copy()
    samples[17] = np.nan
    with pytest.raises(SonareValueError, match="NaN or Inf at index 17"):
        suggest_mix_scene([MixTrackInput("bass", samples)], sample_rate=SAMPLE_RATE)


def test_mismatched_channel_lengths_are_rejected():
    from libsonare import MixTrackInput, SonareValueError, suggest_mix_scene

    tracks = [MixTrackInput("pad", _tone(440.0), _tone(440.0, duration_sec=DURATION_SEC * 0.5))]
    with pytest.raises(SonareValueError, match="lengths must match"):
        suggest_mix_scene(tracks, sample_rate=SAMPLE_RATE)


def test_non_positive_sample_rate_is_rejected():
    from libsonare import MixTrackInput, SonareValueError, suggest_mix_scene

    with pytest.raises(SonareValueError, match="sample_rate"):
        suggest_mix_scene([MixTrackInput("bass", _tone(110.0))], sample_rate=0)


def test_non_finite_option_is_rejected():
    from libsonare import MixTrackInput, SonareValueError, suggest_mix_scene

    with pytest.raises(SonareValueError, match="finite"):
        suggest_mix_scene(
            [MixTrackInput("bass", _tone(110.0))],
            sample_rate=SAMPLE_RATE,
            target_track_lufs=float("nan"),
        )


def test_scene_json_rejects_bad_input_the_same_way():
    from libsonare import MixTrackInput, SonareValueError, suggest_mix_scene_json

    tracks = [MixTrackInput("bass", _tone(110.0)), MixTrackInput("bass", _tone(220.0))]
    with pytest.raises(SonareValueError, match="duplicate"):
        suggest_mix_scene_json(tracks, sample_rate=SAMPLE_RATE)
