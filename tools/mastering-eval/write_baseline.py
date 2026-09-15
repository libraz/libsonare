#!/usr/bin/env python3
"""Turn a run ledger into the committed baseline, as strict JSON.

``run.py --emit-baseline`` writes the right numbers in the right shape, but it
writes them with Python's ``json`` defaults, which emit bare ``NaN`` for a
metric that came back undefined. ``NaN`` is not in the JSON grammar: Python
reads it back, a strict parser rejects the file, and ``baseline.json`` is
committed and meant to be readable from other languages. This module is the
one place that gap is closed.

**An undefined value is not a null.** The ledger already distinguishes three
states -- a number, a metric that does not apply to the row
(``inapplicable_metrics``), and a metric with no implementation
(``unavailable_metrics``). A ``NaN`` is a fourth thing only in appearance:
every one in this ledger is a short-term loudness spread on an item shorter
than the 3.1 s window, which is the same statement as "does not apply to this
row" and carries the same consequence for a comparison, namely that the row
decides nothing about that metric. So it is folded into ``inapplicable_metrics``
with its reason, and the key is dropped from ``metrics`` rather than filled with
a null -- the contract's own wording for the STOI case, "the absence is recorded
rather than filled with a number".

A pair with only one undefined side would be a different statement, so that case
keeps the pair and nulls the one side; nothing in the corpus produces it today.

The same rule governs provenance. ``meta.head`` is this file's own emission, not
the run's; each run's tree and binary are copied from its ledger under
``numbers_provenance`` and ``gate_provenance``. A ledger that recorded none reads
``recorded: false`` with the reason, because a dirty tree is invisible afterwards
and an absent record must not be read as a clean one.

Run from ``bindings/python`` so rye resolves numpy and libsonare::

    SONARE_LIB_PATH=.../libsonare.dylib \\
        rye run python ../../tools/mastering-eval/run.py \\
            --out ../../tools/mastering-eval/runs/w2-baseline-run.json \\
            --emit-baseline /tmp/shape.json
    rye run python ../../tools/mastering-eval/write_baseline.py \\
        --shape /tmp/shape.json \\
        --ledger ../../tools/mastering-eval/runs/w2-baseline-run.json \\
        --non-vacuity ../../tools/mastering-eval/runs/nonvacuity.json
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from nonvacuity import corpus_digest, corpus_paths, corpus_tree_digest  # noqa: E402

SCHEMA = 1

SHORT_TERM_WINDOW_SECONDS = 3.1

NOT_DEFINED = {
    "short_term_spread": (
        f"item is under the {SHORT_TERM_WINDOW_SECONDS} s short-term window, "
        "so no block survives and the spread is not defined"
    ),
}
NOT_DEFINED_FALLBACK = "the metric returned an undefined value on this row"

# What a ledger did not record cannot be supplied afterwards. The working tree a
# run measured on is not in the repository's history, so a run that did not write
# its own provenance leaves it unknown permanently -- and unknown is the one thing
# a reader must not take for clean. `recorded: false` is the field that says so;
# the nulls beside it are there to stop a key lookup failing, not to answer.
NOT_RECORDED = {
    "recorded": False,
    "head": None,
    "head_committed": None,
    "status_src": None,
    "status_all": None,
    "package": None,
    "sonare_lib_path": None,
    "sonare_lib_mtime": None,
    "why": (
        "the ledger this was read from carries no provenance block, so the tree and the "
        "binary these numbers were measured on were not recorded. Unknown, not clean, and "
        "not fillable later: that tree state is not in the repository's history"
    ),
}


def recorded_provenance(ledger: dict) -> dict:
    """A ledger's own provenance, or the block that says it did not write one."""
    block = ledger.get("provenance")
    if not block:
        return dict(NOT_RECORDED)
    return dict(block, recorded=True)


