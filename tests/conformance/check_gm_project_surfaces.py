#!/usr/bin/env python3
"""Accept the GM-program project bounce across the public language surfaces.

The C ABI is the oracle.  Python drives that ABI through the existing ctypes
declarations, then the public Python, Node, and WASM ``Project`` facades render
the same serialized project.  The JavaScript subprocesses use a deliberately
small protocol: one base64-encoded project JSON request on stdin and one
base64-encoded little-endian Float32Array response on stdout.
"""

from __future__ import annotations

import argparse
import base64
import ctypes
import json
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "conformance" / "gm_program_project_v1.json"
PYTHON_SRC = ROOT / "bindings" / "python" / "src"
NODE_INDEX = ROOT / "bindings" / "node" / "dist" / "index.js"
WASM_INDEX = ROOT / "bindings" / "wasm" / "dist" / "index.js"

SAMPLE_RATE = 48_000
BLOCK_SIZE = 128
TOTAL_FRAMES = 12_000
NUM_CHANNELS = 1
ATOL = 1.0e-6
RTOL = 1.0e-5
CONTROL_DELTA = 1.0e-3

# UMP MIDI 1.0 words emitted by Project.midiProgram/noteOn/noteOff.  Keeping
# these values here makes the fixture shape itself part of this acceptance
# check, rather than allowing a malformed or unrelated project to pass.
EXPECTED_EVENTS = (
    (0.0, 0x20C00400),
    (0.0, 0x20903C64),
    (0.5, 0x20803C00),
)


class HarnessFailure(RuntimeError):
    """A deterministic acceptance failure with an actionable message."""


def _load_binding_types() -> tuple[object, ...]:
    """Import the public Python facade and its existing ctypes declarations."""
    if str(PYTHON_SRC) not in sys.path:
        sys.path.insert(0, str(PYTHON_SRC))
    try:
        from libsonare import Project, SynthPatch
        from libsonare._runtime import (
            SonareProjectBounceOptions,
            SonareSynthInstrumentBinding,
            _check,
            _from_c_float_array,
            _get_lib,
            _out_float_array,
        )
    except (ImportError, OSError, RuntimeError) as exc:
        raise HarnessFailure(
            "Python binding unavailable; install the bindings/python environment and "
            f"build the shared library (reason: {exc})"
        ) from exc
    return (
        Project,
        SynthPatch,
        SonareProjectBounceOptions,
        SonareSynthInstrumentBinding,
        _check,
        _from_c_float_array,
        _get_lib,
        _out_float_array,
    )


def _read_fixture() -> tuple[str, bytes]:
    try:
        serialized = FIXTURE.read_text(encoding="utf-8")
        payload = json.loads(serialized)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise HarnessFailure(
            f"cannot read GM project fixture {FIXTURE}: {exc}"
        ) from exc

    if payload.get("version") != 1 or payload.get("sample_rate") != SAMPLE_RATE:
        raise HarnessFailure("fixture must be project version 1 at 48 kHz")
    tracks = payload.get("tracks")
    if not isinstance(tracks, list) or len(tracks) != 1:
        raise HarnessFailure("fixture must contain exactly one track")
    track = tracks[0]
    if (
        track.get("kind") != 1
        or track.get("midi_destination_id") != 0
        or track.get("id") != 1
    ):
        raise HarnessFailure("fixture track must be MIDI kind with destination 0")
    clips = payload.get("clips")
    if not isinstance(clips, list) or len(clips) != 1 or clips[0].get("track_id") != 1:
        raise HarnessFailure("fixture must contain one MIDI clip on track 1")
    sources = payload.get("sources")
    if (
        not isinstance(sources, list)
        or len(sources) != 1
        or sources[0].get("kind") != 1
    ):
        raise HarnessFailure("fixture must contain one MIDI source")

    events = payload.get("midi_content", {}).get("1")
    if not isinstance(events, list) or len(events) != len(EXPECTED_EVENTS):
        raise HarnessFailure("fixture must contain program 4 and one C4 note pair")
    for index, (expected_ppq, expected_word) in enumerate(EXPECTED_EVENTS):
        event = events[index]
        if (
            not isinstance(event, dict)
            or not math.isclose(
                float(event.get("ppq", -1.0)), expected_ppq, abs_tol=0.0
            )
            or int(event.get("data0", -1)) != expected_word
            or int(event.get("data1", -1)) != 0
        ):
            raise HarnessFailure(
                f"fixture MIDI event {index} is not the canonical GM/C4 event"
            )

    return serialized, serialized.encode("utf-8")


