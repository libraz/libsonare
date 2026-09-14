#!/usr/bin/env python3
"""Show that each metric moves for its own degradation and not for the others.

The contract's gate: a set of metrics that all move together is one metric
reported four times, so it is not enough to see a metric respond -- every metric
has to be measured against every degradation. This module builds that full cross
table.

Three tables, because no one material carries every question.

**Home** runs each degradation on the corpus item its expectation is defined
against: a noise item for the denoiser knobs, a click item for the declicker, a
long programme item for the chain. It answers "does the metric respond to its
own degradation", and it is where the expected magnitudes are read.

**Common** and **speech** run every degradation again on one shared material
each, so a whole column comes from one signal and one reference. That is what
answers "do these metrics move together", which a table whose rows each use a
different material cannot settle. The speech table is the one that settles it:
its material is the only corpus item both speech-bearing and longer than the 3.1
s short-term window, so no column on it is out of domain.

Every restoration metric needs a reference signal. A corpus item with a clean
reference supplies one; an item without (the long dynamics beds carry no defect
and no clean twin) uses the processor's own input, recorded per row as
``reference``. The substitute changes what segmental SNR *means* on that row --
it becomes "how far the processing moved the signal" rather than "how much of
the clean signal survived" -- but it does not change what the level-independence
check asks, which is only that a fixed reference and a scaled output give the
same number.

Run from ``bindings/python`` so rye resolves numpy and libsonare, and point
``SONARE_LIB_PATH`` at the build being measured::

    SONARE_LIB_PATH=.../build-mr-w2-shared/lib/libsonare.dylib \\
        rye run python ../../tools/mastering-eval/nonvacuity.py
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import metrics_chain  # noqa: E402
import metrics_repair  # noqa: E402
from run import DOWNMIX, load_item, mono_feeds  # noqa: E402

import libsonare  # noqa: E402

SCHEMA = 1

# The gain a level-only degradation applies. A power of two scales a float
# sample exactly, so anything that moves under it moved for its own reasons
# rather than through a rounding difference.
GAIN_ONLY = 0.5

# Which band the single-band cut removes, as an index into the 32 log-spaced
# centres metrics_chain lays out. 20 sits near 1.9 kHz at 48 kHz.
CUT_BAND_INDEX = 20
CUT_GAIN_DB = -24.0
CUT_Q = 4.0

PRESET = "pop"

# The two shared materials every degradation is restaged on.
#
# The speech bed is the one that settles the co-movement question, because it is
# the only item satisfying both domain limits at once: STOI may be read only on
# speech, short-term loudness spread exists only past the 3.1 s window, and this
# is the single corpus item that is both. Every column is live on it.
#
# The programme bed is kept beside it because it is broadband and transient
# where the speech bed is neither, so a per-band or a dynamics reading has
# somewhere to move that is not one talker.
COMMON_ITEM = "program_build"
COMMON_FEED = "downmix"
SPEECH_ITEM = "speech_dynamic_hum50"
SPEECH_FEED = "downmix"

# The one-step table's extra materials. The noise bed is the only speech item
# that carries an ensemble, so it is the only place a one-step response can be
# read against a redraw floor of the same material; the click bed is where a
# detector threshold has anything to detect.
STEP_NOISE_ITEM = "speech_pink_noise"
STEP_CLICK_ITEM = "speech_click"

NOT_SPEECH_BEARING = "STOI is defined on speech; this material carries none"

# The cases that repeat an earlier degradation on speech-bearing material. They
# belong to the home table only: restaging them on a shared material would run
# the same degradation twice under two names.
SPEECH_TWINS = frozenset({"D1s", "D2s", "D3s", "D7rs", "D8sa", "D8sb"})

SCALAR_METRICS = (
    "seg_snr_db",
    "log_kurtosis_ratio",
    "stoi",
    "log_spectral_distance",
    "integrated_lufs",
    "true_peak_dbtp",
    "short_term_spread",
)
VECTOR_METRICS = ("band_energy_delta",)


def _f32(x: np.ndarray) -> np.ndarray:
    return np.ascontiguousarray(x, dtype=np.float32)


def _rms_matched(reference: np.ndarray, other: np.ndarray) -> np.ndarray:
    """Scale `other` by one global factor so its RMS equals the reference's."""
    rms = float(np.sqrt(np.mean(other**2)))
    if rms <= 0.0:
        return other
    return other * (float(np.sqrt(np.mean(reference**2))) / rms)


