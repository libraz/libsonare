"""Run the eval corpus through the processors and write one run ledger.

This module owns the *bookkeeping* and nothing else: which item, which feed,
which entry point, which parameters, and what the numbers were. Every metric
comes from ``metrics_repair`` or ``metrics_chain``; none is computed here, so a
number in a ledger has exactly one implementation behind it.

Two things a row records that a bare number would hide.

**Which feed it was measured on.** The restoration entry points are mono and the
corpus is stereo, so each restoration job runs three times -- on the downmix and
on each channel -- and the feed is part of the row's identity. A later stereo
entry point adds rows rather than silently changing what an existing row means.

**Which planted quantities were handed to the processor.** Dehum needs the
fundamental and declip needs the threshold, or they operate on the wrong target
entirely and the row measures nothing. Supplying them makes the row a
restoration-quality measurement given correct detection, which is not a
detection measurement; ``planted_params_supplied`` says so per row.
"""

from __future__ import annotations

import argparse
import importlib
import json
import math
import os
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path
from types import ModuleType

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "voicematch"))
# After voicematch, so this directory wins: it has a `corpus.py` of its own, and
# a same-named module there would shadow one of ours without saying so.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import libsonare
from wavio import read_wav

LEDGER_SCHEMA = 1

# How a stereo item becomes the mono feed a restoration entry point takes. The
# value is recorded per row so that changing it invalidates comparisons loudly.
DOWNMIX = "mid_mean"

# The metric functions this harness calls, and the names it will accept for
# each. The first name that exists wins; the candidates exist so that the
# metric modules can settle on their own naming without this file gating them.
# Signatures are positional: a pair metric is f(a, b, sample_rate), a
# single-signal metric is f(x, sample_rate).
RESTORATION_METRICS: dict[str, tuple[tuple[str, ...], str]] = {
    "seg_snr_db": (("segmental_snr", "seg_snr", "seg_snr_db"), "clean_vs_processed"),
    "log_kurtosis_ratio": (("log_kurtosis_ratio", "log_kurt_ratio"), "input_vs_processed"),
    "stoi": (("short_time_objective_intelligibility", "stoi"), "clean_vs_processed"),
    "log_spectral_distance": (
        ("log_spectral_distance", "lsd"),
        "clean_vs_processed",
    ),
}

MASTERING_METRICS: dict[str, tuple[tuple[str, ...], str]] = {
    "integrated_lufs": (("integrated_loudness", "integrated_lufs", "lufs"), "single"),
    "true_peak_dbtp": (("true_peak_dbtp", "true_peak"), "single"),
    "short_term_spread": (("short_term_spread", "short_term_loudness_spread"), "single"),
    "band_energy_delta": (("band_energy_delta", "per_band_energy_delta"), "pair"),
}


# Metrics whose domain is narrower than the corpus. STOI correlates short-time
# band envelopes against a model fitted to and validated on speech; a pure tone
# carries no such envelope, so the number it returns there is outside the domain
# the metric was established in. Reading it anyway is not the conservative
# choice -- it has already reported a *correct* restoration as a regression on
# tonal items, which under "no metric may get worse" rejects a good change.
SPEECH_ONLY_METRICS = ("stoi",)
NOT_SPEECH_BEARING = "material is not speech-bearing"

# A metric whose frames were mostly taken at its own clip bound cannot move much
# further in the bad direction, so reading such a row as "did not get worse" lets
# the bound hide a regression. A metric may expose a companion `<name>_report`
# taking the same arguments and returning the same value alongside the counts;
# the harness prefers it, so the value and the counts always come from one call.
#
# What is recorded is the FRACTION, not the boolean. A row can lose most of its
# frames to the ceiling while the aggregate is nowhere near it, and such a row
# reads as healthy if only the boolean survives.
REPORT_SUFFIX = "_report"
SATURATION_FIELDS = (
    "active_frames",
    "ceiling_frames",
    "floor_frames",
    "ceiling_fraction",
    "floor_fraction",
    "saturated",
)


