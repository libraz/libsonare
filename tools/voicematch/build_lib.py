"""Configuring and building the isolated library a fit renders through.

A fit gets its own build dir (default `build-autofit`), configured
`-DBUILD_SHARED=ON` plus `-DBUILD_TUNING=ON` when the spec has runtime knobs, so
it never disturbs the trees other tooling reads. `build-python-shared` in
particular is shared, and `run` refuses it outright.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

from _repo import REPO_ROOT


def _cached_option(build_dir: Path, name: str) -> str | None:
    """Read one option's value out of an existing CMakeCache.txt."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    for line in cache.read_text().splitlines():
        if line.startswith(f"{name}:"):
            return line.split("=", 1)[1] if "=" in line else None
    return None


def configure_build(build_dir: Path, cmake: str, *, tuning: bool) -> None:
    """Configure the isolated build dir (idempotent unless BUILD_TUNING flips).

    Reconfiguring on a flip matters: `BUILD_TUNING` changes what a
    `SONARE_TUNABLE` declaration means, so a cache left at the other setting
    would build a library that ignores every override — a fit that reports a
    perfectly flat loss and no obvious reason why.
    """
    want = "ON" if tuning else "OFF"
    cached = _cached_option(build_dir, "BUILD_TUNING")
    if cached is not None and cached == want:
        return
    print(f"configuring {build_dir} (Release, BUILD_SHARED=ON, BUILD_TUNING={want})...",
          file=sys.stderr)
    subprocess.run(
        [cmake, "-S", str(REPO_ROOT), "-B", str(build_dir),
         "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED=ON", f"-DBUILD_TUNING={want}"],
        check=True,
    )


def build_shared(build_dir: Path, cmake: str, jobs: int) -> None:
    """Rebuild the shared library target in the isolated build dir."""
    proc = subprocess.run(
        [cmake, "--build", str(build_dir), "--target", "sonare_shared", f"-j{jobs}"],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"build failed (rc={proc.returncode}):\n{proc.stdout[-2000:]}\n{proc.stderr[-2000:]}"
        )
    # Ensure the freshly built dylib is loadable by ctypes on macOS.
    dylib = dylib_path(build_dir)
    if sys.platform == "darwin" and dylib is not None and shutil.which("install_name_tool"):
        subprocess.run(
            ["install_name_tool", "-id", "@loader_path/libsonare.dylib", str(dylib)],
            capture_output=True, text=True,
        )


def dylib_path(build_dir: Path) -> Path | None:
    """Locate the built libsonare shared library in the isolated build dir."""
    direct = build_dir / "lib" / "libsonare.dylib"
    if direct.exists():
        return direct
    for cand in build_dir.rglob("libsonare.dylib"):
        return cand
    for cand in build_dir.rglob("libsonare.so"):
        return cand
    return None
