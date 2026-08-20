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


# ---------------------------------------------------------------------------
# Shipped .pyi stubs vs runtime signatures.
#
# mypy validates CONSUMERS of a stub but cannot see a stub that has drifted from
# its implementation, so a stub is only as trustworthy as whatever compares it
# to runtime. That comparison used to cover ``Audio`` and one analyzer function,
# which left `analyzer.pyi`'s free functions, `engine.pyi` (including the most
# frequently edited facade in the repository) and `types.pyi` unchecked. The
# comparison below is the same one, generalized over every stub and applied to
# every concrete callable each declares.
# ---------------------------------------------------------------------------

STUB_DIR = Path(__file__).parents[1] / "src" / "libsonare"

# Minimum concrete callables each stub must contribute, so the parametrization
# cannot quietly collapse to a smoke check as the facades grow or a decorator
# filter starts over-matching.
_STUB_FLOORS = {"analyzer.pyi": 250, "engine.pyi": 110, "audio.pyi": 50}


def _is_checkable(node: ast.FunctionDef) -> bool:
    """Concrete callables only: dunders, properties and overloads are skipped."""
    if node.name.startswith("__"):
        return False
    decorators = {d.id for d in node.decorator_list if isinstance(d, ast.Name)}
    return not ({"property", "overload"} & decorators)


def _collect_stub_callables() -> list[tuple[str, str, ast.FunctionDef]]:
    """(stub file, dotted name, stub node) for every concrete callable declared."""
    out: list[tuple[str, str, ast.FunctionDef]] = []
    for stub_name in sorted(_STUB_FLOORS):
        tree = ast.parse((STUB_DIR / stub_name).read_text(encoding="utf-8"))
        for node in tree.body:
            if isinstance(node, ast.FunctionDef):
                if _is_checkable(node):
                    out.append((stub_name, node.name, node))
            elif isinstance(node, ast.ClassDef):
                out += [
                    (stub_name, f"{node.name}.{m.name}", m)
                    for m in node.body
                    if isinstance(m, ast.FunctionDef) and _is_checkable(m)
                ]
    return out


STUB_CALLABLES = _collect_stub_callables()


def _resolve(dotted: str) -> object:
    """Resolve a stub's dotted name against the imported package."""
    import libsonare

    target: object = libsonare
    for part in dotted.split("."):
        target = getattr(target, part, None)
        if target is None:
            return None
    return target


@pytest.mark.parametrize(
    ("stub_name", "dotted", "node"),
    [pytest.param(s, d, n, id=f"{s}:{d}") for s, d, n in STUB_CALLABLES],
)
def test_stub_signature_matches_runtime(stub_name: str, dotted: str, node: ast.FunctionDef) -> None:
    """A shipped stub's parameter names, kinds and defaults must match runtime."""
    runtime = _resolve(dotted)
    assert runtime is not None, f"{dotted} is declared in {stub_name} but absent at runtime"
    try:
        runtime_parameters = list(inspect.signature(runtime).parameters.values())
    except (TypeError, ValueError) as exc:  # pragma: no cover - defensive
        pytest.fail(f"{dotted}: runtime object has no inspectable signature ({exc})")

    decorators = {d.id for d in node.decorator_list if isinstance(d, ast.Name)}
    posonly, positional = list(node.args.posonlyargs), list(node.args.args)
    # A classmethod's `cls` is not in the runtime signature; a method's `self`
    # is already absent because the lookup goes through the class attribute.
    if "classmethod" in decorators and positional:
        positional = positional[1:]
    stub_args = posonly + positional

    expected_names = [a.arg for a in stub_args] + [a.arg for a in node.args.kwonlyargs]
    assert [p.name for p in runtime_parameters] == expected_names, (
        f"{dotted}: parameter names drifted from {stub_name}"
    )

    expected_kinds = (
        [inspect.Parameter.POSITIONAL_ONLY] * len(posonly)
        + [inspect.Parameter.POSITIONAL_OR_KEYWORD] * len(positional)
        + [inspect.Parameter.KEYWORD_ONLY] * len(node.args.kwonlyargs)
    )
    assert [p.kind for p in runtime_parameters] == expected_kinds, (
        f"{dotted}: parameter kinds drifted from {stub_name}"
    )

    try:
        defaults = [inspect.Parameter.empty] * (len(stub_args) - len(node.args.defaults)) + [
            _literal_default(d) for d in node.args.defaults
        ]
        defaults += [
            inspect.Parameter.empty if d is None else _literal_default(d)
            for d in node.args.kw_defaults
        ]
    except ValueError:
        # Names such as ``DEFAULT_HOP_LENGTH`` are deliberately resolved by the
        # module at import time; this structural guard does not execute stub
        # expressions, so default VALUES go unchecked for this callable. Names
        # and kinds above are still enforced.
        return

    for parameter, expected in zip(runtime_parameters, defaults, strict=True):
        if expected is inspect.Parameter.empty:
            assert parameter.default is inspect.Parameter.empty, (
                f"{dotted}.{parameter.name} has a runtime default that {stub_name} omits"
            )
        elif expected is Ellipsis:
            # `= ...` is the PEP 484 stub spelling for "has a default, value
            # elided", so require one to exist without pinning its value.
            assert parameter.default is not inspect.Parameter.empty, (
                f"{dotted}.{parameter.name}: {stub_name} declares a default with `...` "
                "but the runtime parameter is required"
            )
        else:
            assert parameter.default == expected, (
                f"{dotted}.{parameter.name} default drifted: {parameter.default!r} != {expected!r}"
            )


