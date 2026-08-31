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


def check_gm_fallback(manifest) -> None:
    """Fail unless every program in a render went through the built-in GM bank.

    Backend 0 is that bank; anything else means a SoundFont was loaded, and the
    render then measures sampled audio while reporting on physical models. The
    harness never loads one, so this is a guard against the day something else
    does — and it belongs to every renderer here, not just the one that grew it
    first.
    """
    for entry in manifest:
        if entry.backend != 0:
            raise RuntimeError(
                f"program {entry.program} rendered via backend {entry.backend}, "
                f"expected GM fallback"
            )


def render_model(smf_bytes: bytes, total_seconds: float, sr: int = 48000, *,
                 rig: bool = True) -> np.ndarray:
    """Render SMF bytes to a (frames, 2) float32 array via the GM fallback bank.

    `rig` selects which side of the instrument's boundary the render stops at.
    The default is the product sound — the bank binds an amplifier after an
    electric guitar's voice, and a listener hears it — which is what an audition
    should play and what a consumer gets from the same file.

    A measurement passes `rig=False` when the reference it will be compared with
    was captured at the instrument's own boundary. Comparing a rigged model
    against a direct reference measures the amplifier as if it were the string,
    and a fit run that way reproduces the amplifier with the instrument's own
    parameters. The capture's `rig` field is the one place that answer lives, so
    the caller reads it from there rather than deciding per call site.
    """
    ensure_lib_path()
    import libsonare  # deferred so SONARE_LIB_PATH is set before the dylib loads

    project = libsonare.Project()
    try:
        project.import_smf(smf_bytes)
        audio = project.bounce_with_sf2_instrument(
            libsonare.Sf2InstrumentConfig(clear_bank_rig=not rig),
            total_frames=int(round(total_seconds * sr)),
            sample_rate=sr,
        )
        manifest = project.soundfont_manifest()
    finally:
        project.close()
    check_gm_fallback(manifest)
    return np.asarray(audio, dtype=np.float32)