# ------------------------------------------------------------------ processors


def _denoise(feed: np.ndarray, sample_rate: int, params: dict) -> np.ndarray:
    return np.asarray(
        libsonare.mastering_repair_denoise_classical(_f32(feed), sample_rate, **params),
        dtype=np.float64,
    )


def _declick(feed: np.ndarray, sample_rate: int, params: dict) -> np.ndarray:
    return np.asarray(
        libsonare.mastering_repair_declick(_f32(feed), sample_rate, **params),
        dtype=np.float64,
    )


def _chain(feed: np.ndarray, sample_rate: int, overrides: dict | None) -> np.ndarray:
    result = libsonare.master_audio(_f32(feed), sample_rate, PRESET, overrides or None)
    return np.asarray(result.samples, dtype=np.float64)


def _band_cut_params(sample_rate: int) -> dict[str, float | bool]:
    centre = float(metrics_chain.band_center_frequencies(sample_rate)[CUT_BAND_INDEX])
    return {
        "band0.type": 0,  # Peak
        "band0.enabled": True,
        "band0.frequencyHz": centre,
        "band0.gainDb": CUT_GAIN_DB,
        "band0.q": CUT_Q,
    }


def _cut_band(x: np.ndarray, sample_rate: int) -> np.ndarray:
    result = libsonare.mastering_process(
        "eq.parametric", _f32(x), sample_rate, _band_cut_params(sample_rate)
    )
    return np.asarray(result.samples, dtype=np.float64)


# ------------------------------------------------------------------- the cases


@dataclass(frozen=True)
class Case:
    """One deliberate degradation: what it is, where it is run, what it should move."""

    key: str
    title: str
    family: str
    item: str
    expected: tuple[str, ...]
    baseline: Callable[[np.ndarray, int], np.ndarray]
    degraded: Callable[[np.ndarray, int], np.ndarray]
    settings: dict[str, Any] = field(default_factory=dict)
    feed: str = "downmix"


def _declick_defaults(defect: dict | None) -> dict:
    """The declicker aimed under the quietest planted click, as run.py aims it."""
    if defect is None:
        return {"threshold": 0.8, "max_click_samples": 8}
    quietest = min(abs(a) for a in defect["amplitudes"])
    return {
        "threshold": round(0.9 * quietest, 6),
        "max_click_samples": max(8, 4 * int(defect["width_samples"])),
    }


