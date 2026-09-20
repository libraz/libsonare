"""One hit against another: the deltas a kit is judged on, and the references' own spread."""

from __future__ import annotations

import numpy as np
from loss import KIT_MIN_MEMBERS, kit_report
from metrics import band_tilt_db


def band_shape_error_db(model: list[float] | None, ref: list[float] | None) -> float | None:
    """RMS distance between two normalised band profiles.

    A magnitude and never a direction, which is the point: the tilt above says
    which way a hit is wrong and this says how much of the error the tilt failed
    to account for. A piece can match on tilt and still be a different
    instrument, with a resonance in the wrong band and a hole beside it.
    """
    if not model or not ref:
        return None
    n = min(len(model), len(ref))
    if not n:
        return None
    diff = np.asarray(model[:n], dtype=np.float64) - np.asarray(ref[:n], dtype=np.float64)
    return float(np.sqrt(np.mean(diff**2)))


def mean_band_decay_delta(model: list, ref: list) -> float | None:
    """Model-minus-reference decay rate, averaged over the octaves both resolved.

    A band that decayed in neither render, or in only one of them, is left out
    rather than counted as agreement: `analyze_hit` reports `None` there, and
    a hit with no energy in a band has no decay rate to be right about.
    """
    pairs = [(m, r) for m, r in zip(model or [], ref or [])
             if m is not None and r is not None]
    return float(np.mean([m - r for m, r in pairs])) if pairs else None


def ring_doublings(model: dict, ref: dict) -> float | None:
    """How much longer the model rings than the reference, in doublings.

    In doublings rather than in milliseconds or percent, because the kit spans
    24x on this quantity — 60 ms of woodblock against 1428 of cymbal — so a
    median taken in milliseconds is the cymbals and a median taken in percent
    prices a doubling at +100 and a halving at -50.

    Refused where either side hit its analysis ceiling: a capped reading is the
    window and not the instrument, the same way a capped damper release is.
    """
    m, r = model.get("decay_ms"), ref.get("decay_ms")
    if not m or not r or m <= 0.0 or r <= 0.0:
        return None
    if model.get("decay_capped") or ref.get("decay_capped"):
        return None
    return float(np.log2(m / r))


def ring_collapsed(model: dict, ref: dict) -> str:
    """Which side reported no ring at all, where both were asked for one.

    `ring_doublings` refuses a zero because no ratio of it exists, and a refusal
    reduced with a skip is how a hit that stopped ringing leaves the median
    instead of failing it — the worst member of a family stops being counted and
    the family reads as improved. A caller has to be able to tell that apart
    from a row the measurement never offered, which is what the `None` guard
    above cannot say on its own.

    Empty where either side carries no reading, since that is a profile without
    the field rather than a voice without a ring.
    """
    m, r = model.get("decay_ms"), ref.get("decay_ms")
    if m is None or r is None:
        return ""
    return "+".join(side for side, v in (("model", m), ("reference", r)) if v <= 0.0)


def band_decay_reach(model: dict, ref: dict) -> tuple[int, int]:
    """Octaves the reference resolved a rate in, and how many the model did too.

    `mean_band_decay_delta` averages the octaves both sides resolved and says
    nothing about how many that was, so a model that stopped resolving the top
    of its spectrum contributes a number drawn from the bottom of it and looks
    like any other row. The count is what makes that visible.

    Not charged, which the loss's `bdecay` does do to the same case and for a
    reason that does not hold here: that term faces a search, so an unresolved
    band has to cost something or a candidate buys a discount by rendering a
    shorter hit. This one faces a fixed model, and the references say partial
    non-resolution is ordinary — kit-a resolves 1667 octave-cells and kit-b is
    silent on 100 of them, kit-b resolves 1804 and kit-a is silent on 237 — so
    a charge levied here would price the capture. The failure the loss's comment
    describes, a hit four times too short, is `ring`'s to report.
    """
    ref_cells = [r for r in (ref.get("band_decay_db_s") or []) if r is not None]
    both = [1 for m, r in zip(model.get("band_decay_db_s") or [],
                              ref.get("band_decay_db_s") or [])
            if m is not None and r is not None]
    return len(both), len(ref_cells)


def print_kit_relations(kit_rows: list[tuple[dict, dict]],
                        groups: dict[str, tuple[int, ...]]) -> None:
    """What the kit's own families do, against what the reference's do.

    Every column of the table above is one instrument against its own reference
    row. A kit is not 47 independent instruments, and the relation between its
    members — the tom series, the hi-hat trio — is where a kit stops sounding
    like a kit long before any single row looks wrong.

    Printed rather than gated, because the gate is a per-dimension median across
    every hit and these are per family: folding them in would average a six-tom
    finding into 282 rows and lose it. `--w-kit` is where the same measurement
    drives a search.
    """
    if not groups or not kit_rows:
        return
    rows = kit_report([m for m, _ in kit_rows], [r for _, r in kit_rows], groups)
    if not rows:
        print("\n  no kit relation could be read: no family the capture declares has "
              f"{KIT_MIN_MEMBERS} members in this run's note filter.")
        return
    print(f"\n{'family':>12} {'relation':>9} | {'reference':>10} {'model':>8} "
          f"{'charge':>8} {'members':>8}")
    print("-" * 62)
    for row in rows:
        print(f"{row['family']:>12} {row['relation']:>9} | {row['spread']:10.2f} "
              f"{row['model_spread']:8.2f} {row['charge']:8.2f} {row['members']:8d}")
    print("\n  In doublings — a factor of two in frequency, in milliseconds or in "
          "amplitude.\n  `reference` and `model` are how far the family's members "
          "spread apart on each\n  side; a model spread well under the reference's is a "
          "family collapsed towards\n  one instrument. A relation the reference does not "
          "itself hold is not listed:\n  which relations a family has is read off the "
          "rows, never declared.")


