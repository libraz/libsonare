"""One held note flattened to the readings a profile row does not carry.

A profile row holds one spectrum and one envelope per note. What tells one
instrument from another of its kind is mostly how the spectrum changes over the
note: which partials the strike puts up, how each group falls afterwards, how
much of each moment is not the string at all, and how the unison beats. Every
reading here is a ratio inside one render, so no gain is removed and none is
needed. Windows are seconds after the note's own onset.

`hull` reads these the same way it reads profile rows; `colour_signature` is the
one reading that belongs to the instrument rather than to a note.
"""

from __future__ import annotations

import itertools

import numpy as np

from .partials import BED_CLEAR_DB, band_envelope, fit_inharmonicity, note_hz, partial_hz, series

#: Analysis windows, seconds after the onset: the strike, the body, and three stations of the fall.
WINDOWS = (
    ("50ms", 0.0, 0.05),
    ("0.2s", 0.1, 0.3),
    ("0.75s", 0.5, 1.0),
    ("2s", 1.5, 2.5),
    ("5s", 4.0, 6.0),
)

#: The window every trajectory is read against.
BODY = "0.2s"

#: Partial groups a ladder is pooled into, as inclusive partial numbers.
GROUPS = (("h1", 1, 1), ("h2-3", 2, 3), ("h4-6", 4, 6), ("h7-12", 7, 12), ("h13-20", 13, 20))

#: Octave bands the strike and the body are split into, by lower edge in hertz.
BAND_EDGES_HZ = tuple(44.0 * 2.0**i for i in range(10))

#: Lower edge of the band read under the fundamental; a note needs an octave of it to be read at all.
UNDER_F0_FLOOR_HZ = 30.0

#: Bins a partial must be apart for a window to resolve the ladder at all.
MIN_PARTIAL_BINS = 4.0

#: Span the beat reading detrends over, seconds after the onset.
BEAT_SPAN_S = (0.3, 4.0)

#: How far under its own start a partial's envelope is still read for beating.
BEAT_RANGE_DB = 45.0

#: Partials whose beating is read.
BEAT_PARTIALS = (1, 2, 3)

#: Third-octave centres the fixed-frequency colour is pooled into.
COLOUR_BANDS_HZ = tuple(100.0 * 2.0 ** (i / 3.0) for i in range(20))

#: Windows the off-partial spectrum is read in; later ones reach the recording floor. On three
#: grands the note-median shape agrees to 2-3 dB rms, single cells differ by up to 15 dB at 0.75 s.
VALLEY_WINDOWS = ("50ms", "0.2s", "0.75s")

#: Span the off-partial spectrum is normalised over, in hertz.
VALLEY_SPAN_HZ = (100.0, 10000.0)

#: Partials masked out of the off-partial spectrum; past the resolvable ones the mask fuses.
VALLEY_MASK_PARTIALS = 64

#: Notes a colour band needs before its median is the instrument rather than a note.
COLOUR_MIN_NOTES = 3


def _db(power: float) -> float:
    return 10.0 * float(np.log10(max(power, 1e-30)))


def _measured_f0(sig: np.ndarray, sr: int, note: int, onset: int) -> float:
    """The fundamental the note actually has, within a semitone of its nominal pitch."""
    seg = sig[onset + int(0.1 * sr) : onset + int(1.1 * sr)]
    nominal = note_hz(note)
    if len(seg) < sr // 2:
        return nominal
    sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), 4 * len(seg)))
    fr = np.fft.rfftfreq(4 * len(seg), 1.0 / sr)
    m = (fr > nominal * 2.0 ** (-1 / 12)) & (fr < nominal * 2.0 ** (1 / 12))
    return float(fr[m][int(np.argmax(sp[m]))]) if m.any() else nominal