def build_cases(manifest: dict) -> list[Case]:
    by_id = {item["id"]: item for item in manifest["items"]}

    def click_params(item_id: str) -> dict:
        return _declick_defaults((by_id[item_id]["defects"] or {}).get("click"))

    spectral = {"mode": "spectralSubtraction"}

    def denoise_case(key, title, item, base, deg, expected) -> Case:
        return Case(
            key=key,
            title=title,
            family="restoration",
            item=item,
            expected=expected,
            baseline=lambda x, sr, p=base: _denoise(x, sr, p),
            degraded=lambda x, sr, p=deg: _denoise(x, sr, p),
            settings={"entry_point": "denoise_classical", "baseline": base, "degraded": deg},
        )

    def chain_case(key, title, item, deg_overrides, expected, tail=None) -> Case:
        def degraded(x: np.ndarray, sr: int) -> np.ndarray:
            out = _chain(x, sr, deg_overrides)
            return tail(out, sr) if tail is not None else out

        return Case(
            key=key,
            title=title,
            family="mastering",
            item=item,
            expected=expected,
            baseline=lambda x, sr: _chain(x, sr, None),
            degraded=degraded,
            settings={
                "entry_point": "master_audio",
                "preset": PRESET,
                "overrides": deg_overrides,
                "post": ("eq.parametric band cut" if tail is not None else None),
            },
        )

    cases: list[Case] = [
        denoise_case(
            "D1",
            "denoise gain_floor 0.05 -> 0.30",
            "sine_white_noise",
            {},
            {"gain_floor": 0.30},
            ("seg_snr_db",),
        ),
        denoise_case(
            "D2",
            "denoise over_subtraction 2.0 -> 6.0 (SpectralSubtraction)",
            "chord_pink_noise",
            dict(spectral, over_subtraction=2.0),
            dict(spectral, over_subtraction=6.0),
            ("log_kurtosis_ratio",),
        ),
        denoise_case(
            "D3",
            "denoise gain_smoothing on -> off",
            "chord_pink_noise",
            {},
            {"gain_smoothing": False},
            ("log_kurtosis_ratio", "log_spectral_distance"),
        ),
        chain_case(
            "D4",
            "chain over-compressed (ratio 20, threshold -40 dB)",
            COMMON_ITEM,
            {
                "dynamics": {
                    "compressor": {
                        "enabled": True,
                        "thresholdDb": -40.0,
                        "ratio": 20.0,
                        "attackMs": 1.0,
                        "releaseMs": 50.0,
                    }
                }
            },
            ("short_term_spread",),
        ),
        chain_case(
            "D5a",
            "chain ceiling raised to +6 dBTP, target unchanged",
            COMMON_ITEM,
            {"loudness": {"ceilingDb": 6.0}},
            ("true_peak_dbtp",),
        ),
        chain_case(
            "D5b",
            "chain ceiling raised to +6 dBTP and target to -6 LUFS",
            COMMON_ITEM,
            {"loudness": {"ceilingDb": 6.0, "targetLufs": -6.0}},
            ("true_peak_dbtp", "integrated_lufs"),
        ),
        chain_case(
            "D6",
            f"chain output band {CUT_BAND_INDEX} cut by {CUT_GAIN_DB:g} dB",
            COMMON_ITEM,
            None,
            ("band_energy_delta",),
            tail=_cut_band,
        ),
        Case(
            key="D7",
            title=f"chain output scaled by {GAIN_ONLY:g}, processing identical",
            family="mastering",
            item=COMMON_ITEM,
            expected=("integrated_lufs", "true_peak_dbtp"),
            baseline=lambda x, sr: _chain(x, sr, None),
            degraded=lambda x, sr: _chain(x, sr, None) * GAIN_ONLY,
            settings={"entry_point": "master_audio", "preset": PRESET, "gain": GAIN_ONLY},
        ),
        Case(
            key="D7r",
            title=f"denoise output scaled by {GAIN_ONLY:g}, processing identical",
            family="restoration",
            item="sine_white_noise",
            expected=("integrated_lufs", "true_peak_dbtp"),
            baseline=lambda x, sr: _denoise(x, sr, {}),
            degraded=lambda x, sr: _denoise(x, sr, {}) * GAIN_ONLY,
            settings={"entry_point": "denoise_classical", "gain": GAIN_ONLY},
        ),
        Case(
            key="D8a",
            title="declick gate raised to 1.0, so no click is detected",
            family="restoration",
            item="sine_click",
            expected=("seg_snr_db",),
            baseline=lambda x, sr: _declick(x, sr, click_params("sine_click")),
            degraded=lambda x, sr: _declick(x, sr, {"threshold": 1.0, "max_click_samples": 8}),
            settings={"entry_point": "declick", "baseline": click_params("sine_click")},
        ),
        Case(
            key="D8b",
            title="declick over-aggressive (gate 0.02, runs up to 256 samples)",
            family="restoration",
            item="sine_click",
            expected=("seg_snr_db",),
            baseline=lambda x, sr: _declick(x, sr, click_params("sine_click")),
            degraded=lambda x, sr: _declick(x, sr, {"threshold": 0.02, "max_click_samples": 256}),
            settings={"entry_point": "declick", "baseline": click_params("sine_click")},
        ),
        Case(
            key="D8c",
            title="declick gate raised to 1.0 on the 40-click bed",
            family="restoration",
            item="chord_click",
            expected=("seg_snr_db",),
            baseline=lambda x, sr: _declick(x, sr, click_params("chord_click")),
            degraded=lambda x, sr: _declick(x, sr, {"threshold": 1.0, "max_click_samples": 8}),
            settings={"entry_point": "declick", "baseline": click_params("chord_click")},
        ),
        # The speech twins. Each repeats the restoration degradation above on the
        # speech-bearing item of the same defect, which is the only place STOI's
        # response to that degradation can be read.
        denoise_case(
            "D1s",
            "denoise gain_floor 0.05 -> 0.30, speech bed",
            "speech_white_noise",
            {},
            {"gain_floor": 0.30},
            ("seg_snr_db",),
        ),
        denoise_case(
            "D2s",
            "denoise over_subtraction 2.0 -> 6.0 (SpectralSubtraction), speech bed",
            "speech_pink_noise",
            dict(spectral, over_subtraction=2.0),
            dict(spectral, over_subtraction=6.0),
            ("log_kurtosis_ratio",),
        ),
        denoise_case(
            "D3s",
            "denoise gain_smoothing on -> off, speech bed",
            "speech_pink_noise",
            {},
            {"gain_smoothing": False},
            ("log_kurtosis_ratio", "log_spectral_distance"),
        ),
        Case(
            key="D7rs",
            title=f"denoise output scaled by {GAIN_ONLY:g} on the speech bed, processing identical",
            family="restoration",
            item="speech_white_noise",
            expected=("integrated_lufs", "true_peak_dbtp"),
            baseline=lambda x, sr: _denoise(x, sr, {}),
            degraded=lambda x, sr: _denoise(x, sr, {}) * GAIN_ONLY,
            settings={"entry_point": "denoise_classical", "gain": GAIN_ONLY},
        ),
        Case(
            key="D8sa",
            title="declick gate raised to 1.0 on the speech bed",
            family="restoration",
            item="speech_click",
            expected=("seg_snr_db",),
            baseline=lambda x, sr: _declick(x, sr, click_params("speech_click")),
            degraded=lambda x, sr: _declick(x, sr, {"threshold": 1.0, "max_click_samples": 8}),
            settings={"entry_point": "declick", "baseline": click_params("speech_click")},
        ),
        Case(
            key="D8sb",
            title="declick over-aggressive on the speech bed",
            family="restoration",
            item="speech_click",
            expected=("seg_snr_db",),
            baseline=lambda x, sr: _declick(x, sr, click_params("speech_click")),
            degraded=lambda x, sr: _declick(x, sr, {"threshold": 0.02, "max_click_samples": 256}),
            settings={"entry_point": "declick", "baseline": click_params("speech_click")},
        ),
    ]
    return cases