def _render_c(
    serialized: bytes,
    use_gm_programs: bool,
    types: tuple[object, ...],
) -> np.ndarray:
    (
        _project,
        SynthPatch,
        SonareProjectBounceOptions,
        SonareSynthInstrumentBinding,
        check,
        from_c_float_array,
        get_lib,
        out_float_array,
    ) = types
    del _project
    lib = get_lib()
    handle = ctypes.c_void_p()
    diagnostics = ctypes.c_char_p()
    check(
        lib.sonare_project_deserialize(
            serialized,
            ctypes.c_size_t(len(serialized)),
            ctypes.byref(handle),
            ctypes.byref(diagnostics),
        )
    )
    if diagnostics:
        lib.sonare_free_string(diagnostics)

    options = SonareProjectBounceOptions()
    options.total_frames = TOTAL_FRAMES
    options.block_size = BLOCK_SIZE
    options.num_channels = NUM_CHANNELS
    options.sample_rate = SAMPLE_RATE
    options.instrument_latency_samples = 0
    binding = SonareSynthInstrumentBinding()
    binding.destination_id = 0
    binding.patch = SynthPatch(preset="sine")._to_c()
    binding.use_gm_programs = int(use_gm_programs)

    try:
        with out_float_array(lib) as (out, out_length):
            check(
                lib.sonare_project_bounce_with_synth_instruments(
                    handle,
                    ctypes.byref(options),
                    ctypes.byref(binding),
                    ctypes.c_size_t(1),
                    ctypes.byref(out),
                    ctypes.byref(out_length),
                )
            )
            samples = from_c_float_array(out, int(out_length.value))
    finally:
        lib.sonare_project_destroy(handle)
    return np.asarray(samples, dtype=np.float32)


def _render_python(
    serialized: str, use_gm_programs: bool, types: tuple[object, ...]
) -> np.ndarray:
    Project = types[0]
    project = Project.from_json(serialized)
    try:
        samples = project.bounce_with_synth_instrument(
            "sine",
            auto_select_gm=use_gm_programs,
            total_frames=TOTAL_FRAMES,
            block_size=BLOCK_SIZE,
            num_channels=NUM_CHANNELS,
            sample_rate=SAMPLE_RATE,
        )
    finally:
        project.close()
    return np.asarray(samples, dtype=np.float32).reshape(-1)


def _js_runner(surface: str, module_path: Path, use_gm_programs: bool) -> str:
    module_url = module_path.resolve().as_uri()
    use_gm_programs_literal = str(use_gm_programs).lower()
    if surface == "node":
        import_line = f"import {{ Project }} from {json.dumps(module_url)};"
        init_line = ""
        close_line = "project.destroy();"
    else:
        import_line = f"import {{ init, Project }} from {json.dumps(module_url)};"
        init_line = "await init();"
        close_line = "project.delete();"
    return f"""\
import fs from 'node:fs';
{import_line}
const encoded = fs.readFileSync(0, 'utf8').replace(/\\s+/g, '');
if (!encoded) throw new Error('empty base64 project payload');
const projectJson = Buffer.from(encoded, 'base64').toString('utf8');
{init_line}
const project = Project.fromJson(projectJson);
try {{
  const samples = project.bounceWithSynthInstrument(
    {{ preset: 'sine', useGmPrograms: {use_gm_programs_literal} }},
    {{ totalFrames: {TOTAL_FRAMES}, blockSize: {BLOCK_SIZE}, numChannels: {NUM_CHANNELS}, sampleRate: {SAMPLE_RATE} }},
  );
  if (!(samples instanceof Float32Array)) throw new Error('render did not return Float32Array');
  process.stdout.write(Buffer.from(samples.buffer, samples.byteOffset, samples.byteLength).toString('base64'));
}} finally {{
  {close_line}
}}
"""


def _run_js_surface(
    surface: str,
    serialized: bytes,
    use_gm_programs: bool,
    node_executable: str,
    module_path: Path,
) -> np.ndarray:
    if not module_path.is_file():
        raise HarnessFailure(f"{surface} public dist is missing: {module_path}")
    command = [
        node_executable,
        "--input-type=module",
        "-e",
        _js_runner(surface, module_path, use_gm_programs),
    ]
    payload = base64.b64encode(serialized).decode("ascii") + "\n"
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
            f"{surface} runner returned malformed base64 Float32 payload"
        ) from exc
    if len(raw) % 4 != 0:
        raise HarnessFailure(
            f"{surface} runner returned {len(raw)} non-Float32 payload bytes"
        )
    return np.frombuffer(raw, dtype="<f4").copy()


def _metrics(
    samples: np.ndarray, reference: np.ndarray | None = None
) -> dict[str, float | int | bool]:
    values = np.asarray(samples, dtype=np.float32).reshape(-1)
    result: dict[str, float | int | bool] = {
        "length": int(values.size),
        "finite": bool(np.isfinite(values).all()),
        "peak": float(np.max(np.abs(values))) if values.size else 0.0,
    }
    if reference is not None:
        delta = values.astype(np.float64) - reference.astype(np.float64)
        max_abs = float(np.max(np.abs(delta))) if delta.size else 0.0
        rmse = float(math.sqrt(float(np.mean(delta * delta)))) if delta.size else 0.0
        reference_rms = math.sqrt(float(np.mean(reference.astype(np.float64) ** 2)))
        nrmse = rmse / reference_rms if reference_rms else math.inf
        snr_db = 20.0 * math.log10(reference_rms / rmse) if rmse else math.inf
        result.update(max_abs=max_abs, rmse=rmse, nrmse=nrmse, snr_db=snr_db)
    return result


