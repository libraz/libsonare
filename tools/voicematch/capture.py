#!/usr/bin/env python3
"""Capture a reference corpus from an AudioUnit instrument, and calibrate the rig.

Three commands, in the order they are used:

    capture.py calibrate --config capture/grand3.json
    capture.py corpus    --config capture/grand3.json
    capture.py verify    --config capture/grand3.json

`calibrate` measures what the *host* has to do to record this plugin
faithfully, and refuses to guess. A disk-streaming sampler has two settings
that silently ruin a recording — how long it is given to load, and whether it
is driven in real time — and neither one announces itself: too little settling
gives a file of the right length holding a peak three orders of magnitude too
small, and too much speed gives one that starts correctly and goes silent in
the middle. Both look fine in a file browser. The calibration bisects the
settle time, renders the same note with and without real time, renders one note
twice to check the plugin is identical to itself, and writes the recipe out.

`corpus` renders the note x velocity x timbre grid one note per process, which
is what keeps a sympathetic-resonance model from letting the previous note bleed
into this one's measurement. Each finished render is appended to the manifest
before the next starts, so an interrupted run leaves an exact record and
re-running resumes from it.

`verify` re-reads what is on disk and reports the failures that leave a
plausible file behind — silence, a clipped peak, a velocity that went nowhere.

The audio this writes stays out of the repository: a sample library's licence
covers what you render from it. `profile.py` turns the corpus into measurements,
and it is the measurements that are committed.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from dataclasses import replace
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _repo import REPO_ROOT  # noqa: E402
from au_oracle import (  # noqa: E402
    AuRenderError,
    AuSource,
    dry_params,
    find_aubounce,
    resolve_preset,
)
from wavio import read_wav  # noqa: E402

HERE = Path(__file__).resolve().parent
DEFAULT_CONFIG = HERE / "capture" / "grand3.json"
# Captured audio is licensed by whoever made the instrument, so it never enters
# the tree. SONARE_VOICEMATCH_ROOT names where a corpus lives; without it the
# renders go to an untracked scratch directory inside the checkout, so a fresh
# clone captures somewhere sane instead of failing on a path only one machine
# has.
CORPUS_ROOT = Path(
    os.environ.get("SONARE_VOICEMATCH_ROOT") or REPO_ROOT / ".cache" / "voicematch"
).expanduser()
DEFAULT_OUT_ROOT = CORPUS_ROOT / "capture"


# --------------------------------------------------------------------------
# config


def load_config(path: Path) -> dict:
    """Read a capture definition and fill in the defaults it left out."""
    cfg = json.loads(path.read_text())
    cfg.setdefault("sample_rate", 48000)
    cfg.setdefault("settle_ms", 4000)
    cfg.setdefault("realtime", True)
    cfg.setdefault("gate_ms", 8000)
    cfg.setdefault("tail", "2s")
    cfg.setdefault("preroll_ms", 100)
    cfg.setdefault("dry", True)
    cfg.setdefault("params", [])
    cfg["_path"] = str(path)
    return cfg


def config_params(cfg: dict) -> tuple[str, ...]:
    """The `--param` list: the config's own, plus every effect section switched off."""
    params = tuple(cfg.get("params", ()))
    if cfg.get("dry", True):
        params = tuple(dict.fromkeys(dry_params(cfg["plugin"]) + params))
    return params


def source_for(cfg: dict, timbre: dict, **overrides) -> AuSource:
    """The `AuSource` that records one timbre of a capture definition."""
    src = AuSource(
        plugin=cfg["plugin"],
        preset=timbre.get("preset", ""),
        params=config_params(cfg),
        settle_ms=int(cfg["settle_ms"]),
        realtime=bool(cfg["realtime"]),
        preroll_ms=int(cfg["preroll_ms"]),
        tail=str(cfg["tail"]),
        sample_rate=int(cfg["sample_rate"]),
    )
    return replace(src, **overrides) if overrides else src


def out_root(cfg: dict, cli_out: str) -> Path:
    if cli_out:
        return Path(cli_out).expanduser().resolve()
    return DEFAULT_OUT_ROOT / cfg["id"]


# --------------------------------------------------------------------------
# calibrate