def restaged(manifest: dict, item: str, feed: str) -> list[Case]:
    """Every degradation again, all on one shared material.

    A speech twin is the same degradation as its non-speech original, so only
    the original is restaged; keeping both would report one measurement twice
    under two names and inflate any agreement read off the table.
    """
    return [
        Case(
            key=case.key,
            title=case.title,
            family=case.family,
            item=item,
            expected=case.expected,
            baseline=case.baseline,
            degraded=case.degraded,
            settings=case.settings,
            feed=feed,
        )
        for case in build_cases(manifest)
        if case.key not in SPEECH_TWINS
    ]


# ---------------------------------------------------------------- one step out


# What counts as one step, fixed here before anything was measured so that the
# answer cannot be chosen after seeing it.
#
# A deliberate degradation says how far a metric moves when a processor is
# broken. A threshold has to sit under the smallest change worth claiming, which
# is a different and much smaller quantity, and no fraction of the first
# estimates the second. So each knob is nudged by one step in both directions --
# both, because a knob's response need not be symmetric and a threshold is
# placed on the improving side.
#
# Two kinds of knob, because one rule does not fit both:
#
# * **Ratio-like** (gain floors, the Berouti alpha and beta, a quantile, a
#   detector threshold): one step is +/-20% of the value, multiplicatively. That
#   is scale-free, so it means the same thing at 0.05 and at 2.0, and it needs no
#   published range for the knob -- which the library does not give.
# * **Logarithmic** (anything in dB or LUFS): one step is +/-0.5 dB, absolutely.
#   A dB value is already a logarithm, so scaling it by 20% is not a scale-free
#   change of anything; 0.5 dB is the smallest increment a mastering decision is
#   normally made in.
#
# A boolean knob has no step. Its only change is a flip, which is what D3 already
# measured, so the booleans are left out rather than given an invented step.
STEP_RELATIVE = 0.20
STEP_DECIBELS = 0.5

# The chain values the explicit baseline asserts are the preset's own. Asserted
# rather than assumed: PRESET-CHECK below runs the chain with these set and with
# nothing set, and its `output_identical` says whether they match.
PRESET_CEILING_DB = -1.0
PRESET_TARGET_LUFS = -14.0


def _ratio_steps(value: float) -> tuple[float, float]:
    return value * (1.0 - STEP_RELATIVE), value * (1.0 + STEP_RELATIVE)


