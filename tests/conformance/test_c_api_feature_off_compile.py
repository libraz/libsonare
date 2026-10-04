"""Compile feature-disabled C API entry points, including their output guards."""

from __future__ import annotations

import os
import shlex
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class FeatureOffCompileTest(unittest.TestCase):
    def test_acoustic_api_compiles_with_acoustic_sim_disabled(self) -> None:
        result = subprocess.run(
            [
                *shlex.split(os.environ.get("CXX", "c++")),
                "-std=c++17",
                "-fsyntax-only",
                "-I",
                str(ROOT / "include"),
                "-I",
                str(ROOT / "src"),
                str(ROOT / "src/c_api/sonare_c_acoustic.cpp"),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