class MetricSet:
    """The resolved metric functions for one family.

    A metric whose implementation raises ``NotImplementedError`` is not fatal --
    it is recorded as null and named in the ledger's ``unavailable_metrics``, so
    a run made before the metric existed cannot pass for a complete one.
    """

    def __init__(self, module: ModuleType, spec: dict[str, tuple[tuple[str, ...], str]]) -> None:
        self.module = module
        self.kinds: dict[str, str] = {}
        self.functions: dict[str, Callable[..., object]] = {}
        self.resolved: dict[str, str] = {}
        self.unavailable: dict[str, str] = {}
        missing: list[str] = []
        for metric, (candidates, kind) in spec.items():
            found = next((n for n in candidates if hasattr(module, n)), None)
            if found is None:
                missing.append(f"{metric} (tried {', '.join(candidates)})")
                continue
            self.functions[metric] = getattr(module, found)
            self.resolved[metric] = found
            self.kinds[metric] = kind
        if missing:
            raise AttributeError(f"{module.__name__} does not provide: " + "; ".join(missing))

        # Prefer the companion report where one exists: it returns the same value
        # the plain function does, so taking both from it costs one call rather
        # than two and removes any chance of the value and the counts disagreeing.
        self.reports: dict[str, str] = {}
        for metric, function_name in self.resolved.items():
            companion = function_name + REPORT_SUFFIX
            if hasattr(module, companion):
                self.reports[metric] = companion
                self.functions[metric] = getattr(module, companion)

    def _call(self, metric: str, *args: object) -> tuple[object, dict | None]:
        """Return the metric's value and, when it reported one, its saturation."""
        if metric in self.unavailable:
            return None, None
        try:
            result = self.functions[metric](*args)
        except NotImplementedError as exc:
            self.unavailable[metric] = str(exc) or "not implemented"
            return None, None
        if metric not in self.reports:
            return result, None
        return result.value, _saturation_of(result)

    def pair(
        self, metric: str, a: np.ndarray, b: np.ndarray, sample_rate: int
    ) -> tuple[object, dict | None]:
        return self._call(metric, a, b, sample_rate)

    def single(self, metric: str, x: np.ndarray, sample_rate: int) -> tuple[object, dict | None]:
        return self._call(metric, x, sample_rate)


# ------------------------------------------------------------------- processors


def _declick_params(defect: dict) -> dict:
    """Aim the declicker's level gate under the quietest planted click.

    Its default gate is 0.8, which on a corpus whose beds sit low enough for a
    click to be impulsive rejects every click in the file.
    """
    quietest = min(abs(a) for a in defect["amplitudes"])
    return {
        "threshold": round(0.9 * quietest, 6),
        "max_click_samples": max(8, 4 * int(defect["width_samples"])),
    }


def _dehum_params(defect: dict) -> dict:
    return {
        "fundamental_hz": float(defect["fundamental_hz"]),
        "harmonics": int(defect["harmonic_count"]),
    }


def _declip_params(defect: dict) -> dict:
    """Use the plateau the file actually carries, not the one that was requested.

    The detector is an inclusive ``>=`` against a level and the file's plateau
    sits one quantization step under the requested threshold, so the requested
    number finds nothing.
    """
    return {"clip_threshold": float(defect.get("threshold_in_file", defect["threshold"]))}


def _denoise_params(_defect: dict) -> dict:
    return {}


def _dereverb_params(defect: dict) -> dict:
    """Hand over the reverberation time the file carries, not the one requested.

    The planter's envelope decays 60 dB in T60, which is the decay law the
    dereverberator assumes when it turns ``t60_sec`` into a late-power estimate,
    so the measured T30 and the knob are one quantity. ``late_delay_ms`` is not:
    the planted predelay is where the tail starts, not the mixing time past which
    it is diffuse, so the library's own default stands and the row does not claim
    a planted value for it.
    """
    measured = defect.get("t60_measured_sec")
    usable = measured is not None and math.isfinite(measured)
    return {"t60_sec": float(measured if usable else defect["t60_requested_sec"])}