# The improvement thresholds, and the rule where there is no threshold to have.
#
# A regression needs none: the corpus is fixed and the computation deterministic,
# so any worsening is real however small. An improvement needs one, and it is not
# a measurement floor -- it answers how much of a measured gain belongs to the
# processor rather than to the one signal it was measured on. Where the defect
# has a random realization that is measurable: hold the processing fixed, redraw
# the defect, read how far the metric wanders. Every number below is the largest
# such wander over the corpus ensembles, from the `ensembles` section of the
# non-vacuity run.
#
# The mastering items have no random realization -- they are deterministic
# syntheses and redrawing them is not defined -- so their variability lives
# across kinds of programme material, which more draws of one item cannot reach.
# They get the consistency rule instead of a number.
#
# `level_matching` is the three-way split a single rule would get wrong:
#
#   not_applicable          the metric is level-independent, so the raw and the
#                           level-matched delta are the same number
#   required_for_improvement  segmental SNR is level-sensitive by construction, so
#                           an improvement claimed on the raw delta can be a gain
#                           change; a regression still reads the raw delta, because
#                           a level change IS a loss of fidelity to the reference
#   forbidden               integrated loudness and true peak measure level itself.
#                           Level-matching them makes them identically zero
LEVEL_INDEPENDENT = "not_applicable"
LEVEL_MATCH_IMPROVEMENT = "required_for_improvement"
LEVEL_MATCH_FORBIDDEN = "forbidden"


def _threshold(
    value: float | None,
    unit: str,
    level_matching: str,
    why: str,
    **extra: Any,
) -> dict[str, Any]:
    return {
        "improvement": value,
        "unit": unit,
        "level_matching": level_matching,
        "improvement_read_from": (
            "delta_level_matched" if level_matching == LEVEL_MATCH_IMPROVEMENT else "delta"
        ),
        "regression_read_from": "delta",
        "why": why,
        **extra,
    }


THRESHOLDS: dict[str, Any] = {
    "regression": {
        "rule": "any_worsening_fails",
        "value": None,
        "why": (
            "the corpus is fixed and the computation deterministic, so the same input measured "
            "twice gives the same number bit for bit; any worsening is real however small"
        ),
    },
    "restoration": {
        "basis": "redraw_floor",
        "basis_source": (
            "runs/nonvacuity.json#ensembles (5 draws per item, processing held fixed). The same "
            "run supplies the steps table, so a floor and the response read against it come from "
            "one corpus; a gate ledger carrying only one of the two leaves resolution unreadable"
        ),
        "seg_snr_db": _threshold(
            1.0,
            "dB",
            LEVEL_MATCH_IMPROVEMENT,
            "redraw floor 0.937 dB. Scaling an output by 0.5 with the processing untouched moved "
            "it 6.108 dB while every level-independent metric stayed at exactly 0, and the "
            "gain_floor degradation's raw +2.623 dB is +0.0199 dB once the level is taken out -- "
            "a factor of 132. An improvement on the raw delta can therefore be a gain change",
            skip_when="saturation.seg_snr_db.ceiling_fraction >= 0.9",
            skip_why=(
                "the clip has taken the row's room to move upward, so it cannot evidence an "
                "improvement. It still detects a regression -- a fully saturated row "
                "(ceiling_fraction 1.000, saturated true) moved 35.000 -> 30.694 dB for a "
                "degraded declick -- which is why stopping only one direction is sound"
            ),
        ),
        "log_kurtosis_ratio": _threshold(
            0.05,
            "ratio",
            LEVEL_INDEPENDENT,
            "redraw floor 0.0494, and the only metric whose response to a one-step knob change "
            "clears its own floor on this material; see resolution.one_step_knob_response",
        ),
        "stoi": _threshold(
            0.05,
            "correlation",
            LEVEL_INDEPENDENT,
            "redraw floor 0.0418; see resolution.stoi for what this line puts out of reach",
        ),
        "log_spectral_distance": _threshold(
            1.1,
            "dB",
            LEVEL_INDEPENDENT,
            "redraw floor 1.080 dB",
        ),
        "band_energy_delta": _threshold(
            6.5,
            "dB",
            LEVEL_INDEPENDENT,
            "redraw floor 6.538 dB, the widest of the 32 bands over the ensembles. This is a "
            "noise bed's floor and does not carry to a mastering row, which falls under the "
            "mastering rule instead",
        ),
        "integrated_lufs": _threshold(
            0.9,
            "LU",
            LEVEL_MATCH_FORBIDDEN,
            "redraw floor 0.902 LU on restoration material. Never level-matched: this metric is "
            "the level, and matching it away leaves identically zero",
        ),
        "true_peak_dbtp": _threshold(
            2.4,
            "dB",
            LEVEL_MATCH_FORBIDDEN,
            "redraw floor 2.334 dB on restoration material. Never level-matched, for the same "
            "reason as integrated loudness",
        ),
        "short_term_spread": _threshold(
            None,
            "LU",
            LEVEL_INDEPENDENT,
            "no corpus item carries both an ensemble and more than the 3.1 s short-term window, "
            "so this metric has no redraw floor at any size; it falls under the mastering rule",
        ),
    },
    "mastering": {
        "rule": "consistent_sign_across_items",
        "values": None,
        "why": (
            "the mastering items are deterministic syntheses with no random realization, so a "
            "redraw floor is not defined for them and the restoration floors are a different "
            "material's. Their variability lives across kinds of programme material rather than "
            "across draws of one item. An improvement is therefore claimed only when it holds "
            "with the same sign on every mastering item in the corpus"
        ),
    },
}