def _note_argv(source: AuSource, out: Path, note: int, velocity: int, gate_ms: int) -> list[str]:
    argv = source.argv(out)
    # `argv` builds a render with no notes in it; a single note is what a
    # calibration probe needs and what the corpus grid is made of.
    return argv[:3] + ["--note", str(note), "--velocity", str(velocity), "--gate-ms", str(gate_ms)] + argv[3:]


def _probe(source: AuSource, out: Path, note: int, velocity: int, gate_ms: int) -> dict:
    """One calibration render. Returns aubounce's summary, or the refusal as data."""
    argv = _note_argv(source, out, note, velocity, gate_ms)
    started = time.monotonic()
    proc = subprocess.run(argv, capture_output=True, text=True)
    wall = time.monotonic() - started
    if proc.returncode != 0:
        return {"error": proc.stderr.strip()[:400], "wall_s": wall}
    try:
        summary = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return {"error": f"no JSON: {proc.stdout[:200]}", "wall_s": wall}
    summary["wall_s"] = round(wall, 2)
    return summary


def calibrate(cfg: dict, out: Path, *, note: int, velocity: int, verbose: bool) -> dict:
    """Measure the host settings this plugin needs, and prove they are stable.

    Reports rather than asserts. The numbers are the point: a future plugin
    update that moves the minimum settle time shows up here as a changed number
    rather than as a corpus that is quietly missing its first note.
    """
    timbre = cfg["timbres"][0]
    base = source_for(cfg, timbre, tail="2s")
    scratch = out / "_calibration"
    scratch.mkdir(parents=True, exist_ok=True)
    gate_ms = 2000
    report: dict = {
        "config": cfg["_path"],
        "plugin": cfg["plugin"],
        "aubounce": str(find_aubounce()),
        "timbre": timbre.get("id", ""),
        "preset": str(resolve_preset(timbre["preset"])) if timbre.get("preset") else "",
        "params": list(base.params),
        "probe": {"note": note, "velocity": velocity, "gate_ms": gate_ms},
    }

    # 1. Real time or not. A sampler driven at full speed reports the gap it
    #    left in the middle of the note; there is nothing else to notice.
    print("== real time ==", file=sys.stderr)
    rt_rows = []
    for realtime in (True, False):
        s = _probe(replace(base, realtime=realtime, settle_ms=max(4000, base.settle_ms)),
                   scratch / f"rt_{realtime}.wav", note, velocity, gate_ms)
        rt_rows.append({"realtime": realtime, **{k: s.get(k) for k in
                                                 ("peak", "dropout_ms", "seconds", "wall_s", "error")}})
        print(f"  realtime={str(realtime):5s} peak={s.get('peak', 0):.4f} "
              f"dropout={s.get('dropout_ms', '?')}ms wall={s.get('wall_s', 0):.1f}s", file=sys.stderr)
    report["realtime"] = rt_rows
    realtime_required = bool(next(r for r in rt_rows if not r["realtime"])["dropout_ms"])
    report["realtime_required"] = realtime_required

    # 2. Settle time, by bisection on "does a note come out at all".
    print("== settle ==", file=sys.stderr)
    src = replace(base, realtime=True)
    settle_rows = []

    def sounds(ms: int) -> bool:
        s = _probe(replace(src, settle_ms=ms), scratch / f"settle_{ms}.wav", note, velocity, gate_ms)
        peak, drop = float(s.get("peak", 0.0)), s.get("dropout_ms", 0)
        ok = peak >= src.min_peak and not drop
        settle_rows.append({"settle_ms": ms, "peak": peak, "dropout_ms": drop,
                            "seconds": s.get("seconds"), "wall_s": s.get("wall_s"), "ok": ok})
        print(f"  settle={ms:6d} peak={peak:.4f} dropout={drop}ms -> {'ok' if ok else 'SILENT'}",
              file=sys.stderr)
        return ok

    lo, hi = 0, 500
    while hi <= 32000 and not sounds(hi):
        lo, hi = hi, hi * 2
    if hi > 32000:
        report["settle"] = settle_rows
        report["settle_min_ms"] = None
        report["error"] = "no settle time up to 32 s produced a note"
        _write(out / "calibration.json", report)
        return report
    while hi - lo > 250:
        mid = (lo + hi) // 2
        if sounds(mid):
            hi = mid
        else:
            lo = mid
    report["settle"] = settle_rows
    report["settle_min_ms"] = hi
    # Twice the measured minimum, because the minimum is what this machine
    # needed while otherwise idle and a loaded machine needs more.
    report["settle_recommended_ms"] = max(2 * hi, 2000)

    # 3. The plugin against itself. Everything downstream assumes a render is a
    #    property of the settings rather than of the moment.
    print("== determinism ==", file=sys.stderr)
    rec = report["settle_recommended_ms"]
    det = replace(src, settle_ms=rec)
    a = _probe(det, scratch / "det_a.wav", note, velocity, gate_ms)
    b = _probe(det, scratch / "det_b.wav", note, velocity, gate_ms)
    same = False
    if "error" not in a and "error" not in b:
        wa, _ = read_wav(scratch / "det_a.wav")
        wb, _ = read_wav(scratch / "det_b.wav")
        same = wa.shape == wb.shape and bool(np.array_equal(wa, wb))
    report["deterministic"] = same
    report["determinism"] = {"a": a.get("peak"), "b": b.get("peak"),
                             "identical_samples": same}
    print(f"  two renders identical: {same}", file=sys.stderr)

    # 4. Dryness. A reference with a room in it makes every timbre metric lie in
    #    the same direction, so the capture has to prove there is none.
    print("== dryness ==", file=sys.stderr)
    try:
        from room import estimate_room

        audio, sr = read_wav(scratch / "det_a.wav")
        # The note occupies preroll .. preroll+gate; what follows it is the
        # damper falling plus whatever space the capture picked up.
        on = det.preroll_ms / 1000.0
        room = estimate_room(audio, sr, [(on, on + gate_ms / 1000.0)])
        rt60 = float(getattr(room, "rt60_s", 0.0) or 0.0)
        report["room"] = {"rt60_s": rt60, "hf_ratio": float(getattr(room, "hf_ratio", 0.0) or 0.0)}
        print(f"  measured RT60 {rt60:.2f}s "
              f"({'dry' if rt60 < 0.35 else 'A ROOM IS IN THE CAPTURE'})", file=sys.stderr)
    except Exception as exc:  # room measurement is a report, never a gate
        report["room"] = {"error": f"{type(exc).__name__}: {exc}"}

    # 5. Level at the reference velocity, so a later capture can be compared to
    #    this one rather than to whatever the master volume happened to be.
    audio, sr = read_wav(scratch / "det_a.wav")
    mono = audio.mean(axis=1)
    report["reference_level"] = {
        "note": note, "velocity": velocity,
        "peak_dbfs": round(float(20 * np.log10(max(np.abs(mono).max(), 1e-12))), 2),
        "rms_dbfs": round(float(20 * np.log10(max(np.sqrt((mono ** 2).mean()), 1e-12))), 2),
    }

    _write(out / "calibration.json", report)
    print(f"\nrecipe: --realtime={'required' if realtime_required else 'optional'} "
          f"--settle-ms {report['settle_recommended_ms']} "
          f"(minimum measured {report['settle_min_ms']})", file=sys.stderr)
    print(f"-> {out / 'calibration.json'}", file=sys.stderr)
    return report


