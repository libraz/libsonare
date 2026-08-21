#!/usr/bin/env python3
"""Accept the mixing assistant's scene suggestion across the language surfaces.

The C ABI is the oracle.  Python drives
``sonare_mixing_assistant_suggest_scene_json`` through the existing ctypes
declarations, then the public Python, Node, and WASM facades are handed the same
tracks and the same options and must return the very same scene JSON.  This is
what a request-object migration or a per-binding option-name table can break
without the C-ABI parity checker seeing anything: parity compares signatures,
not the document a facade produces.

The JavaScript subprocesses use a deliberately small protocol: one
base64-encoded request document on stdin (per-track samples as base64
little-endian Float32 blocks) and the base64-encoded scene JSON on stdout.
"""

from __future__ import annotations

import argparse
import base64
import ctypes
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, NamedTuple

import numpy as np


ROOT = Path(__file__).resolve().parents[2]
PYTHON_SRC = ROOT / "bindings" / "python" / "src"
NODE_INDEX = ROOT / "bindings" / "node" / "dist" / "index.js"
WASM_INDEX = ROOT / "bindings" / "wasm" / "dist" / "index.js"

SAMPLE_RATE = 48_000
FULL_SECONDS = 0.6
SHORT_SECONDS = 0.45

# Canonical (camelCase) assistant option name -> Python keyword argument. The C
# ABI and both JavaScript facades take the canonical spelling, so this table is
# the only place the Python facade's idiomatic naming is applied; a rename on
# either side of it shows up as a scene mismatch rather than as silence.
OPTION_KEYS = {
    "targetTrackLufs": "target_track_lufs",
    "suggestionStrength": "suggestion_strength",
    "eqMaxCutDb": "eq_max_cut_db",
    "mixBusHeadroomDbtp": "mix_bus_headroom_dbtp",
    "enableStructure": "enable_structure",
    "enableGain": "enable_gain",
    "enableBalance": "enable_balance",
    "enableEq": "enable_eq",
    "enableDynamics": "enable_dynamics",
    "enableImage": "enable_image",
    "nFft": "n_fft",
    "hopLength": "hop_length",
}

# Two cases: the defaults, and one option moved off its default. The second case
# is what proves the options actually reach the core on every surface -- four
# surfaces that all ignore an option would agree perfectly and mean nothing.
CASES: dict[str, dict[str, float | bool]] = {
    "defaults": {},
    "suggestionStrength=0.5": {"suggestionStrength": 0.5},
}

SURFACES = ("c", "python", "node", "wasm")

# Surfaces whose scene text must be byte-identical to the C oracle's. The C ABI,
# the Python facade and the Node addon all run the same natively compiled
# serializer over the same samples, so any difference at all -- including one
# that only shows in the printed digits -- is drift.
#
# WASM is not in this set. Its C++ is compiled by a different toolchain against a
# different libm, so transcendental results part company with the native build in
# the last couple of float digits; that reaches the serializer as a genuinely
# different number (7.583096981 vs 7.583096504 ms on a suggested attack time),
# not as a formatting difference, and no amount of normalizing the text can undo
# it. It is therefore compared as a parsed tree with a relative tolerance, which
# still fails on a wrong value, a wrong key, a missing strip or a reordered
# document.
EXACT_MATCH_SURFACES = ("c", "python", "node")
NUMERIC_RTOL = 1.0e-6


class HarnessFailure(RuntimeError):
    """A deterministic acceptance failure with an actionable message."""


class Track(NamedTuple):
    """One fixture track in the shape every surface's facade takes."""

    track_id: str
    name: str
    left: np.ndarray
    right: np.ndarray | None


def _lcg_noise(count: int, seed: int) -> np.ndarray:
    """Deterministic pseudo-noise in ``[-1, 1)``.

    A linear congruential generator rather than a numpy ``Generator``: the
    fixture has to produce the same bytes on every run and under every numpy
    version for a cross-surface comparison to say anything at all.
    """
    state = seed & 0xFFFF_FFFF
    values = np.empty(count, dtype=np.float32)
    for index in range(count):
        state = (state * 1664525 + 1013904223) & 0xFFFF_FFFF
        values[index] = float(state) / 4294967295.0 * 2.0 - 1.0
    return values