# Which ensemble supplies the redraw floor for which stepped knob. A floor is
# only a floor for the processing it was measured under, so the pairing is by
# denoiser mode: the ensemble's baseline and the step's baseline must be the
# same configuration or the ratio compares two different things.
STEP_FLOOR_SOURCE = {
    "gain_floor": "D3s",
    "noise_estimation_quantile": "D3s",
    "over_subtraction": "D2s",
    "spectral_floor": "D2s",
}
STEP_MATERIAL = "speech_pink_noise"

# Above this the knob is resolved; between 1 and this it cleared the floor
# without room to spare, which is a different claim and is reported as such.
MARGINAL_RATIO = 2.0


def _worst(delta: Any) -> float:
    if isinstance(delta, list):
        return max(abs(v) for v in delta)
    return abs(delta)


def one_step_resolution(gate: dict) -> dict[str, Any]:
    """Which metric resolves which knob, as a ratio against the same material's floor.

    Computed from the non-vacuity ledger rather than transcribed, so the table
    cannot drift away from the run it claims to summarize.
    """
    floors = {e["case"]: e["spread"] for e in gate.get("ensembles", [])}
    steps = [r for r in gate.get("tables", {}).get("steps", []) if r["item"] == STEP_MATERIAL]

    metrics = [m for m in steps[0]["delta"]] if steps else []
    ratios: dict[str, dict[str, float]] = {m: {} for m in metrics}
    for knob, source in STEP_FLOOR_SOURCE.items():
        spread = floors.get(source)
        rows = [r for r in steps if r["case"] in (knob + "-", knob + "+")]
        if spread is None or not rows:
            continue
        for metric in metrics:
            if metric == "band_energy_delta":
                floor = spread["band_energy_delta"]["max_range_over_bands"]
            else:
                floor = (spread.get(metric) or {}).get("range")
            if not floor:
                continue
            moved = max(_worst(r["delta"][metric]) for r in rows if r["delta"][metric] is not None)
            ratios[metric][knob] = round(moved / floor, 4)

    clears_all = sorted(m for m, r in ratios.items() if r and min(r.values()) >= 1.0)
    with_room = {
        m: sorted(k for k, v in ratios[m].items() if v >= MARGINAL_RATIO) for m in clears_all
    }
    marginal = {
        m: {k: v for k, v in r.items() if 1.0 <= v < MARGINAL_RATIO}
        for m, r in ratios.items()
        if any(1.0 <= v < MARGINAL_RATIO for v in r.values())
    }
    silent = sorted(m for m, r in ratios.items() if r and max(r.values()) < 1.0)
    return {
        "definition": {
            "step_ratio_knobs": "+/-20% of the value, multiplicatively",
            "step_decibel_knobs": "+/-0.5 dB, absolutely",
            "boolean_knobs": "no step exists; the only change is a flip",
            "fixed_before_measuring": True,
        },
        "material": STEP_MATERIAL,
        "floor_source": {k: f"ensembles[{v}]" for k, v in STEP_FLOOR_SOURCE.items()},
        "knobs": sorted(STEP_FLOOR_SOURCE),
        "ratio_over_floor": {m: r for m, r in ratios.items() if r},
        "clears_floor_for_every_knob": clears_all,
        "clears_with_room_to_spare": with_room,
        "marginal": marginal,
        "silent_for_every_knob": silent,
        "marginal_note": (
            f"a ratio at or above 1 cleared the floor; below {MARGINAL_RATIO:g} it did so without "
            "room to spare, which is not the same claim and is reported separately"
        ),
        "reading": (
            "a metric whose ratio is under 1 did not adjudicate that knob. It abstained; it did "
            "not vote that the change was neutral, and a change may not be accepted on the "
            "strength of its silence"
        ),
    }


