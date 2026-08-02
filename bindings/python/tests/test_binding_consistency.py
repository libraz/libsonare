"""Cross-binding consistency tests for the Python surface.

Covers:
1. Mixer.set_pan string-enum pan_mode and keep-current-mode default.
2. Mixer.process_stereo MixerStereoResult shape + empty-input silent master.
3. Project.set_program default bank=-1 (no Bank Select) vs explicit bank.
4. Project.bounce frees the sentinel buffer on empty bounces (no leak/crash).
5. Audio.from_buffer default sample_rate is 48000.
"""

from __future__ import annotations

import ast
import inspect
import json
from pathlib import Path

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")


def _literal_default(node: ast.expr) -> object:
    """Return a stub default without evaluating arbitrary source code."""
    return ast.literal_eval(node)


def test_audio_stub_signatures_match_runtime() -> None:
    """Keep the public ``Audio`` stub mechanically lockstep with its facade.

    ``mypy`` verifies consumers of ``audio.pyi``, but cannot detect a method
    whose stub quietly diverges from the Python implementation.  Parse the
    shipped stub rather than importing it, then compare every concrete method
    with literal defaults to ``inspect.signature`` on the public class.
    """
    from libsonare.audio import Audio

    stub_path = Path(__file__).parents[1] / "src" / "libsonare" / "audio.pyi"
    module = ast.parse(stub_path.read_text(encoding="utf-8"), filename=str(stub_path))
    audio_stub = next(
        node for node in module.body if isinstance(node, ast.ClassDef) and node.name == "Audio"
    )
    checked = 0
    for node in audio_stub.body:
        if not isinstance(node, ast.FunctionDef):
            continue
        if node.name.startswith("__"):
            continue
        decorators = {
            decorator.id for decorator in node.decorator_list if isinstance(decorator, ast.Name)
        }
        if "property" in decorators or "overload" in decorators:
            continue
        runtime = getattr(Audio, node.name, None)
        assert runtime is not None, f"Audio.{node.name} is present in audio.pyi but not at runtime"
        stub_args = [*node.args.posonlyargs, *node.args.args]
        if "classmethod" in decorators and stub_args:
            stub_args = stub_args[1:]
        runtime_parameters = list(inspect.signature(runtime).parameters.values())
        stub_names = [argument.arg for argument in stub_args]
        assert [parameter.name for parameter in runtime_parameters] == stub_names + [
            argument.arg for argument in node.args.kwonlyargs
        ], f"Audio.{node.name} parameter names drifted"
        assert [
            parameter.kind is inspect.Parameter.KEYWORD_ONLY for parameter in runtime_parameters
        ] == [False] * len(stub_args) + [True] * len(node.args.kwonlyargs), (
            f"Audio.{node.name} keyword-only parameters drifted"
        )
        try:
            defaults = [inspect.Parameter.empty] * (len(stub_args) - len(node.args.defaults)) + [
                _literal_default(default) for default in node.args.defaults
            ]
            defaults.extend(
                inspect.Parameter.empty if default is None else _literal_default(default)
                for default in node.args.kw_defaults
            )
        except ValueError:
            # Names such as ``DEFAULT_HOP_LENGTH`` are deliberately resolved
            # by the module at import time; this structural guard does not
            # execute stub expressions.
            continue
        for parameter, expected in zip(runtime_parameters, defaults, strict=True):
            if expected is inspect.Parameter.empty:
                assert parameter.default is inspect.Parameter.empty, (
                    f"Audio.{node.name}.{parameter.name} unexpectedly has a runtime default"
                )
            else:
                assert parameter.default == expected, (
                    f"Audio.{node.name}.{parameter.name} default drifted: "
                    f"{parameter.default!r} != {expected!r}"
                )
        checked += 1

    # This is intentionally broad enough to prevent the test being weakened
    # into a one-method smoke check when the facade grows.
    assert checked >= 50


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


# --- Pan mode coercion and preservation -------------------------------------


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


# --- Stereo result shape and empty input ------------------------------------


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


# --- Program-change default bank --------------------------------------------


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
        exported = project.export_smf()
        assert bytes([0xC0, 40]) in exported
        assert bytes([0xB0, 0, 0]) not in exported
        assert bytes([0xB0, 32, 0]) not in exported

        project.set_program(clip, 41, bank=0)
        exported = project.export_smf()
        assert bytes([0xC0, 41]) in exported
        assert bytes([0xB0, 0, 0]) in exported
        assert bytes([0xB0, 32, 0]) in exported

        project.set_program_on_channel(clip, 0, 0, 42, bank=1)
        exported = project.export_smf()
        assert bytes([0xC0, 42]) in exported
        assert bytes([0xB0, 0, 0]) in exported
        assert bytes([0xB0, 32, 1]) in exported
    finally:
        project.close()


def test_public_analysis_and_catalog_helpers_return_usable_values() -> None:
    """Exercise public helpers that otherwise only had C-ABI coverage."""
    import math

    from libsonare import (
        analyze_impulse_response,
        chroma,
        cyclic_tempogram,
        decompose_with_init,
        detect_acoustic,
        detect_downbeats,
        detect_key_candidates,
        mastering_insert_param_info,
        mastering_preset_names,
    )

    sample_rate = 22050
    samples = [0.25 * math.sin(2.0 * math.pi * 440.0 * i / sample_rate) for i in range(8192)]
    impulse = [math.exp(-i / 1200.0) for i in range(8192)]

    measured = analyze_impulse_response(impulse, sample_rate, n_octave_bands=3)
    blind = detect_acoustic(samples, sample_rate, n_octave_bands=3, n_third_octave_subbands=6)
    assert len(measured.rt60_bands) == 3
    assert len(blind.rt60_bands) == 3
    # Band estimators report NaN when a synthetic signal has insufficient
    # usable decay in a particular band; the public result must still retain
    # its requested band layout without raising.
    assert any(math.isfinite(value) for value in measured.rt60_bands + blind.rt60_bands)

    candidates = detect_key_candidates(samples, sample_rate, n_fft=1024, hop_length=256)
    assert candidates
    assert all(math.isfinite(candidate.correlation) for candidate in candidates)
    assert isinstance(detect_downbeats(samples, sample_rate), list)

    chromagram = chroma(samples, sample_rate, n_fft=1024, hop_length=256)
    assert chromagram.n_chroma == 12
    assert len(chromagram.features) == chromagram.n_chroma * chromagram.n_frames
    frames, tempogram = cyclic_tempogram([abs(sample) for sample in samples], sample_rate, 256, 128)
    assert frames > 0
    assert len(tempogram) == frames * 60

    w, h = decompose_with_init([1.0, 2.0, 3.0, 4.0], 2, 2, 1, n_iter=2, init="nndsvd")
    assert w.shape == (2, 1)
    assert h.shape == (1, 2)
    assert "pop" in mastering_preset_names()
    assert mastering_insert_param_info("dynamics.compressor")


# --- Empty bounce result ownership ------------------------------------------


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


# --- Audio buffer default sample rate ---------------------------------------


def test_audio_from_buffer_default_sample_rate_is_48000() -> None:
    """Audio.from_buffer defaults sample_rate to 48000 when omitted."""
    from libsonare import Audio

    audio = Audio.from_buffer([0.0] * 1000)
    assert audio.sample_rate == 48000

    explicit = Audio.from_buffer([0.0] * 1000, sample_rate=16000)
    assert explicit.sample_rate == 16000
