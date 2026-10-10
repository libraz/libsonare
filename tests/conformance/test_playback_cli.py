"""`sonare playback`: native and Python front ends agree on output channel width.

Generates a small 5.1 WAV fixture and renders it through both CLI front ends,
checking a headphones target writes 2ch and a 7.1 target writes 8ch
WAVE_FORMAT_EXTENSIBLE -- the format neither stdlib `wave` nor a plain PCM
writer emits, so this is the one check that tells the two channel counts apart
from a file a media player would actually place correctly.

A second suite checks that `--json` diagnostics are read from the same handle
that produced the audio, using a loud tone driven hard enough to always engage
the brickwall limiter as the differential signal. A third checks that
`--config` is refused the same way -- same exit code -- by both front ends on
a missing file and on a JSON root that is not an object.
"""

from __future__ import annotations

import functools
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SAMPLE_RATE = 22050
_FRAMES = 4410  # 0.2 s, matching the fixture length other CLI contract tests use
_WAVE_FORMAT_EXTENSIBLE = 0xFFFE


def _native_cli() -> Path | None:
    """Locate the native sonare-cli binary this run should exercise.

    `SONARE_NATIVE_CLI` names it explicitly (a private `-B` build directory, per
    AGENT.md's rule against building into `build/` at the repo root). Otherwise
    the binary beside the library `SONARE_LIB_PATH` names is used, so both front
    ends come from one build; a couple of conventional build directories are the
    fallback for a plain in-tree build.
    """
    env_path = os.environ.get("SONARE_NATIVE_CLI")
    if env_path:
        return Path(env_path)
    lib_path = os.environ.get("SONARE_LIB_PATH")
    if lib_path:
        sibling = Path(lib_path).resolve().parent.parent / "bin" / "sonare-cli"
        if sibling.is_file():
            return sibling
    for candidate in ("build/bin/sonare-cli", "build-shared/bin/sonare-cli"):
        path = ROOT / candidate
        if path.is_file():
            return path
    return None


def _run_cli(argv: list[str]) -> subprocess.CompletedProcess[str]:
    """Run a CLI from the repo root with the in-tree Python package importable.

    Mirrors tools/conformance/check_cli_contract.py, which puts
    ``bindings/python/src`` on PYTHONPATH so the Python front end resolves the
    source tree rather than whatever an interpreter happens to have installed.
    """
    environment = os.environ.copy()
    python_src = str(ROOT / "bindings" / "python" / "src")
    environment["PYTHONPATH"] = python_src + os.pathsep + environment.get("PYTHONPATH", "")
    return subprocess.run(
        argv, capture_output=True, text=True, cwd=ROOT, env=environment, check=False
    )


def _python_cli_argv() -> list[str]:
    """The Python CLI invocation, preferring the installed console script.

    Matches the sibling bounce-channels test's own convention; `-m
    libsonare.cli` is the fallback for a `sys.executable` without one
    installed alongside it.
    """
    script = Path(sys.executable).parent / "sonare"
    if script.is_file():
        return [str(script)]
    return [sys.executable, "-m", "libsonare.cli"]


@functools.lru_cache(maxsize=1)
def _python_library_error() -> str | None:
    """Why the in-tree Python front end cannot load libsonare, or None if it can."""
    probe = _run_cli(
        [sys.executable, "-c", "from libsonare._ffi import load_library; load_library()"]
    )
    return None if probe.returncode == 0 else (probe.stderr.strip().splitlines() or ["failed"])[-1]


def _python_cli_or_skip(test: unittest.TestCase) -> list[str]:
    """The Python CLI invocation, skipping the test when no library is built.

    The build-free CI gate has neither front end; the native half skips on a
    missing binary, and this is the same rule for the Python half.
    """
    error = _python_library_error()
    if error is not None:
        test.skipTest(f"libsonare shared library not loadable: {error}")
    return _python_cli_argv()


def _write_surround_wav(path: Path) -> None:
    """A 6-channel (5.1) PCM16 WAV with a distinct tone per plane.

    Plain WAVE_FORMAT_PCM: reading a multichannel file does not require the
    EXTENSIBLE tag this test's own assertions are about on the *output* side.
    """
    frequencies = (220.0, 330.0, 440.0, 55.0, 110.0, 165.0)  # L R C LFE Ls Rs
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(len(frequencies))
        wav.setsampwidth(2)
        wav.setframerate(_SAMPLE_RATE)
        frames = bytearray()
        for n in range(_FRAMES):
            for frequency in frequencies:
                sample = 0.2 * math.sin(2.0 * math.pi * frequency * n / _SAMPLE_RATE)
                frames += struct.pack("<h", int(sample * 32767.0))
        wav.writeframesraw(bytes(frames))


def _wav_channels_and_format_tag(path: Path) -> tuple[int, int]:
    """(channel count, wFormatTag) read directly from the fmt chunk."""
    with open(path, "rb") as handle:
        header = handle.read(40)
    fmt_tag = struct.unpack("<H", header[20:22])[0]
    channels = struct.unpack("<H", header[22:24])[0]
    return channels, fmt_tag


def _write_mono_full_scale_wav(path: Path) -> None:
    """A mono, near-full-scale continuous tone, long enough to span several
    of the renderer's 4096-frame render blocks (see `kOfflineRenderBlockFrames`).
    """
    frames = _FRAMES * 4
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(_SAMPLE_RATE)
        samples = bytearray()
        for n in range(frames):
            sample = 0.999 * math.sin(2.0 * math.pi * 220.0 * n / _SAMPLE_RATE)
            samples += struct.pack("<h", int(sample * 32767.0))
        wav.writeframesraw(bytes(samples))