def percussion_row_deltas(m: dict, r: dict) -> dict[str, float | None]:
    """One hit against another, on the dimensions a kit is judged on.

    Factored out so the model-against-reference comparison and the
    reference-against-reference spread run the identical arithmetic. A spread
    computed any other way would be a different ruler, and the ratio between
    them is the whole point of having one.
    """
    tilt_m, tilt_r = band_tilt_db(m.get("bands_db")), band_tilt_db(r.get("bands_db"))
    return {
        "band_tilt": None if tilt_m is None or tilt_r is None else tilt_m - tilt_r,
        "band_shape": band_shape_error_db(m.get("bands_db"), r.get("bands_db")),
        "band_decay": mean_band_decay_delta(m.get("band_decay_db_s"),
                                            r.get("band_decay_db_s")),
        "attack": m["attack_ms"] - r["attack_ms"],
        "crest": m["crest_db"] - r["crest_db"],
        "centroid_pct": (100.0 * (m["centroid_hz"] / r["centroid_hz"] - 1.0)
                         if r.get("centroid_hz") else None),
        # How long the hit rings, which is the whole of dry against wet and is
        # the one gestural dimension `band_decay` cannot stand in for: that one
        # averages the octaves both sides resolved, so a hit that ends early
        # loses those octaves from its own average instead of being charged.
        "ring": ring_doublings(m, r),
        # How much of the hit stands in peaks, which is metal against filtered
        # noise. Absent on either side where the window was too short to
        # transform, which is not a flat spectrum.
        "tonality": (None if m.get("flatness_db") is None or r.get("flatness_db") is None
                     else m["flatness_db"] - r["flatness_db"]),
        # The image. 38 of the kit's drum notes carry a `stereo_spread` and
        # nothing faced it until this column existed.
        "stereo": (None if m.get("stereo_width") is None or r.get("stereo_width") is None
                   else m["stereo_width"] - r["stereo_width"]),
        # How loud the hit actually is. Every other column here is normalised —
        # a band profile against its own loudest band, a crest against its own
        # RMS, a decay against its own peak — which is what makes them measure
        # timbre, and which also makes all of them blind to gain. Rewriting
        # eighteen of the kit's output levels moved not one of them by a digit.
        # `vel_range` is a span and cancels an offset by construction, so it is
        # not this either.
        "level": (m["peak_dbfs"] - r["peak_dbfs"]
                  if m.get("peak_dbfs") is not None
                  and r.get("peak_dbfs") is not None else None),
    }


def percussion_reference_spread(profile: dict,
                                dimensions: list[str] | None = None) -> dict[str, float]:
    """How far a kit's references sit from EACH OTHER, dimension by dimension.

    The percussion counterpart of `reference_spread`, which computes the pitched
    dimensions only — stretch, inharmonicity, a partial stack — none of which a
    kit has. Without this a percussion gate carries no `reference_spread` at
    all, so `status.coverage`'s agreement step has nothing to judge against and
    every dimension reads unjudgeable however many kits were captured. Capturing
    a second reference is necessary for a kit to be scored and was never
    sufficient on its own.

    Runs `percussion_row_deltas` over the (note, velocity) keys two references
    share, pooled over every pair of them, taking the median absolute value —
    the same reduction the model's own error goes through.
    """
    rows = profile.get("rows", [])
    timbres = sorted({r["timbre"] for r in rows})
    if len(timbres) < 2:
        return {}
    by_key: dict[str, dict[tuple[int, int], dict]] = {}
    for r in rows:
        by_key.setdefault(r["timbre"], {})[(r["note"], r["velocity"])] = r

    pooled: dict[str, list[float]] = {}
    for i, a in enumerate(timbres):
        for b in timbres[i + 1:]:
            peaks: dict[str, dict[int, dict[int, float]]] = {}
            for key in sorted(set(by_key[a]) & set(by_key[b])):
                x, y = by_key[a][key], by_key[b][key]
                for k, v in percussion_row_deltas(x, y).items():
                    if v is not None and np.isfinite(v):
                        pooled.setdefault(k, []).append(abs(float(v)))
                for side, src in ((a, x), (b, y)):
                    if src.get("peak_dbfs") is not None:
                        peaks.setdefault(side, {}).setdefault(key[0], {})[key[1]] = \
                            src["peak_dbfs"]
            for note, pa in sorted(peaks.get(a, {}).items()):
                pb = peaks.get(b, {}).get(note, {})
                both = sorted(set(pa) & set(pb))
                if len(both) >= 2:
                    pooled.setdefault("vel_range", []).append(abs(
                        (max(pa[v] for v in both) - min(pa[v] for v in both))
                        - (max(pb[v] for v in both) - min(pb[v] for v in both))))
    spread = {k: float(np.median(v)) for k, v in pooled.items() if v}
    return {k: v for k, v in spread.items() if not dimensions or k in dimensions}