def _make_tracks() -> list[Track]:
    """Build the synthetic multi-track fixture.

    Synthetic rather than recorded: a stem set would be a licensing and
    repository-size problem, and a signal whose band placement is known by
    construction gives every assistant domain something real to decide about.
    The set deliberately mixes mono and stereo tracks and includes one track
    that is shorter than the others, because per-track lengths are the part of
    the multi-track convention a binding is most likely to get wrong.
    """
    full = int(SAMPLE_RATE * FULL_SECONDS)
    short = int(SAMPLE_RATE * SHORT_SECONDS)
    two_pi = 2.0 * np.pi
    long_t = np.arange(full, dtype=np.float64) / SAMPLE_RATE
    short_t = np.arange(short, dtype=np.float64) / SAMPLE_RATE

    # Kick: everything under 130 Hz, struck every 0.25 s and gone almost as soon
    # as it arrives.
    kick_envelope = np.exp(-28.0 * np.mod(long_t, 0.25))
    kick = 0.8 * kick_envelope * np.sin(two_pi * 55.0 * long_t)

    # Bass: the same register held rather than struck. Overlapping the kick's
    # band is deliberate, so the masking measurements have a real pair.
    bass = 0.25 * (
        np.sin(two_pi * 82.0 * long_t) + 0.3 * np.sin(two_pi * 164.0 * long_t)
    )

    # Voice: a harmonic series with a weak fundamental and its energy in the
    # 500-2000 Hz band. The partial weights are what put the centroid and the
    # rolloff where a voice's are; they are not an attempt to sound like one.
    partials = np.array(
        [0.30, 0.70, 0.90, 0.85, 0.80, 0.70, 0.60, 0.55, 0.50, 0.45, 0.40, 0.35]
    )
    vox = np.zeros(full, dtype=np.float64)
    for index, weight in enumerate(partials):
        vox += weight * np.sin(two_pi * 180.0 * (index + 1) * long_t)
    vox *= 0.045
    # A small channel offset so the stereo image has a width to measure rather
    # than a duplicated mono signal.
    vox_right = np.roll(vox, 13)

    # Guitar: plucked, double-tracked at slightly different pitches, and the one
    # track that stops early. 1/sqrt(n) partial weights keep enough upper
    # partials for the pick attack to register as high-mid content.
    def pluck(root_hz: float, phase: float) -> np.ndarray:
        envelope = np.exp(-16.0 * np.mod(short_t, 0.25))
        out = np.zeros(short, dtype=np.float64)
        for partial in range(1, 13):
            out += (1.0 / np.sqrt(partial)) * np.sin(
                two_pi * root_hz * partial * short_t + phase
            )
        return 0.07 * envelope * out

    hiss = 0.01 * _lcg_noise(short, 0x1357_2468)
    return [
        Track("kick", "Kick In", kick.astype(np.float32), None),
        Track("bass", "Bass DI", bass.astype(np.float32), None),
        Track("vox", "Lead Vox", vox.astype(np.float32), vox_right.astype(np.float32)),
        Track(
            "gtr",
            "Gtr Double",
            (pluck(330.0, 0.0) + hiss).astype(np.float32),
            (pluck(331.5, 0.7) - hiss).astype(np.float32),
        ),
    ]


def _load_binding() -> tuple[Any, ...]:
    """Import the public Python facade and the low-level ctypes helpers."""
    if str(PYTHON_SRC) not in sys.path:
        sys.path.insert(0, str(PYTHON_SRC))
    try:
        from libsonare import MixTrackInput, suggest_mix_scene_json
        from libsonare._runtime import (
            SonareMasteringParam,
            _check,
            _get_lib,
            _to_c_float_array,
        )
    except (ImportError, OSError, RuntimeError) as exc:
        raise HarnessFailure(
            "Python binding unavailable; install the bindings/python environment and "
            f"build the shared library (reason: {exc})"
        ) from exc
    return (
        MixTrackInput,
        suggest_mix_scene_json,
        SonareMasteringParam,
        _check,
        _get_lib,
        _to_c_float_array,
    )