# Which planted quantities each processor is handed, so a row can say whether it
# was given the answer. An empty tuple means the processor ran on its defaults.
REPAIR_JOBS: dict[str, tuple[str, Callable[[dict], dict], tuple[str, ...]]] = {
    "click": ("mastering_repair_declick", _declick_params, ("amplitudes", "width_samples")),
    "hum": ("mastering_repair_dehum", _dehum_params, ("fundamental_hz", "harmonic_count")),
    "clip": ("mastering_repair_declip", _declip_params, ("threshold_in_file",)),
    "noise": ("mastering_repair_denoise_classical", _denoise_params, ()),
    "reverb": ("mastering_repair_dereverb_classical", _dereverb_params, ("t60_measured_sec",)),
}


# ------------------------------------------------------------------------- feeds


def _as_stereo(audio: np.ndarray) -> np.ndarray:
    return audio[:, None] if audio.ndim == 1 else audio


def mono_feeds(audio: np.ndarray) -> dict[str, np.ndarray]:
    """The mono feeds a restoration entry point can be given, named.

    A mono item yields one feed rather than three copies of the same numbers,
    which would look like agreement between channels that were never separate.
    """
    stereo = _as_stereo(audio)
    if stereo.shape[1] == 1:
        return {"mono": stereo[:, 0]}
    return {
        "downmix": 0.5 * (stereo[:, 0] + stereo[:, 1]),
        "left": stereo[:, 0],
        "right": stereo[:, 1],
    }


def _aligned(a: np.ndarray, b: np.ndarray) -> tuple[np.ndarray, np.ndarray, int]:
    """Truncate a pair to a common length, reporting what was dropped."""
    n = min(a.shape[0], b.shape[0])
    return a[:n], b[:n], int(abs(a.shape[0] - b.shape[0]))


def _f32(x: np.ndarray) -> np.ndarray:
    return np.ascontiguousarray(x, dtype=np.float32)


def _saturation_of(report: object) -> dict:
    """Pull whichever of the saturation fields a report carries."""
    out: dict[str, object] = {}
    for field in SATURATION_FIELDS:
        if not hasattr(report, field):
            continue
        value = getattr(report, field)
        out[field] = round(float(value), 6) if isinstance(value, float) else value
    return out


# -------------------------------------------------------------------- the rows