def build_steps(manifest: dict) -> list[Case]:
    """One knob moved one step, in each direction, with everything else held."""
    by_id = {item["id"]: item for item in manifest["items"]}
    spectral = {"mode": "spectralSubtraction"}

    def denoise_steps(knob: str, default: float, item: str, extra: dict) -> list[Case]:
        low, high = _ratio_steps(default)
        out = []
        for sign, value in (("-", low), ("+", high)):
            base = dict(extra, **{knob: default})
            stepped = dict(extra, **{knob: value})
            out.append(
                Case(
                    key=f"{knob}{sign}",
                    title=f"denoise {knob} {default:g} -> {value:g} ({sign}{STEP_RELATIVE:.0%})",
                    family="restoration",
                    item=item,
                    expected=(),
                    baseline=lambda x, sr, p=base: _denoise(x, sr, p),
                    degraded=lambda x, sr, p=stepped: _denoise(x, sr, p),
                    settings={"entry_point": "denoise_classical", "knob": knob, "step": "relative"},
                )
            )
        return out

    def chain_steps(field: str, default: float, item: str) -> list[Case]:
        out = []
        for sign, value in (("-", default - STEP_DECIBELS), ("+", default + STEP_DECIBELS)):
            base = {"loudness": {"ceilingDb": PRESET_CEILING_DB, "targetLufs": PRESET_TARGET_LUFS}}
            stepped = {"loudness": dict(base["loudness"], **{field: value})}
            out.append(
                Case(
                    key=f"{field}{sign}",
                    title=(
                        f"chain loudness.{field} {default:g} -> "
                        f"{value:g} ({sign}{STEP_DECIBELS} dB)"
                    ),
                    family="mastering",
                    item=item,
                    expected=(),
                    baseline=lambda x, sr, o=base: _chain(x, sr, o),
                    degraded=lambda x, sr, o=stepped: _chain(x, sr, o),
                    settings={"entry_point": "master_audio", "knob": field, "step": "decibels"},
                )
            )
        return out

    cases: list[Case] = []
    for item in (STEP_NOISE_ITEM, SPEECH_ITEM):
        cases += denoise_steps("gain_floor", 0.05, item, {})
        cases += denoise_steps("over_subtraction", 2.0, item, spectral)
        cases += denoise_steps("spectral_floor", 0.05, item, spectral)
        cases += denoise_steps("noise_estimation_quantile", 0.1, item, {})
    for item in (SPEECH_ITEM, COMMON_ITEM):
        cases += chain_steps("ceilingDb", PRESET_CEILING_DB, item)
        cases += chain_steps("targetLufs", PRESET_TARGET_LUFS, item)

    click = _declick_defaults((by_id[STEP_CLICK_ITEM]["defects"] or {}).get("click"))
    low, high = _ratio_steps(click["threshold"])
    for sign, value in (("-", low), ("+", high)):
        stepped = dict(click, threshold=round(value, 6))
        cases.append(
            Case(
                key=f"declick.threshold{sign}",
                title=(
                    f"declick threshold {click['threshold']:g} -> "
                    f"{stepped['threshold']:g} ({sign}{STEP_RELATIVE:.0%})"
                ),
                family="restoration",
                item=STEP_CLICK_ITEM,
                expected=(),
                baseline=lambda x, sr, p=click: _declick(x, sr, p),
                degraded=lambda x, sr, p=stepped: _declick(x, sr, p),
                settings={"entry_point": "declick", "knob": "threshold", "step": "relative"},
            )
        )

    # Does the explicit chain baseline reproduce the preset, or is "one step from
    # the default" a step from a value the preset never held? Read
    # `output_identical` on this row for the answer.
    cases.append(
        Case(
            key="PRESET-CHECK",
            title="chain with the asserted preset values set explicitly, against no overrides",
            family="mastering",
            item=SPEECH_ITEM,
            expected=(),
            baseline=lambda x, sr: _chain(x, sr, None),
            degraded=lambda x, sr: _chain(
                x,
                sr,
                {"loudness": {"ceilingDb": PRESET_CEILING_DB, "targetLufs": PRESET_TARGET_LUFS}},
            ),
            settings={"entry_point": "master_audio", "knob": None, "step": None},
        )
    )
    return cases


# -------------------------------------------------------------------- measuring