# Facts about a metric's reach that a row of numbers does not carry on its own.
# A metric under its own threshold has abstained rather than voted neutral, and
# the difference decides whether a change may be accepted on its strength.
RESOLUTION_NOTES: dict[str, Any] = {
    "stoi": {
        "no_resolution_for": [
            "repair.denoiseClassical.gain_floor",
            "repair.denoiseClassical.over_subtraction",
            "repair.denoiseClassical.spectral_floor",
            "repair.denoiseClassical.noise_estimation_quantile",
        ],
        "measured": {
            "degradation_scale_response_over_floor": {
                "gain_floor 0.05 -> 0.30": 0.5247,
                "over_subtraction 2.0 -> 6.0": 1.4943,
                "gain_smoothing on -> off": 0.1564,
            },
            "clears_the_floor_for": {
                "chain over-compression": -0.1444,
                "over-aggressive declick": -0.1987,
            },
        },
        "reading": (
            "STOI does not adjudicate a denoiser knob on this corpus -- not at one step, and not "
            "at degradation scale except for over_subtraction. That is an applicability fact "
            "rather than a defect"
        ),
    },
    "true_peak_dbtp": {
        "no_resolution_for": ["loudness.targetLufs"],
        "measured": {
            "target +0.5 LU on program_build": 0.0,
            "target +0.5 LU on speech_dynamic_hum50": 6.1e-05,
            "ceiling +0.5 dB on both items": 0.5,
        },
        "reading": (
            "the built-in presets run loudness-target-limited, so the ceiling binds and the "
            "target does not reach the peak. True peak follows the ceiling exactly and says "
            "nothing about the target"
        ),
    },
    "declick_threshold": {
        "not_a_continuous_knob": True,
        "measured": {"step_down_20pct": 0.0, "step_up_20pct_seg_snr_db": -1.147},
        "reading": (
            "the gate switches in one place, at the quietest planted click's amplitude, so a "
            "relative step is the wrong model for it: -20% changes the detected set not at all "
            "and +20% drops a click entirely. A step for a detector threshold would have to be "
            "defined on the planted amplitude distribution, which this run did not measure"
        ),
    },
}


def _undefined(value: Any) -> bool:
    return isinstance(value, float) and not math.isfinite(value)


def strictify(row: dict) -> tuple[dict, list[str]]:
    """Move every undefined metric out of `metrics` and into `inapplicable_metrics`."""
    metrics = dict(row.get("metrics") or {})
    inapplicable = dict(row.get("inapplicable_metrics") or {})
    moved: list[str] = []

    for name, value in list(metrics.items()):
        if _undefined(value):
            del metrics[name]
            inapplicable[name] = NOT_DEFINED.get(name, NOT_DEFINED_FALLBACK)
            moved.append(name)
            continue
        if not isinstance(value, dict):
            continue
        sides = {side: _undefined(v) for side, v in value.items()}
        if all(sides.values()):
            del metrics[name]
            inapplicable[name] = NOT_DEFINED.get(name, NOT_DEFINED_FALLBACK)
            moved.append(name)
        elif any(sides.values()):
            # One side defined and the other not is a different statement from
            # "the row says nothing": the defined side is still a measurement.
            metrics[name] = {s: (None if bad else value[s]) for s, bad in sides.items()}
            undefined_sides = ", ".join(s for s, bad in sides.items() if bad)
            inapplicable[f"{name}.{undefined_sides}"] = NOT_DEFINED.get(name, NOT_DEFINED_FALLBACK)
            moved.append(f"{name}.{undefined_sides}")

    out = dict(row)
    out["metrics"] = metrics
    out["inapplicable_metrics"] = inapplicable
    return out, moved