def restoration_rows(
    item: dict,
    draws: list[np.ndarray],
    clean: np.ndarray,
    metrics: MetricSet,
    sample_rate: int,
) -> list[dict]:
    """One row per (defect, feed), measured over every draw the item carries.

    Most items carry one draw and the row is that measurement. A noise item
    carries several independent draws of the same condition, and its row is
    their mean with the spread beside it -- because the movement a denoiser
    produces there is smaller than the spread between draws, so one draw
    supports "did not get worse" but not "got better".
    """
    rows: list[dict] = []
    clean_feeds = mono_feeds(clean)
    draw_feeds = [mono_feeds(draw) for draw in draws]
    speech = bool(item.get("speech_bearing", False))

    for defect_name, defect in (item["defects"] or {}).items():
        job = REPAIR_JOBS.get(defect_name)
        if job is None:
            continue
        entry_point, build_params, supplied = job
        processor = getattr(libsonare, entry_point)
        params = build_params(defect)

        for feed_name in clean_feeds:
            reference = clean_feeds[feed_name]
            per_draw: list[dict] = []
            per_draw_untreated: list[dict] = []
            saturations: list[dict] = []
            inapplicable: dict[str, str] = {}
            dropped = 0
            elapsed = 0.0

            for feeds in draw_feeds:
                feed = feeds[feed_name]
                started = time.perf_counter()
                out_raw = processor(_f32(feed), sample_rate, **params)
                elapsed += time.perf_counter() - started

                ref, out, missing = _aligned(reference, np.asarray(out_raw, dtype=np.float64))
                _, raw, _ = _aligned(reference, feed)
                dropped = max(dropped, missing)
                values, saturation, inapplicable = _restoration_metrics(
                    metrics, ref, raw, out, sample_rate, speech_bearing=speech
                )
                untreated, _, _ = _restoration_metrics(
                    metrics, ref, raw, raw, sample_rate, speech_bearing=speech
                )
                per_draw.append(values)
                per_draw_untreated.append(untreated)
                saturations.append(saturation)

            # The improvement a change claims is the delta, and its spread has to
            # be taken pairwise: a draw's processed and untreated numbers come
            # from the same waveform and move together, so combining the two
            # marginal spreads would overstate it.
            deltas = [
                {metric: values[metric] - before[metric] for metric in values}
                for values, before in zip(per_draw, per_draw_untreated, strict=True)
            ]

            row = {
                "family": "restoration",
                "item": item["id"],
                "role": item["role"],
                "defect": defect_name,
                "entry_point": entry_point,
                "feed": feed_name,
                "downmix": DOWNMIX if feed_name == "downmix" else None,
                "sample_rate": sample_rate,
                "params": params,
                "planted_params_supplied": list(supplied),
                "length_mismatch_samples": dropped,
                "seconds": round(elapsed, 4),
                "speech_bearing": speech,
                "realizations": len(per_draw),
                "metrics": _mean_of(per_draw),
                "saturation": _worst_saturation(saturations),
                "inapplicable_metrics": inapplicable,
                "untreated": _mean_of(per_draw_untreated),
                "delta": _mean_of(deltas),
            }
            if len(per_draw) > 1:
                row["metrics_sd"] = _sd_of(per_draw)
                row["untreated_sd"] = _sd_of(per_draw_untreated)
                row["delta_sd"] = _sd_of(deltas)
                row["metrics_per_realization"] = per_draw
                row["untreated_per_realization"] = per_draw_untreated
                row["saturation_basis"] = "worst of the draws"
            rows.append(row)
    return rows


def _mean_of(per_draw: list[dict]) -> dict:
    """Mean of each metric across draws; a single draw passes through unchanged."""
    if len(per_draw) == 1:
        return per_draw[0]
    return {metric: float(np.mean([draw[metric] for draw in per_draw])) for metric in per_draw[0]}


def _sd_of(per_draw: list[dict]) -> dict:
    """Population standard deviation of each metric across draws."""
    return {metric: float(np.std([draw[metric] for draw in per_draw])) for metric in per_draw[0]}


def _worst_saturation(saturations: list[dict]) -> dict:
    """The draw that lost the most frames to a bound, metric by metric.

    Averaging a ceiling fraction would report a row as half-crushed when one
    draw was fully crushed, which is the reading the fraction exists to prevent.
    """
    if len(saturations) == 1:
        return saturations[0]
    worst: dict[str, dict] = {}
    for reported in saturations:
        for metric, block in reported.items():
            current = worst.get(metric)
            if current is None or block.get("ceiling_fraction", 0.0) > current.get(
                "ceiling_fraction", 0.0
            ):
                worst[metric] = block
    return worst


def _restoration_metrics(
    metrics: MetricSet,
    clean: np.ndarray,
    noisy: np.ndarray,
    processed: np.ndarray,
    sample_rate: int,
    *,
    speech_bearing: bool,
) -> tuple[dict, dict, dict]:
    """Every applicable restoration metric for one triple, plus what it reported.

    Log kurtosis ratio is the only one measured against the *input* rather than
    the clean reference: it asks what the processing created, not what survived.

    ``metrics`` carries numbers that were computed and nothing else. A metric
    missing from it is explained in exactly one place: ``inapplicable`` when this
    material is outside its domain, the ledger's ``unavailable_metrics`` when the
    implementation does not exist yet. The two are different answers and a null
    in a shared field would make them the same one.
    """
    values: dict[str, object] = {}
    saturation: dict[str, object] = {}
    inapplicable: dict[str, str] = {}
    for metric, kind in metrics.kinds.items():
        if metric in SPEECH_ONLY_METRICS and not speech_bearing:
            inapplicable[metric] = NOT_SPEECH_BEARING
            continue
        left = noisy if kind == "input_vs_processed" else clean
        value, reported = metrics.pair(metric, left, processed, sample_rate)
        if value is None:
            continue
        values[metric] = _jsonable(value)
        if reported is not None:
            saturation[metric] = reported
    return values, saturation, inapplicable