def _validate_render(name: str, samples: np.ndarray) -> None:
    _validate_shape_and_finiteness(name, samples)
    metrics = _metrics(samples)
    if float(metrics["peak"]) <= 0.01:
        raise HarnessFailure(
            f"{name}: peak {float(metrics['peak']):.9g} is not audible (> 0.01 required)"
        )


def _validate_shape_and_finiteness(name: str, samples: np.ndarray) -> None:
    metrics = _metrics(samples)
    if metrics["length"] != TOTAL_FRAMES:
        raise HarnessFailure(
            f"{name}: expected {TOTAL_FRAMES} samples, got {metrics['length']}"
        )
    if not metrics["finite"]:
        raise HarnessFailure(f"{name}: output contains NaN or Inf")


def _assert_mode_change(
    surface: str, without_gm: np.ndarray, with_gm: np.ndarray
) -> float:
    """Require both modes to be exercised and produce observably different audio."""
    if without_gm.size != with_gm.size:
        raise HarnessFailure(
            f"{surface}: use_gm_programs mode lengths differ "
            f"({without_gm.size} vs {with_gm.size})"
        )
    delta = float(
        np.max(np.abs(without_gm.astype(np.float64) - with_gm.astype(np.float64)))
    )
    if delta <= CONTROL_DELTA:
        raise HarnessFailure(
            f"{surface}: use_gm_programs=false/true delta {delta:.9g} "
            f"does not exceed {CONTROL_DELTA:g}"
        )
    return delta


def _compare_to_c_oracle(
    surface: str,
    use_gm_programs: bool,
    samples: np.ndarray,
    oracle: np.ndarray,
) -> dict[str, float | int | bool]:
    """Compare one surface/mode to the C oracle for that same mode."""
    mode = str(use_gm_programs).lower()
    if samples.size != oracle.size:
        raise HarnessFailure(
            f"{surface} use_gm_programs={mode}: output length differs from "
            "the matching C oracle"
        )
    diagnostics = _metrics(samples, oracle)
    if not np.allclose(samples, oracle, atol=ATOL, rtol=RTOL):
        raise HarnessFailure(
            f"{surface} use_gm_programs={mode}: samplewise comparison failed ("
            f"max_abs={_format_metric(diagnostics['max_abs'])}, "
            f"rmse={_format_metric(diagnostics['rmse'])}, "
            f"nrmse={_format_metric(diagnostics['nrmse'])}, "
            f"snr_db={_format_metric(diagnostics['snr_db'])}, "
            f"atol={ATOL:g}, rtol={RTOL:g})"
        )
    return diagnostics


def _format_metric(value: float | int | bool) -> str:
    if isinstance(value, bool):
        return str(value).lower()
    if isinstance(value, int):
        return str(value)
    if math.isinf(float(value)):
        return "inf"
    return f"{float(value):.9g}"


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
        serialized, serialized_bytes = _read_fixture()
        types = _load_binding_types()
        c_outputs = {
            False: _render_c(serialized_bytes, False, types),
            True: _render_c(serialized_bytes, True, types),
        }
        surface_outputs = {
            "c": c_outputs,
            "python": {
                False: _render_python(serialized, False, types),
                True: _render_python(serialized, True, types),
            },
            "node": {
                False: _run_js_surface(
                    "node", serialized_bytes, False, args.node, args.node_dist
                ),
                True: _run_js_surface(
                    "node", serialized_bytes, True, args.node, args.node_dist
                ),
            },
            "wasm": {
                False: _run_js_surface(
                    "wasm", serialized_bytes, False, args.node, args.wasm_dist
                ),
                True: _run_js_surface(
                    "wasm", serialized_bytes, True, args.node, args.wasm_dist
                ),
            },
        }

        for name, outputs in surface_outputs.items():
            for use_gm_programs, samples in outputs.items():
                mode = str(use_gm_programs).lower()
                _validate_render(f"{name} use_gm_programs={mode}", samples)
            mode_delta = _assert_mode_change(name, outputs[False], outputs[True])
            print(f"{name}: use_gm_programs=false/true max_abs_delta={mode_delta:.9g}")
            for use_gm_programs, samples in outputs.items():
                # Each mode is compared with its same-mode C oracle. Keeping
                # these references paired is what rejects a false/true
                # cross-wiring mutant even when the other mode still passes.
                diagnostics = _compare_to_c_oracle(
                    name, use_gm_programs, samples, c_outputs[use_gm_programs]
                )
                mode = str(use_gm_programs).lower()
                print(
                    f"{name} use_gm_programs={mode}: "
                    + " ".join(
                        f"{key}={_format_metric(value)}"
                        for key, value in diagnostics.items()
                    )
                    + f" atol={ATOL:g} rtol={RTOL:g} pass=true"
                )
    except HarnessFailure as exc:
        print(f"gm cross-surface: FAIL: {exc}", file=sys.stderr)
        return 1
    except (
        Exception
    ) as exc:  # pragma: no cover - preserve actionable CLI failure context.
        print(
            f"gm cross-surface: FAIL: unexpected {type(exc).__name__}: {exc}",
            file=sys.stderr,
        )
        return 1
    print("gm cross-surface: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
