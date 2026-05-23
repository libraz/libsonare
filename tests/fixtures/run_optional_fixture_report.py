#!/usr/bin/env python3
"""Run optional external fixture checks and save an auditable report."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from audit_manifests import ManifestSpec, audit_manifest
from json_safe import dumps_strict, write_json_strict


SUITES = {
    "music": {
        "env": "SONARE_MUSIC_FIXTURE_ROOT",
        "filter": "[music_dataset]",
        "specs": [
            ("music:bpm", "bpm_manifest.tsv", 1, None, 4),
            ("music:key", "key_manifest.tsv", 1, None, 4),
            ("music:meter", "meter_manifest.tsv", 1, None, 5),
            ("music:beat", "beat_manifest.tsv", 1, 2, 6),
            ("music:downbeat", "downbeat_manifest.tsv", 1, 2, 6),
            ("music:chord", "chord_manifest.tsv", 1, 2, 4),
        ],
    },
    "acoustic": {
        "env": "SONARE_ACOUSTIC_FIXTURE_ROOT",
        "filter": "[acoustic_analyzer][dataset]",
        "specs": [("acoustic:rt60", "manifest.tsv", 1, None, 6)],
    },
    "ebu": {
        "env": "SONARE_EBU_R128_FIXTURE_ROOT",
        "filter": "[mastering][ebu_r128]",
        "specs": [("ebu:r128", "manifest.tsv", 0, None, 6)],
    },
}


def default_root(name: str) -> Path:
    if name == "music":
        return Path(os.environ.get("SONARE_MUSIC_FIXTURE_ROOT", "tests/fixtures/music_eval"))
    if name == "acoustic":
        return Path(os.environ.get("SONARE_ACOUSTIC_FIXTURE_ROOT", "tests/fixtures/acoustic"))
    return Path(os.environ.get("SONARE_EBU_R128_FIXTURE_ROOT", "tests/fixtures/ebu_r128"))


def suite_stats(name: str, root: Path) -> list[dict[str, Any]]:
    stats = []
    for spec_name, manifest, audio_column, annotation_column, fixed_columns in SUITES[name]["specs"]:
        spec = ManifestSpec(
            spec_name,
            root / manifest,
            audio_column,
            annotation_column,
            fixed_columns,
        )
        stats.append(asdict(audit_manifest(root, spec, verbose=False)))
    return stats


def ready_count(stats: list[dict[str, Any]]) -> int:
    return sum(int(item["ready"]) for item in stats)


def provenance_violation_count(stats: list[dict[str, Any]]) -> int:
    return sum(int(item.get("provenance_violations", 0)) for item in stats)


def run_suite(name: str, root: Path, sonare_tests: Path, timeout_seconds: float | None) -> dict[str, Any]:
    env = os.environ.copy()
    env[str(SUITES[name]["env"])] = str(root)
    command = [str(sonare_tests), str(SUITES[name]["filter"])]
    try:
        completed = subprocess.run(
            command,
            text=True,
            capture_output=True,
            env=env,
            check=False,
            timeout=timeout_seconds,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout if isinstance(exc.stdout, str) else (exc.stdout or b"").decode(errors="replace")
        stderr = exc.stderr if isinstance(exc.stderr, str) else (exc.stderr or b"").decode(errors="replace")
        return {
            "suite": name,
            "command": command,
            "return_code": None,
            "accepted_return_code": False,
            "timed_out": True,
            "timeout_seconds": timeout_seconds,
            "observations": extract_observations(stdout),
            "stdout": stdout,
            "stderr": stderr,
        }
    return {
        "suite": name,
        "command": command,
        "return_code": completed.returncode,
        "accepted_return_code": completed.returncode in (0, 4),
        "timed_out": False,
        "observations": extract_observations(completed.stdout),
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def skipped_suite(name: str, reason: str) -> dict[str, Any]:
    return {
        "suite": name,
        "command": [],
        "return_code": 0,
        "accepted_return_code": True,
        "timed_out": False,
        "skipped": True,
        "skip_reason": reason,
        "observations": [],
        "stdout": "",
        "stderr": "",
    }


def wasm_size_run(current: Path, baseline: Path | None) -> dict[str, Any]:
    observations: list[dict[str, Any]] = []
    stderr = ""
    accepted = current.exists()
    if not current.exists():
        stderr = f"missing current WASM artifact: {current}"
    else:
        current_bytes = current.stat().st_size
        item: dict[str, Any] = {
            "metric": "wasm_size",
            "fixture": str(current),
            "current_bytes": current_bytes,
        }
        if baseline is not None:
            item["baseline"] = str(baseline)
            if baseline.exists():
                baseline_bytes = baseline.stat().st_size
                item["baseline_bytes"] = baseline_bytes
                item["increase_percent"] = (
                    (current_bytes - baseline_bytes) * 100.0 / baseline_bytes
                    if baseline_bytes > 0
                    else float("inf")
                )
            else:
                accepted = False
                stderr = f"missing baseline WASM artifact: {baseline}"
        observations.append(item)
    return {
        "suite": "wasm_size",
        "command": ["stat", str(current)] + (["--baseline", str(baseline)] if baseline else []),
        "return_code": 0 if accepted else 1,
        "accepted_return_code": accepted,
        "timed_out": False,
        "observations": observations,
        "stdout": "",
        "stderr": stderr,
    }


def normalized_output(text: str) -> str:
    normalized = " ".join(text.split())
    return re.sub(r"(?<=\d)\.\s+(?=\d)", ".", normalized)


def clean_fixture_name(text: str) -> str:
    fixture = text.strip()
    fixture = re.sub(r"\s*/\s*", "/", fixture)
    fixture = re.sub(r"(?<=\S)-\s+(?=\S)", "-", fixture)
    for extension in ("wav", "wave", "flac", "mp3", "ogg", "m4a", "aac"):
        fixture = fixture.replace(f". {extension}", f".{extension}")
    return fixture


def extract_observations(stdout: str) -> list[dict[str, Any]]:
    text = normalized_output(stdout)
    observations: list[dict[str, Any]] = []

    for match in re.finditer(
        r"Report-only BPM fixture (.+?) measured ([0-9.+\-eE]+) BPM; expected "
        r"([0-9.+\-eE]+) \+/- ([0-9.+\-eE]+)%; relative error ([0-9.+\-eE]+)%",
        text,
    ):
        observations.append(
            {
                "metric": "bpm",
                "fixture": clean_fixture_name(match.group(1)),
                "measured_bpm": float(match.group(2)),
                "expected_bpm": float(match.group(3)),
                "tolerance_percent": float(match.group(4)),
                "relative_error_percent": float(match.group(5)),
            }
        )

    for match in re.finditer(
        r"Report-only key fixture (.+?) measured root=\s*([0-9+\-]+) mode=\s*([0-9+\-]+)"
        r"(?:; expected root=\s*([0-9+\-]+) mode=\s*([0-9+\-]+))?"
        r"(?:; expected rank=\s*([0-9+\-]+)"
        r"(?:; best_corr=\s*([0-9.+\-eE]+) expected_corr=\s*([0-9.+\-eE]+) corr_gap=\s*([0-9.+\-eE]+))?"
        r"; mirex_category=([a-z]+) mirex_score=\s*([0-9.+\-eE]+))?",
        text,
    ):
        expected_root = int(match.group(4)) if match.group(4) is not None else None
        expected_mode = int(match.group(5)) if match.group(5) is not None else None
        observations.append(
            {
                "metric": "key",
                "fixture": clean_fixture_name(match.group(1)),
                "measured_root": int(match.group(2)),
                "measured_mode": int(match.group(3)),
                "expected_root": expected_root,
                "expected_mode": expected_mode,
                "expected_rank": int(match.group(6)) if match.group(6) is not None else None,
                "best_correlation": float(match.group(7)) if match.group(7) is not None else None,
                "expected_correlation": float(match.group(8)) if match.group(8) is not None else None,
                "correlation_gap": float(match.group(9)) if match.group(9) is not None else None,
                "mirex_category": match.group(10),
                "mirex_score": float(match.group(11)) if match.group(11) is not None else None,
                "correct": (
                    expected_root is not None
                    and expected_mode is not None
                    and int(match.group(2)) == expected_root
                    and int(match.group(3)) == expected_mode
                ),
            }
        )

    for metric in ("beat", "downbeat"):
        for match in re.finditer(
            rf"Report-only {metric} fixture (.+?) F-measure ([0-9.+\-eE]+); threshold ([0-9.+\-eE]+)"
            rf"(?:; time_signature ([0-9]+/[0-9]+|unknown))?",
            text,
        ):
            item = {
                "metric": metric,
                "fixture": clean_fixture_name(match.group(1)),
                "f_measure": float(match.group(2)),
                "threshold": float(match.group(3)),
            }
            if match.group(4) and match.group(4) != "unknown":
                item["time_signature"] = match.group(4)
            observations.append(item)

    for match in re.finditer(
        r"Report-only beat improvement fixture (.+?) improvement ([0-9.+\-eE]+); "
        r"baseline_f_measure ([0-9.+\-eE]+); adaptive_f_measure ([0-9.+\-eE]+); threshold "
        r"([0-9.+\-eE]+|nan)",
        text,
    ):
        threshold_text = match.group(5)
        observations.append(
            {
                "metric": "beat_improvement",
                "fixture": clean_fixture_name(match.group(1)),
                "improvement": float(match.group(2)),
                "baseline_f_measure": float(match.group(3)),
                "adaptive_f_measure": float(match.group(4)),
                "threshold": float(threshold_text) if threshold_text != "nan" else None,
            }
        )

    for match in re.finditer(
        r"Report-only meter fixture (.+?) measured ([0-9]+)/([0-9]+) confidence "
        r"([0-9.+\-eE]+); expected ([0-9]+)/([0-9]+) min confidence ([0-9.+\-eE]+); correct ([01])",
        text,
    ):
        observations.append(
            {
                "metric": "meter",
                "fixture": clean_fixture_name(match.group(1)),
                "measured_numerator": int(match.group(2)),
                "measured_denominator": int(match.group(3)),
                "confidence": float(match.group(4)),
                "expected_numerator": int(match.group(5)),
                "expected_denominator": int(match.group(6)),
                "min_confidence": float(match.group(7)),
                "correct": match.group(8) == "1",
            }
        )

    for match in re.finditer(
        r"Report-only chord fixture (.+?) WCSR ([0-9.+\-eE]+); threshold ([0-9.+\-eE]+)",
        text,
    ):
        observations.append(
            {
                "metric": "chord_wcsr",
                "fixture": clean_fixture_name(match.group(1)),
                "wcsr": float(match.group(2)),
                "threshold": float(match.group(3)),
            }
        )

    for match in re.finditer(
        r"Report-only chord extended fixture (.+?) WCSR ([0-9.+\-eE]+); threshold "
        r"([0-9.+\-eE]+|nan)",
        text,
    ):
        threshold_text = match.group(3)
        observations.append(
            {
                "metric": "chord_extended_wcsr",
                "fixture": clean_fixture_name(match.group(1)),
                "wcsr": float(match.group(2)),
                "threshold": float(threshold_text) if threshold_text != "nan" else None,
            }
        )

    for match in re.finditer(
        r"Report-only chord detail fixture (.+?) root_accuracy ([0-9.+\-eE]+); "
        r"quality_accuracy ([0-9.+\-eE]+); exact_wcsr ([0-9.+\-eE]+)",
        text,
    ):
        observations.append(
            {
                "metric": "chord_detail",
                "fixture": clean_fixture_name(match.group(1)),
                "root_accuracy": float(match.group(2)),
                "quality_accuracy": float(match.group(3)),
                "exact_wcsr": float(match.group(4)),
            }
        )

    for match in re.finditer(
        r"Report-only chord bass fixture (.+?) accuracy ([0-9.+\-eE]+); threshold ([0-9.+\-eE]+)",
        text,
    ):
        observations.append(
            {
                "metric": "chord_bass_acc",
                "fixture": clean_fixture_name(match.group(1)),
                "accuracy": float(match.group(2)),
                "threshold": float(match.group(3)),
            }
        )

    for match in re.finditer(
        r"Report-only chord change-rate fixture (.+?) changes_per_minute ([0-9.+\-eE]+); threshold "
        r"([0-9.+\-eE]+|nan)",
        text,
    ):
        threshold_text = match.group(3)
        observations.append(
            {
                "metric": "chord_change_rate",
                "fixture": clean_fixture_name(match.group(1)),
                "changes_per_minute": float(match.group(2)),
                "threshold": float(threshold_text) if threshold_text != "nan" else None,
            }
        )

    for match in re.finditer(
        r"Report-only chord change-reduction fixture (.+?) reduction ([0-9.+\-eE]+); "
        r"baseline_changes_per_minute ([0-9.+\-eE]+); smoothed_changes_per_minute "
        r"([0-9.+\-eE]+); threshold ([0-9.+\-eE]+|nan)",
        text,
    ):
        threshold_text = match.group(5)
        observations.append(
            {
                "metric": "chord_change_reduction",
                "fixture": clean_fixture_name(match.group(1)),
                "reduction": float(match.group(2)),
                "baseline_changes_per_minute": float(match.group(3)),
                "smoothed_changes_per_minute": float(match.group(4)),
                "threshold": float(threshold_text) if threshold_text != "nan" else None,
            }
        )

    for match in re.finditer(
        r"Report-only acoustic fixture ((?:(?! Report-only ).)+?) measured RT60 ([0-9.+\-eE]+)s; expected "
        r"([0-9.+\-eE]+)s \+/- ([0-9.+\-eE]+)s(?:; mode ([a-z]+))?",
        text,
    ):
        item = {
            "metric": "rt60",
            "fixture": clean_fixture_name(match.group(1)),
            "measured_sec": float(match.group(2)),
            "expected_sec": float(match.group(3)),
            "tolerance_sec": float(match.group(4)),
        }
        if match.group(5):
            item["mode"] = match.group(5)
        observations.append(item)

    for metric in ("edt", "c50", "c80", "d50"):
        unit = "s" if metric == "edt" else ("dB" if metric in ("c50", "c80") else "")
        escaped_unit = re.escape(unit)
        for match in re.finditer(
            rf"Report-only acoustic fixture ((?:(?! Report-only ).)+?) measured {metric.upper()} "
            rf"([0-9.+\-eE]+){escaped_unit}; expected ([0-9.+\-eE]+)"
            rf"{escaped_unit} \+/- ([0-9.+\-eE]+){escaped_unit}",
            text,
        ):
            observations.append(
                {
                    "metric": metric,
                    "fixture": clean_fixture_name(match.group(1)),
                    "measured": float(match.group(2)),
                    "expected": float(match.group(3)),
                    "tolerance": float(match.group(4)),
                }
            )

    for metric, observed_name in (
        ("ebu_lufs", "integrated_lufs"),
        ("ebu_lra", "lra"),
        ("ebu_true_peak", "true_peak_db"),
    ):
        for match in re.finditer(
            rf"Report EBU R128 fixture ((?:(?! Report EBU R128 fixture ).)+?) measured "
            rf"{observed_name} ([0-9.+\-eE]+); expected "
            rf"([0-9.+\-eE]+) \+/- ([0-9.+\-eE]+)",
            text,
        ):
            observations.append(
                {
                    "metric": metric,
                    "fixture": clean_fixture_name(match.group(1)),
                    "measured": float(match.group(2)),
                    "expected": float(match.group(3)),
                    "tolerance": float(match.group(4)),
                }
            )

    return observations


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--suite",
        choices=("all", "music", "acoustic", "ebu"),
        default="all",
        help="Fixture suite to run.",
    )
    parser.add_argument("--music-root", type=Path, default=default_root("music"))
    parser.add_argument("--acoustic-root", type=Path, default=default_root("acoustic"))
    parser.add_argument("--ebu-root", type=Path, default=default_root("ebu"))
    parser.add_argument("--sonare-tests", type=Path, default=Path("build/bin/sonare_tests"))
    parser.add_argument("--output", type=Path, help="Write JSON report to this path.")
    parser.add_argument(
        "--include-wasm-size",
        action="store_true",
        help="Add current/baseline WASM artifact size observations to the report.",
    )
    parser.add_argument("--wasm-current", type=Path, default=Path("bindings/wasm/dist/sonare.wasm"))
    parser.add_argument("--wasm-baseline", type=Path, help="Baseline WASM artifact for size delta.")
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=0.0,
        help="Per-suite test timeout. 0 disables the timeout.",
    )
    parser.add_argument(
        "--require-ready",
        action="store_true",
        help="Fail if the selected suite has no ready fixture rows.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    selected = tuple(SUITES) if args.suite == "all" else (args.suite,)
    roots = {
        "music": args.music_root,
        "acoustic": args.acoustic_root,
        "ebu": args.ebu_root,
    }
    timeout_seconds = args.timeout_seconds if args.timeout_seconds > 0.0 else None

    report: dict[str, Any] = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "sonare_tests": str(args.sonare_tests),
        "suites": {},
        "runs": [],
    }
    for name in selected:
        stats = suite_stats(name, roots[name])
        ready = ready_count(stats)
        report["suites"][name] = {
            "root": str(roots[name]),
            "stats": stats,
            "ready": ready,
            "provenance_violations": provenance_violation_count(stats),
        }
        if ready == 0 and not args.require_ready:
            report["runs"].append(skipped_suite(name, "no ready optional fixture rows"))
        else:
            report["runs"].append(run_suite(name, roots[name], args.sonare_tests, timeout_seconds))
    if args.include_wasm_size:
        report["runs"].append(wasm_size_run(args.wasm_current, args.wasm_baseline))

    report["ok"] = all(run["accepted_return_code"] for run in report["runs"])
    report["ok"] = report["ok"] and all(
        report["suites"][name]["provenance_violations"] == 0 for name in selected
    )
    if args.require_ready:
        report["ok"] = report["ok"] and all(report["suites"][name]["ready"] > 0 for name in selected)

    if args.output:
        write_json_strict(args.output, report, indent=2, sort_keys=True)
    else:
        print(dumps_strict(report, indent=2, sort_keys=True))

    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
