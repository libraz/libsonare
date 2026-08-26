"""The level a per-piece fit cannot see, measured and handed back as a gain.

The loss removes exactly one gain across the note set it is given. On a keyboard
that is the point — per-note level error stays in, and only the voice's overall
loudness is divided out. On a kit fitted one piece at a time the set is one note,
so the gain it removes IS that piece's own offset: level becomes free, every
candidate is scored as though it had been matched, and the piece comes back at
whatever level its other coordinates happened to produce.

Nothing in the loss can catch it, because the quantity was subtracted before any
term was taken. It shows up one layer out, in the independent measurements, as a
level error that grew while every term improved — which is what it did, twice,
before this module existed.

So the level is measured separately and written back into each piece's own
`gain`. The measurement is the same held level the loss removes, taken over each
piece's own body window, averaged over the velocity layers. A gain the clamp
cannot reach is reported rather than silently truncated: a piece that needs more
than the clamp allows is a finding about the voice, not about this correction.
"""

from __future__ import annotations

import argparse
import sys

import numpy as np

from . import struck, terms

#: What `clamp_synth_patch` accepts for a patch's output gain.
GAIN_MIN = 0.0
GAIN_MAX = 4.0


def parse_profile_levels(text: str) -> dict[int, float]:
    """Per note, the median `level Δdb` from a `profile.py compare` table.

    Read from the tool that owns the measurement rather than measured again
    here. The first attempt did measure it again -- a held level over each
    piece's own body window, which is what the loss removes -- and the two do
    not agree: `analyze_hit` takes the RMS of the hit after a global
    normalisation, so on a long open hi-hat the body window averages in a tail
    the hit measure never sees. Correcting by the wrong one of the two moved the
    open hat 14 dB and left it four decibels quiet, and the gate's level error
    grew while the correction reported success.
    """
    rows: dict[int, list[float]] = {}
    for line in text.splitlines():
        body = line.split("|", 1)
        if len(body) != 2 or not body[0].strip():
            continue
        head = body[0].split()
        if len(head) != 2 or not head[0].isdigit():
            continue
        cells = body[1].split()
        if not cells:
            continue
        try:
            rows.setdefault(int(head[0]), []).append(float(cells[-1]))
        except ValueError:
            continue
    return {n: float(np.median(v)) for n, v in rows.items() if v}


def profile_levels(config: str, notes, lib: str, overrides: str) -> dict[int, float]:
    """Run `profile.py compare` and read its level column."""
    import os
    import subprocess
    from pathlib import Path

    here = Path(__file__).resolve().parents[1]
    env = dict(os.environ)
    if lib:
        env["SONARE_LIB_PATH"] = lib
    if overrides:
        env["SONARE_TUNING_OVERRIDES"] = overrides
    else:
        env.pop("SONARE_TUNING_OVERRIDES", None)
    p = subprocess.run(
        [sys.executable, str(here / "profile.py"), "compare", "--config", config,
         "--notes", ",".join(str(n) for n in notes)],
        capture_output=True, text=True, env=env, cwd=here.parents[1])
    if p.returncode:
        raise RuntimeError(p.stderr[-3000:])
    return parse_profile_levels(p.stdout)


def held_offsets(loss, overrides: str, notes) -> dict[int, float]:
    """Per note, how many dB the model sits above the reference.

    Over the piece's own body window on each side rather than a fixed one: a
    kit's pieces are an order of magnitude apart in length, and a window long
    enough for a ride averages a hi-hat's level with the silence after it.
    """
    out: dict[int, float] = {}
    sr = loss.spectro.sample_rate
    for n in notes:
        pairs = [(n, v) for v in loss.velocities]
        ref = loss.signals(pairs, ref=True)
        mod = loss.signals(pairs, ov=overrides)
        deltas = []
        for k in pairs:
            body, _late = struck.windows(ref[k], sr)
            m = terms.held_db(mod[k], window=body, sr=sr)
            r = terms.held_db(ref[k], window=body, sr=sr)
            if np.isfinite(m) and np.isfinite(r):
                deltas.append(m - r)
        if deltas:
            out[n] = float(np.mean(deltas))
    return out


