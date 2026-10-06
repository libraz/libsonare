"""Model-side renderer: SMF bytes -> audio through libsonare's GM fallback bank.

Renders via `Project.import_smf` + `bounce_with_sf2_instrument` with NO
SoundFont loaded, which forces every program through the built-in synthesizer
GM fallback (`gm_fallback_map` -> NativeSynth physical voices) — exactly the
code path being tuned. The dylib is resolved through SONARE_LIB_PATH, and
nothing rebuilds it — a render measures whatever that file happens to hold, so
`ensure_lib_path` says so when the library is older than the sources.

Every render also asks the library for its render-path record through
`SONARE_RENDER_PATH_DUMP`, at a path unique to that render, and returns it with
the audio as `RenderedAudio`. Only a `-DBUILD_TUNING=ON` library writes one; any
other leaves the evidence `unknown`, which is a statement about the library
rather than a failed render.
"""

from __future__ import annotations

import contextlib
import json
import os
import sys
import tempfile
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/ for _repo

from _repo import REPO_ROOT
from boundary import RenderRequest, canonical_digest
from render_evidence import file_digest

DEFAULT_DYLIB = REPO_ROOT / "build-python-shared" / "lib" / "libsonare.dylib"
REFRESH_HINT = "cmake --build build-python-shared --target sonare_shared -j"

_SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".h", ".hpp"})
_staleness_checked = False

#: The variable a tuning build reads at the start of each offline bounce.
PATH_DUMP_ENV = "SONARE_RENDER_PATH_DUMP"
#: The render-path record layout this reader understands.
PATH_RECORD_SCHEMA = 1
STATUS_RECORDED = "recorded"
STATUS_UNKNOWN = "unknown"
NO_PATH_RECORD = "library wrote no path record (not a tuning build?)"


@dataclass(frozen=True)
class RenderedAudio:
    """One model render and the evidence of what produced it.

    `evidence` holds `request_id`, `build_id`, `path` (the library's render-path
    record, or None), `complete`, `status` (`recorded` or `unknown`) and `reason`.
    """

    audio: np.ndarray
    evidence: dict


def _newest_source_mtime() -> float:
    """Most recent edit under the C++ tree the dylib is compiled from."""
    newest = 0.0
    for root in ("src", "include"):
        for path in (REPO_ROOT / root).rglob("*"):
            if path.suffix in _SOURCE_SUFFIXES:
                newest = max(newest, path.stat().st_mtime)
    return newest


def warn_if_stale(lib: str) -> None:
    """Say on stderr when @p lib predates the sources, once per process.

    A render carries no mark of which library produced it, and the default is a
    build directory nothing keeps current, so an edited voice can be measured
    through the previous generation with every number looking ordinary. It is a
    warning rather than an error because measuring an older library on purpose
    is a legitimate control.
    """
    global _staleness_checked
    if _staleness_checked or not lib:
        return
    _staleness_checked = True
    try:
        built = Path(lib).stat().st_mtime
    except OSError:
        return
    newest = _newest_source_mtime()
    if newest <= built:
        return
    stamp = "%Y-%m-%d %H:%M:%S"
    print(
        f"warning: {lib} was built {time.strftime(stamp, time.localtime(built))} and a "
        f"source file changed {time.strftime(stamp, time.localtime(newest))}; this render "
        f"measures the older library. Refresh with: {REFRESH_HINT}",
        file=sys.stderr,
    )


def ensure_lib_path() -> str:
    """Point the Python binding at the working-tree dylib unless overridden."""
    if "SONARE_LIB_PATH" not in os.environ and DEFAULT_DYLIB.exists():
        os.environ["SONARE_LIB_PATH"] = str(DEFAULT_DYLIB)
    lib = os.environ.get("SONARE_LIB_PATH", "")
    warn_if_stale(lib)
    return lib


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


def loaded_library_path() -> str:
    """The file this process loaded libsonare from, or "" before it has loaded one."""
    runtime = sys.modules.get("libsonare._runtime")
    lib = getattr(runtime, "_lib", None)
    return str(getattr(lib, "_name", "") or "")


@contextlib.contextmanager
def path_record_dump() -> Iterator[Path]:
    """Point `SONARE_RENDER_PATH_DUMP` at a fresh path for one render, then restore it."""
    previous = os.environ.get(PATH_DUMP_ENV)
    with tempfile.TemporaryDirectory(prefix="sonare_path_") as tmp:
        dump = Path(tmp) / "render-path.json"
        os.environ[PATH_DUMP_ENV] = str(dump)
        try:
            yield dump
        finally:
            if previous is None:
                os.environ.pop(PATH_DUMP_ENV, None)
            else:
                os.environ[PATH_DUMP_ENV] = previous


