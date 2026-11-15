"""Render the model for one probe in a fresh process and measure it.

The per-evaluation render `autofit` and the dataset tools drive: the parent runs
`render_model_rows_subprocess`, the child runs `render_metrics_main` (reached
through `render_rows.py _render_metrics`, or `autofit.py _render_metrics`).
"""

from __future__ import annotations

import argparse
import atexit
import json
import multiprocessing
import os
import subprocess
import sys
import tempfile
from concurrent.futures import Executor, ProcessPoolExecutor
from dataclasses import replace
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/ for _repo
sys.path.insert(0, str(Path(__file__).resolve().parent))

import harness_lock
import oracle_cache
from autofit_resolve import _score
from build_lib import library_path
from capture import model_rig
from corpus import Corpus, corpus_from_descriptor, load_corpus
from knobs import merge_overrides
from loss import probe_rows
from metrics import MONO_MODES, normalize_rms, to_mono
from render_model import render_model
from room import apply_room
from smf import write_smf

SR = 48000


def render_rate(corpus) -> int:
    """The rate this run renders at: the corpus's own, or the default without one.

    `corpus_oracle` refuses a corpus whose rate differs from the render's rather
    than resampling a reference, so a run against a capture recorded at anything
    but 48 kHz cannot start until the model moves to meet it. Every capture with
    `source_class: module` is at 44.1 kHz, so before this none of them could be
    fitted against at all.
    """
    return int(corpus.sample_rate) if corpus is not None else SR


#: What `--metric-threads auto` resolves to. Measuring a note is mostly the
#: partial refinement in `skeleton_note`, which spends its time inside numpy
#: with the interpreter lock dropped; past three the lock is what is left and
#: the curve flattens (measured 1.6x at two, 2.1x at three, 2.1x at four). It is
#: also the ceiling this machine is meant to keep concurrent work under.
AUTO_METRIC_THREADS = 3


#: Every render child and row worker runs its linear algebra on one thread. Left to
#: itself OpenBLAS starts one per core in every process, and on a shared 40-thread
#: host that tripled the measurement time of a 12-row probe (28 s against 9 s).
BLAS_ONE_THREAD = {
    "OPENBLAS_NUM_THREADS": "1",
    "OMP_NUM_THREADS": "1",
    "MKL_NUM_THREADS": "1",
    "VECLIB_MAXIMUM_THREADS": "1",
}

_ROW_POOLS: dict[int, ProcessPoolExecutor] = {}


def row_pool(workers: int) -> ProcessPoolExecutor:
    """A process pool of `workers` that measures the rows of one render, shared by the run."""
    if workers not in _ROW_POOLS:
        # Spawned workers take the environment at spawn time, and import numpy before any task.
        os.environ.update(BLAS_ONE_THREAD)
        pool = ProcessPoolExecutor(workers, mp_context=multiprocessing.get_context("spawn"))
        atexit.register(pool.shutdown)
        _ROW_POOLS[workers] = pool
    return _ROW_POOLS[workers]


def _measure_chunk(
    raw_path: str,
    mono_path: str,
    pattern,
    indices: list[int],
    sr: int,
    max_band_hz: float | None,
    tnr_masks: list[dict] | None,
) -> str:
    """Measure the analysis notes at `indices` of one render, read back from its arrays.

    Returned as the JSON a measuring child prints, so the rows reach the parent the same way.
    """
    part = replace(pattern, analysis_notes=[pattern.analysis_notes[i] for i in indices])
    rows = probe_rows(
        np.load(mono_path),
        part,
        sr,
        raw=np.load(raw_path),
        max_band_hz=max_band_hz,
        threads=1,
        tnr_masks=None if tnr_masks is None else [tnr_masks[i] for i in indices],
    )
    return json.dumps(rows)


def _chunks(n: int, parts: int) -> list[list[int]]:
    """`range(n)` cut into `parts` contiguous runs whose lengths differ by at most one."""
    size, extra = divmod(n, parts)
    out, at = [], 0
    for k in range(parts):
        step = size + (k < extra)
        out.append(list(range(at, at + step)))
        at += step
    return [c for c in out if c]


def resolve_metric_threads(args) -> int:
    """How many of a probe's notes one render may measure at once.

    Zero means auto. A run that already renders candidates concurrently is
    handled by the caller, not here: this answers what one render may use, and
    `Evaluator` is what knows whether that render has siblings.
    """
    asked = getattr(args, "metric_threads", 0)
    if asked and asked > 0:
        return asked
    return AUTO_METRIC_THREADS