@pytest.mark.parametrize("stub_name", sorted(_STUB_FLOORS))
def test_stub_coverage_floor(stub_name: str) -> None:
    """Each stub must keep contributing callables to the comparison above."""
    count = sum(1 for name, _dotted, _node in STUB_CALLABLES if name == stub_name)
    assert count >= _STUB_FLOORS[stub_name], (
        f"{stub_name} contributes only {count} checked callables "
        f"(floor {_STUB_FLOORS[stub_name]}); the stub shrank or the collector stopped matching"
    )


def test_types_stub_declares_no_comparable_callable() -> None:
    """``types.pyi`` is result/config shapes, so it has nothing to compare here.

    Recorded as an assertion rather than an omission: every member is a
    property, a dunder or a dataclass field today, so if a real method ever
    appears there this fails and `types.pyi` has to join the comparison above
    instead of silently staying outside it.
    """
    tree = ast.parse((STUB_DIR / "types.pyi").read_text(encoding="utf-8"))
    concrete = [
        f"{cls.name}.{m.name}"
        for cls in tree.body
        if isinstance(cls, ast.ClassDef)
        for m in cls.body
        if isinstance(m, ast.FunctionDef) and _is_checkable(m)
    ]
    assert not concrete, (
        f"types.pyi now declares concrete callables {concrete}; add 'types.pyi' to "
        "_STUB_FLOORS so they are compared against runtime"
    )


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


def _stub_typed_dict_fields() -> dict[str, set[str]]:
    """Annotated field names per ``TypedDict`` class declared in ``types.pyi``."""
    tree = ast.parse((STUB_DIR / "types.pyi").read_text(encoding="utf-8"))
    fields: dict[str, set[str]] = {}
    for node in tree.body:
        if not isinstance(node, ast.ClassDef):
            continue
        if not any(isinstance(base, ast.Name) and base.id == "TypedDict" for base in node.bases):
            continue
        fields[node.name] = {
            member.target.id
            for member in node.body
            if isinstance(member, ast.AnnAssign) and isinstance(member.target, ast.Name)
        }
    return fields


def test_types_stub_typed_dicts_match_runtime() -> None:
    """Every ``TypedDict`` in ``types.pyi`` declares the runtime class's keys.

    The callable comparison above skips ``types.pyi`` entirely because it holds
    no methods, which left its ``TypedDict`` keys unchecked from either side: a
    key present at runtime but missing from the stub is invisible to the tests
    and to ``mypy`` alike, and it is a key describing the C-ABI capabilities
    payload, so the drift is silent on both surfaces at once.
    """
    from libsonare import types as runtime_types

    stub_fields = _stub_typed_dict_fields()
    # Floor: the stub is parsed, not imported, so a parser change that stops
    # matching would otherwise pass by comparing nothing.
    assert len(stub_fields) >= 8, (
        f"only {len(stub_fields)} TypedDicts parsed from types.pyi; the collector stopped matching"
    )

    compared = 0
    for name, declared in sorted(stub_fields.items()):
        runtime = getattr(runtime_types, name, None)
        if runtime is None:
            raise AssertionError(f"types.pyi declares {name}, which does not exist at runtime")
        expected = set(runtime.__annotations__)
        assert declared == expected, (
            f"{name} keys differ: stub-only {sorted(declared - expected)}, "
            f"runtime-only {sorted(expected - declared)}"
        )
        compared += 1
    assert compared == len(stub_fields)
