"""Measures the captured harpsichord oracle corpus into engine design targets.

This reads the per-note WAVs `capture.py` wrote and reports, per timbre and per
note, the quantities a physical model has to reproduce: the free-decay slope
under the held key, the spectral centroid, the tone-to-noise ratio, the partial
series (and so the inharmonicity), the plucking-point comb notch, and the
velocity response. It deliberately reports the oracle on its own terms rather
than as a diff against the current model — a target has to exist before a delta
to it means anything.

Run from the repo root through rye, e.g.

    rye run --pyproject bindings/python/pyproject.toml \
        python tools/voicematch/analyze_harpsichord_corpus.py --timbre gm007
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import wavio  # noqa: E402

CACHE = pathlib.Path(".cache/voicematch/capture/harpsichord")
# capture.py leaves this much lead-in before note-on, then holds the key this
# long. Both come from the capture definition, not from the audio.
PREROLL_S = 0.1
GATE_S = 4.0


def note_hz(note: int) -> float:
    return 440.0 * (2.0 ** ((note - 69) / 12.0))


def note_name(note: int) -> str:
    names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    return f"{names[note % 12]}{note // 12 - 1}"


def load(path: pathlib.Path) -> tuple[np.ndarray, int]:
    data, sr = wavio.read_wav(str(path))
    mono = data.mean(axis=1) if data.ndim > 1 else data
    return mono.astype(np.float64), sr


def envelope_db(x: np.ndarray, sr: int, hop_ms: float = 10.0) -> tuple[np.ndarray, np.ndarray]:
    """RMS envelope in dB on a fixed hop, with its time axis in seconds."""
    hop = max(1, int(sr * hop_ms / 1000.0))
    win = hop * 4
    n = (len(x) - win) // hop
    if n <= 0:
        return np.zeros(0), np.zeros(0)
    frames = np.lib.stride_tricks.sliding_window_view(x, win)[:: hop][:n]
    rms = np.sqrt((frames**2).mean(axis=1) + 1e-30)
    return np.arange(n) * hop / sr, 20.0 * np.log10(rms)


def decay_slope_db_s(x: np.ndarray, sr: int, start_s: float, end_s: float) -> float:
    """Least-squares dB/s over a window of the held-key free decay."""
    t, db = envelope_db(x, sr)
    if t.size == 0:
        return float("nan")
    sel = (t >= start_s) & (t <= end_s)
    if sel.sum() < 4:
        return float("nan")
    slope, _ = np.polyfit(t[sel], db[sel], 1)
    return float(slope)


def spectrum(x: np.ndarray, sr: int, start_s: float, dur_s: float) -> tuple[np.ndarray, np.ndarray]:
    a = int(start_s * sr)
    b = min(len(x), a + int(dur_s * sr))
    seg = x[a:b]
    if seg.size < 1024:
        return np.zeros(0), np.zeros(0)
    n = 1 << int(math.ceil(math.log2(seg.size)))
    win = np.hanning(seg.size)
    mag = np.abs(np.fft.rfft(seg * win, n))
    freq = np.fft.rfftfreq(n, 1.0 / sr)
    return freq, mag


def centroid_hz(freq: np.ndarray, mag: np.ndarray, lo: float = 40.0, hi: float = 12000.0) -> float:
    sel = (freq >= lo) & (freq <= hi)
    m = mag[sel]
    f = freq[sel]
    total = m.sum()
    return float((f * m).sum() / total) if total > 0 else float("nan")


def partials(freq: np.ndarray, mag: np.ndarray, f0: float, count: int = 12) -> list[dict]:
    """Peak-picks each partial in a window around n*f0, so a stretched (sharp)
    partial is found where it actually is rather than where a harmonic series
    would put it. The measured/ideal ratio is the inharmonicity evidence."""
    out = []
    for n in range(1, count + 1):
        target = n * f0
        if target > freq[-1] * 0.95:
            break
        # A window wide enough for a stiff string's stretch but too narrow to
        # capture the neighbouring partial.
        half = min(0.35 * f0, 0.06 * target + 3.0)
        sel = (freq >= target - half) & (freq <= target + half)
        if sel.sum() < 3:
            break
        k = int(np.argmax(mag[sel]))
        f_meas = float(freq[sel][k])
        out.append(
            {
                "n": n,
                "hz": round(f_meas, 2),
                "ideal_hz": round(target, 2),
                "cents": round(1200.0 * math.log2(f_meas / target), 1) if target > 0 else 0.0,
                "db": round(20.0 * math.log10(float(mag[sel][k]) + 1e-30), 2),
            }
        )
    return out


def tone_to_noise_db(freq: np.ndarray, mag: np.ndarray, f0: float) -> float:
    """Energy in the first 16 partials against everything else.

    The partial window is the wider of +/-2% and three FFT bins. A relative-only
    window collapses below one bin at a low fundamental — at 44 Hz it asks for
    +/-0.9 Hz out of a 1.7 Hz grid — so a bass note would score as noise for a
    reason that is entirely the analysis window's."""
    power = mag**2
    bin_hz = float(freq[1] - freq[0]) if freq.size > 1 else 1.0
    tonal = np.zeros(freq.shape, dtype=bool)
    for n in range(1, 17):
        target = n * f0
        if target > freq[-1]:
            break
        half = max(0.02 * target, 3.0 * bin_hz)
        tonal |= (freq >= target - half) & (freq <= target + half)
    band = (freq >= 40.0) & (freq <= 12000.0)
    t = power[tonal & band].sum()
    nz = power[~tonal & band].sum()
    return float(10.0 * math.log10((t + 1e-30) / (nz + 1e-30)))