def _window_spectrum(sig: np.ndarray, sr: int, onset: int, lo: float, hi: float):
    seg = sig[onset + int(lo * sr) : onset + int(hi * sr)]
    if len(seg) < int(0.8 * (hi - lo) * sr):
        return None, None
    win = np.hanning(len(seg))
    # Scaled by the window's own sum, so a steady partial reads the same power in any window length.
    return np.fft.rfftfreq(len(seg), 1.0 / sr), (np.abs(np.fft.rfft(seg * win)) / win.sum()) ** 2


def _partial_powers(fr, power, ks, f0: float) -> dict[int, float]:
    """Power of each partial that stands clear of the valley above it; unresolved ladders give none."""
    bin_hz = float(fr[1] - fr[0])
    if f0 / bin_hz < MIN_PARTIAL_BINS:
        return {}
    out: dict[int, float] = {}
    for (k, f), (_, f_next) in zip(ks, ks[1:] + [(0, ks[-1][1] + f0)]):
        half = min(max(2.0 * bin_hz, f * (2.0 ** (1 / 24) - 1.0)), 0.25 * f0)
        on = float(power[(fr >= f - half) & (fr <= f + half)].sum())
        mid = 0.5 * (f + f_next)
        valley = float(power[(fr >= mid - half) & (fr <= mid + half)].sum())
        if on > valley * 10.0 ** (BED_CLEAR_DB / 10.0):
            out[k] = on
    return out


def _valley_shape(fr, power, f0: float, B: float) -> dict[float, float]:
    """Third-octave shape of what lies between the partials, against its own mean level."""
    bin_hz = float(fr[1] - fr[0])
    off = (fr >= VALLEY_SPAN_HZ[0]) & (fr <= VALLEY_SPAN_HZ[1])
    for _, f in series(f0, B, k_max=VALLEY_MASK_PARTIALS):
        half = max(2.0 * bin_hz, f * (2.0 ** (1 / 24) - 1.0))
        off &= ~((fr >= f - half) & (fr <= f + half))
    if off.sum() < 3:
        return {}
    mean = float(power[off].mean())
    out: dict[float, float] = {}
    for centre in COLOUR_BANDS_HZ:
        m = off & (fr >= centre * 2.0 ** (-1 / 6)) & (fr < centre * 2.0 ** (1 / 6))
        if m.sum() >= 3 and mean > 0.0:
            out[centre] = _db(float(power[m].mean()) / mean)
    return out