def _suggest_c(
    tracks: list[Track], options: dict[str, float | bool], binding: tuple[Any, ...]
) -> str:
    """Call the C ABI directly and return the scene JSON it writes."""
    (
        _track_input,
        _facade,
        mastering_param,
        check,
        get_lib,
        to_c_float_array,
    ) = binding
    lib = get_lib()
    if not hasattr(lib, "sonare_mixing_assistant_suggest_scene_json"):
        raise HarnessFailure(
            "the shared library was built without mixing assistant support "
            "(rebuild with BUILD_MIXING_ASSISTANT=ON)"
        )

    count = len(tracks)
    float_ptr = ctypes.POINTER(ctypes.c_float)
    left_arrays = [to_c_float_array(track.left)[0] for track in tracks]
    right_arrays = [
        None if track.right is None else to_c_float_array(track.right)[0]
        for track in tracks
    ]
    id_buffers = [track.track_id.encode("utf-8") for track in tracks]
    name_buffers = [track.name.encode("utf-8") for track in tracks]

    left_ptrs = (float_ptr * count)(
        *[ctypes.cast(array, float_ptr) for array in left_arrays]
    )
    right_ptrs = (float_ptr * count)(
        *[
            None if array is None else ctypes.cast(array, float_ptr)
            for array in right_arrays
        ]
    )
    id_ptrs = (ctypes.c_char_p * count)(*id_buffers)
    name_ptrs = (ctypes.c_char_p * count)(*name_buffers)
    lengths = (ctypes.c_size_t * count)(*[int(track.left.size) for track in tracks])

    items = list(options.items())
    key_buffers = [key.encode("utf-8") for key, _ in items]
    params = (mastering_param * len(items))(
        *[
            mastering_param(key=key_buffers[index], value=float(value))
            for index, (_, value) in enumerate(items)
        ]
    )

    json_ptr = ctypes.c_char_p()
    check(
        lib.sonare_mixing_assistant_suggest_scene_json(
            left_ptrs,
            right_ptrs,
            id_ptrs,
            name_ptrs,
            lengths,
            ctypes.c_size_t(count),
            ctypes.c_int(SAMPLE_RATE),
            params,
            ctypes.c_size_t(len(items)),
            ctypes.byref(json_ptr),
        )
    )
    try:
        return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


def _suggest_python(
    tracks: list[Track], options: dict[str, float | bool], binding: tuple[Any, ...]
) -> str:
    """Call the public Python facade, whose options are keyword arguments."""
    track_input, facade = binding[0], binding[1]
    inputs = [
        track_input(
            track_id=track.track_id,
            left=track.left,
            right=track.right,
            name=track.name,
        )
        for track in tracks
    ]
    kwargs = {OPTION_KEYS[key]: value for key, value in options.items()}
    return str(facade(inputs, sample_rate=SAMPLE_RATE, **kwargs))


def _encode_request(tracks: list[Track], options: dict[str, float | bool]) -> bytes:
    """Serialize the shared fixture for a JavaScript subprocess."""

    def channel(samples: np.ndarray | None) -> str | None:
        if samples is None:
            return None
        return base64.b64encode(
            np.ascontiguousarray(samples, dtype="<f4").tobytes()
        ).decode("ascii")

    document = {
        "sampleRate": SAMPLE_RATE,
        "options": options,
        "tracks": [
            {
                "id": track.track_id,
                "name": track.name,
                "left": channel(track.left),
                "right": channel(track.right),
            }
            for track in tracks
        ],
    }
    return json.dumps(document).encode("utf-8")


def _js_runner(surface: str, module_path: Path) -> str:
    module_url = module_path.resolve().as_uri()
    if surface == "node":
        import_line = f"import {{ suggestMixSceneJson }} from {json.dumps(module_url)};"
        init_line = ""
    else:
        import_line = (
            f"import {{ init, suggestMixSceneJson }} from {json.dumps(module_url)};"
        )
        init_line = "await init();"
    return f"""\
import fs from 'node:fs';
{import_line}
const encoded = fs.readFileSync(0, 'utf8').replace(/\\s+/g, '');
if (!encoded) throw new Error('empty base64 request payload');
const request = JSON.parse(Buffer.from(encoded, 'base64').toString('utf8'));
// Float32Array reads the host byte order; the fixture is little-endian, which
// every runner this check targets also is.
const channel = (value) => {{
  const bytes = Buffer.from(value, 'base64');
  return new Float32Array(
    bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
  );
}};
const tracks = request.tracks.map((track) => {{
  const out = {{ id: track.id, left: channel(track.left) }};
  if (track.name !== null) out.name = track.name;
  if (track.right !== null) out.right = channel(track.right);
  return out;
}});
{init_line}
const scene = suggestMixSceneJson({{
  tracks,
  sampleRate: request.sampleRate,
  options: request.options,
}});
if (typeof scene !== 'string') throw new Error('suggestMixSceneJson did not return a string');
process.stdout.write(Buffer.from(scene, 'utf8').toString('base64'));
"""


