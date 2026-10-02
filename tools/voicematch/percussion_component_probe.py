"""Measure native contact/wire/shell ablations against the same patch's output.

This establishes component observability, not agreement with an independent
instrument reference. No source files or patch defaults are written.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from autofit import dylib_path, render_model_rows_subprocess
from autofit_resolve import resolve_probe
from fit_quality import quality_report
from loss_aggregate import robust_score_terms
from loss_weights import LossWeights, cli_weights


def probe(build_dir, drum_note=38, velocities="64,100"):
    args = SimpleNamespace(
        program=0,
        drum_note=drum_note,
        pattern="drum",
        notes=str(drum_note),
        velocities=velocities,
        diagnose=True,
        corpus="",
        spec="auto",
        oracle_wav="",
        validate_notes="",
        validate_velocities="",
    )
    resolve_probe(args)
    build_dir = Path(build_dir).resolve()
    library = dylib_path(build_dir)
    if library is None:
        raise RuntimeError(f"No native library under {build_dir}")
    library_hash = hashlib.sha256(library.read_bytes()).hexdigest()
    reference, reference_audio = render_model_rows_subprocess(
        build_dir, 0, "drum", args.notes, velocities_csv=velocities, want_audio=True
    )
    identity = robust_score_terms(reference, reference, percussive=True)
    objective = LossWeights(cli_weights(args))
    objective.anchor(identity)
    prefix = f"d{drum_note:03d}.percussion"
    controls = {"self": ""}
    controls.update(
        {
            f"no_{name}": f"{prefix}.{key}=0"
            for name, key in (("contact", "contact"), ("wire", "wire_buzz"), ("shell", "shell_mix"))
        }
    )
    records = {}
    for name, overrides in controls.items():
        candidate, audio = (
            render_model_rows_subprocess(
                build_dir,
                0,
                "drum",
                args.notes,
                velocities_csv=velocities,
                overrides=overrides,
                want_audio=True,
            )
            if overrides
            else (reference, reference_audio)
        )
        terms = robust_score_terms(candidate, reference, percussive=True)
        if terms is None:
            raise RuntimeError(f"Unscorable native control: {name}")
        records[name] = {
            "overrides": overrides,
            "audio_sha256": hashlib.sha256(np.asarray(audio).tobytes()).hexdigest(),
            "waveform_difference_rms_ratio": float(
                np.linalg.norm(audio - reference_audio)
                / max(np.linalg.norm(reference_audio), 1e-12)
            ),
            "terms": terms,
            "quality": quality_report(terms, objective, baseline_terms=identity),
        }
    override_response = any(
        record["audio_sha256"] != records["self"]["audio_sha256"]
        for name, record in records.items()
        if name != "self"
    )
    if hashlib.sha256(library.read_bytes()).hexdigest() != library_hash:
        raise RuntimeError("Native library changed during component probe")
    return {
        "schema": "native-percussion-components/v1",
        "build_dir": str(build_dir),
        "library_path": str(library.resolve()),
        "library_sha256": library_hash,
        "audio_basis": "per-render RMS-normalized mono; difference is not raw component amplitude",
        "drum_note": drum_note,
        "velocities": velocities,
        "weights": objective.weights,
        "override_response": "observed" if override_response else "inconclusive",
        "controls": records,
        "limitations": [
            "self-derived oracle establishes component observability only",
            "an inactive component can produce an unchanged control",
            "all unchanged controls cannot distinguish inactive components from unreached overrides",
            "does not establish stochastic detection or listening acceptance",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--drum-note", type=int, default=38)
    parser.add_argument("--velocities", default="64,100")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.write_text(
        json.dumps(
            probe(args.build_dir, args.drum_note, args.velocities), indent=2, allow_nan=False
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