def corrections(base: dict, overrides: dict, offsets: dict[int, float]):
    """`(new gains, pieces the clamp cannot satisfy)`.

    A piece already at the clamp that still measures short cannot be corrected
    by a gain at all, and saying so is the point: it is the same finding as a
    fitted coordinate sitting at a range bound.
    """
    gains: dict[str, float] = {}
    stuck: list[tuple[int, float, float]] = []
    for n, off in sorted(offsets.items()):
        key = f"d{n:03d}.gain"
        cur = overrides.get(key, base.get(key))
        if cur is None:
            continue
        want = cur * (10.0 ** (-off / 20.0))
        got = min(max(want, GAIN_MIN), GAIN_MAX)
        if abs(got - want) > 1e-6:
            stuck.append((n, want, got))
        gains[key] = got
    return gains, stuck


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="shape.regain",
        description="Measure each piece's level against the reference and "
                    "write it back into that piece's own gain.")
    ap.add_argument("--capture", default="drums", help="capture definition id")
    ap.add_argument("--corpus", required=True, help="directory holding manifest.json")
    ap.add_argument("--timbre", default="", help="which timbre of the capture")
    ap.add_argument("--notes", default="", help="subset of the capture's notes")
    ap.add_argument("--velocities", default="", help="subset of the capture's velocities")
    ap.add_argument("--lib", default="", help="SONARE_LIB_PATH for model renders")
    ap.add_argument("--cache", default="/tmp/voicematch-shape")
    ap.add_argument("--no-bed", action="store_true",
                    help="skip the recorded-floor subtraction")
    ap.add_argument("--workers", type=int, default=7)
    ap.add_argument("--knobs", default="",
                    help="a saved SONARE_TUNING_DUMP; without one the library "
                         "is asked directly")
    ap.add_argument("--namespaces", default="", help="comma-separated key prefixes")
    ap.add_argument("--overrides", required=True,
                    help="the fitted set whose levels are to be corrected")
    ap.add_argument("--out", default="", help="write the corrected set here")
    ap.add_argument("--config", default="",
                    help="capture definition path for the profile run "
                         "(default: the capture id's own file)")
    ap.add_argument("--held-level", action="store_true",
                    help="correct against the loss's own held level instead of "
                         "the gate's. They disagree on a long piece; this is "
                         "here to reproduce that, not to be used")
    a = ap.parse_args(argv)

    from pathlib import Path

    from . import __main__ as cli
    from .reach import coordinates
    from .render import read_overrides, write_overrides

    cap, corpus, _sigs, loss, notes = cli.build(a)
    ns = tuple(n for n in a.namespaces.split(",") if n)
    base = coordinates(a, cap, corpus, notes, not loss.pitched, ns)
    ov = read_overrides(Path(a.overrides).read_text())
    text = ",".join(f"{k}={v!r}" for k, v in sorted(ov.items()))

    if a.held_level:
        offsets = held_offsets(loss, text, notes)
    else:
        offsets = profile_levels(a.config or f"tools/voicematch/capture/{a.capture}.json",
                                 notes, a.lib, text)
    gains, stuck = corrections(base, ov, offsets)
    for n, off in sorted(offsets.items()):
        key = f"d{n:03d}.gain"
        print(f"note {n:3d}  {off:+6.2f} dB   gain "
              f"{ov.get(key, base.get(key, float('nan'))):.4f} -> "
              f"{gains.get(key, float('nan')):.4f}", file=sys.stderr)
    for n, want, got in stuck:
        print(f"note {n:3d} needs gain {want:.3f}, clamp gives {got:.3f} — "
              f"a gain cannot correct this piece", file=sys.stderr)

    out = write_overrides({**base, **ov, **gains}, base)
    if a.out:
        Path(a.out).write_text(out + "\n")
    else:
        print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