def _suggest_js(
    surface: str, request: bytes, node_executable: str, module_path: Path
) -> str:
    if not module_path.is_file():
        raise HarnessFailure(f"{surface} public dist is missing: {module_path}")
    command = [
        node_executable,
        "--input-type=module",
        "-e",
        _js_runner(surface, module_path),
    ]
    payload = base64.b64encode(request).decode("ascii") + "\n"
    try:
        completed = subprocess.run(
            command,
            cwd=ROOT,
            input=payload,
            capture_output=True,
            check=False,
            text=True,
            env=os.environ.copy(),
        )
    except OSError as exc:
        raise HarnessFailure(
            f"cannot launch {surface} runner {node_executable!r}: {exc}"
        ) from exc
    if completed.returncode != 0:
        detail = (
            completed.stderr.strip()
            or completed.stdout.strip()
            or "no subprocess output"
        )
        raise HarnessFailure(
            f"{surface} runner failed with exit {completed.returncode}: {detail}"
        )
    try:
        raw = base64.b64decode("".join(completed.stdout.split()), validate=True)
    except (ValueError, base64.binascii.Error) as exc:
        raise HarnessFailure(
            f"{surface} runner returned a malformed base64 scene payload"
        ) from exc
    try:
        return raw.decode("utf-8")
    except UnicodeError as exc:
        raise HarnessFailure(
            f"{surface} runner returned a non-UTF-8 scene payload"
        ) from exc


def _parse_scene(surface: str, case: str, scene: str) -> Any:
    try:
        return json.loads(scene)
    except json.JSONDecodeError as exc:
        raise HarnessFailure(
            f"{surface} {case}: scene is not valid JSON: {exc}"
        ) from exc


def _validate_scene(surface: str, case: str, scene: str, tracks: list[Track]) -> None:
    """Reject a scene that is too empty for agreement between surfaces to mean anything.

    Four surfaces that all return ``{}`` agree perfectly, so the substance of the
    document is checked before any of them are compared.
    """
    document = _parse_scene(surface, case, scene)
    if not isinstance(document, dict) or not document:
        raise HarnessFailure(f"{surface} {case}: scene is not a non-empty JSON object")
    strips = document.get("strips")
    if not isinstance(strips, list) or len(strips) != len(tracks):
        raise HarnessFailure(
            f"{surface} {case}: scene must carry one strip per fixture track "
            f"(got {len(strips) if isinstance(strips, list) else 'none'} "
            f"for {len(tracks)} tracks)"
        )
    suggested = {strip.get("id") for strip in strips if isinstance(strip, dict)}
    missing = [track.track_id for track in tracks if track.track_id not in suggested]
    if missing:
        raise HarnessFailure(
            f"{surface} {case}: scene omits strips for {', '.join(missing)}"
        )


def _nested_document(value: str) -> Any:
    """Parse a scene field that carries a whole JSON document inside a string.

    A processor insert keeps its parameters in ``params`` as serialized JSON
    text, so comparing that field as an opaque string would hide every numeric
    difference inside it behind one unreadable "these two long strings differ".
    """
    text = value.strip()
    if not text or text[0] not in "{[":
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return None


def _diff_json(left: Any, right: Any, rtol: float, path: str = "$") -> list[str]:
    """Collect the paths at which two parsed scenes disagree.

    Numbers compare within ``rtol`` relative tolerance; ``0.0`` demands exact
    equality. Everything else -- keys, ordering, strings, booleans, list lengths
    -- always compares exactly.
    """
    left_number = isinstance(left, (int, float)) and not isinstance(left, bool)
    right_number = isinstance(right, (int, float)) and not isinstance(right, bool)
    if left_number and right_number:
        if left == right:
            return []
        scale = max(abs(float(left)), abs(float(right)))
        if rtol > 0.0 and abs(float(left) - float(right)) <= rtol * scale:
            return []
        return [f"{path}: {left!r} vs {right!r}"]
    if type(left) is not type(right):
        return [f"{path}: type {type(left).__name__} vs {type(right).__name__}"]
    if isinstance(left, dict):
        differences: list[str] = []
        for key in sorted(set(left) | set(right)):
            if key not in left:
                differences.append(f"{path}.{key}: missing on the left")
            elif key not in right:
                differences.append(f"{path}.{key}: missing on the right")
            else:
                differences.extend(
                    _diff_json(left[key], right[key], rtol, f"{path}.{key}")
                )
        return differences
    if isinstance(left, list):
        if len(left) != len(right):
            return [f"{path}: length {len(left)} vs {len(right)}"]
        differences = []
        for index, (item, other) in enumerate(zip(left, right)):
            differences.extend(_diff_json(item, other, rtol, f"{path}[{index}]"))
        return differences
    if isinstance(left, str) and left != right:
        nested_left = _nested_document(left)
        nested_right = _nested_document(right)
        if nested_left is not None and nested_right is not None:
            return _diff_json(nested_left, nested_right, rtol, f"{path}(json)")
    if left != right:
        return [f"{path}: {left!r} vs {right!r}"]
    return []