# --------------------------------------------------------------------------
# corpus


def corpus(cfg: dict, out: Path, *, resume: bool, limit: int, verbose: bool) -> int:
    """Render the note x velocity x timbre grid, one note per process."""
    manifest_path = out / "manifest.json"
    done: dict[str, dict] = {}
    if resume and manifest_path.exists():
        done = {r["id"]: r for r in json.loads(manifest_path.read_text()).get("renders", [])}

    # Loudest velocity of a note first, so every quieter render of that note can
    # be checked against a level that is known to have loaded.
    jobs = [
        (t, n, v)
        for t in cfg["timbres"]
        for n in cfg["notes"]
        for v in sorted(cfg["velocities"], reverse=True)
    ]
    if limit:
        jobs = jobs[:limit]

    header = {
        "id": cfg["id"],
        "config": cfg["_path"],
        "plugin": cfg["plugin"],
        "sample_rate": cfg["sample_rate"],
        "gate_ms": cfg["gate_ms"],
        "tail": cfg["tail"],
        "preroll_ms": cfg["preroll_ms"],
        "settle_ms": cfg["settle_ms"],
        "realtime": cfg["realtime"],
        "params": list(config_params(cfg)),
        "timbres": cfg["timbres"],
        "notes": cfg["notes"],
        "velocities": cfg["velocities"],
    }

    todo = [j for j in jobs if _job_id(*j) not in done or not (out / done[_job_id(*j)]["path"]).exists()]
    per = (cfg["settle_ms"] / 1000.0) + (cfg["gate_ms"] / 1000.0) + 2.5
    print(f"{len(todo)} renders to go of {len(jobs)} "
          f"(~{per:.0f}s each, ~{len(todo) * per / 60:.0f} min)", file=sys.stderr)

    # The reference level per (timbre, note): its loudest velocity, from this run
    # or from a previous one that is already in the manifest.
    loudest: dict[tuple[str, int], float] = {}
    for row in done.values():
        k = (row["timbre"], row["note"])
        loudest[k] = max(loudest.get(k, 0.0), float(row.get("peak", 0.0)))

    failures = 0
    started = time.monotonic()
    for i, (timbre, note, vel) in enumerate(todo, 1):
        jid = _job_id(timbre, note, vel)
        rel = Path(timbre["id"]) / f"n{note:03d}_v{vel:03d}.wav"
        src = source_for(cfg, timbre)
        try:
            summary = _render_note(
                src, out / rel, note, vel, int(cfg["gate_ms"]),
                floor_peak=loudest.get((timbre["id"], note), 0.0),
            )
            loudest[(timbre["id"], note)] = max(
                loudest.get((timbre["id"], note), 0.0), float(summary["peak"])
            )
            done[jid] = {
                "id": jid, "timbre": timbre["id"], "note": note, "velocity": vel,
                "path": str(rel), "peak": summary["peak"], "seconds": summary["seconds"],
                "preroll_peak": summary.get("preroll_peak", 0.0),
                "attempts": summary.get("attempts", 1),
            }
        except (AuRenderError, json.JSONDecodeError) as exc:
            failures += 1
            done.pop(jid, None)
            print(f"[{i}/{len(todo)}] FAIL {jid}: {exc}", file=sys.stderr)
        else:
            elapsed = time.monotonic() - started
            eta = elapsed / i * (len(todo) - i)
            print(f"[{i}/{len(todo)}] {jid} peak {summary['peak']:.4f} "
                  f"({summary['seconds']:.1f}s)  eta {eta / 60:.0f}m", file=sys.stderr)
        # Written every time, so an interrupted run is an exact record rather
        # than an approximate one.
        _write(manifest_path, {**header, "renders": sorted(done.values(), key=lambda r: r["id"])})

    print(f"\n{len(done)} renders in {out}, {failures} failed", file=sys.stderr)
    return 1 if failures else 0