def check_strict(path: Path) -> None:
    """Fail loudly if the file a strict parser would reject was written anyway."""

    def reject(constant: str) -> float:
        raise ValueError(f"bare {constant} reached {path}")

    json.loads(path.read_text(), parse_constant=reject)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--shape", type=Path, required=True, help="run.py --emit-baseline output")
    parser.add_argument("--ledger", type=Path, required=True, help="the run ledger beside it")
    parser.add_argument("--non-vacuity", type=Path, required=True, help="the gate's run ledger")
    parser.add_argument("--manifest", type=Path, default=HERE / "audio" / "manifest.json")
    parser.add_argument("--library", default="", help="which dylib produced the numbers")
    parser.add_argument("--out", type=Path, default=HERE / "baseline.json")
    args = parser.parse_args()

    shape = json.loads(args.shape.read_text())
    ledger = json.loads(args.ledger.read_text())
    gate = json.loads(args.non_vacuity.read_text())
    manifest = json.loads(args.manifest.read_text())

    def git(*rest: str) -> str:
        return subprocess.run(
            ["git", *rest], cwd=HERE.parents[1], capture_output=True, text=True, check=False
        ).stdout.strip()

    moved_total: list[str] = []
    families = {}
    for family, rows in shape.items():
        out_rows = {}
        for key, row in rows.items():
            out_rows[key], moved = strictify(row)
            moved_total += [f"{key}: {m}" for m in moved]
        families[family] = out_rows

    baseline = {
        "meta": {
            "schema": SCHEMA,
            "contract": "tools/mastering-eval/docs/objective.md",
            # HEAD when this file was written, which is not when the numbers were
            # measured: see `numbers_provenance` for that run's own record.
            "head": git("rev-parse", "HEAD"),
            "library": args.library,
            "corpus_seed": manifest.get("seed"),
            "corpus_digest": corpus_digest(args.manifest),
            "corpus_digest_paths": len(corpus_paths(args.manifest)),
            "corpus_tree_digest": corpus_tree_digest(args.manifest.parent),
            "preset": ledger["preset"],
            "downmix": ledger["downmix"],
            "unavailable_metrics": ledger["unavailable_metrics"],
            "saturation_probe": ledger["saturation_probe"],
            # Three provenances, and they are three because the work happened three
            # times. `head` is this emission's; the other two are the runs' own, copied
            # from their ledgers rather than taken from git here -- git would answer for
            # now, and now is not when either run measured anything.
            "numbers_provenance": recorded_provenance(ledger),
            "non_vacuity_run": str(args.non_vacuity),
            # Both digests from the gate, because the two answer different questions
            # and a file can move one without the other: the tree digest covers the
            # directory, the file digest covers only what the manifest names. Carrying
            # the tree digest alone would let a file the manifest stopped naming --
            # added, dropped, or renamed out of the list -- change the corpus on one
            # side of the comparison and agree on the other.
            "non_vacuity_corpus_digest": gate.get("corpus_digest"),
            "non_vacuity_corpus_digest_paths": gate.get("corpus_digest_paths"),
            "non_vacuity_corpus_tree_digest": gate.get("corpus_tree_digest"),
            "gate_provenance": recorded_provenance(gate),
            "thresholds": dict(
                THRESHOLDS,
                resolution=dict(RESOLUTION_NOTES, one_step_knob_response=one_step_resolution(gate)),
            ),
        },
        **families,
    }

    args.out.write_text(json.dumps(baseline, indent=2, allow_nan=False) + "\n")
    check_strict(args.out)

    for family, rows in families.items():
        print(f"{family:<12} {len(rows)} rows")
    print(f"{len(moved_total)} undefined values moved to inapplicable_metrics")
    print(f"-> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