def mastering_rows(
    item: dict,
    audio: np.ndarray,
    metrics: MetricSet,
    sample_rate: int,
    preset: str,
) -> list[dict]:
    """The chain measured on the stereo entry and on the mono downmix."""
    stereo = _as_stereo(audio)
    rows: list[dict] = []

    jobs: list[tuple[str, str, Callable[[], tuple[np.ndarray, np.ndarray]]]] = []
    if stereo.shape[1] == 2:
        left, right = stereo[:, 0], stereo[:, 1]

        def stereo_job() -> tuple[np.ndarray, np.ndarray]:
            result = libsonare.master_audio_stereo(_f32(left), _f32(right), sample_rate, preset)
            processed = np.stack([np.asarray(result.left), np.asarray(result.right)], axis=1)
            return stereo, processed

        jobs.append(("stereo", "master_audio_stereo", stereo_job))

    downmix = mono_feeds(stereo)["downmix" if stereo.shape[1] == 2 else "mono"]
    feed_name = "downmix" if stereo.shape[1] == 2 else "mono"

    def mono_job() -> tuple[np.ndarray, np.ndarray]:
        result = libsonare.master_audio(_f32(downmix), sample_rate, preset)
        return downmix, np.asarray(result.samples)

    jobs.append((feed_name, "master_audio", mono_job))

    for name, entry_point, run in jobs:
        started = time.perf_counter()
        source, processed = run()
        elapsed = time.perf_counter() - started
        source, processed, dropped = _aligned(source, processed)
        values, saturation = _mastering_metrics(metrics, source, processed, sample_rate)
        rows.append(
            {
                "family": "mastering",
                "item": item["id"],
                "role": item["role"],
                "defect": None,
                "entry_point": entry_point,
                "feed": name,
                "downmix": DOWNMIX if name == "downmix" else None,
                "sample_rate": sample_rate,
                "params": {"preset": preset},
                "planted_params_supplied": [],
                "length_mismatch_samples": dropped,
                "seconds": round(elapsed, 4),
                "speech_bearing": bool(item.get("speech_bearing", False)),
                "realizations": 1,
                "metrics": values,
                "saturation": saturation,
                "inapplicable_metrics": {},
                "chain_readiness": item.get("chain_readiness"),
            }
        )
    return rows


def _mastering_metrics(
    metrics: MetricSet, source: np.ndarray, processed: np.ndarray, sample_rate: int
) -> tuple[dict, dict]:
    """The chain metrics, reported as the input/output pair the contract asks for."""
    values: dict[str, object] = {}
    saturation: dict[str, object] = {}
    for metric, kind in metrics.kinds.items():
        if kind == "pair":
            value, reported = metrics.pair(metric, source, processed, sample_rate)
            if value is None:
                continue
            values[metric] = _jsonable(value)
            if reported is not None:
                saturation[metric] = reported
            continue
        in_value, in_reported = metrics.single(metric, source, sample_rate)
        out_value, out_reported = metrics.single(metric, processed, sample_rate)
        if in_value is None and out_value is None:
            continue
        values[metric] = {"input": _jsonable(in_value), "output": _jsonable(out_value)}
        if in_reported is not None or out_reported is not None:
            saturation[metric] = {"input": in_reported, "output": out_reported}
    return values, saturation


