"""Whether re-measuring a reference from its cached audio changes anything.

`profile.py measure` is a pure function of the cached capture audio under
`SONARE_VOICEMATCH_ROOT` (or `.cache/voicematch/`) and the analysis code — no
live recording, nothing else read. So running it again over an unchanged
corpus with unchanged code must reproduce the committed `reference/<id>.json`
byte for byte but its `measured_utc` stamp (`profile_status.measurement_stamp`
would carry even that forward, but it is comparing against a fresh scratch
path rather than the reference, so it always mints a new one here).

This is the control a measurement-code change needs run *before* it lands:
without it, a diff the change produces cannot be told apart from a diff the
committed tree already had against today's code. A mismatch here is not a bug
in this script — it is that drift, found before the change it would otherwise
be blamed on.

Writes only under a scratch root passed as `--profile` to `measure`, never
into `reference/` or `capture/`. Read-only over the cached corpus. Exits 0
whatever it finds, on the same terms as `status.py`: a target that failed on
"there is drift" could not be the thing a calibration round reads to decide
whether it is safe to start.

    rye run --pyproject bindings/python/pyproject.toml \\
        python tools/voicematch/reextract_check.py
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import math
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import profile as profile_module

from capture import load_config, out_root

HERE = Path(__file__).resolve().parent
CAPTURE_DIR = HERE / "capture"
REFERENCE_DIR = HERE / "reference"

#: Top-level keys a re-measurement is allowed to differ on. Just the one --
#: see the module docstring for why it always changes.
IGNORED_KEYS = {"measured_utc"}

#: How many individual field diffs to collect per mismatched id. A profile
#: mismatch is almost always one root cause repeated over every row of a grid,
#: and printing all of them would bury the row that is actually informative.
MAX_DIFFS = 20


def shipped_ids() -> list[str]:
    """Every committed capture definition, by id (`.local.json` excluded)."""
    return sorted(p.stem for p in CAPTURE_DIR.glob("*.json") if not p.name.endswith(".local.json"))


def _diff(old: object, new: object, path: str, out: list[str]) -> None:
    if len(out) >= MAX_DIFFS:
        return
    if isinstance(old, dict) and isinstance(new, dict):
        for key in sorted(set(old) | set(new)):
            if key not in old:
                out.append(f"{path}.{key}: absent -> {new[key]!r}")
            elif key not in new:
                out.append(f"{path}.{key}: {old[key]!r} -> absent")
            else:
                _diff(old[key], new[key], f"{path}.{key}", out)
            if len(out) >= MAX_DIFFS:
                return
    elif isinstance(old, list) and isinstance(new, list):
        if len(old) != len(new):
            out.append(f"{path}: {len(old)} rows -> {len(new)} rows")
        for i, (a, b) in enumerate(zip(old, new)):
            # A row's (timbre, note, velocity) identifies it far better than its
            # index, which shifts under an unrelated insertion elsewhere in the
            # grid and then blames the wrong note for someone else's change.
            label = f"{path}[{i}]"
            if isinstance(a, dict) and "note" in a:
                label = (f"{path}[timbre={a.get('timbre')!r} note={a.get('note')} "
                         f"velocity={a.get('velocity')}]")
            _diff(a, b, label, out)
            if len(out) >= MAX_DIFFS:
                return
    elif _unequal(old, new):
        out.append(f"{path}: {old!r} -> {new!r}")


def _unequal(old: object, new: object) -> bool:
    """`!=` with one exception: two NaNs are the same "unmeasurable" answer.

    `nan != nan` is true in Python, so a plain `!=` reads `unscorable partial`
    as a fresh mismatch on every single re-extraction -- a false alarm on
    exactly the rows `measure_hit`/`measure_note` already gave up on, and loud
    enough (multiple partials per row, several rows per grid) to bury a real
    difference sitting beside it in the same diff.
    """
    if isinstance(old, float) and isinstance(new, float) and math.isnan(old) and math.isnan(new):
        return False
    return old != new


def compare(old: dict, new: dict) -> list[str]:
    """Every field difference between two profiles but the ignored keys.

    Each entry names the full path to the field, the row's timbre/note/velocity
    where the field lives inside `rows`, and the value before and after — a
    count alone cannot say whether a mismatch is one instrument's onset drifting
    by a millisecond or every instrument's dynamic range collapsing.
    """
    old = {k: v for k, v in old.items() if k not in IGNORED_KEYS}
    new = {k: v for k, v in new.items() if k not in IGNORED_KEYS}
    out: list[str] = []
    _diff(old, new, "", out)
    return out


def reextract_one(cap_id: str, scratch: Path) -> dict:
    """Re-measure one capture from its cached audio and diff it against `reference/`.

    Returns a status dict rather than raising, so a run over the whole bank
    finishes and reports every id instead of stopping at the first one with no
    cached corpus.
    """
    ref_path = REFERENCE_DIR / f"{cap_id}.json"
    if not ref_path.exists():
        return {"id": cap_id, "status": "no-reference", "diffs": []}
    cfg = load_config(CAPTURE_DIR / f"{cap_id}.json")
    corpus_dir = out_root(cfg, "")
    if not (corpus_dir / "manifest.json").exists():
        return {"id": cap_id, "status": "no-cache", "diffs": []}
    out_path = scratch / f"{cap_id}.json"
    # `measure` is talkative (per-note progress, a summary table) -- useful at
    # a terminal, noise across 129 ids. Captured rather than silenced outright,
    # so a failing id can still show what it printed.
    log = io.StringIO()
    with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
        rc = profile_module.measure(cfg, corpus_dir, out_path)
    if rc != 0:
        return {"id": cap_id, "status": "measure-failed", "diffs": [], "log": log.getvalue()}
    old = json.loads(ref_path.read_text())
    new = json.loads(out_path.read_text())
    diffs = compare(old, new)
    return {"id": cap_id, "status": "match" if not diffs else "mismatch", "diffs": diffs}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ids", default="",
                    help="comma-separated capture ids (default: every shipped capture)")
    ap.add_argument("--out", default="",
                    help="scratch root for re-extracted profiles (default: a fresh temp dir)")
    ap.add_argument("--json", action="store_true",
                    help="print the full per-id results as JSON instead of a summary")
    args = ap.parse_args(argv)

    ids = [i.strip() for i in args.ids.split(",") if i.strip()] or shipped_ids()
    scratch = (Path(args.out).expanduser().resolve() if args.out
               else Path(tempfile.mkdtemp(prefix="voicematch-reextract-")))
    scratch.mkdir(parents=True, exist_ok=True)

    results = []
    for i, cap_id in enumerate(ids, 1):
        result = reextract_one(cap_id, scratch)
        results.append(result)
        print(f"[{i}/{len(ids)}] {cap_id}: {result['status']}", file=sys.stderr)
        for line in result["diffs"]:
            print(f"    {line}", file=sys.stderr)

    if args.json:
        print(json.dumps(results, indent=1, ensure_ascii=False))
        return 0

    by_status: dict[str, list[str]] = {}
    for r in results:
        by_status.setdefault(r["status"], []).append(r["id"])
    matched = len(by_status.get("match", []))
    print(f"\n{matched}/{len(ids)} identical to the committed reference "
          f"(measured_utc excluded) -- scratch root {scratch}")
    for status, cap_ids in sorted(by_status.items()):
        if status == "match":
            continue
        print(f"  {status} ({len(cap_ids)}): {', '.join(cap_ids)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
