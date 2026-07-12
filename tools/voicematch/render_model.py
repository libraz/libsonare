"""Model-side renderer: SMF bytes -> audio through libsonare's GM fallback bank.

Renders via `Project.import_smf` + `bounce_with_sf2_instrument` with NO
SoundFont loaded, which forces every program through the built-in synthesizer
GM fallback (`gm_fallback_map` -> NativeSynth physical voices) — exactly the
code path being tuned. The dylib is resolved through SONARE_LIB_PATH, so the
harness always tests the working tree's freshly built library.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/ for _repo

from _repo import REPO_ROOT  # noqa: E402

DEFAULT_DYLIB = REPO_ROOT / "build-python-shared" / "lib" / "libsonare.dylib"


def ensure_lib_path() -> str:
    """Point the Python binding at the working-tree dylib unless overridden."""
    if "SONARE_LIB_PATH" not in os.environ and DEFAULT_DYLIB.exists():
        os.environ["SONARE_LIB_PATH"] = str(DEFAULT_DYLIB)
    return os.environ.get("SONARE_LIB_PATH", "")


def render_model(smf_bytes: bytes, total_seconds: float, sr: int = 48000) -> np.ndarray:
    """Render SMF bytes to a (frames, 2) float32 array via the GM fallback bank."""
    ensure_lib_path()
    import libsonare  # deferred so SONARE_LIB_PATH is set before the dylib loads

    project = libsonare.Project()
    try:
        project.import_smf(smf_bytes)
        audio = project.bounce_with_sf2_instrument(
            total_frames=int(round(total_seconds * sr)),
            sample_rate=sr,
        )
        manifest = project.soundfont_manifest()
    finally:
        project.close()
    # backend 0 == builtin GM fallback; anything else means an SF2 sneaked in.
    for entry in manifest:
        if entry.backend != 0:
            raise RuntimeError(f"program {entry.program} rendered via backend {entry.backend}, expected GM fallback")
    return np.asarray(audio, dtype=np.float32)