def _jsonable(value: object) -> object:
    """Flatten whatever a metric returned into something json can hold."""
    if isinstance(value, np.generic):
        return value.item()
    if isinstance(value, np.ndarray):
        return [_jsonable(v) for v in value.tolist()]
    if isinstance(value, dict):
        return {str(k): _jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_jsonable(v) for v in value]
    return value


# ------------------------------------------------------------------- the ledger


def row_key(row: dict) -> str:
    """The identity a baseline entry is keyed on."""
    return "/".join(
        [
            row["family"],
            row["item"],
            row["defect"] or "-",
            row["entry_point"],
            row["feed"],
        ]
    )


def as_baseline(rows: list[dict]) -> dict:
    """Shape a run's numbers the way the ledger holds them.

    Thresholds are not filled in here: how much better a metric must get is set
    by measuring what the current code achieves, which is a separate decision
    from recording what it achieved.
    """
    baseline: dict[str, dict] = {"restoration": {}, "mastering": {}}
    for row in rows:
        baseline[row["family"]][row_key(row)] = {
            "metrics": row["metrics"],
            "metrics_sd": row.get("metrics_sd"),
            "delta": row.get("delta"),
            "delta_sd": row.get("delta_sd"),
            "realizations": row["realizations"],
            "saturation": row["saturation"],
            "inapplicable_metrics": row["inapplicable_metrics"],
            "speech_bearing": row["speech_bearing"],
            "feed": row["feed"],
            "entry_point": row["entry_point"],
            "params": row["params"],
        }
    return baseline