def _job_id(timbre: dict, note: int, vel: int) -> str:
    return f"{timbre['id']}/n{note:03d}_v{vel:03d}"


# A render that loaded is never this far below the loudest velocity of its own
# note. Measured on The Grand 3: the real ratio from velocity 24 to 120 is
# around -24 dB, and a render whose samples did not arrive sits at -42 dB.
QUIET_RATIO = 1.0 / 40.0  # -32 dB


def _render_note(src: AuSource, out: Path, note: int, vel: int, gate_ms: int,
                 *, floor_peak: float, attempts: int = 5) -> dict:
    """One corpus render, retried while it comes back too quiet to be the note.

    The failure this catches is a race inside the plugin rather than a setting:
    the same note at the same velocity renders correctly, then near-silently,
    then correctly again, at roughly one failure in three, and *more* settling
    time does not reduce it — 4 s and 16 s both produced the real 0.0122 while
    8 s in between produced 0.0010. So the answer is to notice and try again,
    not to slow the capture down.

    What decides "too quiet" is the loudest velocity already recorded for this
    same note, not an absolute floor: the real notes at the top of the keyboard
    at velocity 24 are quiet enough that any fixed threshold either lets a
    failure through down there or rejects real notes.
    """
    out.parent.mkdir(parents=True, exist_ok=True)
    floor = max(src.min_peak, floor_peak * QUIET_RATIO)
    last = ""
    for attempt in range(attempts):
        proc = subprocess.run(
            _note_argv(src, out, note, vel, gate_ms), capture_output=True, text=True,
        )
        if proc.returncode != 0:
            last = proc.stderr.strip()[:400]
            continue
        try:
            summary = json.loads(proc.stdout)
        except json.JSONDecodeError:
            last = f"no JSON: {proc.stdout[:160]}"
            continue
        if summary.get("dropout_ms"):
            last = (
                f"dropout {summary['dropout_ms']} ms inside the note. On a treble note held for "
                f"seconds this can be the note simply finishing while the key is still down, "
                f"which an aubounce from before it told the two apart reports as a dropout — "
                f"check `aubounce --version` before reading it as a streaming failure."
            )
            continue
        peak = float(summary.get("peak", 0.0))
        if peak < floor:
            last = f"peak {peak:.5f} against a floor of {floor:.5f}: the samples did not arrive"
            continue
        if attempt:
            print(f"       (took {attempt + 1} attempts)", file=sys.stderr)
        summary["attempts"] = attempt + 1
        return summary
    raise AuRenderError(f"{last} (after {attempts} attempts)")