class PlaybackCliDiagnosticsTest(unittest.TestCase):
    """`--json` diagnostics come from the handle that actually rendered the audio.

    ``--target-lufs -5 --dialogue-db 12`` (the loudest and most-boosted flags
    the schema allows) pushed through a near-full-scale tone reliably drives
    the renderer's brickwall limiter, which reports its last block's gain
    reduction: a renderer created only to be queried, never fed the actual
    samples, reports ``limiter_gain_reduction_db: 0`` regardless of input.
    """

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.loud_wav = Path(self._tmp.name) / "mono-full-scale.wav"
        _write_mono_full_scale_wav(self.loud_wav)

    def _run(self, argv: list[str]) -> subprocess.CompletedProcess[str]:
        return _run_cli(argv)

    def _assert_diagnostics_reflect_the_render(self, run_prefix: list[str], output: Path) -> None:
        result = self._run(
            [
                *run_prefix,
                "playback",
                str(self.loud_wav),
                "-o",
                str(output),
                "--target-lufs",
                "-5",
                "--dialogue-db",
                "12",
                "--json",
            ]
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        diagnostics = json.loads(result.stdout)
        self.assertLess(diagnostics["limiter_gain_reduction_db"], 0.0, diagnostics)

    def test_native_json_diagnostics_reflect_the_render(self) -> None:
        native = _native_cli()
        if native is None:
            self.skipTest("native sonare-cli binary not found; set SONARE_NATIVE_CLI")
        self._assert_diagnostics_reflect_the_render(
            [str(native)], Path(self._tmp.name) / "native-loud.wav"
        )

    def test_python_json_diagnostics_reflect_the_render(self) -> None:
        self._assert_diagnostics_reflect_the_render(
            _python_cli_or_skip(self), Path(self._tmp.name) / "python-loud.wav"
        )


_EXIT_INVALID_PARAMETER = 3


class PlaybackCliConfigRefusalTest(unittest.TestCase):
    """`--config` is refused the same way -- a missing file, or a JSON root
    that is not an object -- on both front ends, never silently treated as an
    absent --config.
    """

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.input_wav = Path(self._tmp.name) / "input.wav"
        _write_surround_wav(self.input_wav)
        self.non_object_config = Path(self._tmp.name) / "non-object-config.json"
        self.non_object_config.write_text("[1, 2, 3]")
        self.missing_config = Path(self._tmp.name) / "does-not-exist.json"

    def _run(self, argv: list[str]) -> subprocess.CompletedProcess[str]:
        return _run_cli(argv)

    def _assert_refused(self, run_prefix: list[str], config_path: Path, output: Path) -> None:
        result = self._run(
            [
                *run_prefix,
                "playback",
                str(self.input_wav),
                "-o",
                str(output),
                "--config",
                str(config_path),
            ]
        )
        self.assertEqual(result.returncode, _EXIT_INVALID_PARAMETER, result.stderr)
        self.assertFalse(output.exists())

    def test_native_refuses_missing_config_file(self) -> None:
        native = _native_cli()
        if native is None:
            self.skipTest("native sonare-cli binary not found; set SONARE_NATIVE_CLI")
        self._assert_refused(
            [str(native)], self.missing_config, Path(self._tmp.name) / "native-missing.wav"
        )

    def test_native_refuses_non_object_config_root(self) -> None:
        native = _native_cli()
        if native is None:
            self.skipTest("native sonare-cli binary not found; set SONARE_NATIVE_CLI")
        self._assert_refused(
            [str(native)], self.non_object_config, Path(self._tmp.name) / "native-non-object.wav"
        )

    def test_python_refuses_missing_config_file(self) -> None:
        self._assert_refused(
            _python_cli_or_skip(self),
            self.missing_config,
            Path(self._tmp.name) / "python-missing.wav",
        )

    def test_python_refuses_non_object_config_root(self) -> None:
        self._assert_refused(
            _python_cli_or_skip(self),
            self.non_object_config,
            Path(self._tmp.name) / "python-non-object.wav",
        )


class PlaybackCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.surround_wav = Path(self._tmp.name) / "surround-5.1.wav"
        _write_surround_wav(self.surround_wav)

    def _run(self, argv: list[str]) -> subprocess.CompletedProcess[str]:
        return _run_cli(argv)

    def _assert_target_writes(
        self, run_prefix: list[str], target: str, expected_channels: int, output: Path
    ) -> None:
        result = self._run(
            [*run_prefix, "playback", str(self.surround_wav), "-o", str(output), "--target", target]
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        channels, fmt_tag = _wav_channels_and_format_tag(output)
        self.assertEqual(channels, expected_channels)
        if expected_channels > 2:
            self.assertEqual(fmt_tag, _WAVE_FORMAT_EXTENSIBLE)

    def test_native_headphones_output_is_stereo(self) -> None:
        native = _native_cli()
        if native is None:
            self.skipTest("native sonare-cli binary not found; set SONARE_NATIVE_CLI")
        self._assert_target_writes(
            [str(native)], "headphones", 2, Path(self._tmp.name) / "native-headphones.wav"
        )

    def test_native_seven_one_output_is_extensible(self) -> None:
        native = _native_cli()
        if native is None:
            self.skipTest("native sonare-cli binary not found; set SONARE_NATIVE_CLI")
        self._assert_target_writes([str(native)], "7.1", 8, Path(self._tmp.name) / "native-71.wav")

    def test_python_headphones_output_is_stereo(self) -> None:
        self._assert_target_writes(
            _python_cli_or_skip(self),
            "headphones",
            2,
            Path(self._tmp.name) / "python-headphones.wav",
        )

    def test_python_seven_one_output_is_extensible(self) -> None:
        self._assert_target_writes(
            _python_cli_or_skip(self), "7.1", 8, Path(self._tmp.name) / "python-71.wav"
        )


if __name__ == "__main__":
    unittest.main()