def provenance() -> dict:
    """Which tree and which binary produced a set of numbers.

    Lives here rather than beside either caller because a ledger and a gate run
    have to record it the same way: two spellings of the same fact read as two
    different facts a month later. ``nonvacuity`` and ``write_baseline`` both
    take it from here.

    ``status_src`` and ``status_all`` are the point. A run measured on a dirty
    tree is not the commit it names, and nothing in the repository's history can
    say afterwards what was uncommitted at the time -- so it is recorded now or
    it is unknown forever.
    """

    def git(*args: str) -> str:
        return subprocess.run(
            ["git", *args],
            cwd=Path(__file__).resolve().parents[2],
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
        "recorded": True,
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


def _metric_module(name: str) -> ModuleType:
    """Import one metric module, naming what a caller must supply if it is absent."""
    try:
        return importlib.import_module(name)
    except ModuleNotFoundError as exc:  # pragma: no cover - exercised by a missing module
        if exc.name != name:
            raise
        raise SystemExit(
            f"{name}.py is not in {Path(__file__).resolve().parent}; "
            f"the metrics it must provide are listed in README.md"
        ) from exc


def load_item(root: Path, item: dict) -> tuple[list[np.ndarray], np.ndarray | None, int]:
    """Every draw the item carries, its clean reference, and the rate.

    An item without a ``realizations`` list is one draw, so the caller does not
    have two shapes to handle.
    """
    paths = [draw["audio"] for draw in item.get("realizations", [])] or [item["audio"]]
    draws: list[np.ndarray] = []
    sample_rate = 0
    for path in paths:
        audio, rate = read_wav(root / path)
        if sample_rate and rate != sample_rate:
            raise ValueError(f"{item['id']}: draws disagree on sample rate")
        sample_rate = int(rate)
        draws.append(np.asarray(audio, dtype=np.float64))

    clean = None
    if item.get("clean_reference"):
        clean_audio, clean_rate = read_wav(root / item["clean_reference"])
        if clean_rate != sample_rate:
            raise ValueError(f"{item['id']}: reference is {clean_rate} Hz, item is {sample_rate}")
        clean = np.asarray(clean_audio, dtype=np.float64)
    return draws, clean, sample_rate


def run(
    manifest_path: Path,
    *,
    families: tuple[str, ...],
    preset: str,
    only: tuple[str, ...],
) -> dict:
    manifest = json.loads(manifest_path.read_text())
    root = manifest_path.parent

    repair_metrics = None
    chain_metrics = None
    if "restoration" in families:
        repair_metrics = MetricSet(_metric_module("metrics_repair"), RESTORATION_METRICS)
    if "mastering" in families:
        chain_metrics = MetricSet(_metric_module("metrics_chain"), MASTERING_METRICS)

    rows: list[dict] = []
    skipped: list[dict] = []
    for item in manifest["items"]:
        if only and item["id"] not in only:
            continue
        draws, clean, sample_rate = load_item(root, item)

        if repair_metrics is not None:
            if clean is None or not item.get("defects"):
                skipped.append(
                    {
                        "item": item["id"],
                        "family": "restoration",
                        "reason": "no clean reference or no recorded defect quantity",
                    }
                )
            else:
                rows += restoration_rows(item, draws, clean, repair_metrics, sample_rate)

        if chain_metrics is not None:
            # The chain reads the first draw only. Its metrics are level and
            # spectrum statistics, not the small correlations a noise draw moves.
            rows += mastering_rows(item, draws[0], chain_metrics, sample_rate, preset)

        print(f"{item['id']:<24} {len([r for r in rows if r['item'] == item['id']])} rows")

    unavailable: dict[str, str] = {}
    for metrics in (repair_metrics, chain_metrics):
        if metrics is not None:
            unavailable.update(metrics.unavailable)

    # Which metrics reported saturation at all. A metric absent from here was
    # never checked, which is not the same answer as one that reported a
    # ceiling fraction of zero -- and the two decide a comparison differently.
    probes = {
        "restoration": repair_metrics.reports if repair_metrics else None,
        "mastering": chain_metrics.reports if chain_metrics else None,
    }

    return {
        "schema": LEDGER_SCHEMA,
        "contract": "tools/mastering-eval/docs/objective.md",
        "corpus_manifest": str(manifest_path),
        "corpus_seed": manifest.get("seed"),
        "preset": preset,
        "downmix": DOWNMIX,
        "families": list(families),
        "unavailable_metrics": unavailable,
        "saturation_probe": probes,
        "provenance": provenance(),
        "rows": rows,
        "skipped": skipped,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).resolve().parent / "audio" / "manifest.json",
        help="the corpus manifest corpus.py wrote",
    )
    parser.add_argument(
        "--family",
        choices=["restoration", "mastering", "both"],
        default="both",
        help=(
            "which family to measure; the default is both, and one family alone is "
            "for narrowing a run rather than for keeping the default cheap"
        ),
    )
    parser.add_argument("--preset", default="pop", help="mastering preset the chain runs")
    parser.add_argument(
        "--item",
        action="append",
        default=[],
        help="restrict the run to these corpus item ids (repeatable)",
    )
    parser.add_argument("--out", type=Path, help="where the run ledger goes")
    parser.add_argument(
        "--emit-baseline",
        type=Path,
        help="also write the run's numbers in the baseline's shape, to this path",
    )
    args = parser.parse_args()

    if not args.manifest.exists():
        print(f"no corpus at {args.manifest}; run corpus.py first", file=sys.stderr)
        return 1

    families = ("restoration", "mastering") if args.family == "both" else (args.family,)
    started = time.perf_counter()
    ledger = run(args.manifest, families=families, preset=args.preset, only=tuple(args.item))
    ledger["wall_seconds"] = round(time.perf_counter() - started, 3)

    out = args.out or Path(__file__).resolve().parent / "runs" / "latest.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(ledger, indent=2) + "\n")
    print(f"{len(ledger['rows'])} rows in {ledger['wall_seconds']:.1f}s -> {out}")

    if args.emit_baseline:
        args.emit_baseline.parent.mkdir(parents=True, exist_ok=True)
        args.emit_baseline.write_text(json.dumps(as_baseline(ledger["rows"]), indent=2) + "\n")
        print(f"baseline shape -> {args.emit_baseline}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