def analyze_note(path: pathlib.Path, note: int) -> dict:
    x, sr = load(path)
    f0 = note_hz(note)
    peak = float(np.abs(x).max())
    # The free decay under the held key, skipping the attack transient and
    # stopping short of note-off.
    early = decay_slope_db_s(x, sr, PREROLL_S + 0.25, PREROLL_S + 1.25)
    late = decay_slope_db_s(x, sr, PREROLL_S + 1.5, PREROLL_S + GATE_S - 0.3)
    freq, mag = spectrum(x, sr, PREROLL_S + 0.3, 0.6)
    # After note-off: how fast the damper actually stops the string.
    off = decay_slope_db_s(x, sr, PREROLL_S + GATE_S + 0.03, PREROLL_S + GATE_S + 0.35)
    return {
        "note": note,
        "name": note_name(note),
        "f0": round(f0, 2),
        "peak": round(peak, 5),
        "peak_db": round(20.0 * math.log10(peak + 1e-30), 2),
        "decay_early_db_s": round(early, 2),
        "decay_late_db_s": round(late, 2),
        "damper_db_s": round(off, 2),
        "centroid_hz": round(centroid_hz(freq, mag), 1),
        "tnr_db": round(tone_to_noise_db(freq, mag, f0), 2),
        "partials": partials(freq, mag, f0),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--timbre", default="gm007")
    ap.add_argument("--velocity", type=int, default=88)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    root = CACHE / args.timbre
    if not root.is_dir():
        print(f"no captures under {root}", file=sys.stderr)
        return 1

    notes = sorted(
        {int(p.name[1:4]) for p in root.glob("n???_v???.wav")}
    )
    rows = []
    for note in notes:
        path = root / f"n{note:03d}_v{args.velocity:03d}.wav"
        if not path.is_file():
            continue
        rows.append(analyze_note(path, note))

    # Velocity response: the defining trait, so it is measured across the whole
    # captured axis rather than sampled.
    vel_rows = []
    for note in notes:
        entry = {"note": note, "name": note_name(note), "peaks": {}}
        for vel in sorted({int(p.name[6:9]) for p in root.glob(f"n{note:03d}_v???.wav")}):
            path = root / f"n{note:03d}_v{vel:03d}.wav"
            x, _ = load(path)
            entry["peaks"][vel] = round(float(np.abs(x).max()), 5)
        if len(entry["peaks"]) >= 2:
            vals = list(entry["peaks"].values())
            entry["range_db"] = round(
                20.0 * math.log10(max(vals) / max(min(vals), 1e-9)), 2
            )
            entry["monotonic"] = vals == sorted(vals)
        vel_rows.append(entry)

    if args.json:
        print(json.dumps({"timbre": args.timbre, "notes": rows, "velocity": vel_rows}, indent=2))
        return 0

    print(f"=== {args.timbre} @ velocity {args.velocity} ===")
    print(
        f"{'note':>6} {'f0':>8} {'peak dB':>8} {'early':>8} {'late':>8} "
        f"{'damper':>8} {'centroid':>9} {'TNR':>7}"
    )
    for r in rows:
        print(
            f"{r['name']:>6} {r['f0']:>8.1f} {r['peak_db']:>8.2f} "
            f"{r['decay_early_db_s']:>8.2f} {r['decay_late_db_s']:>8.2f} "
            f"{r['damper_db_s']:>8.1f} {r['centroid_hz']:>9.1f} {r['tnr_db']:>7.2f}"
        )

    print("\n=== inharmonicity (cents sharp of n*f0) ===")
    for r in rows:
        cents = " ".join(f"{p['n']}:{p['cents']:+.0f}" for p in r["partials"][:8])
        print(f"{r['name']:>6}  {cents}")

    print("\n=== partial levels (dB, relative to the strongest) ===")
    for r in rows:
        if not r["partials"]:
            continue
        top = max(p["db"] for p in r["partials"])
        rel = " ".join(f"{p['n']}:{p['db'] - top:+.0f}" for p in r["partials"][:10])
        print(f"{r['name']:>6}  {rel}")

    print("\n=== velocity response (peak, and the range across the axis) ===")
    for r in vel_rows:
        peaks = " ".join(f"v{v}:{p:.4f}" for v, p in r["peaks"].items())
        tail = ""
        if "range_db" in r:
            tail = f"   range {r['range_db']:+.2f} dB" + ("" if r["monotonic"] else "  NON-MONOTONIC")
        print(f"{r['name']:>6}  {peaks}{tail}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