# --------------------------------------------------------------------------
# verify


def verify(out: Path) -> int:
    """Re-read the corpus and report what a plausible file can still be wrong about."""
    manifest_path = out / "manifest.json"
    if not manifest_path.exists():
        print(f"no manifest at {manifest_path}", file=sys.stderr)
        return 2
    manifest = json.loads(manifest_path.read_text())
    renders = manifest["renders"]
    faults, doubts = [], []
    levels: dict[tuple[str, int], list[tuple[int, float]]] = {}

    for row in renders:
        path = out / row["path"]
        if not path.exists():
            faults.append(f"{row['id']}: missing")
            continue
        audio, sr = read_wav(path)
        mono = audio.mean(axis=1) if audio.ndim > 1 else audio
        peak = float(np.abs(mono).max())
        if peak <= 0.0:
            faults.append(f"{row['id']}: digital silence")
            continue
        if peak >= 0.999:
            doubts.append(f"{row['id']}: clips at {peak:.4f}")
        pre = int(manifest["preroll_ms"] * sr / 1000)
        if pre and float(np.abs(mono[:pre]).max()) > 1e-4:
            doubts.append(f"{row['id']}: energy before the note")
        rms = float(np.sqrt((mono[pre:] ** 2).mean()))
        levels.setdefault((row["timbre"], row["note"]), []).append((row["velocity"], rms))

    for (timbre, note), pts in sorted(levels.items()):
        pts.sort()
        for (v0, r0), (v1, r1) in zip(pts, pts[1:]):
            if r1 < r0 * 0.98:
                doubts.append(
                    f"{timbre} n{note}: velocity {v1} is quieter than {v0} "
                    f"({20 * np.log10(max(r1, 1e-12) / max(r0, 1e-12)):+.1f} dB)"
                )

    for line in faults:
        print(f"FAULT  {line}")
    for line in doubts:
        print(f"doubt  {line}")
    print(f"\n{len(renders)} renders: {len(faults)} faults, {len(doubts)} doubts")
    return 1 if faults else 0


# --------------------------------------------------------------------------


def _write(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    for name, help_text in (
        ("calibrate", "measure the host settings this plugin needs"),
        ("corpus", "render the note x velocity x timbre grid"),
        ("verify", "re-read the corpus and report what is wrong with it"),
    ):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--config", default=str(DEFAULT_CONFIG), help="capture definition JSON")
        p.add_argument("--out", default="", help=f"output root (default: {DEFAULT_OUT_ROOT}/<id>)")
        p.add_argument("--verbose", action="store_true")

    cal = sub.choices["calibrate"]
    cal.add_argument("--note", type=int, default=60, help="probe note (default: middle C)")
    cal.add_argument("--velocity", type=int, default=100)

    cor = sub.choices["corpus"]
    cor.add_argument("--no-resume", action="store_true", help="re-render everything")
    cor.add_argument("--limit", type=int, default=0, help="stop after this many renders")

    args = ap.parse_args()
    cfg = load_config(Path(args.config))
    out = out_root(cfg, args.out)

    if args.cmd == "calibrate":
        calibrate(cfg, out, note=args.note, velocity=args.velocity, verbose=args.verbose)
        return 0
    if args.cmd == "corpus":
        return corpus(cfg, out, resume=not args.no_resume, limit=args.limit, verbose=args.verbose)
    return verify(out)


if __name__ == "__main__":
    sys.exit(main())
