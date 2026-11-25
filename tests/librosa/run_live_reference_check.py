"""Generate live librosa references and run the C++ reference tests against them.

This keeps committed fixtures stable while making it easy to verify compatibility
against the librosa version installed in the current Python environment.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import generate_librosa_reference as generator
import librosa

REPO_ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build", help="CMake build directory")
    parser.add_argument(
        "--keep",
        action="store_true",
        help="Keep the generated temporary reference directory",
    )
    args = parser.parse_args()

    repo_root = REPO_ROOT
    build_dir = repo_root / args.build_dir
    executable = "sonare_tests.exe" if os.name == "nt" else "sonare_tests"
    test_binary = build_dir / "bin" / executable
    if not test_binary.is_file():
        print(f"Test binary not found: {test_binary}", file=sys.stderr)
        return 2

    temp_root = Path(tempfile.mkdtemp(prefix="sonare-librosa-reference-"))
    reference_dir = temp_root / "reference"

    try:
        print(f"librosa module: {Path(librosa.__file__).resolve()}", flush=True)
        generator.OUTPUT_DIR = reference_dir
        generator.main()

        env = os.environ.copy()
        env["SONARE_LIBROSA_REFERENCE_DIR"] = str(reference_dir)

        # CI registers whole shards in CTest, so individual case names are only
        # available from Catch2. Keep the default exclusion of hidden cases.
        command = [str(test_binary), '"*reference compatibility*"~[.]']
        return subprocess.run(command, cwd=repo_root, env=env, check=False).returncode
    finally:
        if args.keep:
            print(f"Generated references kept at: {reference_dir}")
        else:
            shutil.rmtree(temp_root, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
