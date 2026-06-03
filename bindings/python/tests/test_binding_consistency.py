"""Cross-binding consistency tests for the Python surface.

Covers:
1. Mixer.set_pan string-enum pan_mode and keep-current-mode default.
2. Mixer.process_stereo MixerStereoResult shape + empty-input silent master.
3. Project.set_program default bank=-1 (no Bank Select) vs explicit bank.
4. Project.bounce frees the sentinel buffer on empty bounces (no leak/crash).
5. Audio.from_buffer default sample_rate is 48000.
"""

from __future__ import annotations

import json

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")


def _first_preset_json() -> str:
    """Return the JSON scene of the first built-in mixing preset."""
    from libsonare._mixing import mixing_scene_preset_json, mixing_scene_preset_names

    names = mixing_scene_preset_names()
    assert names, "expected at least one built-in mixing preset"
    return mixing_scene_preset_json(names[0])


@pytest.fixture()
def mixer():
    """Build a Mixer from the first preset scene and close it afterwards."""
    from libsonare import Mixer

    mixer = Mixer.from_scene_json(_first_preset_json(), sample_rate=48000, block_size=256)
    try:
        yield mixer
    finally:
        mixer.close()


def _pan_mode(mixer, strip_id: str) -> int:
    """Read a strip's current panMode ordinal from the serialized scene."""
    by_id = {s["id"]: s for s in json.loads(mixer.to_scene_json())["strips"]}
    return int(by_id[strip_id]["panMode"])


# --- Fix 1: set_pan string-enum + keep-mode ---------------------------------


def test_set_pan_accepts_string_pan_mode(mixer) -> None:
    """set_pan accepts string pan modes ('stereoPan' / 'dual-pan')."""
    mixer.set_pan("vocal", 0.5, "stereoPan")
    assert _pan_mode(mixer, "vocal") == 1  # PAN_MODE_STEREO_PAN

    mixer.set_pan("vocal", -0.2, "dual-pan")
    assert _pan_mode(mixer, "vocal") == 2  # PAN_MODE_DUAL_PAN

    # An int pan_mode still works for backward compatibility.
    mixer.set_pan("vocal", 0.1, 0)
    assert _pan_mode(mixer, "vocal") == 0  # PAN_MODE_BALANCE


def test_set_pan_keeps_current_mode_by_default(mixer) -> None:
    """Omitting pan_mode keeps the strip's current pan mode (no reset to Balance)."""
    mixer.set_pan("vocal", 0.5, "stereoPan")
    assert _pan_mode(mixer, "vocal") == 1

    # No pan_mode -> keep current mode; only the pan position changes.
    mixer.set_pan("vocal", 0.3)
    assert _pan_mode(mixer, "vocal") == 1

    by_id = {s["id"]: s for s in json.loads(mixer.to_scene_json())["strips"]}
    assert by_id["vocal"]["pan"] == pytest.approx(0.3, abs=1e-5)


def test_set_pan_rejects_invalid_string(mixer) -> None:
    """An unknown pan mode name raises ValueError."""
    with pytest.raises(ValueError):
        mixer.set_pan("vocal", 0.0, "sideways")


# --- Fix 2: process_stereo result shape + empty input -----------------------


def test_process_stereo_returns_named_result(mixer) -> None:
    """process_stereo returns a MixerStereoResult with left/right/sample_rate."""
    from libsonare import MixerStereoResult

    n = mixer.strip_count()
    block = [[0.1] * 256 for _ in range(n)]
    result = mixer.process_stereo(block, block)

    assert isinstance(result, MixerStereoResult)
    assert isinstance(result.left, list)
    assert isinstance(result.right, list)
    assert len(result.left) == 256
    assert len(result.right) == 256
    # sample_rate matches the mixer's configured rate.
    assert result.sample_rate == 48000
    # NamedTuple field access and positional unpacking are both available.
    left, right, sr = result
    assert sr == 48000
    assert left is result.left and right is result.right


def test_process_stereo_empty_input_returns_silent_master(mixer) -> None:
    """No input strips returns a silent (empty) master instead of raising."""
    result = mixer.process_stereo([], [])
    assert result.left == []
    assert result.right == []
    assert result.sample_rate == 48000


# --- Fix 3: set_program default bank=-1 -------------------------------------


def test_set_program_default_bank_is_minus_one() -> None:
    """set_program defaults bank to -1 (no Bank Select), matching the channel API."""
    import inspect

    from libsonare import Project

    sig = inspect.signature(Project.set_program)
    assert sig.parameters["bank"].default == -1

    project = Project()
    try:
        project.set_sample_rate(48000.0)
        _, clip = project.add_midi_clip(0.0, 4.0)

        # Default bank (-1) emits program only; explicit bank>=0 emits Bank Select.
        project.set_program(clip, 40)
        project.set_program(clip, 41, bank=0)
        project.set_program_on_channel(clip, 0, 0, 42, bank=1)
    finally:
        project.close()


# --- Fix 4: empty bounce frees without leaking ------------------------------


def test_empty_bounce_returns_empty_array_repeatably() -> None:
    """An empty/zero-length bounce returns an empty array on repeated calls."""
    from libsonare import Project

    project = Project()
    try:
        project.set_sample_rate(48000.0)
        # No tracks/clips -> nothing to render -> empty output. Repeat to surface
        # any double-free / sentinel-leak regression.
        for _ in range(5):
            audio = project.bounce(num_channels=2, sample_rate=48000)
            assert audio.size == 0
            assert audio.shape == (0, 2)
    finally:
        project.close()


def test_empty_bounce_with_builtin_instrument_is_empty() -> None:
    """The built-in-instrument bounce also frees the sentinel on empty output."""
    from libsonare import Project
    from libsonare._runtime import _get_lib

    if not hasattr(_get_lib(), "sonare_project_bounce_with_builtin_instruments"):
        pytest.skip("libsonare built without the built-in instrument bounce ABI")

    project = Project()
    try:
        project.set_sample_rate(48000.0)
        for _ in range(5):
            audio = project.bounce_with_builtin_instrument(num_channels=2, sample_rate=48000)
            assert audio.size == 0
            assert audio.shape == (0, 2)
    finally:
        project.close()


# --- Fix 5: Audio.from_buffer default sample_rate ---------------------------


def test_audio_from_buffer_default_sample_rate_is_48000() -> None:
    """Audio.from_buffer defaults sample_rate to 48000 when omitted."""
    from libsonare import Audio

    audio = Audio.from_buffer([0.0] * 1000)
    assert audio.sample_rate == 48000

    explicit = Audio.from_buffer([0.0] * 1000, sample_rate=16000)
    assert explicit.sample_rate == 16000
