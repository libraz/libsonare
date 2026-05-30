#!/usr/bin/env python3
"""Cross-binding smoke/parity harness for the realtime voice changer.

The script intentionally uses only stdlib plus the local Python package. It
checks the C++ CLI, Node wrapper, and Python wrapper on the same deterministic
input and compares finite output, duration, and peak ceiling. Exact hashes are
not required because CLI WAV I/O can quantize samples.
"""

from __future__ import annotations

import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SAMPLE_RATE = 48_000
PRESET = "bright-idol"


def tone() -> list[float]:
    count = 4096
    return [
        0.25 * math.sin(2.0 * math.pi * 180.0 * i / SAMPLE_RATE)
        + 0.08 * math.sin(2.0 * math.pi * 720.0 * i / SAMPLE_RATE)
        for i in range(count)
    ]


def write_wav(path: Path, samples: list[float]) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        frames = bytearray()
        for sample in samples:
            value = max(-32768, min(32767, int(round(sample * 32767.0))))
            frames.extend(struct.pack("<h", value))
        wav.writeframes(bytes(frames))


def read_wav(path: Path) -> list[float]:
    with wave.open(str(path), "rb") as wav:
        raw = wav.readframes(wav.getnframes())
    return [value / 32768.0 for (value,) in struct.iter_unpack("<h", raw)]


def metrics(samples: list[float]) -> dict[str, float | int | bool]:
    peak = max((abs(x) for x in samples), default=0.0)
    rms = math.sqrt(sum(x * x for x in samples) / max(1, len(samples)))
    return {
        "length": len(samples),
        "finite": all(math.isfinite(x) for x in samples),
        "peak": peak,
        "rms": rms,
    }


def run() -> int:
    samples = tone()
    with tempfile.TemporaryDirectory() as td:
        temp = Path(td)
        src = temp / "input.wav"
        cli_out = temp / "cli.wav"
        write_wav(src, samples)

        subprocess.run(
            [
                str(ROOT / "build" / "bin" / "sonare"),
                "voice-change",
                str(src),
                "-o",
                str(cli_out),
                "--preset",
                PRESET,
            ],
            check=True,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        env = os.environ.copy()
        env["PYTHONPATH"] = str(ROOT / "bindings" / "python" / "src")
        env.setdefault("SONARE_LIB_PATH", str(ROOT / "build-python-shared" / "lib" / "libsonare.dylib"))
        py_code = (
            "import json, libsonare; "
            f"s={json.dumps(samples)}; "
            f"print(json.dumps(libsonare.voice_change_realtime(s,{SAMPLE_RATE!r},{PRESET!r})))"
        )
        py_samples = json.loads(
            subprocess.check_output([sys.executable, "-c", py_code], cwd=ROOT, env=env, text=True)
        )

        node_code = (
            "import { voiceChangeRealtime } from './bindings/node/dist/index.js';"
            f"const s = new Float32Array({json.dumps(samples)});"
            f"console.log(JSON.stringify(Array.from(voiceChangeRealtime(s,{SAMPLE_RATE},'{PRESET}'))));"
        )
        node_samples = json.loads(
            subprocess.check_output(["node", "--input-type=module", "-e", node_code], cwd=ROOT, text=True)
        )

        results = {
            "cli": metrics(read_wav(cli_out)),
            "python": metrics(py_samples),
            "node": metrics(node_samples),
        }
        print(json.dumps(results, sort_keys=True))

        expected_len = len(samples)
        for name, item in results.items():
            if item["length"] != expected_len or not item["finite"] or item["peak"] > 1.001:
                raise SystemExit(f"{name} failed realtime voice changer parity smoke")
    return 0


if __name__ == "__main__":
    raise SystemExit(run())