def _compare_to_oracle(surface: str, case: str, scene: str, oracle: str) -> str:
    """Compare one surface's scene to the C oracle and report how it matched.

    Raises:
        HarnessFailure: when the scenes differ beyond what the surface is
            allowed (byte-identical text for the native surfaces, ``NUMERIC_RTOL``
            for WASM).
    """
    if scene == oracle:
        return "exact"
    rtol = 0.0 if surface in EXACT_MATCH_SURFACES else NUMERIC_RTOL
    differences = _diff_json(
        _parse_scene("c", case, oracle), _parse_scene(surface, case, scene), rtol
    )
    if not differences:
        if rtol == 0.0:
            raise HarnessFailure(
                f"{surface} {case}: scene text differs from the C oracle while the "
                f"parsed documents agree (oracle {len(oracle)} bytes, "
                f"{surface} {len(scene)} bytes)"
            )
        return f"numeric rtol={rtol:g}"
    shown = differences[:20]
    suffix = (
        ""
        if len(shown) == len(differences)
        else f" (+{len(differences) - len(shown)} more)"
    )
    joined = "; ".join(f"c vs {surface} at {entry}" for entry in shown)
    raise HarnessFailure(
        f"{surface} {case}: scene differs from the C oracle "
        f"(rtol={rtol:g}): {joined}{suffix}"
    )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--node", default=os.environ.get("NODE", "node"), help="Node executable"
    )
    parser.add_argument(
        "--node-dist", type=Path, default=NODE_INDEX, help="Node dist/index.js"
    )
    parser.add_argument(
        "--wasm-dist", type=Path, default=WASM_INDEX, help="WASM dist/index.js"
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    try:
        tracks = _make_tracks()
        binding = _load_binding()
        scenes: dict[str, dict[str, str]] = {}
        for case, options in CASES.items():
            request = _encode_request(tracks, options)
            scenes[case] = {
                "c": _suggest_c(tracks, options, binding),
                "python": _suggest_python(tracks, options, binding),
                "node": _suggest_js("node", request, args.node, args.node_dist),
                "wasm": _suggest_js("wasm", request, args.node, args.wasm_dist),
            }

        for case, per_surface in scenes.items():
            for surface in SURFACES:
                _validate_scene(surface, case, per_surface[surface], tracks)
            oracle = per_surface["c"]
            verdicts = {
                surface: _compare_to_oracle(surface, case, per_surface[surface], oracle)
                for surface in SURFACES
            }
            summary = " ".join(f"{name}={how}" for name, how in verdicts.items())
            print(f"{case}: scene {len(oracle)} bytes, match {summary}")

        # Non-vacuity: an option that no surface honours would produce four
        # identical scenes in both cases and read as a pass.
        for surface in SURFACES:
            if scenes["defaults"][surface] == scenes["suggestionStrength=0.5"][surface]:
                raise HarnessFailure(
                    f"{surface}: suggestionStrength=0.5 produced the default scene, "
                    "so the option never reached the core"
                )
        print("suggestionStrength=0.5 changes the scene on every surface")
    except HarnessFailure as exc:
        print(f"mix assistant cross-surface: FAIL: {exc}", file=sys.stderr)
        return 1
    except (
        Exception
    ) as exc:  # pragma: no cover - preserve actionable CLI failure context.
        print(
            f"mix assistant cross-surface: FAIL: unexpected {type(exc).__name__}: {exc}",
            file=sys.stderr,
        )
        return 1
    print("mix assistant cross-surface: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