def render_evidence(request_id: str, dump: Path, library: str) -> dict:
    """The evidence for one render, from its path record (if written) and the loaded library."""
    record = None
    reason = NO_PATH_RECORD
    if dump.exists():
        try:
            record = json.loads(dump.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            reason = f"path record unreadable: {exc}"
        else:
            if not isinstance(record, dict) or record.get("schema") != PATH_RECORD_SCHEMA:
                schema = record.get("schema") if isinstance(record, dict) else None
                reason = f"path record schema {schema!r} is not {PATH_RECORD_SCHEMA}"
                record = None
    library_file = Path(library) if library else None
    library_sha = (
        file_digest(library_file) if library_file is not None and library_file.is_file() else None
    )
    bank = record.get("bank_registry_digest") if record is not None else None
    return {
        "request_id": request_id,
        "build_id": (
            canonical_digest({"library_sha256": library_sha, "bank_registry_digest": bank})
            if library_sha
            else None
        ),
        "path": record,
        "complete": bool(record.get("complete")) if record is not None else False,
        "status": STATUS_RECORDED if record is not None else STATUS_UNKNOWN,
        "reason": None if record is not None else reason,
    }


def render_model(
    smf_bytes: bytes, total_seconds: float, sr: int = 48000, *, rig: bool = True, preset: str = ""
) -> np.ndarray:
    """`render_model_rendered` for a caller that wants the audio alone."""
    return render_model_rendered(smf_bytes, total_seconds, sr, rig=rig, preset=preset).audio


def render_model_rendered(
    smf_bytes: bytes,
    total_seconds: float,
    sr: int = 48000,
    *,
    rig: bool = True,
    preset: str = "",
    request_id: str | None = None,
) -> RenderedAudio:
    """Render SMF bytes to a (frames, 2) float32 array via the GM fallback bank.

    `preset` switches address space rather than voicing: a public catalogue entry
    carries no GM number, so it is bound as the patch and the file's own program
    changes are ignored. Nothing else reaches the catalogue — the GM manifest
    check does not apply, because no program was resolved through the bank, and
    `rig` does not either, since a rig binding is bank data and a preset is a
    bare patch.

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

    `request_id` names the request this render answers (a `RenderRequest`
    fingerprint); without one it is the digest of the arguments actually used,
    the SMF bytes and the process's tuning overrides included.
    """
    if request_id is None:
        request_id = canonical_digest(
            {
                "smf": bytes(smf_bytes),
                "seconds": float(total_seconds),
                "sample_rate": int(sr),
                "rig": bool(rig),
                "preset": preset,
                "overrides": os.environ.get("SONARE_TUNING_OVERRIDES", ""),
            }
        )
    ensure_lib_path()
    import libsonare  # deferred so SONARE_LIB_PATH is set before the dylib loads

    with path_record_dump() as dump:
        audio, manifest = _bounce(libsonare, smf_bytes, total_seconds, sr, rig=rig, preset=preset)
        evidence = render_evidence(request_id, dump, loaded_library_path())
    if manifest is not None:
        check_gm_fallback(manifest)
    return RenderedAudio(np.asarray(audio, dtype=np.float32), evidence)


def _bounce(libsonare, smf_bytes: bytes, total_seconds: float, sr: int, *, rig: bool, preset: str):
    """One offline bounce; returns the audio and the SoundFont manifest (None for a preset)."""
    project = libsonare.Project()
    try:
        # A bounce rate that differs from the project's own is refused outright
        # (`project_bounce.cpp`), and a new project does not start at the rate a
        # capture was recorded at. Every module capture records 44.1 kHz, so
        # without this the model cannot be rendered against any of them at all.
        project.set_sample_rate(float(sr))
        project.import_smf(smf_bytes)
        if preset:
            audio = project.bounce_with_synth_instrument(
                instrument=preset,
                auto_select_gm=False,
                total_frames=round(total_seconds * sr),
                sample_rate=sr,
            )
            manifest = None
        else:
            audio = project.bounce_with_sf2_instrument(
                libsonare.Sf2InstrumentConfig(clear_bank_rig=not rig),
                total_frames=round(total_seconds * sr),
                sample_rate=sr,
            )
            manifest = project.soundfont_manifest()
    finally:
        project.close()
    return audio, manifest


def _validate_request_environment(request: RenderRequest) -> None:
    if request.overrides != os.environ.get("SONARE_TUNING_OVERRIDES", ""):
        raise ValueError("request overrides disagree with SONARE_TUNING_OVERRIDES")


def render_request(request: RenderRequest) -> np.ndarray:
    """Render the SMF a `RenderRequest` carries, at its rig, preset and sample rate.

    The request's `overrides` travel in `SONARE_TUNING_OVERRIDES`, which the
    library reads when it loads, so they are the caller's to set before this
    process starts; they are not applied here.
    """
    _validate_request_environment(request)
    return render_model(
        request.smf, request.seconds, request.sample_rate, rig=request.rig, preset=request.preset
    )


def render_request_rendered(request: RenderRequest) -> RenderedAudio:
    """`render_request` with its evidence, the request's fingerprint as `request_id`."""
    _validate_request_environment(request)
    return render_model_rendered(
        request.smf,
        request.seconds,
        request.sample_rate,
        rig=request.rig,
        preset=request.preset,
        request_id=request.fingerprint(),
    )