def render_model_rows_subprocess(
    build_dir: Path,
    program: int,
    pattern_name: str,
    notes_csv: str,
    *,
    velocities_csv: str = "",
    overrides: str = "",
    base_overrides: str = "",
    want_audio: bool = False,
    room_ir: Path | None = None,
    corpus: Corpus | None = None,
    gate_ms: int = 0,
    bank: int = 0,
    band_edge_hz: float | None = None,
    metric_threads: int = 1,
    tnr_masks: list[dict] | None = None,
    mono_mode: str = "mean",
    rows_on: Executor | None = None,
    row_workers: int = 1,
) -> tuple[list[dict], np.ndarray | None]:
    """Render the model in a fresh subprocess; return per-note metrics (+ audio).

    With `rows_on`, the child only renders: its signal comes back as arrays and
    the analysis notes are measured in up to `row_workers` contiguous runs on
    that pool, reassembled in order. Each row reads its own window of the same
    signal through the same function, so the rows are the child's rows exactly.

    A subprocess is required for two reasons: a rebuilding run must load the
    just-rebuilt shared library (the same process would keep the first-loaded image
    mapped), and a runtime-knob run must have `SONARE_TUNING_OVERRIDES` in the
    environment before the library's static initialisers read it.

    The mono render comes back through a temporary `.npy` rather than stdout —
    a ten-second probe is millions of samples, which JSON would spend more time
    encoding than the render itself takes.
    """
    harness_lock.check_workers()
    library = library_path(build_dir)
    if library is None:
        raise RuntimeError(f"no libsonare shared library found under {build_dir}")
    env = dict(os.environ) | BLAS_ONE_THREAD
    env["SONARE_LIB_PATH"] = str(library)
    overrides = merge_overrides(base_overrides, overrides)
    if overrides:
        env["SONARE_TUNING_OVERRIDES"] = overrides
    else:
        env.pop("SONARE_TUNING_OVERRIDES", None)

    cmd = [
        sys.executable,
        str(Path(__file__).resolve()),
        "_render_metrics",
        "--program",
        str(program),
        "--pattern",
        pattern_name,
        "--notes",
        notes_csv,
        "--velocities",
        velocities_csv,
        "--mono-mode",
        mono_mode,
    ]
    if bank:
        cmd += ["--bank", str(bank)]
    if metric_threads > 1:
        cmd += ["--metric-threads", str(metric_threads)]
    if gate_ms:
        cmd += ["--drum-gate-ms", str(gate_ms)]
    if band_edge_hz:
        cmd += ["--band-edge-hz", str(band_edge_hz)]
    if tnr_masks is not None:
        cmd += ["--tnr-masks", json.dumps(tnr_masks)]
    if room_ir is not None:
        cmd += ["--room-ir", str(room_ir)]
    if corpus is not None:
        # Only the manifest travels: the child renders the model on the corpus
        # timeline and never touches the captured audio, which stays in the
        # parent where the oracle rows were measured from it once.
        if corpus.cache_file is not None:
            cmd += ["--corpus-cache", str(corpus.cache_file)]
        else:
            cmd += ["--corpus", str(corpus.root), "--corpus-timbre", corpus.timbre]
    split = rows_on is not None and row_workers > 1
    with tempfile.TemporaryDirectory(prefix="autofit_") as tmp:
        audio_path = Path(tmp) / "model.npy"
        raw_path = Path(tmp) / "raw.npy"
        if want_audio or split:
            cmd += ["--dump-audio", str(audio_path)]
        if split:
            cmd += ["--render-only", str(raw_path)]
        proc = subprocess.run(cmd, capture_output=True, check=False, text=True, env=env)
        if proc.returncode != 0:
            raise RuntimeError(
                f"model render failed (rc={proc.returncode}):\n{proc.stderr.strip()[-2000:]}"
            )
        audio = np.load(audio_path) if want_audio and audio_path.exists() else None
        if audio is not None and split:
            audio = audio.astype(np.float32)  # what the child hands over when it measures itself
        if not split:
            return json.loads(proc.stdout), audio
        meta = json.loads(proc.stdout)
        pattern, _, _ = _score(
            program, pattern_name, notes_csv, velocities_csv, corpus=corpus, gate_ms=gate_ms
        )
        n = len(pattern.analysis_notes)
        futures = [
            rows_on.submit(
                _measure_chunk,
                str(raw_path),
                str(audio_path),
                pattern,
                chunk,
                meta["sr"],
                band_edge_hz or None,
                tnr_masks,
            )
            for chunk in _chunks(n, min(row_workers, n))
        ]
        rows = [row for f in futures for row in json.loads(f.result())]
    return rows, audio