def evaluate(
    reference: np.ndarray,
    source: np.ndarray,
    output: np.ndarray,
    sample_rate: int,
    *,
    speech_bearing: bool,
) -> dict:
    """All eight metrics for one processed signal.

    STOI is computed only on speech-bearing material. Outside speech it is not a
    conservative reading but a wrong one -- the contract records that it has
    already called a correct restoration a regression on a tonal item -- so a
    non-speech row carries null and the reason rather than a number that would
    then have to be argued away.
    """
    n = min(reference.shape[0], source.shape[0], output.shape[0])
    ref, src, out = reference[:n], source[:n], output[:n]

    report = metrics_repair.segmental_snr_report(ref, out, sample_rate)
    values: dict[str, Any] = {
        "seg_snr_db": float(report.value),
        "log_kurtosis_ratio": float(metrics_repair.log_kurtosis_ratio(src, out, sample_rate)),
        "stoi": (
            _maybe(
                lambda: float(
                    metrics_repair.short_time_objective_intelligibility(ref, out, sample_rate)
                )
            )
            if speech_bearing
            else None
        ),
        "log_spectral_distance": float(metrics_repair.log_spectral_distance(ref, out, sample_rate)),
        "integrated_lufs": float(metrics_chain.integrated_loudness(out, sample_rate)),
        "true_peak_dbtp": float(metrics_chain.true_peak_dbtp(out, sample_rate)),
        "short_term_spread": float(metrics_chain.short_term_spread(out, sample_rate)),
        "band_energy_delta": [
            float(v) for v in metrics_chain.band_energy_delta(src, out, sample_rate)
        ],
    }
    values["seg_snr_saturation"] = {
        "active_frames": int(report.active_frames),
        "ceiling_frames": int(report.ceiling_frames),
        "floor_frames": int(report.floor_frames),
        "ceiling_fraction": round(float(report.ceiling_fraction), 6),
        "floor_fraction": round(float(report.floor_fraction), 6),
        "saturated": bool(report.saturated),
    }
    return values


def _maybe(call: Callable[[], float]) -> float | None:
    """A metric whose implementation is not there yet reads null, never a number."""
    try:
        return call()
    except NotImplementedError:
        return None


def _delta(before: Any, after: Any) -> Any:
    if before is None or after is None:
        return None
    if isinstance(before, list):
        return [b - a for a, b in zip(before, after, strict=True)]
    if not (np.isfinite(before) and np.isfinite(after)):
        return None
    return after - before


def measure(case: Case, manifest: dict, root: Path, draw: int = 0) -> dict:
    # The runner's own loader and the runner's own downmix, not a second copy of
    # either: this module checks whether the harness's metrics can fail, so it
    # has to be handed the same samples the harness hands them. A private reader
    # here would leave the check measuring a signal the harness never sees.
    item = next(i for i in manifest["items"] if i["id"] == case.item)
    draws, clean, sample_rate = load_item(root, item)
    audio = draws[draw]
    feed = mono_feeds(audio)[case.feed]
    reference = mono_feeds(clean)[case.feed] if clean is not None else feed
    reference_kind = "clean_reference" if clean is not None else "processor_input"
    speech = bool(item.get("speech_bearing", False))

    started = time.perf_counter()
    base_out = case.baseline(feed, sample_rate)
    deg_out = case.degraded(feed, sample_rate)
    processed_seconds = time.perf_counter() - started

    before = evaluate(reference, feed, base_out, sample_rate, speech_bearing=speech)
    after = evaluate(reference, feed, deg_out, sample_rate, speech_bearing=speech)
    deltas = {m: _delta(before[m], after[m]) for m in SCALAR_METRICS + VECTOR_METRICS}

    # The same degradation with its level change taken back out. Two of the
    # metrics are level-sensitive by construction, so a degradation that also
    # moves the output level leaves their response inseparable from it; this
    # column says which part of the movement was the defect.
    matched_out = _rms_matched(base_out, deg_out)
    matched = evaluate(reference, feed, matched_out, sample_rate, speech_bearing=speech)
    matched_deltas = {m: _delta(before[m], matched[m]) for m in SCALAR_METRICS + VECTOR_METRICS}

    return {
        "case": case.key,
        "title": case.title,
        "family": case.family,
        "item": case.item,
        "feed": case.feed,
        "sample_rate": sample_rate,
        "reference": reference_kind,
        "draw": draw,
        "draw_count": len(draws),
        "downmix": DOWNMIX if case.feed == "downmix" else None,
        "speech_bearing": speech,
        "inapplicable_metrics": ({} if speech else {"stoi": NOT_SPEECH_BEARING}),
        "expected": list(case.expected),
        "settings": case.settings,
        "seconds": round(processed_seconds, 3),
        "output_identical": bool(
            base_out.shape == deg_out.shape and np.array_equal(base_out, deg_out)
        ),
        "before": before,
        "after": after,
        "delta": deltas,
        "after_level_matched": matched,
        "delta_level_matched": matched_deltas,
    }


# ------------------------------------------------------------------- the floor