def _beat_depth_db(sig: np.ndarray, sr: int, onset: int, f: float, bw: float) -> float | None:
    """RMS of a partial's envelope about its own smooth fall, in dB: how much the unison beats."""
    env = band_envelope(sig, f, bw, sr)[
        onset + int(BEAT_SPAN_S[0] * sr) : onset + int(BEAT_SPAN_S[1] * sr)
    ]
    env = env[:: max(1, sr // 200)]
    if len(env) < 100:
        return None
    db = 20.0 * np.log10(np.maximum(env, 1e-14))
    live = np.nonzero(db >= db[0] - BEAT_RANGE_DB)[0]
    db = db[: int(live[-1]) + 1]
    if len(db) < 100:
        return None
    t = np.arange(len(db))
    return float(np.std(db - np.polyval(np.polyfit(t, db, 3), t)))


def fingerprint(sig: np.ndarray, sr: int, note: int, onset_s: float):
    """Features of one held note, and its partials' departures from their own smooth ladder.

    @return (features, points): `points` is (hertz, dB) per body-window partial,
            each against a quadratic in log partial number fitted to that note —
            the input @ref colour_signature pools by absolute frequency.
    """
    onset = int(onset_s * sr)
    f0 = _measured_f0(sig, sr, note, onset)
    B = fit_inharmonicity(sig[onset:], f0, sr)
    ks = series(f0, B, k_max=GROUPS[-1][2])
    feats: dict[str, float] = {}
    group_db: dict[str, dict[str, float]] = {}
    total_db: dict[str, float] = {}
    points: list[tuple[float, float]] = []
    for name, lo, hi in WINDOWS:
        fr, power = _window_spectrum(sig, sr, onset, lo, hi)
        if fr is None:
            continue
        audible = (fr >= 30.0) & (fr <= 16000.0)
        whole = float(power[audible].sum())
        if whole <= 0.0:
            continue
        total_db[name] = _db(whole)
        if name in ("50ms", BODY):
            for a, b in itertools.pairwise(BAND_EDGES_HZ):
                feats[f"band{round(a * 2**0.5)}@{name}"] = _db(
                    float(power[(fr >= a) & (fr < b)].sum()) / whole
                )
        if 0.7 * f0 >= 2.0 * UNDER_F0_FLOOR_HZ:
            under = (fr >= UNDER_F0_FLOOR_HZ) & (fr < 0.7 * f0)
            feats[f"under_f0@{name}"] = _db(float(power[under].sum()) / whole)
        if name in VALLEY_WINDOWS:
            for centre, level in _valley_shape(fr, power, f0, B).items():
                feats[f"valley{round(centre)}@{name}"] = level
        parts = _partial_powers(fr, power, ks, f0)
        if not parts:
            continue
        on = sum(parts.values())
        top = partial_hz(f0, B, ks[-1][0]) + 0.5 * f0
        between = float(power[(fr >= 0.7 * f0) & (fr <= top)].sum()) - on
        feats[f"off_partial@{name}"] = _db(max(between, 0.0) / whole)
        group_db[name] = {}
        for g, k_lo, k_hi in GROUPS:
            p = sum(v for k, v in parts.items() if k_lo <= k <= k_hi)
            if p > 0.0:
                group_db[name][g] = _db(p)
                feats[f"{g}_share@{name}"] = _db(p / on)
        if name == BODY and len(parts) >= 6:
            k = np.log(np.array(sorted(parts), float))
            level = np.array([_db(parts[i] / on) for i in sorted(parts)])
            smooth = np.polyval(np.polyfit(k, level, 2), k)
            points = [
                (partial_hz(f0, B, i), float(d)) for i, d in zip(sorted(parts), level - smooth)
            ]
    for name, total in total_db.items():
        if name != BODY and BODY in total_db:
            feats[f"level@{name}"] = total - total_db[BODY]
        for g, v in group_db.get(name, {}).items():
            if name != BODY and g in group_db.get(BODY, {}):
                feats[f"{g}@{name}"] = v - group_db[BODY][g]
    bw = max(8.0, 0.3 * f0)
    for k in BEAT_PARTIALS:
        f = partial_hz(f0, B, k)
        if f < 15500.0:
            depth = _beat_depth_db(sig, sr, onset, f, bw)
            if depth is not None:
                feats[f"beat_h{k}_db"] = depth
    return feats, points


def colour_signature(points_by_note: dict[int, list[tuple[float, float]]]) -> dict[float, float]:
    """What the instrument does to a partial by where it lands in hertz, whatever note sent it.

    Each note's ladder has had its own smooth trend removed, so a strike-position
    notch stays with the partial number and averages out across notes while a
    resonance of the body stays at its frequency and does not.
    """
    out: dict[float, float] = {}
    for centre in COLOUR_BANDS_HZ:
        lo, hi = centre * 2.0 ** (-1 / 6), centre * 2.0 ** (1 / 6)
        per_note = [
            float(np.median([d for f, d in pts if lo <= f < hi]))
            for pts in points_by_note.values()
            if any(lo <= f < hi for f, _ in pts)
        ]
        if len(per_note) >= COLOUR_MIN_NOTES:
            out[centre] = float(np.median(per_note))
    return out


def signature_agreement(a: dict[float, float], b: dict[float, float]) -> float | None:
    """Correlation of two colour signatures over the bands both hold."""
    shared = sorted(set(a) & set(b))
    if len(shared) < 5:
        return None
    x, y = np.array([a[c] for c in shared]), np.array([b[c] for c in shared])
    if x.std() == 0.0 or y.std() == 0.0:
        return None
    return float(np.corrcoef(x, y)[0, 1])