# --------------------------------------------------------------------------- #
# Internal render subcommand (runs in the per-eval subprocess)
# --------------------------------------------------------------------------- #
def render_metrics_main(argv: list[str]) -> int:
    """Render the model for one score and print its per-note metrics as JSON.

    Invoked as a subprocess by the optimiser with SONARE_LIB_PATH (and, for a
    runtime-knob fit, SONARE_TUNING_OVERRIDES) already set in the environment.
    """
    p = argparse.ArgumentParser(prog="autofit.py _render_metrics")
    p.add_argument("--program", type=int, required=True)
    p.add_argument("--bank", type=int, default=0)
    p.add_argument("--pattern", required=True)
    p.add_argument("--notes", default="")
    p.add_argument("--velocities", default="")
    p.add_argument(
        "--dump-audio",
        default="",
        dest="dump_audio",
        help="also write the normalized mono render to this .npy path",
    )
    p.add_argument(
        "--room-ir",
        default="",
        dest="room_ir",
        help="convolve the render with this .npy impulse response first",
    )
    p.add_argument("--corpus", default="", help="capture manifest laying out the probe")
    p.add_argument("--corpus-timbre", default="", dest="corpus_timbre")
    p.add_argument(
        "--corpus-cache",
        default="",
        dest="corpus_cache",
        help="oracle cache standing in for the manifest",
    )
    p.add_argument("--drum-gate-ms", type=int, default=0, dest="drum_gate_ms")
    p.add_argument("--mono-mode", default="mean", dest="mono_mode", choices=list(MONO_MODES))
    p.add_argument(
        "--band-edge-hz",
        type=float,
        default=0.0,
        dest="band_edge_hz",
        help="the reference's own measurable ceiling; bands above it "
        "are excluded from the profile and from its normalisation",
    )
    p.add_argument(
        "--metric-threads",
        type=int,
        default=1,
        dest="metric_threads",
        help="measure this many of the probe's notes at once",
    )
    p.add_argument(
        "--tnr-masks",
        default="",
        dest="tnr_masks",
        help="the oracle rows' tone-to-noise masks as JSON, one per analysis note",
    )
    p.add_argument(
        "--render-only",
        default="",
        dest="render_only",
        help="write the render to this .npy (with the mono to --dump-audio) and measure nothing",
    )
    a = p.parse_args(argv)

    corpus = load_corpus(a.corpus, a.corpus_timbre) if a.corpus else None
    if a.corpus_cache:
        cache = Path(a.corpus_cache)
        corpus = corpus_from_descriptor(oracle_cache.read(cache).descriptor, cache.parent, cache)
    pattern, total, _ = _score(
        a.program, a.pattern, a.notes, a.velocities, corpus=corpus, gate_ms=a.drum_gate_ms
    )
    smf_bytes = write_smf(
        pattern.notes, program=a.program, bank=a.bank, channel=pattern.channel, end_pad=pattern.tail
    )
    # The model stops where the reference did. With no capture to ask, that is
    # the instrument's own boundary: a fit moves the voice, and the amplifier the
    # bank binds after it is not the voice's to answer for.
    rig = model_rig(corpus.rig) if corpus is not None else False
    audio = np.asarray(
        render_model(smf_bytes, total, render_rate(corpus), rig=rig), dtype=np.float32
    )
    if a.room_ir:
        # Applied here rather than in the parent so the per-note metrics and the
        # multi-scale term both see the same roomed signal.
        audio = apply_room(audio, np.load(a.room_ir))
    # Shape metrics read the downmix; level reads `audio` as equal power.
    raw = to_mono(audio, a.mono_mode)
    mono = normalize_rms(raw)
    if a.render_only:
        np.save(a.render_only, audio)
        np.save(a.dump_audio, mono)
        print(json.dumps({"sr": render_rate(corpus)}))
        return 0
    rows = probe_rows(
        mono,
        pattern,
        render_rate(corpus),
        raw=audio,
        max_band_hz=a.band_edge_hz or None,
        threads=a.metric_threads,
        tnr_masks=json.loads(a.tnr_masks) if a.tnr_masks else None,
    )
    if a.dump_audio:
        np.save(a.dump_audio, mono.astype(np.float32))
    print(json.dumps(rows))
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "_render_metrics":
        sys.exit(render_metrics_main(sys.argv[2:]))
    sys.exit("usage: render_rows.py _render_metrics <args>")