def ensemble(case: Case, manifest: dict, root: Path) -> dict | None:
    """How far a metric moves between draws that differ only in their noise seed.

    This is the floor a threshold has to clear and the one number the degradation
    tables cannot supply. Those say how much a metric moves when the processing
    is deliberately broken; this says how much it moves when *nothing* changed
    except which realization of the same statistical material was fed in. An
    improvement smaller than this is indistinguishable from having redrawn the
    corpus, however deterministic the arithmetic is on one draw.

    Only the baseline processing is run, on every draw the item carries.
    """
    item = next(i for i in manifest["items"] if i["id"] == case.item)
    draws, clean, sample_rate = load_item(root, item)
    if len(draws) < 2:
        return None
    speech = bool(item.get("speech_bearing", False))

    per_draw = []
    for audio in draws:
        feed = mono_feeds(audio)[case.feed]
        reference = mono_feeds(clean)[case.feed] if clean is not None else feed
        out = case.baseline(feed, sample_rate)
        per_draw.append(evaluate(reference, feed, out, sample_rate, speech_bearing=speech))

    spread: dict[str, Any] = {}
    for metric in SCALAR_METRICS:
        values = [d[metric] for d in per_draw]
        if any(v is None or not np.isfinite(v) for v in values):
            spread[metric] = None
            continue
        spread[metric] = {
            "min": min(values),
            "max": max(values),
            "range": max(values) - min(values),
            "std": float(np.std(values)),
        }
    bands = np.array([d["band_energy_delta"] for d in per_draw])
    spread["band_energy_delta"] = {
        "max_range_over_bands": float(np.max(bands.max(axis=0) - bands.min(axis=0))),
        "max_std_over_bands": float(np.max(np.std(bands, axis=0))),
    }
    return {
        "case": case.key,
        "item": case.item,
        "feed": case.feed,
        "draws": len(draws),
        "speech_bearing": speech,
        "settings": case.settings,
        "spread": spread,
        "per_draw": per_draw,
    }


# --------------------------------------------------------------------- printing


def _cell(metric: str, delta: Any) -> str:
    if delta is None:
        return "n/a"
    if metric == "band_energy_delta":
        worst = int(np.argmax(np.abs(delta)))
        return f"{delta[worst]:+.4f}@{worst}"
    return f"{delta:+.6g}"


def print_table(rows: list[dict], title: str, delta_key: str = "delta") -> None:
    print(f"\n## {title}")
    header = ["case", *(m[:14] for m in SCALAR_METRICS), "band_delta"]
    print(" | ".join(header))
    for row in rows:
        cells = [_cell(m, row[delta_key][m]) for m in SCALAR_METRICS + VECTOR_METRICS]
        print(" | ".join([row["case"], *cells]))


def provenance() -> dict:
    def git(*args: str) -> str:
        return subprocess.run(
            ["git", *args],
            cwd=HERE.parents[1],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.strip()

    status = git("status", "--porcelain")
    # Which binary produced the numbers, not only which package was imported:
    # the loader prefers build/lib/libsonare.dylib unless SONARE_LIB_PATH says
    # otherwise, and a run that measured a stale dylib is indistinguishable from
    # one that did not unless the path and its mtime are on the record.
    dylib = os.environ.get("SONARE_LIB_PATH")
    return {
        "head": git("rev-parse", "HEAD"),
        "head_committed": git("log", "-1", "--format=%cI"),
        "status_src": [
            line for line in status.splitlines() if " src/" in line or line[3:].startswith("src/")
        ],
        "status_all": status.splitlines(),
        "package": getattr(libsonare, "__file__", None),
        "sonare_lib_path": dylib,
        "sonare_lib_mtime": (
            time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(Path(dylib).stat().st_mtime))
            if dylib and Path(dylib).exists()
            else None
        ),
    }


CORPUS_TREE_RECIPE = "find . -type f | sort | xargs shasum -a 256 | shasum -a 256"


def corpus_tree_digest(root: Path) -> str | None:
    """Digest of the whole corpus directory under the recipe the freeze publishes.

    Runs :data:`CORPUS_TREE_RECIPE` rather than reproducing it, because the
    number is only useful if it equals the published one, and a reimplementation
    that merely ought to agree is the thing being guarded against -- a first
    attempt here differed from the shell's answer on the same bytes, which as a
    hand-rolled "equivalent" would have read as a broken freeze.
    """
    result = subprocess.run(
        ["sh", "-c", CORPUS_TREE_RECIPE],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return None
    return result.stdout.split()[0]


def corpus_paths(manifest_path: Path) -> list[Path]:
    """Every audio file the manifest names, in manifest order and deduplicated.

    An ensemble item names its draws under ``realizations[*].audio`` and repeats
    the first of them as ``audio``, so reading only ``audio`` and
    ``clean_reference`` leaves every draw past the first uncovered -- sixteen
    files on this corpus, and exactly the ones an ensemble measurement reads.
    """
    manifest = json.loads(manifest_path.read_text())
    root = manifest_path.parent
    seen: dict[Path, None] = {}
    for item in manifest["items"]:
        names = [item.get("audio"), item.get("clean_reference")]
        names += [draw.get("audio") for draw in item.get("realizations", [])]
        for name in names:
            if name:
                seen.setdefault(root / name, None)
    return list(seen)


def corpus_digest(manifest_path: Path) -> str:
    """One digest over the manifest and every audio file it names.

    The generator can rewrite the corpus while a run is in flight, and a run
    whose early rows read one corpus and whose late rows read another is not a
    cross table of anything. Taking this before and after says which of the two
    happened instead of leaving it to mtimes.

    This covers only what the manifest names, where :func:`corpus_tree_digest`
    covers the directory; a file the manifest stopped naming moves one and not
    the other, which is worth being able to tell apart.

    A named file that is missing raises. Folding it in as empty bytes would give
    a deleted file and an empty one the same digest, and a digest whose job is to
    say whether the contents are the same must not absorb an absence quietly.
    """
    digest = hashlib.sha256(manifest_path.read_bytes())
    for path in corpus_paths(manifest_path):
        digest.update(path.read_bytes())
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--manifest",
        type=Path,
        default=HERE / "audio" / "manifest.json",
        help="the corpus manifest corpus.py wrote",
    )
    parser.add_argument("--out", type=Path, default=HERE / "runs" / "w2-nonvacuity.json")
    parser.add_argument(
        "--table",
        choices=["home", "common", "speech", "steps", "all"],
        default="all",
        help="which table to run",
    )
    parser.add_argument("--case", action="append", default=[], help="restrict to these case keys")
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text())
    root = args.manifest.parent
    digest_before = corpus_digest(args.manifest)
    tree_before = corpus_tree_digest(root)

    tables: dict[str, list[dict]] = {}
    for name, cases in (
        ("home", build_cases(manifest)),
        ("common", restaged(manifest, COMMON_ITEM, COMMON_FEED)),
        ("speech", restaged(manifest, SPEECH_ITEM, SPEECH_FEED)),
        ("steps", build_steps(manifest)),
    ):
        if args.table not in (name, "all"):
            continue
        rows = []
        for case in cases:
            if args.case and case.key not in args.case:
                continue
            print(f"{name}/{case.key:<5} {case.item:<20}", flush=True)
            rows.append(measure(case, manifest, root))
        tables[name] = rows
        print_table(rows, name)
        print_table(
            rows, f"{name} (degraded output level-matched to baseline)", "delta_level_matched"
        )

    # The redraw floor, from the baseline processing alone on every draw an item
    # carries. Restricted to the cases whose material is an ensemble.
    ensembles = []
    if args.table == "all":
        for case in build_cases(manifest):
            if args.case and case.key not in args.case:
                continue
            spread = ensemble(case, manifest, root)
            if spread is not None:
                print(f"ensemble/{case.key:<5} {case.item:<20} {spread['draws']} draws", flush=True)
                ensembles.append(spread)

    payload = {
        "schema": SCHEMA,
        "contract": "tools/mastering-eval/docs/objective.md",
        "corpus_manifest": str(args.manifest),
        "corpus_seed": manifest.get("seed"),
        "corpus_digest": digest_before,
        "corpus_digest_after": (digest_after := corpus_digest(args.manifest)),
        "corpus_stable_during_run": digest_before == digest_after,
        "corpus_digest_paths": len(corpus_paths(args.manifest)),
        "corpus_tree_digest": tree_before,
        "corpus_tree_digest_after": (tree_after := corpus_tree_digest(root)),
        "corpus_tree_stable_during_run": tree_before == tree_after,
        "gain_only": GAIN_ONLY,
        "band_cut": {"index": CUT_BAND_INDEX, "gain_db": CUT_GAIN_DB, "q": CUT_Q},
        "provenance": provenance(),
        "tables": tables,
        "ensembles": ensembles,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2) + "\n")
    print(f"\n-> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
